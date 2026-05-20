// Blocking queue implemented based on circular array
#ifndef BLOCK_QUEUE_H
#define BLOCK_QUEUE_H

#include <iostream>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include "utils/lock/lock.h"

template <typename T>
class BlockingQueue
{
public:
    explicit BlockingQueue(int capacity = 1000)
    {
        if (capacity <= 0)
        {
            throw std::invalid_argument("BlockingQueue's capacity must be greater than 0!");
        }
        m_capacity = capacity;
        m_array = new T[m_capacity];
        m_size = 0;
        m_head = -1;
        m_tail = -1;
    }

    ~BlockingQueue()
    {
        m_mtx.lock();
        if (m_array != nullptr)
        {
            delete[] m_array;
        }
        m_mtx.unlock();
    }

    // clear the blocking queue to original status
    void clear()
    {
        m_mtx.lock();
        m_size = 0;
        m_head = -1;
        m_tail = -1;
        m_mtx.unlock();
    }

    // ckeck if the queue is full
    bool isFull()
    {
        m_mtx.lock();
        int size_snapshot = m_size;
        m_mtx.unlock();
        return (size_snapshot >= m_capacity) ? true : false;
    }

    // check if the queue is empty
    bool empty()
    {
        m_mtx.lock();
        int size_snapshot = m_size;
        m_mtx.unlock();
        return size_snapshot == 0;
    }

    // get the front item
    bool front(T &value)
    {
        m_mtx.lock();
        // the queue is empty, you can't copy the front
        if (m_size == 0)
        {
            m_mtx.unlock();
            return false;
        }
        // there is a minor performance issue here, involving the overhead of copy construction
        value = m_array[m_head];
        m_mtx.unlock();
        return true;
    }

    // get the back item
    bool back(T &value)
    {
        m_mtx.lock();
        // the queue is empty, you can't copy the end
        if (m_size == 0)
        {
            m_mtx.unlock();
            return false;
        }
        // there is a minor performance issue here, involving the overhead of copy construction
        value = m_array[m_tail];
        m_mtx.unlock();
        return true;
    }

    // get the size of current queue
    int size()
    {
        m_mtx.lock();
        int size_snapshot = m_size;
        m_mtx.unlock();
        return size_snapshot;
    }

    // get the capacity of this queue
    int capacity()
    {
        m_mtx.lock();
        int capacity_snapshot = m_capacity;
        m_mtx.unlock();
        return capacity_snapshot;
    }

    // produce item
    bool push(const T &item)
    {
        m_mtx.lock();
        if (m_size >= m_capacity)
        {
            m_cv.broadcast();
            m_mtx.unlock();
            return false;
        }
        m_tail = (m_tail + 1) % m_capacity;
        m_array[m_tail] = item;
        m_size++;
        // notify one thread to consume this queue
        m_cv.signal();
        m_mtx.unlock();
        return true;
    }

    // consume item without timeout
    bool pop(T &item)
    {
        std::unique_lock<std::mutex> m_ul(m_mtx.get());
        // if queue is empty, wait util having item
        m_cv.wait(m_ul, [this]()
                  { return m_size > 0; });
        m_head = (m_head + 1) % m_capacity;
        item = m_array[m_head];
        m_size--;
        return true;
    }

    // consume item with timeout
    bool pop(T &item, int time_ms)
    {
        std::unique_lock<std::mutex> m_ul(m_mtx.get());
        // if queue is empty, wait util having item
        // if timeout, it will return false
        if (!m_cv.timewait(m_ul, time_ms))
        {
            return false;
        }
        m_head = (m_head + 1) % m_capacity;
        item = m_array[m_head];
        m_size--;
        return true;
    }

private:
    T *m_array;
    int m_capacity;
    int m_head;
    int m_tail;
    int m_size;
    Lock m_mtx;
    Cond m_cv;
};

#endif