#ifndef LOCKER_H
#define LOCKER_H

#include <mutex>
#include <condition_variable>
#include <chrono>

// 1. Semaphore class
class Sem
{
public:
    // constructor with explicit keyword to prevent implicit conversions, which can lead to unexpected behavior
    explicit Sem(int num = 0) : m_count(num) { m_shutdown = false; }
    ~Sem() = default;

    // P operation: wait and consume resource
    bool wait()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        // wait until there is at least one resource available
        m_cv.wait(lock, [this]()
                  { return m_count > 0 || m_shutdown; });
        if (m_shutdown)
        {
            return false; // if shutdown, return false to indicate failure
        }
        // get resource successfully, m_count - 1
        m_count--;
        return true;
    }

    // V operation: release and give back resource
    void post()
    {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            // release resource, m_count + 1
            m_count++;
        }
        // notify a waiting thread after releasing lock
        m_cv.notify_one();
    }

    // broadcast to unblock all waiting threads, used for shutdown
    void shutdown()
    {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_shutdown = true; // set shutdown flag to true to unblock all waiting threads
        }
        m_cv.notify_all(); // wake up all waiting threads
    }

private:
    int m_count;
    bool m_shutdown;
    std::mutex m_mutex;
    std::condition_variable m_cv;
};

// 2. Mutual exclusion lock class
class Lock
{
public:
    Lock() = default;
    ~Lock() = default;

    void lock()
    {
        m_mutex.lock();
    }

    void unlock()
    {
        m_mutex.unlock();
    }

    std::mutex &get()
    {
        return m_mutex;
    }

private:
    std::mutex m_mutex;
};

// 3. condition variable class
class Cond
{
public:
    Cond() = default;
    ~Cond() = default;

    void wait(std::unique_lock<std::mutex> &lock)
    {
        m_cv.wait(lock);
    }

    // predicate template
    template <class Predicate>
    void wait(std::unique_lock<std::mutex> &lock, Predicate pred)
    {
        m_cv.wait(lock, pred);
    }

    template <class Predicate>
    bool timewait(std::unique_lock<std::mutex> &lock, int time_ms, Predicate pred)
    {
        // set an expiration time to wait for the resource. If the time expires or not satisfy pred, return false.
        return m_cv.wait_for(lock, std::chrono::milliseconds(time_ms), pred);
    }

    void signal()
    {
        m_cv.notify_one();
    }

    void broadcast()
    {
        m_cv.notify_all();
    }

private:
    std::condition_variable m_cv;
};

#endif // LOCKER_H