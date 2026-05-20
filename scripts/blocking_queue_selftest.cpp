#include "blocking_queue.h"

#include <cassert>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

// single thread condition
namespace {

int g_failures = 0;

void expect(bool condition, const char *message) {
    if (!condition) {
        ++g_failures;
        std::cerr << "[FAIL] " << message << "\n";
    } else {
        std::cout << "[PASS] " << message << "\n";
    }
}

void test_constructor_invalid_capacity() {
    bool threw = false;
    try {
        BlockingQueue<int> q(0);
        (void)q;
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    expect(threw, "constructor rejects non-positive capacity");
}

void test_empty_front_back() {
    BlockingQueue<int> q(3);
    int v = -1;
    expect(q.empty(), "empty queue reports empty");
    expect(!q.front(v), "front on empty returns false");
    expect(!q.back(v), "back on empty returns false");
}

void test_push_pop_order() {
    BlockingQueue<int> q(3);
    expect(q.push(10), "push 10");
    expect(q.push(20), "push 20");
    int v = 0;
    expect(q.pop(v) && v == 10, "pop returns FIFO 10");
    expect(q.pop(v) && v == 20, "pop returns FIFO 20");
    expect(q.empty(), "queue empty after pops");
}

void test_full_and_capacity() {
    BlockingQueue<int> q(2);
    expect(q.capacity() == 2, "capacity reports correctly");
    expect(q.push(1), "push 1");
    expect(q.push(2), "push 2");
    expect(q.isFull(), "queue reports full");
    expect(!q.push(3), "push fails when full");
}

void test_wrap_around() {
    BlockingQueue<int> q(2);
    int v = 0;
    expect(q.push(1), "wrap push 1");
    expect(q.push(2), "wrap push 2");
    expect(q.pop(v) && v == 1, "wrap pop 1");
    expect(q.push(3), "wrap push 3 after pop");
    expect(q.pop(v) && v == 2, "wrap pop 2");
    expect(q.pop(v) && v == 3, "wrap pop 3");
}

void test_clear() {
    BlockingQueue<int> q(2);
    int v = 0;
    q.push(1);
    q.push(2);
    q.clear();
    expect(q.empty(), "clear empties queue");
    expect(!q.pop(v, 1), "pop after clear times out");
}

void test_timeout_pop() {
    BlockingQueue<int> q(1);
    int v = 0;
    auto start = std::chrono::steady_clock::now();
    bool ok = q.pop(v, 50);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    expect(!ok, "timeout pop returns false on empty");
    expect(elapsed.count() >= 40, "timeout pop waits approximately given time");
}

void test_blocking_pop_with_producer() {
    BlockingQueue<int> q(1);
    int v = 0;
    std::thread producer([&q]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        q.push(42);
    });

    bool ok = q.pop(v);
    producer.join();
    expect(ok && v == 42, "blocking pop waits until producer pushes");
}

} // namespace

int main() {
    test_constructor_invalid_capacity();
    test_empty_front_back();
    test_push_pop_order();
    test_full_and_capacity();
    test_wrap_around();
    test_clear();
    test_timeout_pop();
    test_blocking_pop_with_producer();

    if (g_failures == 0) {
        std::cout << "All blocking queue tests passed.\n";
        return 0;
    }

    std::cerr << g_failures << " test(s) failed.\n";
    return 1;
}
