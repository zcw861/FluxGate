# FluxGate

FluxGate 是一个使用 **C++11、Linux epoll 与 CMake** 实现的四层 TCP 负载均衡网关。项目采用 **Acceptor + 多 Worker Reactor** 架构：主线程只负责接入和分发客户端连接，每个 Worker 独占一个 epoll 事件循环，再通过加权最少连接算法选择健康后端并执行全双工非阻塞转发。

本项目由早期负载均衡服务器原型重新设计。并发模型、连接生命周期、配置格式、资源管理、健康检查和测试体系均已重写，不再沿用原项目的 `processpool / mgr / conn` 结构。

## 核心能力

- **多 Reactor 并发模型**：主线程负责 `accept4()`，多个 Worker 独立运行 epoll，避免多个线程并发修改同一会话。
- **跨线程连接投递**：使用线程安全队列保存新客户端，通过 `eventfd` 唤醒目标 Worker。
- **加权最少连接**：按 `active_connections / weight` 选择后端，支持不同容量节点。
- **非阻塞双向代理**：正确处理 `EINPROGRESS`、短读短写、`EAGAIN`、TCP 半关闭和连接错误。
- **固定缓冲区背压**：每个会话维护两个方向的固定容量缓冲区，写满时暂停来源侧读取，避免无界内存增长。
- **主动健康检查**：连续失败达到阈值后摘除节点，连续成功达到阈值后恢复，降低瞬时抖动误判。
- **RAII 资源管理**：`UniqueFd` 自动关闭描述符，`BackendLease` 自动维护后端活动连接计数。
- **可观测性与退出**：提供连接、失败和字节指标，支持 `SIGINT/SIGTERM` 优雅停止。
- **工程化验证**：提供 CTest、冒烟测试、故障转移测试、ApacheBench、wrk 以及 ASan/UBSan 构建选项。

## 架构概览

```text
                         +------------------------+
Client connections ---->| Acceptor / Dispatcher  |
                         +-----------+------------+
                                     |
                         least active sessions
                    +----------------+----------------+
                    |                |                |
             +------v------+  +------v------+  +------v------+
             | Worker 0    |  | Worker 1    |  | Worker N    |
             | epoll loop  |  | epoll loop  |  | epoll loop  |
             +------+------+  +------+------+  +------+------+ 
                    |                |                |
                    +----------------+----------------+
                                     |
                         weighted least connections
                    +----------------+----------------+
                    |                                 |
               +----v----+                       +----v----+
               | app-1   |                       | app-2   |
               | weight=1|                       | weight=2|
               +---------+                       +---------+

HealthChecker -------------------------------> backend health state
Metrics reporter ----------------------------> runtime counters
```

完整线程模型、会话状态机与背压流程见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)。逐文件和逐函数说明见 [docs/CODE_GUIDE.md](docs/CODE_GUIDE.md)。

## 环境要求

- Linux 4.x 或更高版本
- CMake 3.16 或更高版本
- 支持 C++11 的 GCC 或 Clang
- Python 3、curl：本地联调使用
- ApacheBench、Nginx：自动压力测试使用
- wrk：持续高并发测试使用

## 构建与测试

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

检查配置和后端地址解析：

```bash
./build/fluxgate -c config/fluxgate.conf --check
```

启动：

```bash
./build/fluxgate -c config/fluxgate.conf
```

按 `Ctrl+C` 或发送 `SIGTERM` 可触发优雅退出。

启用 AddressSanitizer 和 UndefinedBehaviorSanitizer：

```bash
cmake -S . -B build-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DFLUXGATE_ENABLE_SANITIZERS=ON
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
```

## 配置说明

默认配置位于 `config/fluxgate.conf`：

```ini
[server]
listen_host = 0.0.0.0
listen_port = 8080
workers = 4
backlog = 1024
max_events = 2048
buffer_size = 65536
connect_timeout_ms = 3000
idle_timeout_seconds = 60
stats_interval_seconds = 10
log_level = info

[health_check]
interval_ms = 3000
timeout_ms = 800
failure_threshold = 3
success_threshold = 2

[backend app-1]
host = 127.0.0.1
port = 9001
weight = 1

[backend app-2]
host = 127.0.0.1
port = 9002
weight = 2
```

关键参数：

| 参数 | 作用 |
|---|---|
| `workers` | Worker Reactor 线程数量 |
| `max_events` | 单次 `epoll_wait()` 可返回的最大事件数 |
| `buffer_size` | 每个会话每个方向的固定缓冲区大小 |
| `connect_timeout_ms` | 连接后端的最长等待时间 |
| `idle_timeout_seconds` | 无读写活动会话的清理时间 |
| `failure_threshold` | 连续多少次探测失败后摘除节点 |
| `success_threshold` | 连续多少次探测成功后恢复节点 |
| `weight` | 后端在加权最少连接算法中的容量权重 |

## 一键联调

脚本会启动两个本地 HTTP 后端和 FluxGate，验证双后端转发，并主动终止一个后端检查故障摘除：

```bash
./tools/smoke_test.sh ./build/fluxgate
```

期望输出包含两个节点及故障后的 `app-2`：

```text
{"backend": "app-1", "path": "/demo"}
{"backend": "app-2", "path": "/demo"}
...
{"backend": "app-2", "path": "/failover"}
smoke test passed
```

## 压力测试

### ApacheBench 自动测试

```bash
ulimit -n 65535
BACKEND_MODE=nginx ./tools/stress_test.sh ./build/fluxgate
```

脚本自动执行：单后端直连基线、50/100/200 并发 keep-alive、100 并发短连接，以及节点摘除后的稳态测试，并生成 Markdown 报告。

### wrk 持续测试

服务和两个 Nginx 后端保持运行后执行：

```bash
wrk -t4 -c200 -d60s --latency http://127.0.0.1:18080/benchmark
wrk -t6 -c500 -d300s --latency http://127.0.0.1:18080/benchmark
```

详细环境、命令、结果和解释见 [docs/BENCHMARK.md](docs/BENCHMARK.md)。

## 实测结果

测试环境：AMD Ryzen 5 5600H、12 个逻辑处理器、Manjaro Linux、4 个 FluxGate Worker、两个本地 Nginx 后端、本机回环网络。

| 工具与场景 | 请求量 | 吞吐量 | 平均延迟 | P99 | 客户端错误 |
|---|---:|---:|---:|---:|---:|
| wrk，200 并发，60 秒 | 7,037,381 | 117,285.96 req/s | 1.69 ms | 3.13 ms | 未报告 |
| wrk，500 并发，300 秒 | 33,789,395 | 112,599.55 req/s | 4.41 ms | 6.50 ms | 未报告 |
| ab，200 并发 keep-alive | 80,000 | 62,736.24 req/s | — | 6 ms | 0 |
| ab，100 并发短连接 | 5,000 | 13,181.73 req/s | — | 9 ms | 0 |
| ab，节点摘除后 100 并发 | 20,000 | 62,694.94 req/s | — | 3 ms | 0 |

其中 500 并发持续 5 分钟累计处理约 **3379 万次请求**，吞吐保持约 **11.26 万 req/s**。结果仅代表当前硬件、回环网络、静态小响应和对应参数，不应直接外推为生产环境固定上限。

## 工程目录

```text
FluxGate/
├── CMakeLists.txt              # 顶层构建、测试、安装和 Sanitizer 开关
├── cmake/                      # 编译器告警策略
├── config/                     # 默认运行配置
├── deploy/                     # systemd 与 logrotate 示例
├── docs/                       # 架构、代码导读、压测、验证和简历材料
├── include/fluxgate/           # 类型声明、接口和资源封装
├── src/                        # 网络、调度、会话和主程序实现
├── tests/                      # CTest 单元测试
├── tools/                      # 后端模拟、冒烟测试和压力测试脚本
└── benchmark-results/reference # 精选可复核测试结果
```

## 已验证内容

- Release 模式完整构建通过。
- CTest 单元测试通过。
- 双后端 HTTP 双向转发通过。
- 故障节点摘除和剩余节点接管通过。
- ApacheBench 长连接、短连接和故障后稳态测试均为 0 失败请求。
- wrk 200 并发持续 60 秒，完成 703 万次请求，未报告 Socket errors。
- wrk 500 并发持续 300 秒，完成 3379 万次请求，未报告 Socket errors。
- ASan/UBSan 构建和测试通过。

## 项目边界

FluxGate 是四层 TCP 代理，可透传 HTTP、HTTPS、数据库协议和自定义 TCP 协议，但不会解析应用层报文，也不负责 TLS 终止。当前版本尚未实现配置热加载、Prometheus HTTP 指标接口、动态服务发现、一致性哈希和跨节点控制面。

## 文档入口

- [架构设计](docs/ARCHITECTURE.md)
- [逐文件代码导读](docs/CODE_GUIDE.md)
- [压力测试](docs/BENCHMARK.md)
- [构建与验证](docs/VALIDATION.md)
- [原型重构对照](docs/REWRITE_NOTES.md)
- [简历与面试材料](docs/RESUME.md)
