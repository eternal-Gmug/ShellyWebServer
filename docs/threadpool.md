# 高性能服务器线程池(ThreadPool)说明

## 技术点说明
### 1、**mutable**的作用
代码片段
```
int m_max_requests;          // the max requests server allowed
std::queue<T *> m_workqueue; // task queue
mutable Lock m_queue_lock;   // the mutex lock protecting workqueue
Cond queue_cv;               // queue's condition variable
```
原因在于下面有一个const获取队列长度的函数
```
size_t queue_size() const
{
    // 因为这是一个 const 函数，编译器认为这个函数里所有的成员变量都变成了 const（不可修改）
    // 而 m_queue_mutex.lock() 和 unlock() 本质上是要修改锁内部的计数器/状态位
    std::lock_guard<std::mutex> lock(m_queue_mutex); // 👈 编译死在这里
    
    return m_workqueue.size();
}
```
使用mutable允许在const函数内，这个参数依旧有**被修改的权利**，逻辑上的const只是你看一眼有多少任务，整个类的业务数据确实没有被修改；但在物理层面为了在多线程环境下安全地看一眼，底层的互斥锁必须在物理上发生“加锁，解锁”的状态改变。

### 2、为什么m_stop要使用atomic<bool>的原子性读写操作
- 1、atomic<bool>的作用是什么

保证读写操作是原子性不可分割的，主线程写```m_stop = true```时，不会有其他线程在这个操作进行到一半时读到一个“中间状态”。

保证内存可见性和顺序，现代CPU和编译器为了优化性能，它会四处乱序执行指令，并不每次从主存中读写，而是从各自的核心中读取缓存，并且利用```std::memory_order_release```防止指令重排，一旦这一行代码执行完，其他线程只要通过acquire或默认序去读就**一定能读到最新的值**。

- 2、为什么m_stop需要用atomic去修饰呢？

首先它作为停止工作线程的标识符，它会被当作工作线程的谓词变量去判断是否需要结束执行函数，在这种多线程并发场景下原子化操作它就非常有必要，当然也可以通过给它加个```lock_guard()```作用锁去修改它的值，但作为一个布尔值，使用无锁原子化的操作更加廉价。


