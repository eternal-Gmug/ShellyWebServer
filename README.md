# ShellyWebServer
This is a self-developed C++ Web server based on the Linux system, which implements basic functions such as thread pool, IO multiplexing, and HTTP connection. After further improvement, it will gradually add new features such as AI large model services. Please stay tuned...

## Project Structure
```
.
├── bin/                 # 单模块构建产物输出（可选），CMake构建生成在build文件夹下
├── build/               # CMake 构建目录
├── conf/                # 运行时配置文件
├── docs/                # 模块技术说明文档
├── include/             # 全局对外头文件（按模块划分）
├── resources/           # 前端静态资源（HTML/CSS/JS）
├── scripts/             # 辅助脚本
├── src/                 # 服务端源码
│   ├── main.cpp         # 程序入口
│   ├── concurrency/     # 线程池/协程池与并发控制
│   ├── http/            # HTTP 解析与响应构建
│   ├── limit/           # 限流与连接控制
│   ├── log/             # 日志系统
│   ├── net/             # 套接字与 IO 多路复用
│   ├── router/          # URL 路由与业务分发（用于后续的扩展业务）
│   ├── server/          # 服务器主循环与生命周期管理
│   ├── services/        # 大模型服务与扩展能力
│   ├── timer/           # 定时器与调度任务
│   └── utils/           # 通用工具与共享能力
└── tests/               # 单元/集成测试
```
