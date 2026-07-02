#ifndef FLUXGATE_NET_H
#define FLUXGATE_NET_H

#include <cstdint>
#include <string>
#include <sys/socket.h>

namespace fluxgate {

/** @brief 已解析的 TCP 地址，可同时容纳 IPv4 和 IPv6 sockaddr。 */
struct ResolvedEndpoint {
    sockaddr_storage address;
    socklen_t length;
    std::string text; // 用于日志显示的 ip:port。
};

/** @brief 非阻塞 connect() 的返回结果。 */
struct ConnectResult {
    int fd;          // 新建的后端 Socket，所有权转交给调用者。
    bool connecting; // true 表示 connect 返回 EINPROGRESS，需等待 EPOLLOUT 完成。
};

void setNonBlocking(int fd); // 设置 O_NONBLOCK，保证系统调用不会阻塞 Worker。
void setCloseOnExec(int fd); // 设置 FD_CLOEXEC，防止未来 exec 时泄漏描述符。
void setTcpNoDelay(int fd);  // 设置 TCP_NODELAY，降低小包交互延迟。

/** @brief 创建、绑定并监听非阻塞 TCP Socket，返回值由调用者负责关闭。 */
int createListenSocket(const std::string& host, std::uint16_t port, int backlog);

/** @brief 使用 getaddrinfo 将主机名和端口解析为 sockaddr。 */
ResolvedEndpoint resolveTcpEndpoint(const std::string& host, std::uint16_t port);

/** @brief 发起非阻塞后端连接；立即成功或 EINPROGRESS 都视为正常结果。 */
ConnectResult connectNonBlocking(const ResolvedEndpoint& endpoint);

/** @brief 健康检查专用：在 timeoutMs 内等待非阻塞连接完成。 */
bool waitForConnect(const ResolvedEndpoint& endpoint, int timeoutMs);

/** @brief 将 accept 得到的客户端 sockaddr 转换为 ip:port 文本。 */
std::string peerAddress(const sockaddr_storage& address);

}

#endif
