# 构建与验证记录

验证日期：2026-07-02

## 环境

- Manjaro Linux，Linux 6.18.32-1-MANJARO x86_64
- AMD Ryzen 5 5600H，12 个逻辑处理器
- CMake 3.16+
- GCC/Clang C++11 模式
- 两个本地 Nginx 测试后端

## 已验证项目

1. Release 模式 CMake 配置与编译：通过。
2. CTest 单元测试：通过。
3. `--check` 配置解析和后端地址解析：通过。
4. 双后端 HTTP 双向转发：通过。
5. 主动终止后端后的健康摘除和剩余节点接管：通过。
6. AddressSanitizer：通过，未发现已知内存错误。
7. UndefinedBehaviorSanitizer：通过，未发现已知未定义行为。
8. CMake `install`：可安装可执行文件和默认配置。
9. ApacheBench 200 并发、80,000 次 keep-alive：0 失败，62,736.24 req/s，P99 6 ms。
10. ApacheBench 100 并发、5,000 次短连接：0 失败，13,181.73 req/s，P99 9 ms。
11. 节点摘除后 ApacheBench 20,000 次请求：0 失败，62,694.94 req/s，P99 3 ms。
12. wrk 200 并发、60 秒：7,037,381 次请求，117,285.96 req/s，平均 1.69 ms，P99 3.13 ms。
13. wrk 500 并发、300 秒：33,789,395 次请求，112,599.55 req/s，平均 4.41 ms，P99 6.50 ms。

wrk 两轮均未输出 `Socket errors` 统计。测试结束时出现的少量 `Connection reset by peer` 集中在客户端主动终止 keep-alive 连接的收尾阶段。

## 基础复现命令

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/fluxgate -c config/fluxgate.conf --check
./tools/smoke_test.sh ./build/fluxgate
ulimit -n 65535
BACKEND_MODE=nginx ./tools/stress_test.sh ./build/fluxgate
```

## 持续压测命令

```bash
wrk -t4 -c200 -d60s --latency http://127.0.0.1:18080/benchmark
wrk -t6 -c500 -d300s --latency http://127.0.0.1:18080/benchmark
```

## Sanitizer

```bash
cmake -S . -B build-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DFLUXGATE_ENABLE_SANITIZERS=ON
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
ASAN_OPTIONS=detect_leaks=1 ./tools/smoke_test.sh ./build-asan/fluxgate
```

## 结果边界

- ab 结果是短时微基准，主要用于功能和分档比较。
- wrk 结果更适合描述持续 keep-alive 吞吐能力。
- 回环网络结果不能直接等同于跨主机生产性能。
- “摘除后 0 失败”不表示后端宕机瞬间完全无损。
