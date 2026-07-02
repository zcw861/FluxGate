#!/usr/bin/env bash
# 一键功能联调：启动两个演示后端和 FluxGate，验证分流、健康检查和故障接管。
set -euo pipefail

# 项目根目录和可覆盖的测试端口，默认使用高位端口避免与开发服务冲突。
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BINARY="${1:-$ROOT_DIR/build/fluxgate}"
LISTEN_PORT="${FLUXGATE_SMOKE_PORT:-28080}"
BACKEND1_PORT="${FLUXGATE_SMOKE_BACKEND1_PORT:-29001}"
BACKEND2_PORT="${FLUXGATE_SMOKE_BACKEND2_PORT:-29002}"
TEMP_CONFIG="$(mktemp)"

# 无论正常退出还是测试失败，都终止子进程并删除临时配置。
cleanup() {
    kill "${GATEWAY_PID:-}" "${BACKEND1_PID:-}" "${BACKEND2_PID:-}" 2>/dev/null || true
    rm -f "$TEMP_CONFIG"
}
trap cleanup EXIT

# 生成独立测试配置，不修改仓库中的默认配置文件。
cat >"$TEMP_CONFIG" <<CONFIG
[server]
listen_host = 127.0.0.1
listen_port = $LISTEN_PORT
workers = 2
backlog = 128
max_events = 256
buffer_size = 65536
connect_timeout_ms = 1000
idle_timeout_seconds = 10
stats_interval_seconds = 0
log_level = warn

[health_check]
interval_ms = 1000
timeout_ms = 200
failure_threshold = 2
success_threshold = 1

[backend app-1]
host = 127.0.0.1
port = $BACKEND1_PORT
weight = 1

[backend app-2]
host = 127.0.0.1
port = $BACKEND2_PORT
weight = 2
CONFIG

# 启动两个带节点标识的本地 HTTP 后端。
python3 -u "$ROOT_DIR/tools/demo_upstream.py" --port "$BACKEND1_PORT" --name app-1 >/tmp/fluxgate-app1.log 2>&1 &
BACKEND1_PID=$!
python3 -u "$ROOT_DIR/tools/demo_upstream.py" --port "$BACKEND2_PORT" --name app-2 >/tmp/fluxgate-app2.log 2>&1 &
BACKEND2_PID=$!
# 启动被测 FluxGate，并将日志写入 /tmp 供失败时诊断。
"$BINARY" -c "$TEMP_CONFIG" >/tmp/fluxgate.log 2>&1 &
GATEWAY_PID=$!

# 轮询网关直到首次请求成功，避免固定 sleep 带来的机器差异。
response=""
for _ in $(seq 1 30); do
    if response="$(curl --fail --silent --show-error --max-time 1 "http://127.0.0.1:$LISTEN_PORT/demo" 2>/dev/null)"; then
        echo "$response"
        break
    fi
    sleep 0.5
done

if [[ -z "$response" ]]; then
    echo "gateway did not become ready" >&2
    cat /tmp/fluxgate.log >&2 || true
    exit 1
fi

# 连续请求用于观察两个后端均能被调度。
for _ in $(seq 1 5); do
    curl --fail --silent --show-error "http://127.0.0.1:$LISTEN_PORT/demo"
    echo
done

# 模拟一个后端宕机，验证健康检查会摘除故障节点并继续转发。
kill "$BACKEND1_PID" 2>/dev/null || true
wait "$BACKEND1_PID" 2>/dev/null || true
BACKEND1_PID=""
# 轮询故障转移结果，直到健康检查摘除 app-1 并由 app-2 返回响应。
failover_response=""
for _ in $(seq 1 20); do
    if failover_response="$(curl --fail --silent --show-error --max-time 1 "http://127.0.0.1:$LISTEN_PORT/failover" 2>/dev/null)"; then
        if [[ "$failover_response" == *'"backend": "app-2"'* ]]; then
            echo "$failover_response"
            break
        fi
    fi
    sleep 0.5
done

if [[ "$failover_response" != *'"backend": "app-2"'* ]]; then
    echo "backend failover test failed" >&2
    cat /tmp/fluxgate.log >&2 || true
    exit 1
fi

echo "smoke test passed"
