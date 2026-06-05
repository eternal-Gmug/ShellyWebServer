# 高性能服务器定时器(Timer)说明

## 架构总览

本项目的定时器系统由两个独立模块协作完成：

| 模块 | 路径 | 职责 |
|------|------|------|
| **TimerManager** | `include/timer_manager.h` + `src/timer/` | 基于 `timerfd` + `multiset` 的定时器调度引擎 |
| **SignalHandler** | `src/utils/signal/signal.h` + `src/utils/signal/signal.cpp` | 基于 `sigaction` + pipe 的统一事件源信号处理 |

```
┌──────────────────────────────────────────────────┐
│                  epoll_wait()                     │
│        监听 timer_fd 和 signal_pipefd[0]          │
└────┬─────────────────────────┬───────────────────┘
     │ timer_fd 可读           │ signal_pipefd[0] 可读
     ▼                         ▼
┌─────────────┐          ┌────────────────┐
│ TimerManager│          │ SignalHandler   │
│  .tick()    │          │  .signalHandler │
│  处理过期    │          │  (异步信号触发)  │
│  定时器回调  │          │  write()→pipe   │
└─────────────┘          └────────────────┘
     │                         │
     │ 依赖                    │ 依赖
     ▼                         ▼
┌──────────┐             ┌──────────┐
│ Lock/Sem │             │  Log     │
│ (lock.h) │             │ (log.h)  │
└──────────┘             └──────────┘
```

**核心设计理念**：两个模块都将异步事件（定时器到期、OS 信号）统一转化为 epoll 可监听的 fd 事件，使主线程只需在 epoll 循环中统一处理所有事件，避免多线程同步的复杂性。

---

## 一、TimerManager — 定时器调度引擎

### 1.1 为什么用 timerfd 替代 SIGALRM？

| 方案 | 问题 |
|------|------|
| `SIGALRM` + `signal()` | 信号处理函数中只能调用异步信号安全函数；无法传递上下文；多线程下信号投递目标不确定 |
| `SIGALRM` + pipe + `signal()` | 需要信号处理函数写 pipe，增加了一次内核态往返，且 `signal()` 会重置 |
| **`timerfd` + epoll** ✅ | timerfd 是一个普通 fd，epoll 直接监听，无需信号处理函数；支持纳秒精度；`TFD_CLOEXEC` 防止 fd 泄漏到子进程 |

### 1.2 核心数据结构

```
m_timers: multiset<TimerEntry>      ← 按 expire_time 自动排序，O(log n) 插入/删除
m_timer_map: unordered_map<TimerId, iterator>  ← O(1) 按 ID 查找定时器
m_lock: mutable Lock                ← 保护上述两个容器
m_next_id: uint64_t                 ← 单调递增 ID 分配器，从 1 开始（0 表示无效）
timer_fd: int                       ← timerfd 文件描述符
```

**为什么用 `multiset` 而非 `priority_queue`？**
- `multiset` 支持 O(log n) 的任意位置删除（`removeTimer`）
- `multiset` 支持 C++17 的 `extract()/insert()` 节点操作（`adjustTimer` 无需重新分配内存）
- `priority_queue` 只能访问堆顶，无法高效删除或调整非堆顶元素

**为什么额外维护 `m_timer_map`？**
- `multiset` 只能按 `expire_time` 索引，无法按 `TimerId` 索引
- `m_timer_map` 提供 TimerId → multiset iterator 的 O(1) 映射，支持 `removeTimer(id)` 和 `adjustTimer(id)` 的快速定位

### 1.3 关键算法

#### 1.3.1 `addTimer()` — 添加定时器

```
1. 校验 callback != nullptr（否则返回 0）
2. 持锁：分配 m_next_id++，计算 expire = now + duration
3. 构造 TimerEntry 并 insert 到 m_timers
4. 在 m_timer_map 中建立 id → iterator 映射
5. 若新定时器排在 multiset 首位 → 调用 updateNextTimerfd() 更新内核 timerfd
6. 解锁，返回 TimerId
```

**为什么需要第 5 步？**  
如果新定时器比当前所有定时器都早到期，必须更新内核 timerfd 的到期时间，否则 epoll 会在旧的最早时间才唤醒，新定时器将延迟触发。

#### 1.3.2 `removeTimer()` — 删除定时器

```
1. 持锁：在 m_timer_map 中查找 id
2. 若不存在 → 解锁返回 false
3. 记录是否删除的是 multiset 首位（isFirst）
4. 从 m_timers 和 m_timer_map 中删除
5. 若 isFirst 且 m_timers 非空 → updateNextTimerfd() 更新到新的最早时间
6. 解锁返回 true
```

#### 1.3.3 `adjustTimer()` — 调整定时器

```
1. 持锁：在 m_timer_map 中查找 id，不存在则返回 false
2. 记录 isFirst（是否为首位）
3. m_timers.extract(it->second)：从 multiset 中取出节点句柄（不释放内存）
4. node.value().expire_time = now + new_duration：原地修改过期时间
5. m_timers.insert(std::move(node.value()))：重新插入（multiset 自动重排）
6. 若 isFirst → updateNextTimerfd()：原首位调整后必须刷新 timerfd
7. it->second = new_iter：更新 m_timer_map 中的迭代器
8. 解锁返回 true
```

**为什么用 `extract()/insert()` 而非 `erase()/insert()`？**
- `extract()` 返回 node handle，分离节点但不销毁数据，避免了 `TimerEntry` 中 `std::function` 的析构+重构开销
- C++17 特性，零拷贝调整

**第 6 步无条件更新策略**：  
只要原来排在首位，无论调整后是否仍在首位，都调用 `updateNextTimerfd()`。原因是：
- 调整后可能更早到期 → timerfd 需要指向新时间
- 调整后可能更晚到期 → timerfd 需要指向新的最早定时器
- 若仍在首位但时间不变 → `timerfd_settime` 重新设置相同时间，开销极小，但逻辑简洁

#### 1.3.4 `tick()` — 处理过期定时器

```
1. read(timer_fd)：读取并清除 timerfd 的到期计数
   - 若返回 EAGAIN（非阻塞模式下无到期事件） → 正常继续
   - 若返回其他错误 → 记录日志并返回
2. 持锁遍历 m_timers：
   while (it->expire_time <= now):
     - const_cast move 出 callback（避免 std::function 拷贝开销）
     - 从 m_timer_map 中删除
     - it = m_timers.erase(it) 删除并前进
3. 若 m_timers 非空 → updateNextTimerfd() 设置下一个到期时间
4. 解锁
5. 在锁外逐一执行 expired_callbacks 中的回调
```

**为什么回调在锁外执行？**  
回调函数可能调用 `addTimer()`、`removeTimer()` 等 TimerManager 接口（重入），如果在锁内执行会死锁。锁外执行保证了重入安全。

**关于 `const_cast`**：  
`std::multiset` 的迭代器指向 const 元素（修改可能破坏排序），此处仅 move 走 `callback`（不影响排序的 `expire_time` 和 `id`），且元素立即被 `erase`。更标准的写法是使用 `extract()` 获取 node handle，但 `const_cast` + `erase` 在同一临界区内且元素不再被访问，实践中安全。

#### 1.3.5 `updateNextTimerfd()` — 同步内核 timerfd

```
1. 若 m_timers 为空 → timerfd_settime(..., {0,0})  disarm 定时器
2. 否则计算 next_expire - now 的纳秒差
3. 若差值 < 0（已过期） → 设为 1ns（timerfd_settime 不接受 0 值，0 表示 disarm）
4. timerfd_settime(timer_fd, 0, &new_value, nullptr) 设置相对时间
```

**注意**：此函数必须在持锁状态下调用，因为它读取 `m_timers.begin()->expire_time`。

### 1.4 并发模型

- **单锁保护**：`m_lock` 保护 `m_timers`、`m_timer_map`、`m_next_id`
- **锁外回调**：`tick()` 在释放锁后执行用户回调，防止死锁
- **无死锁风险**：整个模块只使用一把锁，不存在锁顺序问题
- **mutable 锁**：`m_lock` 声明为 `mutable`，允许 `activeTimers() const` 加锁

### 1.5 RAII 定时器句柄 — TimerHandle

```
class TimerHandle {
    TimerId id;              // 定时器 ID，0 表示无效
    TimerManager* manager;   // 所属 TimerManager 指针
};
```

| 操作 | 行为 |
|------|------|
| 构造 `TimerHandle(id, manager)` | 接管一个已创建的定时器 |
| 析构 `~TimerHandle()` | 自动调用 `cancel()` → `manager->removeTimer(id)` |
| `cancel()` | 主动取消定时器，句柄失效 |
| `release()` | 放弃所有权，返回 TimerId，定时器继续运行 |
| 移动构造/赋值 | 转移所有权，源句柄失效（`id=0, manager=nullptr`） |
| 拷贝 | `= delete`（禁止，保证唯一所有权） |

**设计意图**：调用方无需手动 `removeTimer()`——只要 `TimerHandle` 对象在作用域内，定时器就有效；离开作用域自动取消。这是典型的 RAII 资源管理模式。

---

## 二、SignalHandler — 统一事件源信号处理

### 2.1 设计思想：将异步信号同步化

Linux 信号是异步的——内核在任意时刻打断用户代码执行信号处理函数。在这类 handler 中只能调用「异步信号安全」的函数（POSIX 列表），不能安全地操作复杂数据结构、分配内存、加锁。

**解决方案（Self-Pipe Trick）**：

```
OS 信号 → signalHandler() → write(signal_pipefd[1], &sig, 1)
                                   │
                                   ▼
                         signal_pipefd[0] 变为可读
                                   │
                                   ▼
                         epoll 检测到 EPOLLIN 事件
                                   │
                                   ▼
                         主线程在 epoll 循环中安全处理
```

信号被转化为一个普通的 fd 可读事件，主线程在正常上下文中处理——此时可以安全地执行任何操作。

### 2.2 核心组件

| 组件 | 说明 |
|------|------|
| `static int* signal_pipefd` | pipe 文件描述符数组，`[0]` 读端、`[1]` 写端，由外部传入并保证生命周期 |
| `static int signal_epoll_fd` | epoll 实例 fd，用于后续 `addFd()` 注册 |
| `init(pipefd, epoll_fd)` | 注入外部创建的 pipe 和 epoll fd |
| `addSignal(sig, handler)` | 用 `sigaction` 注册信号处理函数 |
| `signalHandler(sig)` | 静态信号处理函数，将信号号写入 pipe |

### 2.3 `addSignal()` — 信号注册

```
1. 构造 sigaction 结构体并清零
2. sa.sa_handler = handler     → 设置处理函数
3. SA_RESTART                  → 被信号中断的系统调用自动重启
4. sigfillset(&sa.sa_mask)     → 处理函数执行期间屏蔽所有信号
5. sigaction(sig, &sa, nullptr) → 注册（永久有效，不会像 signal() 那样重置）
```

**`sigfillset(&sa.sa_mask)` 的作用**：在信号处理函数执行期间，将所有其他信号挂起排队，等当前处理完成后再逐一投递，防止嵌套重入导致栈溢出或数据竞争。

### 2.4 `signalHandler()` — 信号 → pipe 写入

```
1. int save_errno = errno     → 备份全局 errno
2. write(signal_pipefd[1], &msg, 1) → 将信号号（1 字节）写入 pipe 写端
3. errno = save_errno         → 恢复 errno
```

**关键细节**：
- 只发送 1 字节（信号号 ≤ 255）：标准信号值（SIGINT=2, SIGTERM=15 等）远小于 256，1 字节足够
- `errno` 保存/恢复：信号处理函数可能在任意代码点打断，必须保护全局 `errno` 不被污染
- `signal_pipefd` 判空：若未初始化则跳过写入，防止空指针解引用

### 2.5 辅助工具函数

| 函数 | 功能 |
|------|------|
| `setNonBlocking(fd)` | 用 `fcntl(fd, F_SETFL, O_NONBLOCK)` 设置非阻塞 |
| `addFd(epollfd, fd, one_shot, trigger_mode)` | 将 fd 注册到 epoll，支持 ET/LT 模式和 `EPOLLONESHOT` |
| `showError(connfd, msg)` | 发送错误消息到客户端 socket 并关闭连接 |

**`addFd()` 中 `EPOLLRDHUP` 的使用**：所有注册的 fd 都携带 `EPOLLRDHUP` 标志，使得对端关闭连接时内核直接触发该事件，epoll 循环中无需额外 `read()` 即可判断，节省一次系统调用。

---

## 技术点说明
- 1、steady_clock的使用  
  ```std::chrono::steady_clock::time_point expire_time; // Expiration time of the timer```在这里不使用system_clock就是因为系统时间会因为管理员调整系统时间而发生变化，但steady_clock是按照你操作系统启动的时间来计算的，它不会受系统时间的影响，严格保证递增。  
  **使用场景**:  
  1、对于倒计时，测试耗时的任务，需要严格保证时间流向，使用steady_clock
  2、而对于需要将真实时间戳展示或存储的任务，则需要使用system_clock来记录当前系统时间

- 2、注册fd时**EPOLLRDHUP**的作用是什么？  
  **作用：标记对端关闭连接**  
  我们先来考虑如果没有**EPOLLRDHUP**的场景：  
  1）当对端关闭连接时，TCP协议会发送一个FIN包  
  2）服务器内核收到FIN后，它会将这个socket设置为可读，并触发EPOLLIN事件  
  3）**关键的问题**在于：服务器只知道有数据可读，但是不知道这是客户端正常发的数据还是客户端要断开连接的信号  
  4）所以服务器就必须通过系统调用read()或recv()，如果返回值大于0就是正常的数据，如果等于0就是断开连接的信号
  使用**EPOLLRDHUP**的优势：
  1）引入**EPOLLRDHUP**后，内核在收到FIN包会直接触发**EPOLLRDHUP**事件，它会在服务器epoll_wait返回时直接判断event.events & EPOLLRDHUP是否为真，如果为真不需要调用read()就可以直接清理资源，减少一次系统调用的开销

- 3、信号处理函数的处理哲学——“统一事件源”机制的核心纽带  
  ```
  void SignalHandler::signalHandler(int sig) {
    // save original errno to avoid being modified by the signal handler
    int save_errno = errno;
    int msg = sig;
    // write the signal number to the pipe
    if (signal_pipefd) {
        send(signal_pipefd[1], reinterpret_cast<char*>(&msg), 1, 0); // Send the signal number through the write end of the pipe
    }
    errno = save_errno; // Restore the original errno before returning
  }
  ```
  **核心作用**：当Linux系统发生异步信号时，安全、快速地将信号转化成“网络事件（管道写入）”，从而通知主线程的epoll异步处理，避免在信号处理函数中执行复杂、非异步信号安全（如printf、malloc/free）的逻辑。  
  1）为了实现异步信号处理函数的可重入，当主线程被中断执行信号处理函数时，**需要先将原先的全局错误码做一个备份**，以便于执行完信号处理后能够重新恢复。  
  2）主要作用是将信号量写入管道的写端：  
     网络发送函数的第二个参数需要传入**char***，需要通过**reinterpret_cast（重新解释类型转换）**，之所以要使用这个，原因如下：  
     1. 第一是因为int*和char*属于完全没有继承关系、互不相干的类型，static_cast只能用于父子类指针、隐式转换，reinterpret_cast能够将它底层的二进制0和1**直接换一个类型看待**，但这个**不做安全性检查（⚠）**，完全依赖程序员的处理逻辑。  
     2. 第二在于传入的信号值（如SIGALRM=14, SIGTERM=15）一般都很小，尽管int有4个字节，但1个字节（对应send函数第三个参数）足够容纳，而且高效，同时只有**reinterpret_cast**才能控制指针转移和长度控制，精准高效地在管道中传入这1个字节的数据。

- 4. 添加信号的实现哲学
  ```
  bool SignalHandler::addSignal(int sig, void (*handler)(int), bool restart = true) {
    struct sigaction sa {};
    memset(&sa, '\0', sizeof(sa)); // Clear the sigaction structure
    sa.sa_handler = handler; // Set the signal handler function
    if (restart) {
        sa.sa_flags |= SA_RESTART; // Automatically restart interrupted system calls if restart is true
    }
    sigfillset(&sa.sa_mask); // Block all signals during the execution of the signal handler
    if (sigaction(sig, &sa, nullptr) < 0) { // Register the signal handler for the specified signal
        LOG_ERROR("Failed to register signal handler for signal {}: {}", sig, strerror(errno));
        return false;
    }
    return true;
  }
  ```
  1) **为什么要使用sigaction？**  
   传统的signal函数**在信号处理函数被触发一次后，它对该信号的绑定就会回置到系统的默认行为（解绑）**，你要持续捕获这个信号就需要在信号处理函数中先执行一次signal()，在旧的绑定失效到新的绑定建立中间会有一段空窗期，如果这时有一个新的信号进来，程序通常就会直接崩溃。  
   sigaction能够保证你不显式修改，信号永久生效，不会重置。
  2) **SA_RESTART的作用**  
   当有个被阻塞中断的线程因为信号的处理返回一个EINTR而终止，SA_RESTART能够保证在执行完信号处理后重新执行这个被阻塞中断的线程。
  3) **sigfillset(&sa.sa_mask)的作用是什么？**
   问题：如果在执行一个信号处理函数时有其他信号同时进来怎么办？  
   解决方案：先**挂起**这些信号，等到手中的处理函数执行完以后再来安全处理后续到来的信号。
  