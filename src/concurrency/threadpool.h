#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <iostream>
#include <vector>
#include <queue>
#include <atomic>
#include <thread>
#include <functional>
#include <utility>
#include "utils/lock/lock.h"
#include "../include/log.h"
#include "../include/sql_connection_pool.h"

template <typename T>
class threadpool
{
public:
    /*
     * @param actor_model   reactor model: 1 = proactor, others = semi-synchronous / semi-reactor
     * @param connPool      database connection pool pointer (not write for the time being)
     * @param thread_number thread number (default: 8)
     * @param max_requests  max requests number（default: 10000）
     */
    threadpool(int actor_model, int thread_number = 8, int max_request = 10000);
    ~threadpool();

    // prohibit copying and duplication
    threadpool(const threadpool &) = delete;
    threadpool &operator=(const threadpool &) = delete;
    threadpool(threadpool &&) = delete;
    threadpool &operator=(threadpool &&) = delete;

    /*
     * add task（Proactor mode，set status）
     * @return false (queue is full, refuse request)
     */
    bool append(T *request, int status);

    /*
     * add task（semi-synchronous / semi-reactor，don't set status）
     * @return false (queue is full, refuse request)
     */
    bool append_p(T *request);

    // template <class F, class... Args>
    // bool enqueue(F &&f, Args &&...args)
    // {
    //     std::function<void()> task = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
    //     m_queue_lock.lock();
    //     workqueue.emplace(std::move(task));
    //     m_queue_lock.unlock();
    //     queue_cv.signal();
    //     return true;
    // }

    // Query the current length of the waiting queue
    size_t queue_size() const;

    // return thread quantity
    int thread_count() { return m_thread_num; }

private:
    void run();

    int m_thread_num;                   // the threads' number of threadpool
    std::vector<std::thread> m_workers; // work thread

    int m_max_requests;          // the max requests server allowed
    std::queue<T *> m_workqueue; // task queue
    // std::queue<std::function<void()>> workqueue;
    connection_pool* m_connpool; // the database connection pool pointer, which will be passed to the task for database operations
    mutable Lock m_queue_lock;   // the mutex lock protecting workqueue
    Cond queue_cv;               // queue's condition variable

    std::atomic<bool> m_stop; // record threadpool status
    int m_actor_model;        // record reactor model, 1 means reactor, 0 means proactor
};

template <typename T>
threadpool<T>::threadpool(int actor_model, int thread_number, int max_request)
    : m_thread_num(thread_number),
      m_max_requests(max_request),
      m_stop(false),
      m_actor_model(actor_model)
{
    if (thread_number <= 0 || max_request <= 0)
    {
        LOG_ERROR("thread_number and max_request must be greater than 0! current thread_number: %d, max_request: %d", thread_number, max_request);
        throw std::invalid_argument("thread_number and max_request must be greater than 0!");
    }
    // pre-allocate thread_number threads and bind them to the run function
    m_workers.reserve(thread_number);

    // create worker threads
    for (int i = 0; i < thread_number; i++)
    {
        m_workers.emplace_back(&threadpool::run, this);
    }

    // initialize database connection pool pointer to the singleton (no ownership, do not delete)
    m_connpool = &connection_pool::GetInstance();
}

template <typename T>
threadpool<T>::~threadpool()
{
    // set stop flag (atomic variable, no need to lock)
    m_stop.store(true, std::memory_order_release);
    // wake up all worker threads to let them exit
    queue_cv.broadcast();
    // join all worker threads
    for (std::thread &worker : m_workers)
    {
        if (worker.joinable())
        {
            worker.join();
        }
    }
}

// TODO: append and append_p have dependency on HTTPConn class, this should be deal later
template <typename T>
bool threadpool<T>::append(T *request, int status)
{
    (void)request;
    (void)status;
    return false;
}

template <typename T>
bool threadpool<T>::append_p(T *request)
{
    (void)request;
    return false;
}

template <typename T>
size_t threadpool<T>::queue_size() const
{
    m_queue_lock.lock();
    size_t size_snapshot = m_workqueue.size();
    m_queue_lock.unlock();
    return size_snapshot;
}

template <typename T>
void threadpool<T>::run()
{
    while (!m_stop.load(std::memory_order_acquire))
    {
        T *request = nullptr;
        // 1. get task from the queue
        {
            std::unique_lock<std::mutex> lock(m_queue_lock.get());
            // 1.1 use predicate to avoid spurious wakeup
            queue_cv.wait(lock, [this]()
                          { return m_stop.load(std::memory_order_acquire) || !m_workqueue.empty(); });
                // 1.2 if stop flag is true and there is no task, exit thread
                if (m_stop.load(std::memory_order_acquire) && m_workqueue.empty())
            {
                return;
            }
            // 1.3 get task from the queue
            request = m_workqueue.front();
            m_workqueue.pop();
        } // release lock before processing task

        if (request == nullptr)
        {
            continue;
        }

        // TODO: process task based on different reactor models
        if(m_actor_model == 1){
            // 2.1 Reactor model, the main thread only listens for events and doesn't read request data, so worker thread needs to read the request data and set the status before processing the request
        }
        else{
            // 2.2 Proactor model, the main thread has already read the request data and set the status, so worker thread can directly process the request
            connectionRAII mysqlcon(&request->mysql, m_connpool);
            request->process();
        }
    }
}

#endif