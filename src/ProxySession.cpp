//ProxySession.cpp：实现客户端与后端之间的非阻塞双向转发状态机。
#include "fluxgate/ProxySession.h"

#include <cerrno>
#include <cstring>
#include <sys/epoll.h>
#include <sys/socket.h>
#include "fluxgate/Logger.h"

namespace fluxgate {

//接管两个Socket和后端租约，并为双向转发各创建一个固定容量缓冲区。
ProxySession::ProxySession(std::uint64_t id,
                           int clientFd,
                           int upstreamFd,
                           bool upstreamConnecting,
                           BackendLease backend,
                           std::size_t bufferSize,
                           const std::string& clientAddress)
    : id_(id),
      clientFd_(clientFd),
      upstreamFd_(upstreamFd),
      upstreamConnecting_(upstreamConnecting),
      backend_(std::move(backend)),
      clientToUpstream_(bufferSize),
      upstreamToClient_(bufferSize),
      clientReadClosed_(false),
      upstreamReadClosed_(false),
      clientWriteShutdown_(false),
      upstreamWriteShutdown_(false),
      closing_(false),
      clientAddress_(clientAddress),
      createdAt_(std::chrono::steady_clock::now()),
      lastActivity_(createdAt_) {
    if (!upstreamConnecting_) {
        backend_->recordConnectSuccess();
    }
}

ProxySession::~ProxySession() {}

//一个入口统一处理客户端和后端事件。返回 false 时不立即析构，由 Worker 批次结束后延迟删除。
bool ProxySession::handleEvent(int fd, std::uint32_t events, Metrics& metrics) {
    if (closing_) {
        return false;
    }

    //非阻塞connect完成时，EPOLLOUT只表示“结果可读取”，仍需SO_ERROR判断成功或失败。
    if (fd == upstreamFd_.get() && upstreamConnecting_) {
        if ((events & (EPOLLOUT | EPOLLERR | EPOLLHUP)) == 0U) {
            return true;
        }
        if (!finishUpstreamConnect(metrics)) {
            closing_ = true;
            return false;
        }
    }

    //EPOLLERR必须通过SO_ERROR获取真实Socket错误码。
    if ((events & EPOLLERR) != 0U) {
        int error = 0;
        socklen_t length = sizeof(error);
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length);
        //ECONNRESET/EPIPE常见于压测工具到达时限后主动终止 keep-alive 连接，
        //仍然关闭会话，但降为DEBUG，避免正常收尾淹没真正的服务端告警。
        if (error == ECONNRESET || error == EPIPE) {
            FG_LOG_DEBUG("session=%llu socket=%d peer closed connection: %s",
                         static_cast<unsigned long long>(id_), fd, std::strerror(error));
        } else {
            FG_LOG_WARN("session=%llu socket=%d error=%s",
                        static_cast<unsigned long long>(id_), fd,
                        error == 0 ? "unknown" : std::strerror(error));
        }
        closing_ = true;
        return false;
    }

    const bool peerHangup = (events & (EPOLLRDHUP | EPOLLHUP)) != 0U;

    if (fd == clientFd_.get()) {
        if (!clientReadClosed_ && (events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) != 0U) {
            const IoResult result = readInto(clientFd_.get(), clientToUpstream_, metrics, true);
            if (result == IoResult::Error) {
                closing_ = true;
                return false;
            }
            if (result == IoResult::Closed || (peerHangup && result == IoResult::WouldBlock)) {
                clientReadClosed_ = true;
            }
        }
        if ((events & EPOLLOUT) != 0U && !upstreamToClient_.empty()) {
            const IoResult result = writeFrom(clientFd_.get(), upstreamToClient_);
            if (result == IoResult::Error || result == IoResult::Closed) {
                closing_ = true;
                return false;
            }
        }
    } else if (fd == upstreamFd_.get()) {
        if (!upstreamReadClosed_ && (events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) != 0U) {
            const IoResult result = readInto(upstreamFd_.get(), upstreamToClient_, metrics, false);
            if (result == IoResult::Error) {
                closing_ = true;
                return false;
            }
            if (result == IoResult::Closed || (peerHangup && result == IoResult::WouldBlock)) {
                upstreamReadClosed_ = true;
            }
        }
        if ((events & EPOLLOUT) != 0U && !clientToUpstream_.empty()) {
            const IoResult result = writeFrom(upstreamFd_.get(), clientToUpstream_);
            if (result == IoResult::Error || result == IoResult::Closed) {
                closing_ = true;
                return false;
            }
        }
    } else {
        closing_ = true;
        return false;
    }

    updateHalfClose();
    if (completed()) {
        closing_ = true;
        return false;
    }
    return true;
}

//只有来源缓冲区还有空间时才继续读客户端；有响应待发送时才关注EPOLLOUT。
std::uint32_t ProxySession::clientEvents() const {
    std::uint32_t events = EPOLLRDHUP;
    if (!clientReadClosed_ && clientToUpstream_.writableBytes() > 0U) {
        events |= EPOLLIN;
    }
    if (!upstreamToClient_.empty()) {
        events |= EPOLLOUT;
    }
    return events;
}

//连接阶段只等待EPOLLOUT；连接完成后按双向缓冲区状态动态启停读写事件。
std::uint32_t ProxySession::upstreamEvents() const {
    std::uint32_t events = EPOLLRDHUP;
    if (upstreamConnecting_) {
        events |= EPOLLOUT;
        return events;
    }
    if (!upstreamReadClosed_ && upstreamToClient_.writableBytes() > 0U) {
        events |= EPOLLIN;
    }
    if (!clientToUpstream_.empty()) {
        events |= EPOLLOUT;
    }
    return events;
}

bool ProxySession::idleFor(std::chrono::seconds timeout) const {
    return std::chrono::steady_clock::now() - lastActivity_ >= timeout;
}

bool ProxySession::connectTimedOut(std::chrono::milliseconds timeout) const {
    return upstreamConnecting_ && std::chrono::steady_clock::now() - createdAt_ >= timeout;
}

bool ProxySession::finishUpstreamConnect(Metrics& metrics) {
    int error = 0;
    socklen_t length = sizeof(error);
    if (::getsockopt(upstreamFd_.get(), SOL_SOCKET, SO_ERROR, &error, &length) < 0 || error != 0) {
        //单次业务连接失败可能来自临时端口耗尽、accept队列抖动或瞬时超时。
        //数据面只记录失败，不直接摘除节点；节点健康状态统一由HealthChecker的独立探测维护，
        //避免高并发下一个偶发错误触发级联摘除并放大故障。
        backend_->recordConnectFailure();
        metrics.onConnectFailure();
        FG_LOG_WARN("session=%llu failed to connect backend=%s error=%s",
                    static_cast<unsigned long long>(id_), backend_->name().c_str(),
                    error == 0 ? std::strerror(errno) : std::strerror(error));
        return false;
    }

    upstreamConnecting_ = false;
    backend_->recordConnectSuccess();
    touch();
    FG_LOG_DEBUG("session=%llu connected backend=%s",
                 static_cast<unsigned long long>(id_), backend_->name().c_str());
    return true;
}

//循环recv直到内核无更多数据、缓冲区满、EOF或错误，适配epoll水平触发模式。
ProxySession::IoResult ProxySession::readInto(int fd, ByteBuffer& buffer, Metrics& metrics, bool clientSide) {
    bool progressed = false;
    for (;;) {
        buffer.makeWritable();
        if (buffer.writableBytes() == 0U) {
            return progressed ? IoResult::Progress : IoResult::WouldBlock;
        }

        const ssize_t count = ::recv(fd, buffer.writeData(), buffer.writableBytes(), 0);
        if (count > 0) {
            const std::size_t bytes = static_cast<std::size_t>(count);
            buffer.hasWritten(bytes);
            if (clientSide) {
                metrics.addClientToBackend(bytes);
            } else {
                metrics.addBackendToClient(bytes);
            }
            progressed = true;
            touch();
            continue;
        }
        if (count == 0) {
            return IoResult::Closed;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return progressed ? IoResult::Progress : IoResult::WouldBlock;
        }
        return IoResult::Error;
    }
}

//循环send并处理短写；MSG_NOSIGNAL防止对已关闭连接写入时触发SIGPIPE。
ProxySession::IoResult ProxySession::writeFrom(int fd, ByteBuffer& buffer) {
    bool progressed = false;
    while (!buffer.empty()) {
        const ssize_t count = ::send(fd, buffer.readData(), buffer.readableBytes(), MSG_NOSIGNAL);
        if (count > 0) {
            buffer.consume(static_cast<std::size_t>(count));
            progressed = true;
            touch();
            continue;
        }
        if (count == 0) {
            return IoResult::Closed;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return progressed ? IoResult::Progress : IoResult::WouldBlock;
        }
        return IoResult::Error;
    }
    return IoResult::Progress;
}

//一侧读EOF后先排空该方向缓存，再关闭目标侧写方向，保证已接收数据不丢失。
void ProxySession::updateHalfClose() {
    if (clientReadClosed_ && clientToUpstream_.empty() && !upstreamWriteShutdown_ && !upstreamConnecting_) {
        ::shutdown(upstreamFd_.get(), SHUT_WR);
        upstreamWriteShutdown_ = true;
    }
    if (upstreamReadClosed_ && upstreamToClient_.empty() && !clientWriteShutdown_) {
        ::shutdown(clientFd_.get(), SHUT_WR);
        clientWriteShutdown_ = true;
    }
}

bool ProxySession::completed() const {
    //后端已经结束响应且待发送数据已清空时，继续保留客户端连接已无意义。
    return upstreamReadClosed_ && upstreamToClient_.empty();
}

void ProxySession::touch() {
    lastActivity_ = std::chrono::steady_clock::now();
}

}
