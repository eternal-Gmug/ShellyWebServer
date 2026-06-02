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
- `timewait(lock, time_ms)`：超时等待（无谓词），单位为毫秒。返回 `true` 表示未被超时唤醒，**但调用方需自行检查共享状态**，因为虚假唤醒也可能返回 `true`。
- `timewait(lock, time_ms, pred)`：带谓词的超时等待（模板重载）。仅在谓词满足时返回 `true`，超时时返回 `false`。**推荐在多线程场景下优先使用此版本**，可自动处理虚假唤醒。
- `signal()`：唤醒一个等待线程。
- `broadcast()`：唤醒所有等待线程。

> **通知顺序建议**：`signal()`/`broadcast()` 应放在锁释放之后调用，避免被唤醒的线程立即因拿不到锁而再次阻塞（减少无效调度）。

示例（谓词版）：
```cpp
Lock lock;
Cond cond;
bool ready = false;

// 等待线程（谓词版，自动处理虚假唤醒）
{
  std::unique_lock<std::mutex> lk(lock.get());
  cond.wait(lk, [&]{ return ready; });
}

// 超时等待线程（谓词版）
{
  std::unique_lock<std::mutex> lk(lock.get());
  if (cond.timewait(lk, 5000, [&]{ return ready; }))
  {
    // 条件满足
  }
  else
  {
    // 超时
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
- 条件变量等待请搭配明确的共享状态条件。`Cond` 提供了带谓词模板的 `wait()` 和 `timewait()` 重载，可自动规避虚假唤醒，推荐优先使用谓词版本。
- 通知操作（`signal`/`broadcast`）建议放在 `unlock()` 之后，减少被唤醒线程的无效锁竞争。

### Sem::shutdown() 不可逆与重新初始化模式

`shutdown()` 将内部的 `m_shutdown` 标志置为 `true` 并广播唤醒所有等待线程，此后 `wait()` 和 `timewait()` 将**永久返回 `false`**。`Sem` 不提供 `unshutdown()` 或 `reset()` 方法，因为：

- 存在竞态窗口：如果在 `unshutdown()` 和线程重试 `wait()` 之间池状态不一致，可能导致逻辑错误
- 更安全的做法是销毁旧 `Sem` 对象并构造新实例

**推荐模式** — 使用 `std::unique_ptr<Sem>` 管理信号量生命周期：

```cpp
class connection_pool {
    std::unique_ptr<Sem> reserve;

    void DestroyPool() {
        if (reserve) reserve->shutdown();  // 唤醒阻塞线程
        // ... 清理其他资源 ...
        // 注意：不在此处 reset()，让旧 Sem 保持 shutdown 态
    }

    bool init(int max_conn) {
        // ... 建连 ...
        reserve = std::make_unique<Sem>(max_conn);  // 新 Sem 覆盖旧对象
        return true;
    }

    MYSQL* GetConnection(int timeout_ms) {
        if (!reserve->timewait(timeout_ms))  // 旧 Sem 已 shutdown → 立即返回 false
            return nullptr;
        // ... 正常借出 ...
    }

    ~connection_pool() {
        DestroyPool();
        reserve.reset();  // 最终释放
    }
};
```

> **设计要点**：`DestroyPool()` 只调用 `shutdown()` 而不 `reset()`，确保已 shutdown 的 `Sem` 仍在位——任何在 `DestroyPool()` 和 `init()` 之间调用 `GetConnection()` 的线程都会因 `timewait()` 返回 `false` 而安全退出，**不会访问空指针**。`init()` 时 `make_unique` 创建新 `Sem` 覆盖旧对象，旧 `Sem` 的析构自动执行。
