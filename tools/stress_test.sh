#!/usr/bin/env bash
# 自动压力测试：管理后端和网关生命周期，执行 ab 分档测试并生成 Markdown 报告。
set -euo pipefail

# 路径、端口和请求量均可通过环境变量覆盖，便于 CI 或不同机器复现。
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BINARY="${1:-${ROOT_DIR}/build/fluxgate}"
LISTEN_PORT="${LISTEN_PORT:-18080}"
BACKEND1_PORT="${BACKEND1_PORT:-19001}"
BACKEND2_PORT="${BACKEND2_PORT:-19002}"
RESULT_DIR="${RESULT_DIR:-${ROOT_DIR}/benchmark-results/$(date +%Y%m%d-%H%M%S)}"
PYTHON="${PYTHON:-python3}"
AB="${AB:-ab}"
BACKEND_MODE="${BACKEND_MODE:-auto}"
AB_TIMEOUT_SECONDS="${AB_TIMEOUT_SECONDS:-60}"
BACKEND_DRIVER=""
DIRECT_REQUESTS="${DIRECT_REQUESTS:-30000}"
PROXY_C50_REQUESTS="${PROXY_C50_REQUESTS:-30000}"
PROXY_C100_REQUESTS="${PROXY_C100_REQUESTS:-50000}"
PROXY_C200_REQUESTS="${PROXY_C200_REQUESTS:-80000}"
SHORT_REQUESTS="${SHORT_REQUESTS:-5000}"
FAILOVER_REQUESTS="${FAILOVER_REQUESTS:-20000}"

# 每次运行使用独立结果目录和临时工作目录，防止测试相互污染。
mkdir -p "${RESULT_DIR}"
TMP_DIR="$(mktemp -d)"
PIDS=()

# trap 统一回收所有已启动进程。先 TERM 给出退出机会，再 KILL 处理残留。
cleanup() {
    local pid
    for pid in "${PIDS[@]:-}"; do
        kill -TERM "${pid}" >/dev/null 2>&1 || true
    done
    sleep 0.2
    for pid in "${PIDS[@]:-}"; do
        kill -KILL "${pid}" >/dev/null 2>&1 || true
        wait "${pid}" >/dev/null 2>&1 || true
    done
    rm -rf "${TMP_DIR}"
}
trap cleanup EXIT INT TERM

# 检查外部工具，缺失时在启动任何服务前失败。
require_command() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "missing required command: $1" >&2
        exit 1
    }
}

# 使用 Bash /dev/tcp 轮询端口，替代固定等待时间。
wait_port() {
    local host="$1"
    local port="$2"
    local retries="${3:-100}"
    local i
    for ((i = 0; i < retries; ++i)); do
        if (echo >/dev/tcp/"${host}"/"${port}") >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.05
    done
    echo "port ${host}:${port} did not become ready" >&2
    return 1
}

# 执行一轮 ApacheBench，并把完整原始输出保存到结果目录。
run_ab() {
    local name="$1"
    local requests="$2"
    local concurrency="$3"
    local url="$4"
    local keepalive="${5:-no}"
    local output="${RESULT_DIR}/${name}.txt"
    local options=(-r -s 10 -n "$requests" -c "$concurrency")

    if [[ "$keepalive" == "yes" ]]; then
        options+=(-k)
    fi

    echo "[stress] ${name}: n=${requests}, c=${concurrency}, keepalive=${keepalive}, url=${url}"
    if command -v timeout >/dev/null 2>&1; then
        timeout "${AB_TIMEOUT_SECONDS}s" "$AB" "${options[@]}" "$url" >"$output" 2>&1
    else
        "$AB" "${options[@]}" "$url" >"$output" 2>&1
    fi
}

# 从 ab 文本中提取单行指标。
extract_value() {
    local file="$1"
    local label="$2"
    grep -m1 "^${label}" "$file" | sed -E "s/^${label}[[:space:]]*//" | xargs
}

# 从 ab 延迟分位表中提取指定百分位，单位为毫秒。
extract_percentile_ms() {
    local file="$1"
    local percentile="$2"
    awk -v p="$percentile" '$1 == p"%" {print $2; exit}' "$file"
}

# 将一份 ab 原始输出转换为 REPORT.md 中的一行表格。
append_result_row() {
    local label="$1"
    local file="$2"
    local completed failed rps mean p50 p95 p99 transfer
    completed="$(extract_value "$file" "Complete requests:")"
    failed="$(extract_value "$file" "Failed requests:")"
    rps="$(extract_value "$file" "Requests per second:")"
    mean="$(grep -m1 '^Time per request:.*(mean)$' "$file" | awk '{print $4}')"
    p50="$(extract_percentile_ms "$file" 50)"
    p95="$(extract_percentile_ms "$file" 95)"
    p99="$(extract_percentile_ms "$file" 99)"
    transfer="$(extract_value "$file" "Transfer rate:")"
    printf '| %s | %s | %s | %s | %s ms | %s ms | %s ms | %s |\n' \
        "$label" "$completed" "$failed" "$rps" "$p50" "$p95" "$p99" "$transfer" >>"${RESULT_DIR}/REPORT.md"
}

require_command "$PYTHON"
require_command "$AB"
[[ -x "$BINARY" ]] || {
    echo "FluxGate binary not found or not executable: $BINARY" >&2
    exit 1
}

# 生成压测专用配置：高 backlog、较大事件数组、短健康检查周期。
cat >"${TMP_DIR}/stress.conf" <<EOF
[server]
listen_host = 127.0.0.1
listen_port = ${LISTEN_PORT}
workers = 4
backlog = 4096
max_events = 8192
buffer_size = 65536
connect_timeout_ms = 1000
idle_timeout_seconds = 30
stats_interval_seconds = 60
log_level = warn

[health_check]
interval_ms = 500
timeout_ms = 300
failure_threshold = 5
success_threshold = 2

[backend app-1]
host = 127.0.0.1
port = ${BACKEND1_PORT}
weight = 1

[backend app-2]
host = 127.0.0.1
port = ${BACKEND2_PORT}
weight = 1
EOF

# 启动一个隔离的 Nginx 实例，返回固定 JSON，尽量避免后端先成为瓶颈。
start_nginx_backend() {
    local name="$1"
    local port="$2"
    local directory="${TMP_DIR}/${name}"
    mkdir -p "${directory}/logs"
    cat >"${directory}/nginx.conf" <<EOF
worker_processes 1;
daemon off;
pid logs/nginx.pid;
error_log stderr warn;
events { worker_connections 16384; }
http {
    access_log off;
    server {
        listen 127.0.0.1:${port} backlog=8192;
        default_type application/json;
        keepalive_timeout 30s;
        location / {
            return 200 '{"backend":"${name}","path":"\$uri"}';
        }
    }
}
EOF
    nginx -p "${directory}/" -c "${directory}/nginx.conf" >"${RESULT_DIR}/${name}.log" 2>&1 &
    LAST_BACKEND_PID=$!
}

# 没有 Nginx 时使用 asyncio 后端作为兼容方案。
start_python_backend() {
    local name="$1"
    local port="$2"
    "$PYTHON" "${ROOT_DIR}/tools/async_upstream.py" --port "$port" --name "$name" >"${RESULT_DIR}/${name}.log" 2>&1 &
    LAST_BACKEND_PID=$!
}

# auto 模式优先选择 Nginx；可显式指定 nginx 或 python-asyncio。
if [[ "$BACKEND_MODE" == "nginx" ]] || { [[ "$BACKEND_MODE" == "auto" ]] && command -v nginx >/dev/null 2>&1; }; then
    BACKEND_DRIVER="nginx"
    start_nginx_backend app-1 "$BACKEND1_PORT"
    BACKEND1_PID=$LAST_BACKEND_PID
    PIDS+=("$BACKEND1_PID")
    start_nginx_backend app-2 "$BACKEND2_PORT"
    BACKEND2_PID=$LAST_BACKEND_PID
    PIDS+=("$BACKEND2_PID")
else
    BACKEND_DRIVER="python-asyncio"
    start_python_backend app-1 "$BACKEND1_PORT"
    BACKEND1_PID=$LAST_BACKEND_PID
    PIDS+=("$BACKEND1_PID")
    start_python_backend app-2 "$BACKEND2_PORT"
    BACKEND2_PID=$LAST_BACKEND_PID
    PIDS+=("$BACKEND2_PID")
fi

wait_port 127.0.0.1 "$BACKEND1_PORT"
wait_port 127.0.0.1 "$BACKEND2_PORT"

"$BINARY" -c "${TMP_DIR}/stress.conf" >"${RESULT_DIR}/fluxgate.log" 2>&1 &
FLUXGATE_PID=$!
PIDS+=("$FLUXGATE_PID")
wait_port 127.0.0.1 "$LISTEN_PORT"
sleep 1

# 预热，避免首次连接和页缓存影响正式结果。
"$AB" -r -s 10 -k -n 1000 -c 20 "http://127.0.0.1:${LISTEN_PORT}/warmup" >/dev/null 2>&1

# 单后端直连基线，用于区分后端开销和代理开销。
run_ab "direct-keepalive-c100" "$DIRECT_REQUESTS" 100 "http://127.0.0.1:${BACKEND1_PORT}/benchmark" yes

# 持久连接分档压测，主要测试 epoll 转发与缓冲区处理能力。
run_ab "proxy-keepalive-c50" "$PROXY_C50_REQUESTS" 50 "http://127.0.0.1:${LISTEN_PORT}/benchmark" yes
run_ab "proxy-keepalive-c100" "$PROXY_C100_REQUESTS" 100 "http://127.0.0.1:${LISTEN_PORT}/benchmark" yes
run_ab "proxy-keepalive-c200" "$PROXY_C200_REQUESTS" 200 "http://127.0.0.1:${LISTEN_PORT}/benchmark" yes

# 短连接测试保留较小请求量，防止本机临时端口/TIME_WAIT 干扰结果。
run_ab "proxy-short-c100" "$SHORT_REQUESTS" 100 "http://127.0.0.1:${LISTEN_PORT}/short" no

# 故障摘除压测使用新的 FluxGate 进程，避免前面各轮 keep-alive 会话仍在内核中回收，
# 从而让结果只反映“一个后端不可用时，健康检查摘除节点并由剩余节点承载流量”的能力。
kill -TERM "$FLUXGATE_PID" >/dev/null 2>&1 || true
sleep 0.2
kill -KILL "$FLUXGATE_PID" >/dev/null 2>&1 || true
wait "$FLUXGATE_PID" >/dev/null 2>&1 || true
kill -TERM "$BACKEND1_PID" "$BACKEND2_PID" >/dev/null 2>&1 || true
sleep 0.2
kill -KILL "$BACKEND1_PID" "$BACKEND2_PID" >/dev/null 2>&1 || true
wait "$BACKEND1_PID" >/dev/null 2>&1 || true
wait "$BACKEND2_PID" >/dev/null 2>&1 || true

# 只重新启动 app-2，构造独立且无历史连接残留的单节点故障场景。
rm -rf "${TMP_DIR}/app-2"
if [[ "$BACKEND_DRIVER" == "nginx" ]]; then
    start_nginx_backend app-2 "$BACKEND2_PORT"
else
    start_python_backend app-2 "$BACKEND2_PORT"
fi
BACKEND2_PID=$LAST_BACKEND_PID
PIDS+=("$BACKEND2_PID")
wait_port 127.0.0.1 "$BACKEND2_PORT"

"$BINARY" -c "${TMP_DIR}/stress.conf" >"${RESULT_DIR}/fluxgate-failover.log" 2>&1 &
FLUXGATE_PID=$!
PIDS+=("$FLUXGATE_PID")
wait_port 127.0.0.1 "$LISTEN_PORT"
sleep 4
run_ab "failover-keepalive-c100" "$FAILOVER_REQUESTS" 100 "http://127.0.0.1:${LISTEN_PORT}/failover" yes

# 故障后稳态测试完成后采集进程资源信息，用于写入报告。
RSS_KB="$(ps -o rss= -p "$FLUXGATE_PID" | xargs || true)"
THREADS="$(ps -o nlwp= -p "$FLUXGATE_PID" | xargs || true)"
TEST_TIME="$(date --iso-8601=seconds)"
KERNEL_INFO="$(uname -srmo)"
CPU_COUNT="$(nproc)"
CPU_MODEL="$(LC_ALL=C lscpu 2>/dev/null | awk -F: '/Model name/ {sub(/^[ \t]+/, "", $2); model=$2} END {print model}')"
AB_VERSION="$($AB -V 2>&1 | sed -n '1p')"

# 先写报告头部，再依次解析每轮 ab 输出追加结果行。
cat >"${RESULT_DIR}/REPORT.md" <<EOF
# FluxGate 压力测试报告

## 测试环境

- 时间：${TEST_TIME}
- 内核：${KERNEL_INFO}
- CPU：${CPU_COUNT} 个逻辑处理器，${CPU_MODEL:-unknown}
- FluxGate 工作线程：4
- FluxGate RSS：${RSS_KB:-unknown} KiB
- FluxGate 线程数：${THREADS:-unknown}
- 压测工具：${AB_VERSION}
- 后端实现：${BACKEND_DRIVER}
- 主要测试模型：HTTP/1.1 keep-alive，每个并发客户端复用一条代理连接
- 补充测试模型：${SHORT_REQUESTS} 次 HTTP 短连接，用于评估连接建立与回收开销

## 测试结果

| 场景 | 完成请求 | 失败请求 | 吞吐量 | P50 | P95 | P99 | 传输速率 |
|---|---:|---:|---:|---:|---:|---:|---:|
EOF

append_result_row "直连单后端 keep-alive，c=100" "${RESULT_DIR}/direct-keepalive-c100.txt"
append_result_row "FluxGate keep-alive，c=50" "${RESULT_DIR}/proxy-keepalive-c50.txt"
append_result_row "FluxGate keep-alive，c=100" "${RESULT_DIR}/proxy-keepalive-c100.txt"
append_result_row "FluxGate keep-alive，c=200" "${RESULT_DIR}/proxy-keepalive-c200.txt"
append_result_row "FluxGate 短连接，c=100" "${RESULT_DIR}/proxy-short-c100.txt"
append_result_row "app-1 宕机后 keep-alive，c=100" "${RESULT_DIR}/failover-keepalive-c100.txt"

DIRECT_RPS="$(extract_value "${RESULT_DIR}/direct-keepalive-c100.txt" "Requests per second:" | awk '{print $1}')"
PROXY_RPS="$(extract_value "${RESULT_DIR}/proxy-keepalive-c100.txt" "Requests per second:" | awk '{print $1}')"
THROUGHPUT_DIFF="$(awk -v d="$DIRECT_RPS" -v p="$PROXY_RPS" 'BEGIN {if (d > 0) printf "%+.2f", (p-d)*100/d; else print "N/A"}')"
FAILOVER_FAILED="$(extract_value "${RESULT_DIR}/failover-keepalive-c100.txt" "Failed requests:")"

cat >>"${RESULT_DIR}/REPORT.md" <<EOF

## 结论

- keep-alive、c=100 时，FluxGate 吞吐量为 ${PROXY_RPS} req/s；同一后端直连基线为 ${DIRECT_RPS} req/s。
- 相对单后端直连基线的吞吐差异为 ${THROUGHPUT_DIFF}%；正值表示本轮代理结果更高，负值表示更低。由于代理测试使用两个后端且单轮时间较短，该值只用于本轮对照，不能解释为代理本身提升性能。
- app-1 停止并被健康检查摘除后，剩余节点承载压测的失败请求数为 ${FAILOVER_FAILED}。
- 原始 ApacheBench 输出、FluxGate 日志和后端日志均保存在本目录，便于复核。

> 结果仅代表当前机器、内核、端口配置与本次长短连接模型。写入简历前，应在目标 Linux 主机上重复测试，并标注硬件、后端实现和并发参数。
EOF

echo
echo "stress test passed"
echo "report: ${RESULT_DIR}/REPORT.md"
cat "${RESULT_DIR}/REPORT.md"
