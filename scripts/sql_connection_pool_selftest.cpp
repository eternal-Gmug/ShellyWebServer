#include "../include/sql_connection_pool.h"
#include "../include/log.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

// ── Test Framework ────────────────────────────────────────────

struct TestState {
    int failed = 0;
    int skipped = 0;

    void pass(const char *msg) {
        std::cout << "[PASS] " << msg << "\n";
    }
    void fail(const char *msg) {
        ++failed;
        std::cerr << "[FAIL] " << msg << "\n";
    }
    void skip(const char *msg) {
        ++skipped;
        std::cout << "[SKIP] " << msg << "\n";
    }
    void expect(bool cond, const char *msg) {
        cond ? pass(msg) : fail(msg);
    }
};

// ── Helpers ───────────────────────────────────────────────────

const char *env_or_null(const char *name) {
    const char *val = std::getenv(name);
    return (val && *val) ? val : nullptr;
}

std::string trim_ws(const std::string &text) {
    const std::string ws = " \t\r\n";
    auto start = text.find_first_not_of(ws);
    if (start == std::string::npos) return "";
    return text.substr(start, text.find_last_not_of(ws) - start + 1);
}

void load_dotenv(const std::string &path) {
    std::ifstream file(path);
    if (!file.is_open()) return;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim_ws(line.substr(0, eq));
        std::string val = trim_ws(line.substr(eq + 1));
        if (key.empty() || val.empty()) continue;
        if ((val.front() == '"' && val.back() == '"') ||
            (val.front() == '\'' && val.back() == '\''))
            val = val.substr(1, val.size() - 2);
        setenv(key.c_str(), val.c_str(), 1);
    }
}

struct DbCreds {
    std::string host, user, pass, db;
    int port = 3306;
    bool valid = false;
};

DbCreds load_creds() {
    DbCreds c;
    const char *h = env_or_null("MYSQL_HOST");
    const char *u = env_or_null("MYSQL_USER");
    const char *p = env_or_null("MYSQL_PASSWORD");
    const char *d = env_or_null("MYSQL_DB");
    const char *pt = env_or_null("MYSQL_PORT");
    if (!h || !u || !p || !d || !pt) return c;
    c.host = h; c.user = u; c.pass = p; c.db = d;
    c.port = std::atoi(pt);
    c.valid = true;
    return c;
}

bool init_pool(connection_pool &pool, const DbCreds &c, int max_conn, int log_flag) {
    return pool.init(c.host, c.user, c.pass, c.db, c.port, max_conn, log_flag);
}

void reset_pool(connection_pool &pool) {
    pool.DestroyPool();
}

// ───────────────────────────────────────────────────────────────
// 🟢  Normal Functional Tests
// ───────────────────────────────────────────────────────────────

void test_init_success(TestState &s, const DbCreds &c) {
    if (!c.valid) { s.skip("init success - no DB creds"); return; }
    connection_pool &pool = connection_pool::GetInstance();
    reset_pool(pool);
    bool ok = init_pool(pool, c, 3, -1);
    s.expect(ok, "init returns true with valid credentials");
    s.expect(pool.GetFreeConn() == 3, "GetFreeConn == max_conn after init");
    reset_pool(pool);
}

void test_get_release_single(TestState &s, const DbCreds &c) {
    if (!c.valid) { s.skip("get/release single - no DB creds"); return; }
    connection_pool &pool = connection_pool::GetInstance();
    reset_pool(pool);
    if (!init_pool(pool, c, 2, -1)) { s.fail("get/release single - init failed"); return; }

    MYSQL *conn = pool.GetConnection();
    s.expect(conn != nullptr, "GetConnection returns non-null");
    s.expect(pool.GetFreeConn() == 1, "GetFreeConn == 1 after one get");

    bool released = pool.ReleaseConnection(conn);
    s.expect(released, "ReleaseConnection returns true");
    s.expect(pool.GetFreeConn() == 2, "GetFreeConn == 2 after release");

    reset_pool(pool);
}

void test_get_all_release_all(TestState &s, const DbCreds &c) {
    if (!c.valid) { s.skip("get all / release all - no DB creds"); return; }
    connection_pool &pool = connection_pool::GetInstance();
    reset_pool(pool);
    const int N = 4;
    if (!init_pool(pool, c, N, -1)) { s.fail("get all / release all - init failed"); return; }

    std::vector<MYSQL *> conns;
    for (int i = 0; i < N; ++i) {
        MYSQL *con = pool.GetConnection();
        s.expect(con != nullptr, "get all: connection non-null");
        conns.push_back(con);
    }
    s.expect(pool.GetFreeConn() == 0, "GetFreeConn == 0 after getting all");

    for (auto *con : conns) {
        s.expect(pool.ReleaseConnection(con), "release all: ReleaseConnection ok");
    }
    s.expect(pool.GetFreeConn() == N, "GetFreeConn == N after releasing all");

    reset_pool(pool);
}

void test_raii_basic(TestState &s, const DbCreds &c) {
    if (!c.valid) { s.skip("RAII basic - no DB creds"); return; }
    connection_pool &pool = connection_pool::GetInstance();
    reset_pool(pool);
    if (!init_pool(pool, c, 2, -1)) { s.fail("RAII basic - init failed"); return; }

    {
        MYSQL *conn = nullptr;
        connectionRAII raii(&conn, &pool);
        s.expect(conn != nullptr, "RAII: conn acquired in constructor");
        s.expect(pool.GetFreeConn() == 1, "RAII: free count decreased");
    }
    s.expect(pool.GetFreeConn() == 2, "RAII: free count restored after scope exit");

    reset_pool(pool);
}

void test_raii_move(TestState &s, const DbCreds &c) {
    if (!c.valid) { s.skip("RAII move - no DB creds"); return; }
    connection_pool &pool = connection_pool::GetInstance();
    reset_pool(pool);
    if (!init_pool(pool, c, 3, -1)) { s.fail("RAII move - init failed"); return; }

    // move construct: transfer ownership, source becomes empty
    {
        MYSQL *c1 = nullptr;
        connectionRAII r1(&c1, &pool);
        s.expect(c1 != nullptr, "RAII move: first RAII acquired conn");
        s.expect(pool.GetFreeConn() == 2, "RAII move: free=2 after first get");

        MYSQL *c2 = nullptr;
        connectionRAII r2(std::move(r1));
        // r2 now owns the connection; r1 is empty (its destructor no-ops)
        s.expect(pool.GetFreeConn() == 2, "RAII move: free=2 unchanged after move ctor");
        // r2 destructor releases
    }
    s.expect(pool.GetFreeConn() == 3, "RAII move ctor: pool restored after scope");

    // move assignment: old connection released, new one transferred
    {
        MYSQL *c3 = nullptr;
        connectionRAII r3(&c3, &pool);
        s.expect(c3 != nullptr, "RAII move assign: r3 acquired");

        MYSQL *c4 = nullptr;
        connectionRAII r4(&c4, &pool);
        s.expect(c4 != nullptr, "RAII move assign: r4 acquired");
        s.expect(pool.GetFreeConn() == 1, "RAII move assign: free=1 after two gets");

        r4 = std::move(r3);
        // r4's old connection (c4) released; r4 now owns c3; r3 is empty
        s.expect(pool.GetFreeConn() == 2, "RAII move assign: old released, free=2");
    }
    s.expect(pool.GetFreeConn() == 3, "RAII move assign: all released");

    reset_pool(pool);
}

void test_log_flag_modes(TestState &s, const DbCreds &c) {
    if (!c.valid) { s.skip("log flag modes - no DB creds"); return; }
    connection_pool &pool = connection_pool::GetInstance();

    // log enabled (flag=0)
    reset_pool(pool);
    bool ok0 = init_pool(pool, c, 1, 0);
    s.expect(ok0, "log_flag=0: init succeeds");
    if (ok0) {
        MYSQL *conn = pool.GetConnection();
        s.expect(conn != nullptr, "log_flag=0: GetConnection ok");
        pool.ReleaseConnection(conn);
    }

    // log disabled (flag=-1)
    reset_pool(pool);
    bool ok1 = init_pool(pool, c, 1, -1);
    s.expect(ok1, "log_flag=-1: init succeeds");
    if (ok1) {
        MYSQL *conn = pool.GetConnection();
        s.expect(conn != nullptr, "log_flag=-1: GetConnection ok");
        pool.ReleaseConnection(conn);
    }

    reset_pool(pool);
}

// ───────────────────────────────────────────────────────────────
// 🟡  Edge / Error Tests
// ───────────────────────────────────────────────────────────────

void test_init_failure_bad_creds(TestState &s) {
    connection_pool &pool = connection_pool::GetInstance();
    reset_pool(pool);
    bool ok = pool.init("127.0.0.1", "no_such_user", "bad_pass", "no_such_db", 3306, 1, -1);
    s.expect(!ok, "init fails with invalid credentials");
    s.expect(pool.GetFreeConn() == 0, "GetFreeConn == 0 after failed init");
}

void test_init_zero_max_conn(TestState &s, const DbCreds &c) {
    if (!c.valid) { s.skip("init zero max_conn - no DB creds"); return; }
    connection_pool &pool = connection_pool::GetInstance();
    reset_pool(pool);
    bool ok = init_pool(pool, c, 0, -1);
    s.expect(ok, "init with max_conn=0 returns true (empty pool)");
    s.expect(pool.GetFreeConn() == 0, "GetFreeConn == 0 for empty pool");

    MYSQL *conn = pool.GetConnection(100);
    s.expect(conn == nullptr, "GetConnection on empty pool times out");

    reset_pool(pool);
}

void test_get_timeout(TestState &s, const DbCreds &c) {
    if (!c.valid) { s.skip("get timeout - no DB creds"); return; }
    connection_pool &pool = connection_pool::GetInstance();
    reset_pool(pool);
    if (!init_pool(pool, c, 1, -1)) { s.fail("get timeout - init failed"); return; }

    MYSQL *held = pool.GetConnection();
    s.expect(held != nullptr, "get timeout: first get succeeds");

    auto t0 = std::chrono::steady_clock::now();
    MYSQL *conn2 = pool.GetConnection(100);
    auto t1 = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);

    s.expect(conn2 == nullptr, "GetConnection times out when pool exhausted");
    s.expect(elapsed.count() >= 80, "timeout waited at least ~80ms");

    pool.ReleaseConnection(held);
    reset_pool(pool);
}

void test_release_null(TestState &s) {
    connection_pool &pool = connection_pool::GetInstance();
    bool ok = pool.ReleaseConnection(nullptr);
    s.expect(!ok, "ReleaseConnection(nullptr) returns false");
}

void test_release_unowned(TestState &s, const DbCreds &c) {
    if (!c.valid) { s.skip("release unowned - no DB creds"); return; }
    connection_pool &pool = connection_pool::GetInstance();
    reset_pool(pool);
    if (!init_pool(pool, c, 1, -1)) { s.fail("release unowned - init failed"); return; }

    MYSQL *raw = mysql_init(nullptr);
    if (!raw) { s.skip("release unowned - mysql_init failed"); reset_pool(pool); return; }

    // releasing a connection not from the pool: should not crash
    pool.ReleaseConnection(raw);
    s.pass("ReleaseConnection on external conn does not crash");

    reset_pool(pool);
}

void test_destroy_pool_twice(TestState &s, const DbCreds &c) {
    if (!c.valid) { s.skip("destroy twice - no DB creds"); return; }
    connection_pool &pool = connection_pool::GetInstance();
    reset_pool(pool);
    if (!init_pool(pool, c, 2, -1)) { s.fail("destroy twice - init failed"); return; }

    pool.DestroyPool();
    pool.DestroyPool();  // second destroy should be safe
    s.pass("DestroyPool twice does not crash");

    MYSQL *conn = pool.GetConnection(100);
    s.expect(conn == nullptr, "GetConnection after DestroyPool returns nullptr");
}

void test_get_free_conn_thread_safe(TestState &s, const DbCreds &c) {
    if (!c.valid) { s.skip("GetFreeConn thread-safe - no DB creds"); return; }
    connection_pool &pool = connection_pool::GetInstance();
    reset_pool(pool);
    if (!init_pool(pool, c, 3, -1)) { s.fail("GetFreeConn thread-safe - init failed"); return; }

    std::atomic<int> errors{0};
    auto reader = [&pool, &errors](int) {
        for (int i = 0; i < 50; ++i) {
            int n = pool.GetFreeConn();
            if (n < 0 || n > 10) ++errors;
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i)
        threads.emplace_back(reader, i);
    for (auto &t : threads) t.join();

    s.expect(errors == 0, "GetFreeConn concurrent reads stay in valid range");
    reset_pool(pool);
}

// ───────────────────────────────────────────────────────────────
// 🔴  Concurrency / Stress Tests
// ───────────────────────────────────────────────────────────────

void test_concurrent_get_release(TestState &s, const DbCreds &c) {
    if (!c.valid) { s.skip("concurrent get/release - no DB creds"); return; }
    connection_pool &pool = connection_pool::GetInstance();
    reset_pool(pool);
    const int POOL_SIZE = 5;
    const int THREADS = 10;
    const int OPS_PER_THREAD = 100;

    if (!init_pool(pool, c, POOL_SIZE, -1)) {
        s.fail("concurrent get/release - init failed"); return;
    }

    std::atomic<int> total_gets{0};
    std::atomic<int> total_releases{0};
    std::atomic<int> errors{0};

    auto worker = [&](int id) {
        for (int i = 0; i < OPS_PER_THREAD; ++i) {
            MYSQL *conn = pool.GetConnection(5000);
            if (!conn) { ++errors; continue; }
            ++total_gets;
            std::this_thread::sleep_for(std::chrono::microseconds(50 + (id * 17) % 200));
            if (!pool.ReleaseConnection(conn)) ++errors;
            else ++total_releases;
        }
    };

    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> threads;
    for (int i = 0; i < THREADS; ++i)
        threads.emplace_back(worker, i);
    for (auto &t : threads) t.join();
    auto t1 = std::chrono::steady_clock::now();

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    int expected = THREADS * OPS_PER_THREAD;
    double throughput = (elapsed_ms > 0) ? (total_gets.load() * 1000.0 / elapsed_ms) : 0;

    s.expect(errors == 0, "concurrent: no errors");
    s.expect(total_gets == expected, "concurrent: all gets succeeded");
    s.expect(total_releases == expected, "concurrent: all releases succeeded");
    s.expect(pool.GetFreeConn() == POOL_SIZE, "concurrent: pool back to full");

    std::cout << "  [PERF] " << THREADS << "t × " << OPS_PER_THREAD
              << " ops, " << elapsed_ms << "ms, "
              << static_cast<int>(throughput) << " ops/sec\n";

    reset_pool(pool);
}

void test_concurrent_mpmc(TestState &s, const DbCreds &c) {
    if (!c.valid) { s.skip("MPMC - no DB creds"); return; }
    connection_pool &pool = connection_pool::GetInstance();
    reset_pool(pool);
    const int POOL_SIZE = 4;
    const int WORKERS = 7;
    const int DURATION_SEC = 2;

    if (!init_pool(pool, c, POOL_SIZE, -1)) {
        s.fail("MPMC - init failed"); return;
    }

    std::atomic<bool> stop{false};
    std::atomic<int> errors{0};
    std::atomic<int> total_ops{0};

    auto worker = [&](int id) {
        while (!stop) {
            MYSQL *conn = pool.GetConnection(2000);
            if (!conn) continue;
            std::this_thread::sleep_for(std::chrono::microseconds(50 + id * 30));
            if (!pool.ReleaseConnection(conn)) ++errors;
            ++total_ops;
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < WORKERS; ++i)
        threads.emplace_back(worker, i);

    std::this_thread::sleep_for(std::chrono::seconds(DURATION_SEC));
    stop = true;
    for (auto &t : threads) t.join();

    s.expect(errors == 0, "MPMC: no errors");
    s.expect(total_ops > 0, "MPMC: operations completed");
    s.expect(pool.GetFreeConn() == POOL_SIZE, "MPMC: pool restored to full");

    std::cout << "  [PERF] MPMC " << WORKERS << " workers, "
              << total_ops << " ops in " << DURATION_SEC << "s, ~"
              << (total_ops / DURATION_SEC) << " ops/sec\n";

    reset_pool(pool);
}

void test_stress_sustained(TestState &s, const DbCreds &c) {
    if (!c.valid) { s.skip("stress test - no DB creds"); return; }
    connection_pool &pool = connection_pool::GetInstance();
    reset_pool(pool);
    const int POOL_SIZE = 4;
    const int THREADS = 8;
    const int DURATION_SEC = 3;

    if (!init_pool(pool, c, POOL_SIZE, -1)) {
        s.fail("stress test - init failed"); return;
    }

    std::atomic<bool> stop{false};
    std::atomic<long long> total_ops{0};
    std::atomic<long long> timeout_count{0};
    std::atomic<int> errors{0};

    auto worker = [&](int id) {
        while (!stop) {
            MYSQL *conn = pool.GetConnection(3000);
            if (!conn) { ++timeout_count; continue; }
            std::this_thread::sleep_for(
                std::chrono::microseconds(500 + (id * 271) % 1500));
            if (!pool.ReleaseConnection(conn)) ++errors;
            ++total_ops;
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < THREADS; ++i)
        threads.emplace_back(worker, i);

    std::this_thread::sleep_for(std::chrono::seconds(DURATION_SEC));
    stop = true;
    for (auto &t : threads) t.join();

    long long ops = total_ops.load();
    double throughput = ops / static_cast<double>(DURATION_SEC);

    s.expect(errors == 0, "stress: no errors during sustained load");
    s.expect(ops > 0, "stress: operations completed");
    s.expect(pool.GetFreeConn() == POOL_SIZE, "stress: pool restored to full");

    std::cout << "  [PERF] Stress: " << THREADS << "t × " << DURATION_SEC
              << "s, " << ops << " ops, "
              << static_cast<int>(throughput) << " ops/sec, "
              << timeout_count << " timeouts\n";

    reset_pool(pool);
}

void test_destroy_during_wait(TestState &s, const DbCreds &c) {
    if (!c.valid) { s.skip("destroy during wait - no DB creds"); return; }
    connection_pool &pool = connection_pool::GetInstance();
    reset_pool(pool);
    const int POOL_SIZE = 1;
    if (!init_pool(pool, c, POOL_SIZE, -1)) {
        s.fail("destroy during wait - init failed"); return;
    }

    MYSQL *held = pool.GetConnection();
    s.expect(held != nullptr, "destroy-during-wait: first get ok");

    std::atomic<bool> unblocked{false};
    std::thread waiter([&]() {
        MYSQL *c = pool.GetConnection(10000);
        unblocked = true;
        if (c) pool.ReleaseConnection(c);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    pool.DestroyPool();

    auto t0 = std::chrono::steady_clock::now();
    while (!unblocked && std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::steady_clock::now() - t0)
                              .count() < 5) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    waiter.join();

    s.expect(unblocked, "destroy-during-wait: blocked thread unblocked after DestroyPool");
    if (held) mysql_close(held);

    reset_pool(pool);
}

} // namespace

// ───────────────────────────────────────────────────────────────
// main
// ───────────────────────────────────────────────────────────────

int main() {
    Log::getInstance().init();

    TestState s;
    load_dotenv(".env");
    DbCreds creds = load_creds();

    std::cout << "\n=== sql_connection_pool Selftest ===\n";
    if (creds.valid) {
        std::cout << "  DB: " << creds.user << "@" << creds.host << ":"
                  << creds.port << "/" << creds.db << "\n";
    } else {
        std::cout << "  No DB credentials found. DB-dependent tests will be skipped.\n";
        std::cout << "  Set MYSQL_HOST/MYSQL_USER/MYSQL_PASSWORD/MYSQL_DB/MYSQL_PORT\n";
    }
    std::cout << "\n";

    // ── 🟢 Normal Functional Tests ──
    std::cout << "── 🟢 Normal Functional Tests ──\n";
    test_init_success(s, creds);
    test_get_release_single(s, creds);
    test_get_all_release_all(s, creds);
    test_raii_basic(s, creds);
    test_raii_move(s, creds);
    test_log_flag_modes(s, creds);

    // ── 🟡 Edge / Error Tests ──
    std::cout << "\n── 🟡 Edge / Error Tests ──\n";
    test_init_failure_bad_creds(s);
    test_init_zero_max_conn(s, creds);
    test_get_timeout(s, creds);
    test_release_null(s);
    test_release_unowned(s, creds);
    test_destroy_pool_twice(s, creds);
    test_get_free_conn_thread_safe(s, creds);

    // ── 🔴 Concurrency / Stress Tests ──
    std::cout << "\n── 🔴 Concurrency / Stress Tests ──\n";
    test_concurrent_get_release(s, creds);
    test_concurrent_mpmc(s, creds);
    test_stress_sustained(s, creds);
    test_destroy_during_wait(s, creds);

    // ── Summary ──
    std::cout << "\n========================================\n";
    if (s.failed == 0) {
        std::cout << "✅ All tests passed";
    } else {
        std::cout << "❌ " << s.failed << " test(s) FAILED";
    }
    if (s.skipped > 0)
        std::cout << " (" << s.skipped << " skipped)";
    std::cout << "\n========================================\n";

    return s.failed > 0 ? 1 : 0;
}
