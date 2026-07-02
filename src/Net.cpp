//Net.cpp：封装地址解析、Socket 属性、监听创建、非阻塞连接和健康探测。
#include "fluxgate/Net.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdexcept>
#include <unistd.h>
#include "fluxgate/UniqueFd.h"

namespace fluxgate {
namespace {

//将当前 errno 转换为包含上下文的异常信息。
std::runtime_error systemError(const std::string& action) {
    return std::runtime_error(action + ": " + std::strerror(errno));
}

//允许服务重启后快速重新绑定仍处于 TIME_WAIT 影响下的监听地址。
void setReuseAddress(int fd) {
    const int enabled = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) < 0) {
        throw systemError("setsockopt(SO_REUSEADDR) failed");
    }
}

}

void setNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        throw systemError("fcntl(O_NONBLOCK) failed");
    }
}

void setCloseOnExec(int fd) {
    const int flags = ::fcntl(fd, F_GETFD, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
        throw systemError("fcntl(FD_CLOEXEC) failed");
    }
}

void setTcpNoDelay(int fd) {
    const int enabled = 1;
    if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) < 0) {
        throw systemError("setsockopt(TCP_NODELAY) failed");
    }
}

//使用AF_UNSPEC同时支持IPv4与IPv6，并缓存第一个可用地址。
ResolvedEndpoint resolveTcpEndpoint(const std::string& host, std::uint16_t port) {
    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result = NULL;
    const std::string service = std::to_string(port);
    const int status = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &result);
    if (status != 0) {
        throw std::runtime_error("getaddrinfo failed for " + host + ": " + gai_strerror(status));
    }

    ResolvedEndpoint endpoint;
    std::memset(&endpoint.address, 0, sizeof(endpoint.address));
    endpoint.length = 0;

    for (addrinfo* item = result; item != NULL; item = item->ai_next) {
        if (item->ai_addrlen <= sizeof(endpoint.address)) {
            std::memcpy(&endpoint.address, item->ai_addr, item->ai_addrlen);
            endpoint.length = static_cast<socklen_t>(item->ai_addrlen);
            break;
        }
    }
    ::freeaddrinfo(result);

    if (endpoint.length == 0) {
        throw std::runtime_error("no usable address for " + host);
    }

    char addressBuffer[INET6_ADDRSTRLEN];
    std::memset(addressBuffer, 0, sizeof(addressBuffer));
    if (endpoint.address.ss_family == AF_INET) {
        const sockaddr_in* address = reinterpret_cast<const sockaddr_in*>(&endpoint.address);
        ::inet_ntop(AF_INET, &address->sin_addr, addressBuffer, sizeof(addressBuffer));
    } else {
        const sockaddr_in6* address = reinterpret_cast<const sockaddr_in6*>(&endpoint.address);
        ::inet_ntop(AF_INET6, &address->sin6_addr, addressBuffer, sizeof(addressBuffer));
    }
    endpoint.text = std::string(addressBuffer) + ":" + service;
    return endpoint;
}

//UniqueFd负责中途失败时关闭 Socket；成功后 release() 把所有权交给调用者。
int createListenSocket(const std::string& host, std::uint16_t port, int backlog) {
    const ResolvedEndpoint endpoint = resolveTcpEndpoint(host, port);
    UniqueFd socketFd(::socket(endpoint.address.ss_family, SOCK_STREAM, IPPROTO_TCP));
    if (!socketFd.valid()) {
        throw systemError("socket failed");
    }

    setCloseOnExec(socketFd.get());
    setNonBlocking(socketFd.get());
    setReuseAddress(socketFd.get());

    if (::bind(socketFd.get(), reinterpret_cast<const sockaddr*>(&endpoint.address), endpoint.length) < 0) {
        throw systemError("bind failed");
    }
    if (::listen(socketFd.get(), backlog) < 0) {
        throw systemError("listen failed");
    }
    return socketFd.release();
}

//EINPROGRESS不是错误，表示连接将在后续EPOLLOUT事件中完成。
ConnectResult connectNonBlocking(const ResolvedEndpoint& endpoint) {
    UniqueFd socketFd(::socket(endpoint.address.ss_family, SOCK_STREAM, IPPROTO_TCP));
    if (!socketFd.valid()) {
        throw systemError("upstream socket failed");
    }

    setCloseOnExec(socketFd.get());
    setNonBlocking(socketFd.get());
    setTcpNoDelay(socketFd.get());

    const int status = ::connect(socketFd.get(), reinterpret_cast<const sockaddr*>(&endpoint.address), endpoint.length);
    if (status == 0) {
        ConnectResult result = { socketFd.release(), false };
        return result;
    }
    if (errno == EINPROGRESS) {
        ConnectResult result = { socketFd.release(), true };
        return result;
    }
    throw systemError("connect to " + endpoint.text + " failed");
}

//健康检查不进入Worker epoll，使用poll + SO_ERROR在限定时间内确认连通性。
bool waitForConnect(const ResolvedEndpoint& endpoint, int timeoutMs) {
    try {
        ConnectResult result = connectNonBlocking(endpoint);
        UniqueFd socketFd(result.fd);
        if (!result.connecting) {
            return true;
        }

        pollfd descriptor;
        descriptor.fd = socketFd.get();
        descriptor.events = POLLOUT;
        descriptor.revents = 0;

        int pollResult = 0;
        do {
            pollResult = ::poll(&descriptor, 1, timeoutMs);
        } while (pollResult < 0 && errno == EINTR);

        if (pollResult <= 0) {
            return false;
        }

        int error = 0;
        socklen_t length = sizeof(error);
        return ::getsockopt(socketFd.get(), SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0;
    } catch (const std::exception&) {
        return false;
    }
}

std::string peerAddress(const sockaddr_storage& address) {
    char buffer[INET6_ADDRSTRLEN];
    std::memset(buffer, 0, sizeof(buffer));
    std::uint16_t port = 0;

    if (address.ss_family == AF_INET) {
        const sockaddr_in* value = reinterpret_cast<const sockaddr_in*>(&address);
        ::inet_ntop(AF_INET, &value->sin_addr, buffer, sizeof(buffer));
        port = ntohs(value->sin_port);
    } else if (address.ss_family == AF_INET6) {
        const sockaddr_in6* value = reinterpret_cast<const sockaddr_in6*>(&address);
        ::inet_ntop(AF_INET6, &value->sin6_addr, buffer, sizeof(buffer));
        port = ntohs(value->sin6_port);
    } else {
        return "unknown";
    }
    return std::string(buffer) + ":" + std::to_string(port);
}

}
