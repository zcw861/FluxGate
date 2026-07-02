# FluxGate 代码导读

本文档按“启动流程—连接流程—数据流程—后台任务”的顺序解释每个目录、源文件、类和关键函数。源码中同时保留了 Doxygen 风格注释，可结合本文档阅读。

## 1. 阅读顺序

建议按以下顺序阅读：

1. `src/main.cpp`：理解进程启动和 Acceptor。
2. `include/fluxgate/Worker.h`、`src/Worker.cpp`：理解 Worker Reactor。
3. `include/fluxgate/ProxySession.h`、`src/ProxySession.cpp`：理解双向转发状态机。
4. `Backend*`：理解负载均衡和连接计数。
5. `HealthChecker*`：理解故障摘除与恢复。
6. `Net*`、`UniqueFd.h`、`ByteBuffer.h`：理解底层封装。
7. `Config*`、`Logger*`、`Metrics*`：理解工程支撑模块。
8. `tests/` 和 `tools/`：理解验证方式。

## 2. 构建文件

### `CMakeLists.txt`

负责整个工程的构建关系：

- 强制 C++11；
- 将核心实现构建为 `fluxgate_core` 静态库；
- 构建主程序 `fluxgate`；
- 可选构建 `fluxgate_tests`；
- 提供 ASan/UBSan 开关；
- 定义安装可执行文件和默认配置的规则。

### `cmake/CompilerWarnings.cmake`

定义 `fluxgate_set_warnings(target)`，为不同编译器统一开启较严格告警。所有核心目标都调用该函数，避免某个模块绕过质量约束。

## 3. 主程序

### `src/main.cpp`

#### `stopRequested`

信号处理器和主循环之间的最小通信变量。类型为 `volatile sig_atomic_t`，信号处理器只把它设为 1，不执行日志、锁或动态内存操作。

#### `signalHandler()`

响应 SIGINT/SIGTERM，只请求退出。

#### `installSignals()`

安装退出信号并忽略 SIGPIPE。代理向已关闭 Socket 写入时由 `send()` 返回错误处理，不允许 SIGPIPE 直接终止进程。

#### `Options` 与 `parseOptions()`

支持：

- `-c/--config`：指定配置文件；
- `--check`：只解析配置和地址，不启动服务；
- `--version`：输出版本；
- `--help`：输出帮助。

#### `selectWorker()`

从一个轮转起点开始，选择 `activeSessions()` 最少的 Worker。轮转起点用于在会话数相同的 Worker 间保持公平。

#### `main()`

主要生命周期：

1. 解析命令行；
2. 读取配置；
3. 初始化日志和后端池；
4. `--check` 模式直接返回；
5. 创建监听 Socket；
6. 创建并启动 Worker；
7. 启动 HealthChecker；
8. 可选启动指标线程；
9. poll 监听 Socket，循环 accept4；
10. 设置客户端 TCP_NODELAY；
11. 选择 Worker 并转交 fd；
12. 收到退出信号后按反向顺序停止全部线程。

## 4. Worker Reactor

### `include/fluxgate/Worker.h`

声明 Worker 拥有的资源和接口。

#### `AcceptedClient`

主线程传入 Worker 的最小数据：客户端 fd 和日志显示地址。成功 `enqueue()` 后 fd 所有权转移给 Worker。

#### 关键成员

- `epollFd_`：本 Worker 独占 epoll；
- `wakeFd_`：接收新连接/停止通知的 eventfd；
- `queue_`：主线程投递的新连接；
- `fdToSession_`：根据 epoll 返回 fd 定位会话；
- `sessions_`：持有全部会话所有权；
- `activeSessions_`：供主线程进行 Worker 负载选择。

### `src/Worker.cpp`

#### `Worker()`

创建 `epoll_create1(EPOLL_CLOEXEC)` 和 `eventfd(EFD_NONBLOCK | EFD_CLOEXEC)`，然后把 eventfd 注册进 epoll。

#### `start()` / `stop()` / `join()`

管理 Worker 线程。`stop()` 除了设置标志，还会写 eventfd，保证线程不会继续阻塞在 `epoll_wait()`。

#### `enqueue()`

1. 检查 Worker 是否运行；
2. 在短临界区内检查队列上限并压入连接；
3. 写 eventfd 唤醒 Worker；
4. 返回是否成功接管 fd。

若返回 false，主线程仍负责关闭客户端 fd。

#### `run()`

Worker 主循环：

1. `epoll_wait()` 等待事件；
2. eventfd 可读时清空计数并调用 `drainQueue()`；
3. 普通 fd 事件交给对应 `ProxySession::handleEvent()`；
4. 根据新状态调用 `updateInterests()` 修改 epoll 事件；
5. 当前批次结束后统一删除 closing 会话；
6. 每秒调用 `sweepIdleSessions()`；
7. 停止时清理队列和全部剩余会话。

#### `drainQueue()`

把共享队列与局部队列交换后立即释放锁，再逐个创建会话，减少 Acceptor 被阻塞的时间。

#### `createSession()`

1. 从 `BackendPool` 获得 `BackendLease`；
2. 无健康节点则拒绝客户端；
3. 发起非阻塞后端连接；
4. 构造 `ProxySession`；
5. 把客户端和后端 fd 注册进 epoll；
6. 写入两个索引映射；
7. 增加 Worker 和全局活动会话指标。

业务连接失败只记录指标，不直接把节点设为不健康。

#### `updateInterests()`

调用会话的 `clientEvents()` 和 `upstreamEvents()`，通过 `EPOLL_CTL_MOD` 动态启停读写事件。

#### `closeSession()`

按顺序完成：

1. 从 epoll 注销两端 fd；
2. 删除 fd 映射；
3. 删除会话所有权映射；
4. 减少活动计数。

最后一个 `shared_ptr` 释放后，`ProxySession` 析构并自动关闭两个 Socket、归还后端租约。

#### `sweepIdleSessions()`

统一检测：

- 后端连接建立超时；
- 已建立会话空闲超时。

先收集过期对象，再调用 `closeSession()`，避免遍历容器时直接删除当前元素。

## 5. ProxySession

### `include/fluxgate/ProxySession.h`

定义单条代理连接的完整状态：两个 fd、两个缓冲区、连接阶段、半关闭标志、关闭标志和时间戳。

### `src/ProxySession.cpp`

#### 构造函数

接管客户端 fd、后端 fd 和 `BackendLease`。若后端连接立即成功，直接记录成功次数；EINPROGRESS 状态则等待后续事件确认。

#### `handleEvent()`

统一处理两端事件：

- 后端仍在连接时先调用 `finishUpstreamConnect()`；
- `EPOLLERR` 通过 `SO_ERROR` 获取真实错误；
- 客户端可读时读入 `clientToUpstream_`；
- 客户端可写时发送 `upstreamToClient_`；
- 后端可读时读入 `upstreamToClient_`；
- 后端可写时发送 `clientToUpstream_`；
- 更新半关闭；
- 判断会话是否完成。

返回 false 只表示应关闭，不在函数内部删除对象。

#### `clientEvents()`

- 客户端读方向未关闭，且请求缓冲区有空间：关注 `EPOLLIN`；
- 有后端响应待写回：关注 `EPOLLOUT`；
- 始终关注 `EPOLLRDHUP`。

#### `upstreamEvents()`

- 连接阶段：关注 `EPOLLOUT`；
- 连接完成后，响应缓冲区有空间才关注后端 `EPOLLIN`；
- 有客户端请求待转发才关注后端 `EPOLLOUT`。

#### `finishUpstreamConnect()`

用 `getsockopt(SO_ERROR)` 检查非阻塞 connect 结果。成功后清除连接阶段标志、记录成功并刷新活动时间；失败时记录指标并返回 false。

#### `readInto()`

循环 `recv()`：

- 成功：写入 ByteBuffer、更新方向字节指标和活动时间；
- 0：对端 EOF；
- EINTR：重试；
- EAGAIN：等待下一次事件；
- 其他：真实错误。

#### `writeFrom()`

循环 `send(MSG_NOSIGNAL)` 并正确处理短写。数据没有全部发送时保留在 ByteBuffer，下次 `EPOLLOUT` 继续发送。

#### `updateHalfClose()`

当来源侧 EOF 且对应缓存已经排空时，对目标侧调用 `shutdown(SHUT_WR)`，只关闭写方向，不影响反向响应继续传输。

#### `completed()`

后端已经结束响应，并且后端到客户端缓冲区清空后，会话即可关闭。

## 6. 缓冲区和 fd 管理

### `include/fluxgate/ByteBuffer.h`

固定容量字节数组加读写游标：

- `readableBytes()`：待发送数据；
- `writableBytes()`：尾部可写空间；
- `hasWritten()`：recv 后推进写游标；
- `consume()`：send 后推进读游标；
- `makeWritable()`：头部有空闲但尾部已满时压缩未发送数据。

固定容量是背压设计的一部分，不是缺少自动扩容。

### `include/fluxgate/UniqueFd.h`

RAII 文件描述符：

- 析构自动 close；
- 禁止拷贝；
- 支持移动；
- `release()` 转移所有权；
- `reset()` 替换或关闭。

## 7. 后端与调度

### `Backend.h/.cpp`

`Backend` 保存：静态配置、解析地址、健康状态、活动连接数、成功连接数和失败连接数。

运行期计数使用原子变量，因为多个 Worker 和健康检查线程会并发访问。

### `BackendPool.h/.cpp`

#### `BackendLease`

构造增加活动连接数，析构减少。它把“连接计数回滚”从大量错误分支中移出，交给 RAII。

#### `BackendPool::acquire()`

在短临界区中：

1. 设置轮转起点；
2. 跳过不健康节点；
3. 使用交叉乘法比较 `active/weight`；
4. 返回选中节点的租约。

## 8. 健康检查

### `HealthChecker.h/.cpp`

#### `start()` / `stop()` / `join()`

管理独立探测线程。`stop()` 使用条件变量唤醒等待，避免退出时仍等待完整探测周期。

#### `run()`

启动后立即探测一次，然后按 `intervalMs_` 等待下一轮。

#### `checkOnce()`

对每个后端调用 `waitForConnect()`：

- 成功：失败计数清零，成功计数加一；
- 失败：成功计数清零，失败计数加一；
- 跨过阈值才修改健康状态并输出状态变化日志。

## 9. 网络封装

### `Net.h/.cpp`

- `setNonBlocking()`：设置 `O_NONBLOCK`；
- `setCloseOnExec()`：设置 `FD_CLOEXEC`；
- `setTcpNoDelay()`：关闭 Nagle，降低小包延迟；
- `resolveTcpEndpoint()`：使用 `getaddrinfo()` 解析 IPv4/IPv6；
- `createListenSocket()`：创建、配置、bind 和 listen；
- `connectNonBlocking()`：发起后端连接并区分立即成功/EINPROGRESS；
- `waitForConnect()`：健康检查使用 poll 等待连接结果；
- `peerAddress()`：把客户端 sockaddr 转成文本。

## 10. 配置、日志和指标

### `Config.h/.cpp`

严格解析 INI：

- 去除空白和注释；
- section 和键名大小写不敏感；
- 所有整数必须完整解析并在范围内；
- 未知字段直接报错；
- 后端必须包含名称、host 和 port；
- 后端名称不能重复；
- 健康检查超时必须小于周期。

### `Logger.h/.cpp`

进程单例、互斥保护、printf 风格接口。日志包含时间、级别、源文件和行号。宏负责自动传入调用位置。

### `Metrics.h/.cpp`

原子累计：接入、活动、拒绝、连接失败和双向字节。`snapshot()` 生成单行文本供指标线程输出。

## 11. 测试与工具

### `tests/fluxgate_tests.cpp`

- `testConfigParser()`：验证配置字段；
- `testWeightedLeastConnections()`：验证 1:2 权重得到 2:4 租约分配；
- `testUnhealthyBackendIsSkipped()`：验证摘除节点不参与调度。

### `tools/demo_upstream.py`

冒烟测试后端。每次返回节点名和路径，主动关闭连接，便于功能观察。

### `tools/async_upstream.py`

支持 keep-alive 的 asyncio 后端，在没有 Nginx 时作为压力测试备用。

### `tools/smoke_test.sh`

自动生成临时配置、启动两个后端和网关、检查双节点响应、终止一个后端并验证剩余节点接管。

### `tools/stress_test.sh`

管理完整 ab 测试流程，自动保存原始输出和生成报告。退出时会清理所有临时进程，因此后续 wrk 测试需要单独保持服务运行。

### `tools/benchmark.sh`

对已启动服务执行快速压测，优先使用 wrk，否则使用 ab。
