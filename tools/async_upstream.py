#!/usr/bin/env python3
"""FluxGate 压力测试使用的轻量异步 HTTP/1.1 后端。

该服务支持 keep-alive，并在单个 asyncio 事件循环中处理大量连接。它用于缺少 Nginx 时的
备用测试后端；正式吞吐对比仍建议使用 Nginx，避免 Python 运行时先成为瓶颈。
"""

import argparse
import asyncio
import json
import signal
from typing import Dict


async def handle_client(
    reader: asyncio.StreamReader,
    writer: asyncio.StreamWriter,
    node_name: str,
) -> None:
    """在一条连接中循环读取 HTTP 请求并按 Connection 语义决定是否复用。"""
    try:
        while True:
            try:
                # 只需要解析请求行和请求头，因此读取到空行即可。
                request = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), timeout=30.0)
            except (asyncio.IncompleteReadError, asyncio.LimitOverrunError, asyncio.TimeoutError):
                break

            lines = request.split(b"\r\n")
            first_line = lines[0].decode("ascii", errors="replace")
            parts = first_line.split(" ")
            path = parts[1] if len(parts) >= 2 else "/"
            version = parts[2] if len(parts) >= 3 else "HTTP/1.0"

            headers: Dict[bytes, bytes] = {}
            for raw_line in lines[1:]:
                if not raw_line or b":" not in raw_line:
                    continue
                key, value = raw_line.split(b":", 1)
                headers[key.strip().lower()] = value.strip().lower()

            connection = headers.get(b"connection", b"")
            should_close = connection == b"close" or (version != "HTTP/1.1" and connection != b"keep-alive")
            body = json.dumps({"backend": node_name, "path": path}, separators=(",", ":")).encode("utf-8")
            connection_header = b"close" if should_close else b"keep-alive"

            response = (
                b"HTTP/1.1 200 OK\r\n"
                b"Content-Type: application/json\r\n"
                + f"Content-Length: {len(body)}\r\n".encode("ascii")
                + b"Connection: " + connection_header + b"\r\n"
                + b"\r\n"
                + body
            )
            writer.write(response)
            await writer.drain()
            if should_close:
                break
    except (ConnectionError, BrokenPipeError):
        # 压测客户端主动结束连接属于常见收尾路径，无需输出堆栈。
        pass
    finally:
        writer.close()
        try:
            await writer.wait_closed()
        except ConnectionError:
            pass


async def run_server(host: str, port: int, node_name: str) -> None:
    """启动 asyncio TCP 服务，并在 SIGINT/SIGTERM 到来时停止接受新连接。"""
    server = await asyncio.start_server(
        lambda reader, writer: handle_client(reader, writer, node_name),
        host,
        port,
        backlog=4096,
    )
    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()

    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, stop_event.set)
        except NotImplementedError:
            # 某些非 Unix 事件循环不支持信号处理；FluxGate 本身仍限定 Linux。
            pass

    sockets = server.sockets or []
    addresses = ", ".join(str(sock.getsockname()) for sock in sockets)
    print(f"{node_name} listening on {addresses}", flush=True)

    async with server:
        await stop_event.wait()


def main() -> None:
    """解析命令行并启动异步后端。"""
    parser = argparse.ArgumentParser(description="FluxGate asynchronous benchmark upstream")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--name", required=True)
    args = parser.parse_args()
    asyncio.run(run_server(args.host, args.port, args.name))


if __name__ == "__main__":
    main()
