# FluxGate 架构设计

## 1. 设计目标

FluxGate 面向 Linux 通用 TCP 转发场景，重点解决以下工程问题：

- 大量连接下避免“每连接一线程”的调度和内存开销；
- 不允许任何业务 Socket 阻塞 Worker；
- 正确处理短读短写、TCP 半关闭和慢连接背压；
- 后端故障时能够自动摘除，并在恢复后重新加入；
- 文件描述符、连接计数和线程生命周期必须异常安全；
- 项目必须能通过 CMake 构建、自动测试和压力测试复现。

FluxGate 是四层代理，不解析 HTTP 等应用层协议。数据面只关注 TCP 字节流和连接状态。

## 2. 线程与 Reactor 模型

### 2.1 主线程：Acceptor / Dispatcher

主线程负责：

1. 创建非阻塞监听 Socket；
2. 使用 `poll()` 等待监听 Socket 可读；
3. 循环 `accept4(SOCK_NONBLOCK | SOCK_CLOEXEC)` 取出全部已完成连接；
4. 根据 Worker 当前活动会话数选择负载较低的线程；
5. 将客户端 fd 放入目标 Worker 队列，并写 `eventfd` 唤醒它。

主线程不参与后续数据转发。这样可以让监听接入与会话 I/O 解耦，并使每条会话始终归属于一个固定 Worker。

### 2.2 Worker Reactor

每个 Worker 拥有：

- 一个独立 epoll 实例；
- 一个用于跨线程通知的 `eventfd`；
- 一个受互斥锁保护的新连接队列；
- `fd -> ProxySession` 映射；
- `session_id -> ProxySession` 所有权映射；
- 一个独立事件循环线程。

主线程只在入队时短暂持有队列锁。Worker 被唤醒后把共享队列整体交换到局部队列，再释放锁并创建会话，因此 DNS 以外的连接操作不会阻塞 Acceptor。

同一会话的客户端与后端 Socket 均由同一 Worker 处理，数据面不需要给 `ProxySession` 加锁。

## 3. 新连接建立流程

```text
accept4 client
      |
      v
select least-loaded Worker
      |
      v
enqueue + eventfd wakeup
      |
      v
BackendPool::acquire()
      |
      v
connectNonBlocking(upstream)
      |
      +---- immediate success
      |
      +---- EINPROGRESS -> wait EPOLLOUT -> SO_ERROR
      |
      v
register client/upstream fd in epoll
      |
      v
publish ProxySession into maps
```

会话只有在两个 fd 均成功注册到 epoll 后才发布到 Worker 映射中。若中间步骤失败，`UniqueFd`、`BackendLease` 和局部 `shared_ptr` 会自动回收资源。

## 4. ProxySession 双向状态机

一条会话拥有两个 Socket 和两个固定容量缓冲区：

```text
client ---- clientToUpstream ----> backend
client <--- upstreamToClient ----- backend
```

### 4.1 非阻塞连接

`connect()` 有三种关键结果：

- 返回 0：连接立即成功；
- 返回 -1 且 `errno == EINPROGRESS`：连接正在建立；
- 其他错误：本次会话创建失败。

EINPROGRESS 状态下，后端 fd 只关注 `EPOLLOUT` 和关闭事件。事件到达后使用 `getsockopt(SO_ERROR)` 获取真实连接结果，因为 `EPOLLOUT` 只表示连接操作已经结束，不保证一定成功。

业务连接失败只记录指标和节点失败次数，不直接修改健康状态。节点摘除统一由 `HealthChecker` 的独立探测决定，避免一次瞬时连接失败放大为级联摘除。

### 4.2 读取

`readInto()` 循环调用 `recv()`，直到：

- 内核返回 `EAGAIN/EWOULDBLOCK`；
- 固定缓冲区没有可写空间；
- 对端返回 EOF；
- 出现不可恢复错误。

该循环适配当前水平触发 epoll。每次成功读取都会推进写游标、累计方向字节数并刷新最近活动时间。

### 4.3 写入

`writeFrom()` 循环调用 `send(MSG_NOSIGNAL)`：

- 处理一次只写出部分数据的短写；
- `EAGAIN` 时保留未发送数据并等待下一次 `EPOLLOUT`；
- 使用 `MSG_NOSIGNAL` 避免写入已关闭连接时触发 SIGPIPE；
- 成功发送后推进读游标。

### 4.4 动态事件兴趣与背压

`clientEvents()` 和 `upstreamEvents()` 根据状态动态生成 epoll 事件：

- 来源方向缓冲区还有空间时才关注 `EPOLLIN`；
- 目标方向存在待发送数据时才关注 `EPOLLOUT`；
- 后端仍处于连接阶段时只关注连接完成事件；
- 始终关注 `EPOLLRDHUP` 以识别对端半关闭。

当目标侧发送缓慢，缓冲区会逐渐填满。填满后来源 Socket 的 `EPOLLIN` 被移除，直到目标侧排空数据再恢复。这就是固定内存上限下的背压。

### 4.5 TCP 半关闭

TCP 的读写方向可以独立关闭。收到一侧 EOF 后不能立即销毁会话：

1. 标记该侧读方向关闭；
2. 继续把已经读取的缓存发送到另一侧；
3. 缓存排空后对另一侧执行 `shutdown(SHUT_WR)`；
4. 继续等待反方向剩余数据；
5. 后端响应结束且响应缓存清空后关闭会话。

## 5. 会话延迟删除

一次 `epoll_wait()` 可能同时返回同一会话的客户端和后端事件。如果处理第一个事件时立即删除会话并关闭 fd，Linux 可能快速复用这个数字，导致当前批次的后续事件错误关联。

FluxGate 的处理方式：

1. `ProxySession` 只设置 `closing_`；
2. 当前事件批次继续跳过所有 closing 会话；
3. 批次结束后 Worker 收集 closing 会话；
4. 统一从 epoll、fd 映射和会话映射中移除。

## 6. 后端负载均衡

使用加权最少连接：

```text
score = active_connections / weight
```

为了避免浮点误差，代码通过交叉乘法比较：

```text
candidate.active * selected.weight
    <
selected.active * candidate.weight
```

只有健康节点参与选择。相同分数下，通过不断变化的轮转起点避免总是选择配置中的第一个节点。

“选择节点”和“活动连接数加一”位于同一个短临界区，避免多个 Worker 同时看到相同旧计数并集中命中同一节点。

## 7. 主动健康检查

`HealthChecker` 在独立线程中周期执行带超时的 TCP Connect 探测。每个后端保存：

- 连续失败次数；
- 连续成功次数。

状态规则：

- 健康节点连续失败达到 `failure_threshold` 后变为 `UNHEALTHY`；
- 不健康节点连续成功达到 `success_threshold` 后变为 `HEALTHY`；
- 状态未跨阈值时保持不变。

这种迟滞机制可以降低瞬时网络抖动、accept 队列压力或短暂超时造成的状态反复切换。

## 8. 资源管理

### 8.1 UniqueFd

`UniqueFd` 独占管理 Socket、epoll 和 eventfd：

- 析构自动 `close()`；
- 不可拷贝，避免重复关闭；
- 可移动，支持安全转移所有权；
- `release()` 用于把 fd 交给其他资源对象。

### 8.2 BackendLease

`BackendLease` 表示会话对后端的占用：

- 构造时增加 `activeConnections`；
- 移动时只转移所有权；
- 析构或 `reset()` 时自动减少计数。

这使所有错误路径都不需要手工回滚后端连接数。

### 8.3 shared_ptr 会话所有权

`sessions_` 保存会话主所有权，`fdToSession_` 允许通过 epoll 返回的 fd 快速定位同一对象。延迟删除完成后，映射中的引用全部移除，会话自动析构并关闭两个 Socket。

## 9. 超时与定期任务

Worker 每秒执行一次统一扫描：

- 后端连接仍处于 EINPROGRESS 且超过 `connect_timeout_ms`：记录连接失败并关闭；
- 最近活动时间超过 `idle_timeout_seconds`：关闭空闲会话。

统一扫描避免为每条连接创建独立定时器。

主进程可选启动指标线程，按 `stats_interval_seconds` 输出：

- 累计接入；
- 当前活动；
- 拒绝数；
- 后端连接失败；
- 双向字节数；
- 每个后端健康状态和连接统计。

## 10. 优雅退出

信号处理器只修改 `sig_atomic_t` 标志，避免在异步信号环境中执行非安全操作。主循环发现标志后按以下顺序退出：

1. 停止接收新连接；
2. 停止健康检查；
3. 请求全部 Worker 停止并通过 eventfd 唤醒；
4. Worker 清理排队 fd 和活动会话；
5. join 健康检查与 Worker；
6. 停止指标线程；
7. 输出最终指标并退出。

## 11. 已知边界与扩展方向

- 当前 Acceptor 使用单监听 Socket；可扩展 `SO_REUSEPORT` 多 Acceptor。
- 指标仅输出日志；可增加 Prometheus 管理端口。
- 配置启动后固定；可增加 SIGHUP 热加载。
- 负载算法可扩展一致性哈希、源地址哈希和慢启动。
- 可增加熔断、有限重试预算和动态服务发现。
- 数据面可研究 `splice()`、零拷贝或 io_uring。
