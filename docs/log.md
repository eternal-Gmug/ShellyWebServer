# 高性能服务器日志类(Log)说明

## 技术点说明
- 1、为什么要使用虚析构函数
```
Log(){}
virtual ~Log(){}
```
举个栗子，如下：
```
Log* logger = new NetLog(); // 父类指针指向子类对象
// ...
delete logger; // 销毁对象
```
如果没有virtual，直接调用delete logger，由于指针类型是Log*，它只会调用父类Log的析构函数，对于子类析构函数里写的内存释放逻辑永远不会调用，造成**内存泄漏**！

**补充知识——虚函数表和虚表指针**
C++的多态是靠虚函数表和虚表指针来实现的，当一个类里有虚函数时，它会在构造函数执行时让编译器在该类的对象内存空间里的**最前面**塞一个虚函数表指针（vptr），这个指针指向一张保存了所有虚函数地址的表（vtable）！

---

## 架构概览

`Log` 类是本服务器的全局日志模块，采用 **Meyer's 单例模式**（局部静态变量），对外提供四个宏接口：`LOG_DEBUG / LOG_INFO / LOG_WARN / LOG_ERROR`。

```
同步模式流程：
  LOG_INFO → write_log() → 持锁格式化 → fwrite 直接写盘

异步模式流程：
  LOG_INFO → write_log() → 持锁格式化 → 解锁 → push 阻塞队列
                                              ↓
                              async_write_log() 后台线程 ← pop 队列 → fwrite 写盘
```

---

## 双模式写入原理

### 同步模式（`log_queue_size = 0`）

```
write_log 调用
  ├─ m_mtx.lock()
  ├─ 3.1/3.2/3.3 文件轮转检查（跨天/跨小时/超行）
  ├─ 4~6 往 m_buf 格式化时间戳 + 用户内容
  ├─ 7 fwrite(m_buf) 直接写盘
  ├─ log_line++
  └─ m_mtx.unlock()
```

每条日志在持有锁期间**原地完成**，调用方线程负责全部写入。优点是**日志严格按调用顺序落盘**，缺点是高并发下锁竞争明显。

### 异步模式（`log_queue_size > 0`）

```
write_log 调用
  ├─ m_mtx.lock()
  ├─ 3.1/3.2/3.3 文件轮转检查
  ├─ 4~6 格式化 m_buf → 拷贝为 std::string log_str
  ├─ log_line++
  ├─ m_mtx.unlock()          ← 🔑 锁在此处释放！
  │
  ├─ m_log_queue->push(log_str)   ← 无锁操作（阻塞队列内部自带锁）
  │
  └─ 若 push 失败（队列满）:
        ├─ m_mtx.lock()
        ├─ fwrite 直接写盘   ← 退化同步
        └─ m_mtx.unlock()

后台线程 async_write_log（init 时创建，detach）:
  ┌─ while (m_log_queue->pop(msg))
  │     ├─ m_mtx.lock()
  │     ├─ fputs(msg, m_fp)
  │     └─ m_mtx.unlock()
  └─ 队列 close 后 pop 返回 false → 退出循环，线程结束
```

**核心设计**：格式化在锁内，push 在锁外。异步线程只做文件写入，与调用方线程的竞争区间被缩短到仅剩格式化阶段。

---

## 文件三级分级切分

### 目录结构

```
./serverlogs/                         ← init 指定（默认）
  └── 2026_05_24_logs/               ← yyyy_mm_dd_logs 文件夹
        ├── 09_server.log            ← 当前小时文件
        ├── 09_server.log.1          ← 第1次超行轮转
        ├── 09_server.log.2          ← 第2次超行轮转
        ├── 10_server.log            ← 下一小时文件
        └── ...
```

### 三级检测逻辑（`write_log` 第 3 段，持锁内依次判断）

| 级别 | 条件 | 操作 | 恢复状态 |
|------|------|------|---------|
| 跨天 | `tm_yday ≠ m_today` | 关旧文件 → `mkdir` 新日期文件夹 | `m_today` 更新，`m_hour = -1`（级联触发跨小时） |
| 跨小时 | `tm_hour ≠ m_hour` | 关旧文件 → `fopen` 新 `hh_server.log` | `m_hour` 更新，`m_file_suffix = 0`，`log_line = 0` |
| 超行数 | `log_line ≥ log_max_lines` | 关旧文件 → `m_file_suffix++` → `fopen` 新 `hh_server.log.{N}` | `log_line = 0` |

**关键**：三级检测是**顺序 `if-if-if`** 而非 `if-else if`，确保任意组合（如跨天 + 跨小时 + 超行同时发生）都能逐级正确处理。

### 跨天/跨小时的级联 ── 技巧

```
跨天分支:
  m_hour = -1   ← 不直接开文件，只建目录

然后自动进入跨小时分支:
  if (my_tm.tm_hour != -1) → true
  → 开新小时文件

这避免了在跨天分支里重复"开文件"代码。
```

---

## 锁与并发模型

```
Log::m_mtx   — 保护 m_buf / m_fp / log_line / m_today / m_hour / m_file_suffix
Queue::m_mtx — BlockingQueue 内部互斥锁，保护 push/pop 队列操作
```

两把锁**互不嵌套持有**，不会形成死锁。

| 场景 | Log::m_mtx 持有 | Queue::m_mtx 持有 |
|------|----------------|-------------------|
| `write_log` 格式化阶段 | ✅ | ❌ |
| `write_log` 异步 push 阶段 | ❌（已释放） | ✅（push 内部） |
| `async_write_log` 写盘阶段 | ✅ | ❌（pop 已返回才拿 Log 锁） |
| 队列满兜底写盘 | ✅ | ❌ |

---

## 异步队列满 → 退化同步写入（⚠️ 已知：会导致日志顺序不一致）

当 `push` 因阻塞队列满而返回 `false` 时，当前实现会**临时重拿 `m_mtx` 并直接 fwrite**：

```cpp
if (!m_log_queue->push(std::move(log_str)))
{
    m_mtx.lock();     // 短暂重拿锁
    fwrite(log_str.c_str(), 1, log_str.size(), m_fp);
    m_mtx.unlock();
}
```

**为什么这样做**：保证日志零丢失 —— 宁可退化同步，也不丢弃。

**带来的问题**：日志写入顺序可能与调用顺序不一致。

```
时间线示例：
  线程A  write_log("A1") → push 成功（入队）
  线程B  write_log("B1") → push 失败（队满）→ fwrite 直写
  线程A  write_log("A2") → push 成功（入队）

  后台线程消费队列 → 先落盘 A1、再落盘 A2
  但 B1 早已直写到磁盘中间位置！

磁盘上的最终顺序:  A1 → B1 → A2
                    ↑ B1 插队到了 A1 和 A2 之间
```

**缓解方式**：增大队列容量、增大缓冲区避免队满；或者如果对顺序有严格要求，可增大缓冲区以降低队满概率，或考虑改为丢弃策略。

---

## 缓冲区设计

`m_buf` 是一块**可复用的堆缓冲区**（`unique_ptr<char[]>`），在 `init()` 时分配（默认 8KB）。

```
写入流程：
  snprintf(m_buf, ...)       → 时间戳
  vsnprintf(m_buf + n, ...)  → 用户格式化内容
  std::string(m_buf, total)  → 拷贝为独立 string（异步）
  或 fwrite(m_buf, ...)      → 直接写盘（同步）
```

**为什么不用逐段 fprintf**：
- 减少系统调用：全部拼接在用户态完成，一次写出。
- 支持异步入队：拼接完即是一个完整的 `std::string`。
- 同步原子性：持锁期间一次写出，不被其他线程打断。

---

## 生命周期管理与优雅关闭

```
~Log()
  ├─ m_log_queue->close()         ← ① 发关闭信号 + 唤醒异步线程
  ├─ m_async_thread->join()       ← ② 等待异步线程消费残量后退出
  └─ fclose(m_fp)                 ← ③ 安全关文件
```

**必须按此顺序**，否则有 use-after-free 风险。

---

## 注意事项

- `localtime()` 非线程安全（使用内部静态缓冲区），已标注 TODO，建议后续改为 `localtime_r()`。
- 跨天检测使用 `tm_yday`（年积日，0~365）而非 `tm_mday`（月内日期），避免月末/月初误判。
- 单例模式下 `init()` 多次调用不会重置成员，建议只调一次。
- `dir_name` 在 `init()` 中做了末尾 `/` 的自动补全，避免传入路径不以 `/` 结尾时拼接错误。
- 