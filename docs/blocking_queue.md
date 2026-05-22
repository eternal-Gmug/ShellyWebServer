# 阻塞队列（BlockingQueue）说明

本模块实现了一个基于循环数组的阻塞队列，适用于生产者/消费者模型。

## 功能概览
- 固定容量队列：构造时指定容量。
- 线程安全：内部使用互斥锁与条件变量。
- 阻塞消费：当队列为空时，消费者阻塞等待。
- 超时消费：支持带超时的 `pop`，通过谓词机制避免虚假唤醒。
- 优雅关闭：支持 `close()` 唤醒所有阻塞消费者，配合 `m_closed` 标志实现安全退出。

## 接口说明
- `push(const T& item)`: 入队，队满返回 `false`。
- `pop(T& item)`: 出队，队空则阻塞等待；队列关闭且无数据时返回 `false`。
- `pop(T& item, int time_ms)`: 超时出队，超时或队列关闭且无数据时返回 `false`。
- `close()`: 关闭队列，唤醒所有阻塞消费者以消费残量后退出。
- `front(T& value) / back(T& value)`: 查看队头/队尾元素。
- `size() / capacity() / empty() / isFull()`: 状态查询。
- `clear()`: 清空队列（保留容量）。

## 使用示例
```cpp
BlockingQueue<int> q(100);
q.push(1);
int v = 0;
q.pop(v);        // 阻塞等待直到有数据
```

### 优雅关闭示例
```cpp
// 消费者线程
int item;
while (q.pop(item))   // pop 返回 false 表示关闭且无残量
{
    process(item);
}
// 线程安全退出

// 主线程退出前
q.close();   // 唤醒消费者，让其消费残量后退出
```

## 核心机制说明

### 关闭机制 (`close` + `m_closed`)
- `close()` 先将 `m_closed` 置为 `true`，解锁后再 `broadcast()` 唤醒所有阻塞消费者。
- `pop()` 的等待谓词为 `m_size > 0 || m_closed`，即"有数据或已关闭"时唤醒。
- 被唤醒后：如果队列还有数据 → 先取数据返回 `true`；如果队列已空且 `m_closed == true` → 返回 `false`。
- 这保证调用 `close()` 后消费者先把残量取完再退出，不丢数据。

### 超时 `pop` 的谓词版
- 超时版使用 `Cond::timewait(lock, time_ms, pred)` 的谓词重载。
- `wait_for` 带谓词语义：超时且谓词仍为 `false` 才返回 `false`；谓词 `true` 立即返回。自动屏蔽虚假唤醒。
- 谓词与无超时版一致：`m_size > 0 || m_closed`。

### 通知顺序
- `push()` 先解锁再 `signal()`，`close()` 先解锁再 `broadcast()`，减少被唤醒线程的无效锁竞争。

## 技术点说明
- `front(T& value) / back(T& value)`: 查看队头/队尾元素时，不返回元素引用，因为返回后如果有一个线程刚好pop掉队首元素，那么返回的元素引用就变成了悬空引用，但这种方案也会有拷贝构造带来的性能开销问题。

## 可选优化建议
- `push`/`pop` 可考虑 `notify_one()` 替代 `broadcast()`，减少不必要的唤醒。(accept)
- `front`/`back` 可改为返回 `std::optional<T>` 以避免“输出参数”。
- `clear()` 可顺带重置数组内容（仅在 `T` 需要显式清理时）。
