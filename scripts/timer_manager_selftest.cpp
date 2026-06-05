#include "timer_manager.h"
#include "log.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>
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

struct CallbackTracker {
    std::mutex mtx;
    std::vector<int> called_ids;
    void record(int id) {
        std::lock_guard<std::mutex> lk(mtx);
        called_ids.push_back(id);
    }
    bool was_called(int id) {
        std::lock_guard<std::mutex> lk(mtx);
        for (int v : called_ids)
            if (v == id) return true;
        return false;
    }
    int total() {
        std::lock_guard<std::mutex> lk(mtx);
        return static_cast<int>(called_ids.size());
    }
    void reset() {
        std::lock_guard<std::mutex> lk(mtx);
        called_ids.clear();
    }
};

// ───────────────────────────────────────────────────────────────
// 🟢  Normal Functional Tests
// ───────────────────────────────────────────────────────────────

void test_init_creates_timerfd(TestState &s) {
    TimerManager tm;
    bool ok = tm.init();
    s.expect(ok, "init() returns true");
    s.expect(tm.getTimerFd() >= 0, "getTimerFd() returns valid fd (>= 0)");
}

void test_add_timer_returns_valid_id(TestState &s) {
    TimerManager tm;
    tm.init();
    CallbackTracker tracker;

    TimerId id = tm.addTimer(std::chrono::milliseconds(100),
                             [&tracker] { tracker.record(1); });
    s.expect(id != 0, "addTimer returns non-zero ID");
    s.expect(tm.activeTimers() == 1, "activeTimers == 1 after addTimer");
}

void test_add_multiple_timers(TestState &s) {
    TimerManager tm;
    tm.init();
    CallbackTracker tracker;

    TimerId id1 = tm.addTimer(std::chrono::milliseconds(500), [&tracker] { tracker.record(1); });
    TimerId id2 = tm.addTimer(std::chrono::milliseconds(300), [&tracker] { tracker.record(2); });
    TimerId id3 = tm.addTimer(std::chrono::milliseconds(400), [&tracker] { tracker.record(3); });

    s.expect(id1 != 0 && id2 != 0 && id3 != 0, "all addTimer calls return non-zero IDs");
    s.expect(id1 != id2 && id2 != id3 && id1 != id3, "timer IDs are unique");
    s.expect(tm.activeTimers() == 3, "activeTimers == 3");
}

void test_remove_timer(TestState &s) {
    TimerManager tm;
    tm.init();
    CallbackTracker tracker;

    TimerId id = tm.addTimer(std::chrono::milliseconds(100), [&tracker] { tracker.record(1); });
    s.expect(tm.activeTimers() == 1, "activeTimers == 1 after add");

    bool removed = tm.removeTimer(id);
    s.expect(removed, "removeTimer returns true for existing ID");
    s.expect(tm.activeTimers() == 0, "activeTimers == 0 after remove");

    // wait and tick - callback should NOT fire
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    tm.tick();
    s.expect(!tracker.was_called(1), "callback not fired after timer was removed");
}

void test_tick_processes_expired_timers(TestState &s) {
    TimerManager tm;
    tm.init();
    CallbackTracker tracker;

    tm.addTimer(std::chrono::milliseconds(20), [&tracker] { tracker.record(1); });
    tm.addTimer(std::chrono::milliseconds(40), [&tracker] { tracker.record(2); });
    s.expect(tm.activeTimers() == 2, "activeTimers == 2 before tick");

    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    tm.tick();

    s.expect(tracker.was_called(1), "callback 1 fired (20ms timer)");
    s.expect(tracker.was_called(2), "callback 2 fired (40ms timer)");
    s.expect(tm.activeTimers() == 0, "activeTimers == 0 after all expired");
}

void test_tick_does_not_fire_future_timers(TestState &s) {
    TimerManager tm;
    tm.init();
    CallbackTracker tracker;

    tm.addTimer(std::chrono::milliseconds(500), [&tracker] { tracker.record(1); });
    tm.tick();

    s.expect(!tracker.was_called(1), "future timer callback NOT fired");
    s.expect(tm.activeTimers() == 1, "future timer still active after tick");
}

void test_adjust_timer_extends_duration(TestState &s) {
    TimerManager tm;
    tm.init();
    CallbackTracker tracker;

    TimerId id = tm.addTimer(std::chrono::milliseconds(20), [&tracker] { tracker.record(1); });

    // extend to 200ms
    bool adjusted = tm.adjustTimer(id, std::chrono::milliseconds(200));
    s.expect(adjusted, "adjustTimer returns true for valid ID");

    // wait past original time but before adjusted time
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    tm.tick();
    s.expect(!tracker.was_called(1), "callback NOT fired at original time after extension");

    // wait until adjusted time
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    tm.tick();
    s.expect(tracker.was_called(1), "callback fired at adjusted time");
}

void test_adjust_timer_shortens_duration(TestState &s) {
    TimerManager tm;
    tm.init();
    CallbackTracker tracker;

    TimerId id = tm.addTimer(std::chrono::milliseconds(200), [&tracker] { tracker.record(1); });

    // shorten to 20ms
    tm.adjustTimer(id, std::chrono::milliseconds(20));

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    tm.tick();
    s.expect(tracker.was_called(1), "callback fired at shortened time");
}

void test_timer_handle_raii(TestState &s) {
    TimerManager tm;
    tm.init();
    CallbackTracker tracker;

    {
        TimerHandle h = tm.addTimerWithHandle(std::chrono::milliseconds(100),
                                              [&tracker] { tracker.record(1); });
        s.expect(h.valid(), "TimerHandle is valid after creation");
        s.expect(tm.activeTimers() == 1, "activeTimers == 1 while handle alive");
    }
    // handle destroyed → removeTimer called
    s.expect(tm.activeTimers() == 0, "activeTimers == 0 after handle destroyed");
}

void test_timer_handle_move(TestState &s) {
    TimerManager tm;
    tm.init();

    TimerHandle h1 = tm.addTimerWithHandle(std::chrono::milliseconds(500), []{});
    s.expect(h1.valid(), "h1 valid");
    s.expect(tm.activeTimers() == 1, "activeTimers == 1");

    TimerHandle h2(std::move(h1));
    s.expect(!h1.valid(), "h1 invalid after move");
    s.expect(h2.valid(), "h2 valid after move");
    s.expect(tm.activeTimers() == 1, "activeTimers unchanged after move");

    TimerHandle h3 = std::move(h2);
    s.expect(!h2.valid(), "h2 invalid after second move");
    s.expect(h3.valid(), "h3 valid after second move");
}

void test_timer_handle_release(TestState &s) {
    TimerManager tm;
    tm.init();

    TimerHandle h = tm.addTimerWithHandle(std::chrono::milliseconds(500), []{});
    TimerId id = h.release();
    s.expect(id != 0, "release returns valid ID");
    s.expect(!h.valid(), "handle invalid after release");
    s.expect(tm.activeTimers() == 1, "timer still active after release (ownership transferred)");

    tm.removeTimer(id);
}

void test_multiple_ticks_no_timers(TestState &s) {
    TimerManager tm;
    tm.init();

    // tick() on empty timer manager should not crash
    tm.tick();
    tm.tick();
    s.expect(tm.activeTimers() == 0, "tick() on empty manager is safe");
    s.pass("tick() on empty manager does not crash");
}

void test_timer_ids_are_monotonic(TestState &s) {
    TimerManager tm;
    tm.init();

    TimerId prev = 0;
    for (int i = 0; i < 10; ++i) {
        TimerId id = tm.addTimer(std::chrono::milliseconds(1000), []{});
        s.expect(id > prev, "timer ID monotonically increasing");
        prev = id;
    }
}

// ───────────────────────────────────────────────────────────────
// 🟡  Edge / Error Tests
// ───────────────────────────────────────────────────────────────

void test_add_timer_null_callback(TestState &s) {
    TimerManager tm;
    tm.init();

    TimerId id = tm.addTimer(std::chrono::milliseconds(100), nullptr);
    s.expect(id == 0, "addTimer with nullptr callback returns 0");
    s.expect(tm.activeTimers() == 0, "activeTimers == 0 after null callback rejected");
}

void test_remove_timer_invalid_id(TestState &s) {
    TimerManager tm;
    tm.init();

    bool removed = tm.removeTimer(99999);
    s.expect(!removed, "removeTimer with non-existent ID returns false");

    removed = tm.removeTimer(0);
    s.expect(!removed, "removeTimer with ID=0 returns false");
}

void test_adjust_timer_invalid_id(TestState &s) {
    TimerManager tm;
    tm.init();

    bool adjusted = tm.adjustTimer(99999, std::chrono::milliseconds(100));
    s.expect(!adjusted, "adjustTimer with non-existent ID returns false");
}

void test_add_timer_zero_duration(TestState &s) {
    TimerManager tm;
    tm.init();
    CallbackTracker tracker;

    TimerId id = tm.addTimer(std::chrono::milliseconds(0), [&tracker] { tracker.record(1); });
    s.expect(id != 0, "addTimer with 0 duration returns valid ID");

    // immediate tick should fire it
    tm.tick();
    s.expect(tracker.was_called(1), "zero-duration timer fires on immediate tick");
}

void test_double_remove(TestState &s) {
    TimerManager tm;
    tm.init();

    TimerId id = tm.addTimer(std::chrono::milliseconds(100), []{});
    s.expect(tm.removeTimer(id), "first remove succeeds");
    s.expect(!tm.removeTimer(id), "second remove (same ID) returns false");
}

void test_remove_all_then_tick(TestState &s) {
    TimerManager tm;
    tm.init();

    std::vector<TimerId> ids;
    for (int i = 0; i < 5; ++i)
        ids.push_back(tm.addTimer(std::chrono::milliseconds(50 + i * 10), []{}));

    for (auto id : ids)
        tm.removeTimer(id);

    s.expect(tm.activeTimers() == 0, "all timers removed");
    tm.tick(); // should not crash
    s.pass("tick() after removing all timers does not crash");
}

void test_adjust_to_past(TestState &s) {
    TimerManager tm;
    tm.init();
    CallbackTracker tracker;

    // adjust a future timer to expire immediately
    TimerId id = tm.addTimer(std::chrono::milliseconds(500), [&tracker] { tracker.record(1); });
    tm.adjustTimer(id, std::chrono::milliseconds(-500)); // effectively immediate

    tm.tick();
    s.expect(tracker.was_called(1), "timer adjusted to past fires on immediate tick");
}

void test_concurrent_add_and_tick(TestState &s) {
    TimerManager tm;
    tm.init();

    // This is tick with empty set → no expired timers
    tm.tick();
    s.expect(tm.activeTimers() == 0, "activeTimers == 0 after empty tick");

    // add a timer and tick immediately (it may not fire if duration hasn't passed)
    tm.addTimer(std::chrono::milliseconds(100), []{});
    tm.tick(); // future timer, should not fire
    s.expect(tm.activeTimers() == 1, "future timer not removed by premature tick");
}

// ───────────────────────────────────────────────────────────────
// 🔴  Concurrency / Stress Tests
// ───────────────────────────────────────────────────────────────

void test_concurrent_add_timers(TestState &s) {
    TimerManager tm;
    tm.init();
    const int N_THREADS = 8;
    const int TIMERS_PER_THREAD = 200;
    std::atomic<int> added{0};

    auto worker = [&]() {
        for (int i = 0; i < TIMERS_PER_THREAD; ++i) {
            TimerId id = tm.addTimer(std::chrono::milliseconds(10000), []{});
            if (id != 0) ++added;
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < N_THREADS; ++i)
        threads.emplace_back(worker);
    for (auto &t : threads)
        t.join();

    size_t active = tm.activeTimers();
    s.expect(active == static_cast<size_t>(added.load()),
             "activeTimers == total added timers after concurrent add");
    s.expect(added.load() == N_THREADS * TIMERS_PER_THREAD,
             "all timers added successfully");

    // cleanup
    while (tm.activeTimers() > 0) {
        auto it = tm.addTimer(std::chrono::milliseconds(0), []{});
        (void)it;
        tm.tick();
    }
}

void test_concurrent_add_and_remove(TestState &s) {
    TimerManager tm;
    tm.init();
    const int N_THREADS = 4;
    const int OPS_PER_THREAD = 500;
    std::atomic<int> total_added{0};
    std::atomic<int> total_removed{0};

    auto worker = [&]() {
        for (int i = 0; i < OPS_PER_THREAD; ++i) {
            TimerId id = tm.addTimer(std::chrono::milliseconds(10000 + i), []{});
            if (id != 0) {
                ++total_added;
                if (i % 3 == 0) { // remove ~1/3 of timers
                    tm.removeTimer(id);
                    ++total_removed;
                }
            }
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < N_THREADS; ++i)
        threads.emplace_back(worker);
    for (auto &t : threads)
        t.join();

    s.expect(total_added.load() >= N_THREADS * OPS_PER_THREAD / 2,
             "significant number of timers added");
    s.expect(total_removed.load() >= total_added.load() / 4,
             "significant number of timers removed");

    // cleanup
    while (tm.activeTimers() > 0) {
        auto it = tm.addTimer(std::chrono::milliseconds(0), []{});
        (void)it;
        tm.tick();
    }
}

void test_concurrent_callbacks_fire_outside_lock(TestState &s) {
    TimerManager tm;
    tm.init();
    std::atomic<int> callbacks_fired{0};
    std::atomic<int> reentrant_ops{0};

    // Add timers that immediately re-enter TimerManager from callback
    for (int i = 0; i < 3; ++i) {
        tm.addTimer(std::chrono::milliseconds(10), [&]() {
            ++callbacks_fired;
            // reentrant: add another timer from within callback (should not deadlock)
            tm.addTimer(std::chrono::milliseconds(5), [&]() {
                ++reentrant_ops;
            });
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    tm.tick(); // fire first batch

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    tm.tick(); // fire reentrant timers

    s.expect(callbacks_fired.load() == 3, "all original callbacks fired");
    s.expect(reentrant_ops.load() == 3, "all reentrant timers fired (no deadlock)");
}

} // namespace

int main() {
    TestState s;

    std::cout << "\n=== 🟢 Normal Functional Tests ===\n";
    test_init_creates_timerfd(s);
    test_add_timer_returns_valid_id(s);
    test_add_multiple_timers(s);
    test_remove_timer(s);
    test_tick_processes_expired_timers(s);
    test_tick_does_not_fire_future_timers(s);
    test_adjust_timer_extends_duration(s);
    test_adjust_timer_shortens_duration(s);
    test_timer_handle_raii(s);
    test_timer_handle_move(s);
    test_timer_handle_release(s);
    test_multiple_ticks_no_timers(s);
    test_timer_ids_are_monotonic(s);

    std::cout << "\n=== 🟡 Edge / Error Tests ===\n";
    test_add_timer_null_callback(s);
    test_remove_timer_invalid_id(s);
    test_adjust_timer_invalid_id(s);
    test_add_timer_zero_duration(s);
    test_double_remove(s);
    test_remove_all_then_tick(s);
    test_adjust_to_past(s);
    test_concurrent_add_and_tick(s);

    std::cout << "\n=== 🔴 Concurrency / Stress Tests ===\n";
    test_concurrent_add_timers(s);
    test_concurrent_add_and_remove(s);
    test_concurrent_callbacks_fire_outside_lock(s);

    std::cout << "\n";
    if (s.failed == 0 && s.skipped == 0) {
        std::cout << "✅ All TimerManager tests passed.\n";
        return 0;
    }
    if (s.failed == 0) {
        std::cout << "✅ All TimerManager tests passed (" << s.skipped << " skipped).\n";
        return 0;
    }
    std::cerr << "❌ " << s.failed << " test(s) failed";
    if (s.skipped > 0) std::cerr << ", " << s.skipped << " skipped";
    std::cerr << ".\n";
    return 1;
}
