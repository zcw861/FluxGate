#!/usr/bin/env python3
"""FluxGate 冒烟测试使用的简单 HTTP 后端。

该服务每个请求返回自身节点名和请求路径，便于观察负载均衡结果。
它使用 ThreadingHTTPServer 并主动关闭连接，目标是功能联调而不是性能测试。
"""

import argparse
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Handler(BaseHTTPRequestHandler):
    """处理 GET 请求，并返回当前后端节点身份。"""

    server_version = "FluxGateDemo/1.0"

    def do_GET(self) -> None:
        """生成包含 backend 与 path 的 JSON 响应。"""
        body = json.dumps(
            {"backend": self.server.node_name, "path": self.path},
            ensure_ascii=False,
        ).encode("utf-8")

        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt: str, *args: object) -> None:
        """在默认访问日志前加入节点名，便于区分两个测试后端。"""
        print(f"[{self.server.node_name}] {self.address_string()} - {fmt % args}")


def main() -> None:
    """解析节点参数并持续运行本地 HTTP 服务。"""
    parser = argparse.ArgumentParser(description="FluxGate smoke-test upstream")
    parser.add_argument("--port", type=int, required=True, help="listen port")
    parser.add_argument("--name", required=True, help="backend name returned in JSON")
    args = parser.parse_args()

    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    server.node_name = args.name
    print(f"{args.name} listening on 127.0.0.1:{args.port}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
