# ShellyWebServer — 工作区级 Agent 指令

## 两个项目的角色

| 项目 | 角色 | 规则 |
|------|------|------|
| **ShellyWebServer** | 🏗️ 重构目标（当前项目） | ✅ 可读写，所有改动的唯一目标 |
| **TinyWebServer** | 📖 参考实现（旧项目） | ❌ 只读分析，绝不直接修改 |

## 核心工作原则

### 1. 旧项目只读，重构项目可写
- TinyWebServer 仅用于阅读、对照、理解旧设计的意图
- 所有代码改动只发生在 ShellyWebServer
- 如果需要把旧项目的某段逻辑迁移过来：先在旧项目中定位 → 理解 → 再用当前项目的风格重写

### 2. 保持当前项目的架构风格
- 当前项目有清晰的模块划分（`src/concurrency/`, `src/http/`, `src/log/` 等）
- 迁移旧逻辑时，必须适配当前项目的目录结构和命名规范
- 不要直接复制旧项目的文件/类名，要按当前项目的组织方式重新落位

### 3. 对照式开发的推荐流程
当需要从旧项目迁移功能时：
1. **定位**：在 TinyWebServer 中找到对应实现
2. **分析**：总结旧实现的接口、数据流、边界处理
3. **对比**：检查 ShellyWebServer 中是否已有部分实现
4. **设计**：规划如何在当前架构中落地（不破坏现有模块边界）
5. **实现**：在 ShellyWebServer 中编写代码
6. **验证**：对照旧实现的边界条件，确保没有遗漏

## 项目结构对照（核心模块）

| 模块 | TinyWebServer（旧） | ShellyWebServer（新） |
|------|---------------------|----------------------|
| 日志 | `log/` | `src/log/` + `include/log.h` |
| 线程池 | `threadpool/` | `src/concurrency/threadpool.h` |
| 数据库 | `CGImysql/` | `src/database/` + `include/sql_connection_pool.h` |
| 锁/同步 | `lock/` | `src/utils/lock/lock.h` |
| HTTP | `http/` | `src/http/` |
| 服务器 | `webserver.cpp/h` | `src/server/` |
| 定时器 | `timer/` | `src/timer/` |
| 配置 | `config.cpp/h` | `conf/` |

## 代码风格约定（当前项目）
- C++17 标准
- CMake 构建（`build/` 目录）
- 头文件在 `include/`，实现在 `src/<模块>/`
- 使用 `POOL_LOG_*` / `LOG_*` 宏做条件日志
- 信号量/锁统一使用 `src/utils/lock/lock.h` 中的 `Sem` / `Lock` / `Cond`

## 常用 Agent 任务模板

### 模板 A：对照迁移
```
参考 TinyWebServer 中 <旧模块路径> 的实现，
在 ShellyWebServer 的 <新模块路径> 中实现对应功能。
要求：保持当前项目架构，不复制旧代码风格，只迁移逻辑。
```

### 模板 B：差异审查
```
对比 TinyWebServer 的 <旧模块> 和 ShellyWebServer 的 <新模块>，
列出当前实现相比旧实现缺失的边界处理、错误处理或功能点。
只分析，不要修改代码。
```

### 模板 C：补全实现
```
ShellyWebServer 的 <新模块> 目前只有框架。
参考 TinyWebServer 的 <旧模块> 完整实现，补全当前模块的具体逻辑。
注意适配当前项目的类名、锁机制、日志宏。
```

### 模板 D：验证补漏
```
检查 ShellyWebServer 的 <模块> 是否包含了 TinyWebServer <旧模块>
中所有关键的边界条件处理（如空指针、超时、资源释放等）。
列出遗漏项并给出修复建议。
```