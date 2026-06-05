#include "../src/utils/signal/signal.h"
#include "log.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

// ── Test Framework ────────────────────────────────────────────

struct TestState {
    int failed = 0;
    int skipped = 0;

    void pass(const char *msg) { std::cout << "[PASS] " << msg << "\n"; }
    void fail(const char *msg) {
        ++failed;
        std::cerr << "[FAIL] " << msg << "\n";
    }
    void skip(const char *msg) {
        ++skipped;
        std::cout << "[SKIP] " << msg << "\n";
    }
    void expect(bool cond, const char *msg) { cond ? pass(msg) : fail(msg); }
};

// ── Helpers ───────────────────────────────────────────────────

struct PipeGuard {
    int fds[2] = {-1, -1};
    PipeGuard() {
        if (pipe(fds) < 0) {
            fds[0] = fds[1] = -1;
        }
    }
    ~PipeGuard() {
        if (fds[0] >= 0) close(fds[0]);
        if (fds[1] >= 0) close(fds[1]);
    }
    bool ok() const { return fds[0] >= 0 && fds[1] >= 0; }
};

struct EpollGuard {
    int fd = -1;
    EpollGuard() { fd = epoll_create1(EPOLL_CLOEXEC); }
    ~EpollGuard() { if (fd >= 0) close(fd); }
    bool ok() const { return fd >= 0; }
};

struct SocketPairGuard {
    int fds[2] = {-1, -1};
    SocketPairGuard() {
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) {
            fds[0] = fds[1] = -1;
        }
    }
    ~SocketPairGuard() {
        if (fds[0] >= 0) close(fds[0]);
        if (fds[1] >= 0) close(fds[1]);
    }
    bool ok() const { return fds[0] >= 0 && fds[1] >= 0; }
};

// ───────────────────────────────────────────────────────────────
// 🟢  Normal Functional Tests
// ───────────────────────────────────────────────────────────────

void test_init_stores_pipefd(TestState &s) {
    PipeGuard pg;
    if (!pg.ok()) { s.skip("init - pipe creation failed"); return; }

    SignalHandler sh;
    sh.init(pg.fds, -1);
    s.expect(SignalHandler::signal_pipefd != nullptr,
             "signal_pipefd is not null after init");
    s.expect(SignalHandler::signal_pipefd[0] == pg.fds[0],
             "signal_pipefd[0] matches pipe read end");
    s.expect(SignalHandler::signal_pipefd[1] == pg.fds[1],
             "signal_pipefd[1] matches pipe write end");
}

void test_set_nonblocking(TestState &s) {
    PipeGuard pg;
    if (!pg.ok()) { s.skip("setNonBlocking - pipe creation failed"); return; }

    int ret = SignalHandler::setNonBlocking(pg.fds[0]);
    s.expect(ret >= 0, "setNonBlocking returns >= 0 on success");

    int flags = fcntl(pg.fds[0], F_GETFL);
    s.expect((flags & O_NONBLOCK) != 0, "fd is set to non-blocking after setNonBlocking");
}

void test_add_fd_to_epoll(TestState &s) {
    PipeGuard pg;
    EpollGuard ep;
    if (!pg.ok() || !ep.ok()) { s.skip("addFd - resource creation failed"); return; }

    // add pipe read end to epoll
    SignalHandler::addFd(ep.fd, pg.fds[0], false, 0);
    s.pass("addFd with level-triggered (trigger_mode=0) does not crash");

    // verify fd is registered by writing and polling
    char c = 'x';
    write(pg.fds[1], &c, 1); // write to pipe

    epoll_event ev{};
    int n = epoll_wait(ep.fd, &ev, 1, 100);
    s.expect(n == 1, "epoll_wait detects data on pipe after addFd");
}

void test_add_fd_edge_triggered(TestState &s) {
    PipeGuard pg;
    EpollGuard ep;
    if (!pg.ok() || !ep.ok()) { s.skip("addFd ET - resource creation failed"); return; }

    SignalHandler::addFd(ep.fd, pg.fds[0], false, 0);

    // trigger_mode=1 → ET
    SignalHandler::setNonBlocking(pg.fds[0]);
    // Remove old fd and re-add with ET (simplistic — just test addFd doesn't crash)
    epoll_ctl(ep.fd, EPOLL_CTL_DEL, pg.fds[0], nullptr);
    SignalHandler::addFd(ep.fd, pg.fds[0], true, 1);
    s.pass("addFd with edge-triggered + one_shot does not crash");
}

void test_add_signal_success(TestState &s) {
    // Use SIGUSR1 - safe for testing, doesn't terminate
    bool ok = SignalHandler::addSignal(SIGUSR1, SignalHandler::signalHandler, true);
    s.expect(ok, "addSignal for SIGUSR1 returns true");
}

void test_show_error_sends_message(TestState &s) {
    SocketPairGuard sp;
    if (!sp.ok()) { s.skip("showError - socketpair failed"); return; }

    SignalHandler::showError(sp.fds[1], "TEST_ERROR_MSG");

    // read from the other end
    char buf[128] = {0};
    ssize_t n = read(sp.fds[0], buf, sizeof(buf) - 1);
    s.expect(n > 0, "showError: data received on peer socket");
    if (n > 0) {
        s.expect(std::string(buf).find("TEST_ERROR_MSG") != std::string::npos,
                 "error message contains expected text");
    }
}

void test_signal_handler_writes_to_pipe(TestState &s) {
    PipeGuard pg;
    if (!pg.ok()) { s.skip("signalHandler - pipe creation failed"); return; }

    SignalHandler sh;
    sh.init(pg.fds, -1);

    // call signalHandler directly (not via signal delivery)
    SignalHandler::signalHandler(SIGUSR1);

    // read from the read end of the pipe
    char buf[4] = {0};
    ssize_t n = read(pg.fds[0], buf, sizeof(buf));
    s.expect(n == 1, "signalHandler writes exactly 1 byte to pipe");
    s.expect(static_cast<int>(buf[0]) == SIGUSR1,
             "written byte equals signal number");
}

void test_signal_handler_multiple_signals(TestState &s) {
    PipeGuard pg;
    if (!pg.ok()) { s.skip("signalHandler multiple - pipe creation failed"); return; }

    SignalHandler sh;
    sh.init(pg.fds, -1);

    SignalHandler::signalHandler(SIGINT);
    SignalHandler::signalHandler(SIGTERM);
    SignalHandler::signalHandler(SIGPIPE);

    char buf[3] = {0};
    // set read end non-blocking for this test
    fcntl(pg.fds[0], F_SETFL, fcntl(pg.fds[0], F_GETFL) | O_NONBLOCK);
    ssize_t n = read(pg.fds[0], buf, sizeof(buf));
    s.expect(n == 3, "3 signal writes produce 3 bytes in pipe");
    if (n >= 3) {
        s.expect(static_cast<int>(buf[0]) == SIGINT,  "first byte = SIGINT");
        s.expect(static_cast<int>(buf[1]) == SIGTERM, "second byte = SIGTERM");
        s.expect(static_cast<int>(buf[2]) == SIGPIPE, "third byte = SIGPIPE");
    }
}

// ───────────────────────────────────────────────────────────────
// 🟡  Edge / Error Tests
// ───────────────────────────────────────────────────────────────

void test_set_nonblocking_invalid_fd(TestState &s) {
    int ret = SignalHandler::setNonBlocking(-1);
    s.expect(ret == -1, "setNonBlocking with invalid fd returns -1");

    ret = SignalHandler::setNonBlocking(99999);
    s.expect(ret == -1, "setNonBlocking with non-existent fd returns -1");
}

void test_add_signal_invalid_signal(TestState &s) {
    // NSIG is usually 64 or 65; use a value far beyond
    bool ok = SignalHandler::addSignal(9999, SignalHandler::signalHandler);
    s.expect(!ok, "addSignal with invalid signal number returns false");
}

void test_show_error_null_message(TestState &s) {
    SocketPairGuard sp;
    if (!sp.ok()) { s.skip("showError null - socketpair failed"); return; }

    // should NOT crash with null message; uses default
    SignalHandler::showError(sp.fds[1], nullptr);
    s.pass("showError with nullptr message does not crash");
}

void test_signal_handler_null_pipe(TestState &s) {
    // reset pipefd to null to test guard
    PipeGuard pg;
    SignalHandler sh;
    sh.init(nullptr, -1);

    // should NOT crash — guard checks signal_pipefd
    SignalHandler::signalHandler(SIGTERM);
    s.pass("signalHandler with null signal_pipefd does not crash");
}

void test_add_fd_invalid_epoll(TestState &s) {
    PipeGuard pg;
    if (!pg.ok()) { s.skip("addFd invalid epoll - pipe failed"); return; }

    // This should not crash
    SignalHandler::addFd(-1, pg.fds[0], false, 0);
    s.pass("addFd with invalid epollfd does not crash");

    SignalHandler::addFd(99999, pg.fds[0], false, 0);
    s.pass("addFd with non-existent epollfd does not crash");
}

// ───────────────────────────────────────────────────────────────
// 🔴  Concurrency / Stress Tests
// ───────────────────────────────────────────────────────────────

void test_concurrent_signal_writes(TestState &s) {
    PipeGuard pg;
    if (!pg.ok()) { s.skip("concurrent signal - pipe creation failed"); return; }
    SignalHandler sh;
    sh.init(pg.fds, -1);

    const int N_THREADS = 8;
    const int CALLS_PER_THREAD = 100;
    std::atomic<int> total_sent{0};

    auto worker = [&]() {
        for (int i = 0; i < CALLS_PER_THREAD; ++i) {
            SignalHandler::signalHandler(SIGUSR1);
            ++total_sent;
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < N_THREADS; ++i)
        threads.emplace_back(worker);
    for (auto &t : threads)
        t.join();

    // read all bytes from pipe
    fcntl(pg.fds[0], F_SETFL, fcntl(pg.fds[0], F_GETFL) | O_NONBLOCK);

    int total_read = 0;
    char buf[256];
    while (true) {
        ssize_t n = read(pg.fds[0], buf, sizeof(buf));
        if (n <= 0) break;
        total_read += n;
    }

    s.expect(total_read == total_sent.load(),
             "bytes read from pipe equals total signal writes");
    s.expect(total_sent.load() == N_THREADS * CALLS_PER_THREAD,
             "all signal writes completed");
}

} // namespace

int main() {
    TestState s;

    std::cout << "\n=== 🟢 Normal Functional Tests ===\n";
    test_init_stores_pipefd(s);
    test_set_nonblocking(s);
    test_add_fd_to_epoll(s);
    test_add_fd_edge_triggered(s);
    test_add_signal_success(s);
    test_show_error_sends_message(s);
    test_signal_handler_writes_to_pipe(s);
    test_signal_handler_multiple_signals(s);

    std::cout << "\n=== 🟡 Edge / Error Tests ===\n";
    test_set_nonblocking_invalid_fd(s);
    test_add_signal_invalid_signal(s);
    test_show_error_null_message(s);
    test_signal_handler_null_pipe(s);
    test_add_fd_invalid_epoll(s);

    std::cout << "\n=== 🔴 Concurrency / Stress Tests ===\n";
    test_concurrent_signal_writes(s);

    std::cout << "\n";
    if (s.failed == 0 && s.skipped == 0) {
        std::cout << "✅ All SignalHandler tests passed.\n";
        return 0;
    }
    if (s.failed == 0) {
        std::cout << "✅ All SignalHandler tests passed (" << s.skipped << " skipped).\n";
        return 0;
    }
    std::cerr << "❌ " << s.failed << " test(s) failed";
    if (s.skipped > 0) std::cerr << ", " << s.skipped << " skipped";
    std::cerr << ".\n";
    return 1;
}
