#!/usr/bin/env bash
# 对已经启动的 FluxGate 执行一轮简化压测。
# 优先使用多线程 wrk；没有 wrk 时退回 ApacheBench。
set -euo pipefail

# 第一个参数是目标 URL，未提供时测试默认监听端口。
TARGET="${1:-http://127.0.0.1:8080/}"

if command -v wrk >/dev/null 2>&1; then
    # 4 个压测线程、200 个并发连接、持续 30 秒，并输出延迟分布。
    exec wrk -t4 -c200 -d30s --latency "$TARGET"
fi

if command -v ab >/dev/null 2>&1; then
    # ab 作为兼容方案：2 万请求、200 并发并启用 keep-alive。
    exec ab -r -k -n 20000 -c 200 "$TARGET"
fi

echo "Install wrk or ApacheBench (ab) to run the benchmark." >&2
exit 1
