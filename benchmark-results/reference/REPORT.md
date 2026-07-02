# FluxGate 参考压力测试结果

测试日期：2026-07-02

环境：AMD Ryzen 5 5600H、12 个逻辑处理器、Manjaro Linux、4 个 FluxGate Worker、两个本地 Nginx 后端、本机回环网络。

## ApacheBench

| 场景 | 完成请求 | 失败请求 | 吞吐量 | P99 |
|---|---:|---:|---:|---:|
| 单 Nginx 直连 keep-alive，c=100 | 30,000 | 0 | 60,448.77 req/s | 3 ms |
| FluxGate keep-alive，c=50 | 30,000 | 0 | 62,014.22 req/s | 2 ms |
| FluxGate keep-alive，c=100 | 50,000 | 0 | 62,902.66 req/s | 3 ms |
| FluxGate keep-alive，c=200 | 80,000 | 0 | 62,736.24 req/s | 6 ms |
| FluxGate 短连接，c=100 | 5,000 | 0 | 13,181.73 req/s | 9 ms |
| app-1 摘除后 keep-alive，c=100 | 20,000 | 0 | 62,694.94 req/s | 3 ms |

## wrk

| 场景 | 请求量 | 吞吐量 | 平均延迟 | P99 | 最大延迟 |
|---|---:|---:|---:|---:|---:|
| 4 threads，200 connections，60s | 7,037,381 | 117,285.96 req/s | 1.69 ms | 3.13 ms | 22.85 ms |
| 6 threads，500 connections，300s | 33,789,395 | 112,599.55 req/s | 4.41 ms | 6.50 ms | 26.87 ms |

wrk 两轮均未输出 Socket errors 行。结果只代表当前硬件、回环网络、静态小响应和对应参数。
