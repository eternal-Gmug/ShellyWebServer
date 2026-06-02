# 高性能服务器数据库连接池(SQLConnectionPool)说明

## 架构设计

```
┌──────────────────────────────────────────────────────┐
│                   connectionRAII                      │
│  (RAII 封装：构造时借出，析构时归还)                      │
└──────────┬───────────────────────────────┬────────────┘
           │ 持有                            │ 持有
           ▼                                 ▼
   MYSQL* conRAII                  connection_pool* poolRAII
           │                                 │
           │  ┌──────────────────────────────┘
           ▼  ▼
┌──────────────────────────────────────────────────────┐
│                  connection_pool (单例)                │
│                                                      │
│  ┌──────────┐  ┌──────────┐  ┌────────────────────┐ │
│  │  Sem     │  │  Lock    │  │ std::queue<MYSQL*> │ │
│  │ reserve  │  │  lock    │  │    connQue         │ │
│  │ (信号量)  │  │ (互斥锁)  │  │   (空闲连接队列)    │ │
│  └──────────┘  └──────────┘  └────────────────────┘ │
│                                                      │
│  createConnection()           POOL_LOG_*() 宏        │
│  (三层重试建连)                (条件日志)              │
└──────────────────────────────────────────────────────┘
```

**核心思想**：单例池管理一组预建 MySQL 连接，通过 **信号量** 控制并发借出数量，通过 **互斥锁** 保护队列操作，通过 **RAII** 保证借还配对。

---

## 关键原理

### 1. 单例模式 (Singleton)
`connection_pool` 采用 Meyers' Singleton（C++11 线程安全）：
```cpp
static connection_pool &GetInstance() {
    static connection_pool connPool;  // 局部静态变量，首次调用时构造
    return connPool;
}
```
构造函数私有，确保全局唯一实例。多线程首次并发调用时，C++11 保证只构造一次。

### 2. 信号量控制并发
`reserve = std::make_unique<Sem>(m_FreeConn)` — 信号量初始值等于池容量。

| 操作 | 做了什么 |
|------|---------|
| `GetConnection()` | 先 `reserve->wait()` → 阻塞直到有空闲连接 → 持锁弹队列 |
| `ReleaseConnection()` | 持锁推回队列 → `reserve->post()` → 唤醒一个等待者 |

信号量保证了 "借出数 ≤ 池容量"，线程在信号量上阻塞而非忙等。

### 3. 互斥锁保护队列
`Lock lock` 封装了 `std::mutex`，所有对 `connQue`、`m_FreeConn`、`m_CurConn` 的修改都在 `lock.lock()/unlock()` 之间。信号量 `wait()/post()` 本身不在锁内，避免死锁。

### 4. 连接健康检查与替换
归还连接时调用 `mysql_ping(conn)`：
- **通则**：正常放回队列
- **不通则**：关闭坏连接，尝试 `createConnection()` 创建新连接替代。若替代失败，当前实现返回 `false`（池容量暂时缩减，记录告警日志）

### 5. 日志双开关体系
通过编译期宏 + 运行时变量两层控制：

```
POOL_LOG_INFO(pool, ...)
    │
    ├─ 编译期：#ifdef POOL_LOG_DISABLED → ((void)0)  零依赖 Log 模块
    │
    └─ 运行时：pool_log_enabled(pool) → 检查 m_log_flag
         │
         ├─ m_log_flag ==  0  → 输出日志
         └─ m_log_flag == -1  → 静默
```

---

## 技术实现

### 初始化流程 (init)

```
init(url, user, pass, db, port, max_conn, log_flag)
 │
 ├─ m_destroyed = false     ← 重置销毁标志，支持重新初始化（修复项）
 ├─ 保存连接参数到成员变量
 ├─ for i in [0, max_conn):
 │    └─ createConnection()  ← 单次建连（含三层重试）
 │         ├─ 成功 → push 到 connQue, m_FreeConn++
 │         └─ 失败 → 清理已建连接 + return false（m_destroyed 保持 false）
 │
 ├─ reserve = std::make_unique<Sem>(m_FreeConn)  ← 创建全新信号量，覆盖旧 shutdown 态
 └─ return true
```

关键点：
- 初始化是**全量**的：max_conn 条连接必须全部建成功才返回 `true`，任何一条失败则整体失败并销毁已建连接
- 信号量在**所有连接入队之后**才创建，避免 `GetConnection()` 在初始化完成前被唤醒
- **`m_destroyed` 重置**：`init()` 开头将 `m_destroyed` 置为 `false`，这使得单例池在 `DestroyPool()` 后可以再次 `init()` 复用。若 `init()` 中途失败则 `m_destroyed` 保持 `false`（未调用 `DestroyPool()` 设置它），允许重试

### 借出连接 (GetConnection)

```
GetConnection(time_ms)
 │
 ├─ reserve->timewait(time_ms)   ← 信号量 P 操作，无空闲则阻塞/超时
 │    └─ 超时或 shutdown → return nullptr
 ├─ lock.lock()                  ← 获取互斥锁
 ├─ if m_destroyed              ← 二次确认：防止在 wait→lock 窗口期间池被销毁
 │    └─ unlock + reserve->post() + return nullptr
 ├─ conn = connQue.front()
 ├─ connQue.pop()
 ├─ m_FreeConn--, m_CurConn++
 ├─ lock.unlock()
 └─ return conn
```

关键点：
- `reserve->timewait()` 在锁外调用：如果先持锁再 wait，其他线程无法 `ReleaseConnection()` → 死锁
- `timewait()` 返回后立即持锁取队列：防止 wait→lock 之间的 TOCTOU 竞态
- **`m_destroyed` 二次确认**：持锁后再次检查销毁标志。`reserve->timewait()` 可能因 `shutdown()` 被唤醒返回 `false`，也可能因正常 `post()` 唤醒但在 `lock` 之前池被销毁——此时需要将信号量计数归还（`reserve->post()`）并返回 `nullptr`

### 归还连接 (ReleaseConnection)

```
ReleaseConnection(conn)
 │
 ├─ conn == nullptr → return false
 │
 ├─ mysql_ping(conn)
 │    │
 │    ├─ != 0 (不健康)
 │    │    ├─ mysql_close(conn)
 │    │    ├─ replacement = createConnection()
 │    │    ├─ 持锁：push(replacement), m_FreeConn++, m_CurConn--
 │    │    └─ reserve->post()
 │    │
 │    └─ == 0 (健康)
 │         ├─ 持锁：push(conn), m_FreeConn++, m_CurConn--
 │         └─ reserve->post()
```

关键点：
- `mysql_ping()` 在锁外调用（避免长时间持锁）
- 健康检查和替代创建都在锁外，只有队列操作在锁内
- 替代失败时，`m_CurConn` 已减但 `m_FreeConn` 未增 → 池容量暂时缩减
- **隐藏Bug**：如果替代失败，池容量就会缩减，导致同步量与真实的连接数不一致，有UB风险

### 建连重试 (createConnection)

```
createConnection()
 │
 └─ for attempt in [1, MAX_RETRY(3)]:
      ├─ mysql_init(con)
      │    └─ 失败 → sleep(500ms) → continue
      ├─ mysql_options(CONNECT_TIMEOUT, 3s)
      ├─ mysql_real_connect(...)
      │    ├─ 成功 → return con
      │    └─ 失败 → mysql_close(con), con=nullptr
      └─ attempt < 3 → sleep(500ms) → continue
```

三层保护：
| 层 | 机制 | 解决问题 |
|----|------|---------|
| 应用层重试 | `for` 循环 + `sleep` | MySQL 服务尚未启动 |
| 连接超时 | `MYSQL_OPT_CONNECT_TIMEOUT=3` | TCP 默认 75s 太长 |
| 自动重连 | `MYSQL_OPT_RECONNECT`(已注释) | 运行时断连自动恢复 |

### 销毁池 (DestroyPool)

```
DestroyPool()
 │
 ├─ if reserve: reserve->shutdown()  ← 通知所有阻塞在 timewait/wait 的线程退出
 ├─ lock.lock()
 ├─ m_destroyed = true              ← 设置销毁标志，阻止新的 GetConnection
 ├─ while connQue 非空:
 │    ├─ mysql_close(connQue.front())
 │    └─ connQue.pop()
 ├─ m_CurConn = 0, m_FreeConn = 0
 └─ lock.unlock()
```

关键点：
- `DestroyPool()` **不会** `reserve.reset()`：`reserve` 仍然指向一个 `shutdown` 状态的 `Sem` 对象，`reset()` 仅在析构函数 `~connection_pool()` 中执行
- `reserve->shutdown()` 在持锁之前调用：避免持锁期间通知等待线程导致的调度开销
- 销毁后再次 `init()` 时，`std::make_unique<Sem>(m_FreeConn)` 会创建全新信号量覆盖旧对象，因此 `Sem` 的 `m_shutdown` 不可逆不是问题
- 正在借出（已被 `GetConnection` 弹出队列）的连接不会被 `DestroyPool` 关闭，调用方需自行释放

### 生命周期与重新初始化

`connection_pool` 是单例，但在服务器运行期间可能需要多次初始化和销毁（如配置热更新、数据库切换）。这依赖 `m_destroyed` 原子标志的正确管理：

| 生命周期事件 | `m_destroyed` 状态 | `reserve` 状态 | 行为 |
|-------------|-------------------|----------------|------|
| 首次构造 | `false`（成员初始化器） | `nullptr` | 就绪，等待 `init()` |
| `init()` 成功 | `false` | 指向新 `Sem` | 正常运行 |
| `init()` 失败 | `false` | `nullptr` | 可重试 `init()` |
| `DestroyPool()` | `true` | 指向 shutdown 态 `Sem` | 拒绝所有借出 |
| 再次 `init()` | `false`（`init()` 开头重置） | 指向新 `Sem`（覆盖旧对象） | 恢复正常运行 |

> **修复项**：最初实现中 `init()` 缺少 `m_destroyed = false` 重置，导致 `DestroyPool()` → `init()` 后 `GetConnection()` 永久返回 `nullptr`。表现为单例池只能使用一次，第二次 `init()` 虽成功建连但所有借出操作被 `m_destroyed` 守卫拦截。

### 连接健康检查替换失败的容量缩减

归还连接时若 `mysql_ping` 失败，池会尝试创建替代连接。若替代创建也失败：

```
ReleaseConnection(conn) — conn 不健康
 │
 ├─ mysql_close(conn)
 ├─ replacement = createConnection()
 │    └─ 失败 → lock → m_CurConn--  → unlock → return false
 │              ↑ m_FreeConn 未增，信号量未 post
```

此时池的实际容量 **静默缩减 1**：`m_CurConn` 减少但信号量计数未恢复，导致后续 `GetConnection()` 可能因信号量耗尽而超时，即使 `connQue` 中仍有连接。这是一个已知边界缺陷，触发条件为数据库完全不可达（所有新建连尝试均失败）。

---

## 技术点说明

- 1、connectionRAII类的必要性
  如果没有这个RAII类，那connection_pool就是一个裸类，每次连接都需要手动创建和释放，如：
  ```
  MYSQL *mysql = connPool->GetConnection();
  // ↓ 如果这中间任何一步出错（异常、提前 return）
  request->read_once();
  request->process();
  connPool->ReleaseConnection(mysql);  // ← 这行永远不会执行
  ```
  这样的后果就是连接内存泄漏，连接资源得不到释放，最终会导致池耗尽，服务宕机

  而有了```connectionRAII```就可以写成下面这样子：
  ```
  {
    connectionRAII mysqlcon(&request->mysql, m_connPool);
    // 构造时自动 Get
    request->read_once();
    request->process();
    // 无论上面是否异常，析构函数保证 Release
   }
   ```
   无论是否发生异常都能够释放创建资源，做到资源即取即用

   **为什么能保证调用析构函数Release呢？**
   原因就在于你定义的connectionRAII对象会将它存放在**栈上**，当这个变量离开作用域时一定会将它的析构函数从栈上弹出，连接资源得以释放被其他线程复用。
   
- 2、mysql重连的三层重试
  ```
  for (int attempt = 0; attempt < MAX_RETRY && !connected; attempt++)
        {
            con = mysql_init(con);
            if (con == nullptr)
            {
                LOG_ERROR("MySQL initialization failed for connection %d: %s", i + 1, mysql_error(con));
                continue; // retry initialization
            }
            // set retry connections and timeout option
            unsigned int timeout = 3;
            mysql_options(con, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
            mysql_options(con, MYSQL_OPT_RECONNECT, &timeout);

            MYSQL *result = mysql_real_connect(con, url.c_str(), user.c_str(), password.c_str(), database_name.c_str(), port, nullptr, 0);

            if (result == nullptr)
            {
                LOG_ERROR("MySQL connection failed for connection %d: %s", i + 1, mysql_error(con));
                mysql_close(con); // clean up the failed connection
                con = nullptr;
                std::this_thread::sleep_for(std::chrono::milliseconds(RETRY_INTERVAL_MS)); // wait before retrying
                continue;                                                                  // retry connection
            }
            connected = true;
        }
  ```
  外层重试常用于MySQL服务器启动比本地应用服务慢的场景，解决了MySQL服务器还未启动，稍后重试的问题。
  
  ```MYSQL_OPT_CONNECT_TIMEOUT```调整了底层TCP系统调用的超时时间（默认是75秒），解决了连接不上别傻等的问题（⚠在MySQL 8.0.34+ 已被标记为 deprecated，推荐在应用层处理重连）。
  
  ```MYSQL_OPT_RECONNECT```调整了**运行时**重连的超时时间，解决了运行时连接断开**不重连**的问题。