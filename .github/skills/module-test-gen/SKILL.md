---
name: module-test-gen
description: >
  模块测试脚本生成 — 读取 ShellyWebServer 中任意模块的头文件与实现文件，
  自动生成包含正常/异常功能测试和高并发性能测试的单元集成测试脚本，
  生成到 scripts/ 目录。支持自动构建运行并输出测试报告。
  Use when: 生成测试、写测试、测试脚本、单元测试、集成测试、性能测试、
  并发测试、压力测试、selftest、自测、测试用例、测试报告、跑测试。
argument-hint: '<模块名> [--dry-run]'
user-invocable: true
---

# 模块测试脚本生成（Module Test Generation）

对 ShellyWebServer 项目中任意模块，自动分析其接口与实现，生成覆盖功能测试、异常测试和高并发性能测试的完整自测脚本。

## 硬约束

| 约束 | 说明 |
|------|------|
| 📁 输出位置 | 测试脚本生成到 `scripts/<模块名>_selftest.cpp` |
| 🔧 构建集成 | 按 `CMakeLists.txt` 中既有 `*_selftest` 模式添加构建目标 |
| 🧪 测试框架 | 使用无第三方依赖的手写断言（`expect()` 风格，与既有 selftest 一致） |
| 📐 深度 | 必须完整阅读模块的公开头文件（`include/`）和所有实现文件（`src/<模块>/`）后再生成 |

---

## 项目约定

### 模块路径

| 组件 | 路径 |
|------|------|
| 公开头文件 | `include/<module>.h` 或 `include/<module>_*.h` |
| 实现文件 | `src/<module>/*.cpp`、`src/<module>/*.h` |
| 文档 | `docs/<module>.md`（如有） |

### 测试脚本模板约定

- 每个测试函数以 `test_<场景描述>` 命名，使用 `snake_case`
- 断言用 `expect(condition, "描述")` 或 `expect_true(state, condition, "描述")` 风格
- 全局失败计数器 `g_failures` 或 `TestState state`
- `main()` 依次调用所有测试函数，最后汇总通过/失败数
- 需要外部资源（如 MySQL 连接）的测试通过环境变量或 `.env` 文件注入，不可用时 `SKIP`
- 性能/并发测试使用 `<thread>`、`<atomic>`、`<chrono>`

### CMake 集成

每个 selftest 在 `CMakeLists.txt` 中按以下模式添加：

```cmake
add_executable(<module>_selftest
    scripts/<module>_selftest.cpp
)
target_include_directories(<module>_selftest PRIVATE
    ${INCLUDE_DIR}
    ${SRC_DIR}
)
target_link_libraries(<module>_selftest PRIVATE
    <依赖模块列表>
)
```

---

# 五步生成流程

## 步骤 1：定位并阅读模块全部源文件

### 1.1 按模块名精准定位

1. **头文件定位** — 在 `include/` 下按模块名匹配所有相关 `.h` 文件
2. **实现文件定位** — 在 `src/<模块名>/` 下匹配所有 `.cpp` 和内部 `.h` 文件
3. **文档定位** — 检查 `docs/<模块名>.md`，若有则阅读以了解设计意图
4. **交叉验证** — 搜索模块核心类名，确认无遗漏文件
5. **排除已有** — 检查 `scripts/` 下是否已有同名 `_selftest.cpp`，若存在则提示用户

### 1.2 完整阅读

对每个定位到的文件，完整阅读：
- 所有公开类/结构体的声明、成员函数签名、成员变量
- 所有自由函数、枚举、宏、常量
- 模板参数约束、`static_assert`
- RAII 包装类（如 `connectionRAII`）
- 日志宏定义（如 `POOL_LOG_*`）

## 步骤 2：提取 API 并分类测试场景

### 2.1 API 清单

从模块头文件中提取所有公开接口，按类别整理：

| 类别 | 子类 | 示例 |
|------|------|------|
| 构造/析构 | 默认构造、参数构造、拷贝/移动、析构 | `connection_pool()` |
| 初始化/销毁 | `init()`、`DestroyPool()` | `bool init(...)` |
| 核心操作 | 增删改查、获取/释放 | `GetConnection()`、`ReleaseConnection()` |
| 查询/状态 | getter、状态检查 | `GetFreeConn()`、`empty()` |
| 配置/参数 | 设置项、开关 | `m_log_flag` |
| 回调/事件 | 注册回调、事件处理 | `on_timer()` |

### 2.2 测试场景分类

对每个 API 生成以下三类测试场景：

#### 🟢 正常功能测试（Happy Path）
- 合法输入 → 预期正常输出
- 典型使用流程的端到端测试
- 状态转换的正确性

#### 🟡 异常/边界测试（Error & Edge）
- **空输入**：`nullptr`、空字符串、空容器
- **非法参数**：负数、越界、超出范围、格式错误
- **资源耗尽**：连接池满、队列满、内存不足
- **重复操作**：重复 init、重复 destroy、double-free
- **超时**：阻塞操作的超时行为
- **并发竞态**：资源竞争、状态不一致

#### 🔴 高并发/压力测试（Concurrency & Stress）
- **多生产者单消费者**（MPSC）或 **多生产者多消费者**（MPMC）
- **竞态窗口**：短时间内大量并发请求同一资源
- **压力负载**：持续高负载下的稳定性
- **死锁检测**：验证锁顺序一致性
- **资源泄漏**：长时间运行后资源是否正常回收

### 2.3 依赖分析

识别模块依赖：
- 本模块链接了哪些其他 ShellyWebServer 模块（在 `CMakeLists.txt` 中查看 `link_module_deps`）
- 是否依赖外部库（如 `mysqlclient`、`pthread`）
- 测试是否需要 mock 或 stub 依赖模块

## 步骤 3：生成测试脚本

### 3.1 脚本结构

生成的 `scripts/<模块名>_selftest.cpp` 按以下结构组织：

```cpp
#include "<module>.h"
// 其他必要的 include

#include <cassert>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#include <atomic>

namespace {

// --- 测试框架 ---
int g_failures = 0;

void expect(bool condition, const char *message) {
    if (!condition) {
        ++g_failures;
        std::cerr << "[FAIL] " << message << "\n";
    } else {
        std::cout << "[PASS] " << message << "\n";
    }
}

// --- 🟢 正常功能测试 ---
void test_<normal_scenario_1>() { ... }
void test_<normal_scenario_2>() { ... }

// --- 🟡 异常/边界测试 ---
void test_<edge_scenario_1>() { ... }
void test_<edge_scenario_2>() { ... }

// --- 🔴 高并发/压力测试 ---
void test_<concurrent_scenario_1>() { ... }
void test_<concurrent_scenario_2>() { ... }

} // namespace

int main() {
    // 🟢 功能测试
    test_<normal_scenario_1>();
    test_<normal_scenario_2>();
    // ...

    // 🟡 异常测试
    test_<edge_scenario_1>();
    test_<edge_scenario_2>();
    // ...

    // 🔴 并发测试
    test_<concurrent_scenario_1>();
    test_<concurrent_scenario_2>();
    // ...

    if (g_failures == 0) {
        std::cout << "\n✅ All <module> tests passed.\n";
        return 0;
    }
    std::cerr << "\n❌ " << g_failures << " test(s) failed.\n";
    return 1;
}
```

### 3.2 编写测试用例的规范

- **每个测试函数独立、可单独运行**，不依赖其他测试的副作用
- **使用 RAII 确保资源清理**，测试失败也不泄漏
- **并发测试使用 `std::atomic` 计数器**记录操作次数
- **超时测试使用 `std::chrono`** 验证等待时间在合理范围内
- **需要外部资源的测试**：先检查资源可用性，不可用则输出 `[SKIP]` 并返回
- **性能测试输出指标**：如吞吐量（ops/sec）、延迟（avg/max/min）

### 3.3 添加 CMake 构建目标

在 `CMakeLists.txt` 末尾的测试区域添加：

```cmake
add_executable(<module>_selftest
    scripts/<module>_selftest.cpp
)
target_include_directories(<module>_selftest PRIVATE
    ${INCLUDE_DIR}
    ${SRC_DIR}
)
target_link_libraries(<module>_selftest PRIVATE
    <module>
    <dependent_modules...>
    <external_libs...>
)
```

## 步骤 4：输出测试场景清单并询问用户

生成脚本和 CMake 配置后，以表格形式输出测试场景清单：

```markdown
## 📋 测试场景清单：`<模块名>`

### 🟢 正常功能测试（N 项）
| # | 测试函数 | 场景描述 |
|---|---------|---------|
| 1 | `test_xxx` | ... |

### 🟡 异常/边界测试（N 项）
| # | 测试函数 | 场景描述 |
|---|---------|---------|

### 🔴 高并发/压力测试（N 项）
| # | 测试函数 | 场景描述 |
|---|---------|---------|
```

然后**必须**向用户提问：

> 是否立即编译并运行测试？

提供两个选项：
- **是，自动测试** → 进入步骤 5
- **否，仅生成脚本** → 输出手动测试的全流程说明

## 步骤 5：自动编译运行并输出测试报告

### 5.1 构建

```bash
cd /home/ming/Documents/Project/ShellyWebServer/build
cmake .. && make <module>_selftest
```

### 5.2 执行

```bash
./<module>_selftest
```

### 5.3 输出测试报告

```markdown
## 🧪 测试报告：`<模块名>`

| 指标 | 值 |
|------|-----|
| 编译状态 | ✅ 通过 / ❌ 失败 |
| 测试总数 | N |
| 通过 | N |
| 失败 | N |
| 跳过 | N |
| 耗时 | Xms |

### 失败用例详情（如有）
| # | 用例 | 失败原因 |
|---|------|---------|

### 性能指标（并发测试）
| 指标 | 值 |
|------|-----|
| 总操作数 | N |
| 并发线程数 | N |
| 吞吐量 | N ops/sec |
| 平均延迟 | X μs |
```

---

## 手动测试全流程（当用户选择不自动运行时输出）

```markdown
## 🔧 手动测试流程

### 1. 构建测试
\`\`\`bash
cd /home/ming/Documents/Project/ShellyWebServer/build
cmake .. && make <module>_selftest
\`\`\`

### 2. 运行测试
\`\`\`bash
./<module>_selftest
\`\`\`

### 3. 解读输出
- `[PASS]` — 测试通过
- `[FAIL]` — 测试失败，查看 stderr 中的错误信息
- `[SKIP]` — 测试跳过（如缺少外部资源）

### 4. 调试失败
- 使用 `gdb ./<module>_selftest` 调试
- 检查模块实现的边界条件
- 验证 CMake 链接的依赖库是否完整
\`\`\`
