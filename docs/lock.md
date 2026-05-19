# 锁与同步封装说明

本文件说明 `Sem`、`Lock` 与 `Cond` 三个同步原语封装的用途与使用方式，便于在服务端并发模块中快速复用。

## 模块概览
- `Sem`: 基于计数器的信号量，控制并发资源数量。
- `Lock`: 互斥锁的轻量封装，提供 `lock()/unlock()` 与底层 `std::mutex` 访问。
- `Cond`: 条件变量封装，支持等待、超时等待以及通知。

## 类说明与用法
### Sem
- 典型用途：控制同时访问某资源的线程数。
- 关键行为：`wait()` 等待资源计数大于 0；`post()` 释放资源并唤醒一个等待线程。

示例：
```cpp
Sem sem(3);

sem.wait();
// ... 访问受限资源 ...
sem.post();
```

### Lock
- 典型用途：保护共享数据结构，避免并发读写冲突。
- `get()` 可用于和 `std::unique_lock<std::mutex>` 配合使用。

示例：
```cpp
Lock lock;
{
  std::lock_guard<std::mutex> guard(lock.get());
  // ... 临界区 ...
}
```

### Cond
- 典型用途：线程间等待与通知。
- `wait()`：无超时等待。
- `timewait()`：超时等待，单位为毫秒。
- `signal()`：唤醒一个等待线程。
- `broadcast()`：唤醒所有等待线程。

示例：
```cpp
Lock lock;
Cond cond;
bool ready = false;

// 等待线程
{
  std::unique_lock<std::mutex> lk(lock.get());
  while (!ready) {
    cond.wait(lk);
  }
}

// 通知线程
{
  std::lock_guard<std::mutex> guard(lock.get());
  ready = true;
}
cond.signal();
```

## 约定与注意事项
- `Sem::wait()` 可能阻塞，调用方需确保在退出路径上成对调用 `post()`。
- `Cond::timewait()` 以毫秒为单位，适合短时间等待的场景。
- 条件变量等待请搭配明确的共享状态条件，实现中重载了等待函数的模板谓词版本，避免虚假唤醒带来的逻辑错误。
