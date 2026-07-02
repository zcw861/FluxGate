~~~~~~~~# FluxGate 压力测试

本文档记录 FluxGate 的测试模型、复现步骤、实测结果和结果边界。测试分为两类：

1. **ApacheBench 自动测试**：快速比较直连、代理、短连接和节点摘除后的稳态表现。
2. **wrk 持续测试**：验证 60 秒与 300 秒高并发运行时的吞吐和尾延迟。

## 1. 测试环境

- 日期：2026-07-02
- 操作系统：Manjaro Linux
- 内核：Linux 6.18.32-1-MANJARO x86_64
- CPU：AMD Ryzen 5 5600H with Radeon Graphics
- 逻辑处理器：12
- FluxGate Worker：4
- FluxGate 线程数：7
- FluxGate RSS：约 17.1 MiB（自动 ab 测试记录）
- 后端：两个本地 Nginx 实例
- 网络：本机回环地址
- 应用模型：HTTP/1.1 静态小 JSON 响应

压测前建议提高文件描述符限制：

```bash
ulimit -n 65535
```

## 2. ApacheBench 自动测试

### 2.1 依赖

```bash
# Arch Linux / Manjaro
sudo pacman -S apache nginx

# Ubuntu / Debian
sudo apt install apache2-utils nginx
```

### 2.2 执行

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ulimit -n 65535
BACKEND_MODE=nginx ./tools/stress_test.sh ./build/fluxgate
```

脚本执行流程：

1. 生成隔离的临时配置。
2. 启动两个本地 Nginx 后端。
3. 启动 4 Worker 的 FluxGate。
4. 预热连接和页面缓存。
5. 执行单后端直连基线。
6. 执行 50、100、200 并发 keep-alive。
7. 执行 100 并发短连接。
8. 重启测试环境，仅保留 `app-2`。
9. 等待健康检查摘除 `app-1` 后执行稳态测试。
10. 保存原始输出、日志和自动生成的 Markdown 报告。

### 2.3 实测结果

| 场景 | 完成请求 | 失败请求 | 吞吐量 | P50 | P95 | P99 |
|---|---:|---:|---:|---:|---:|---:|
| 直连单 Nginx，keep-alive，c=100 | 30,000 | 0 | 60,448.77 req/s | 2 ms | 2 ms | 3 ms |
| FluxGate keep-alive，c=50 | 30,000 | 0 | 62,014.22 req/s | 1 ms | 2 ms | 2 ms |
| FluxGate keep-alive，c=100 | 50,000 | 0 | 62,902.66 req/s | 2 ms | 3 ms | 3 ms |
| FluxGate keep-alive，c=200 | 80,000 | 0 | 62,736.24 req/s | 3 ms | 6 ms | 6 ms |
| FluxGate 短连接，c=100 | 5,000 | 0 | 13,181.73 req/s | 8 ms | 9 ms | 9 ms |
| `app-1` 摘除后 keep-alive，c=100 | 20,000 | 0 | 62,694.94 req/s | 1 ms | 3 ms | 3 ms |

直连单后端和代理双后端不是严格相同的控制变量，且每轮 ab 测试持续时间较短。因此不能根据某一轮结果宣称“代理比直连更快”。这组结果主要用于验证自动化测试流程、零失败转发和故障摘除后的稳态承载能力。

## 3. wrk 持续高并发测试

`stress_test.sh` 退出时会自动关闭临时后端和 FluxGate。运行 wrk 前必须保持两个后端和网关持续运行，并确认端口存在：

```bash
ss -lntp | grep -E '18080|19001|19002'
curl http://127.0.0.1:18080/benchmark
```

### 3.1 200 并发，60 秒

命令：

```bash
wrk -t4 -c200 -d60s --latency http://127.0.0.1:18080/benchmark
```

结果：

```text
4 threads and 200 connections
Latency Avg: 1.69ms
Latency P50: 1.65ms
Latency P90: 2.16ms
Latency P99: 3.13ms
Latency Max: 22.85ms
7,037,381 requests in 60s
Requests/sec: 117,285.96
Transfer/sec: 21.59MB
```

`wrk` 未输出 `Socket errors` 行，表示该轮未报告 connect/read/write/timeout 错误。测试结束瞬间 FluxGate 可能记录少量 `Connection reset by peer`，这是 wrk 到达时限后主动结束仍在复用的 keep-alive 连接造成的收尾事件，不等同于压测请求失败。

### 3.2 500 并发，300 秒

命令：

```bash
wrk -t6 -c500 -d300s --latency http://127.0.0.1:18080/benchmark
```

结果：

```text
6 threads and 500 connections
Latency Avg: 4.41ms
Latency P50: 4.24ms
Latency P90: 5.09ms
Latency P99: 6.50ms
Latency Max: 26.87ms
33,789,395 requests in 300s
Requests/sec: 112,599.55
Transfer/sec: 20.72MB
```

该轮持续 5 分钟，累计处理约 3379 万次请求。与 200 并发相比，并发提高 2.5 倍后吞吐量只下降约 4%，平均延迟和 P99 上升，说明当前本机环境已接近吞吐平台区间，继续增加连接数主要增加排队延迟。

## 4. 结果汇总

| 场景 | 吞吐量 | 平均延迟 | P99 | 持续时间 |
|---|---:|---:|---:|---:|
| wrk，200 并发 | 117,285.96 req/s | 1.69 ms | 3.13 ms | 60 秒 |
| wrk，500 并发 | 112,599.55 req/s | 4.41 ms | 6.50 ms | 300 秒 |

吞吐量、并发和平均延迟满足近似关系：

```text
200 / 117285.96 ≈ 1.70 ms
500 / 112599.55 ≈ 4.44 ms
```

与 wrk 输出的 1.69 ms 和 4.41 ms 基本一致，说明数据内部自洽。

## 5. 故障转移结论边界

当前自动脚本验证的是：

> `app-1` 被主动健康检查确认并摘除后，剩余 `app-2` 在 100 并发下继续完成 20,000 次请求，失败请求为 0。

它没有证明“后端宕机瞬间所有既有连接和新请求均零失败”。故障窗口中可能存在已选中故障节点但尚未完成连接的会话。若要验证故障瞬间表现，应在 wrk 持续运行期间终止一个后端，并记录摘除耗时、Socket errors 和延迟尖峰。

## 6. 长稳测试后的资源检查

测试结束后建议执行：

```bash
grep "metrics" .wrk-env/fluxgate.log | tail -n 10
ls /proc/$(cat .wrk-env/fluxgate.pid)/fd | wc -l
ss -antp | grep ':18080' | awk '{print $1}' | sort | uniq -c
ps -o pid,nlwp,%cpu,%mem,rss,vsz,cmd -p "$(cat .wrk-env/fluxgate.pid)"
```

重点检查：

- `active` 最终回到 0；
- `rejected` 和 `connect_failures` 没有异常增长；
- 文件描述符数量在连接关闭后回落；
- 不存在持续增长的 `CLOSE-WAIT`；
- RSS 不随测试轮次持续增长。

## 7. 使用限制

上述结果仅代表当前 CPU、内核、本机回环、Nginx 静态响应、Worker 数量和工具参数。真实网络中的 RTT、包丢失、后端业务耗时、响应体大小、TLS 和日志级别都会影响吞吐与延迟。
