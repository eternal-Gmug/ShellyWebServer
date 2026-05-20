# 阻塞队列（BlockingQueue）说明

本模块实现了一个基于循环数组的阻塞队列，适用于生产者/消费者模型。

## 功能概览
- 固定容量队列：构造时指定容量。
- 线程安全：内部使用互斥锁与条件变量。
- 阻塞消费：当队列为空时，消费者阻塞等待。
- 超时消费：支持带超时的 `pop`。

## 接口说明
- `push(const T& item)`: 入队，队满返回 `false`。
- `pop(T& item)`: 出队，队空则阻塞等待。
- `pop(T& item, int time_ms)`: 出队，超时返回 `false`。
- `front(T& value) / back(T& value)`: 查看队头/队尾元素。
- `size() / capacity() / empty() / isFull()`: 状态查询。
- `clear()`: 清空队列（保留容量）。

## 使用示例
```cpp
BlockingQueue<int> q(100);
q.push(1);
int v = 0;
q.pop(v);
```

## 技术点说明
- `front(T& value) / back(T& value)`: 查看队头/队尾元素时，不返回元素引用，因为返回后如果有一个线程刚好pop掉队首元素，那么返回的元素引用就变成了悬空引用，但这种方案也会有拷贝构造带来的性能开销问题。

## 可选优化建议
- `push`/`pop` 可考虑 `notify_one()` 替代 `broadcast()`，减少不必要的唤醒。(accept)
- `front`/`back` 可改为返回 `std::optional<T>` 以避免“输出参数”。
- `clear()` 可顺带重置数组内容（仅在 `T` 需要显式清理时）。
