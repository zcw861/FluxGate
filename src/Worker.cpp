//Worker.cpp：实现独立epoll Reactor、跨线程连接投递和会话生命周期管理。
#include "fluxgate/Worker.h"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include "fluxgate/Logger.h"
#include "fluxgate/Net.h"

namespace fluxgate {
namespace {

//为epoll/eventfd 系统调用附加errno描述。
std::runtime_error epollError(const std::string& action) {
    return std::runtime_error(action + ": " + std::strerror(errno));
}

}

//每个Worker 创建独立epoll和eventfd，避免多个线程并发操作同一个事件循环。
Worker::Worker(std::size_t index, const AppConfig& config, BackendPool& backendPool, Metrics& metrics)
    : index_(index),
      config_(config),
      backendPool_(backendPool),
      metrics_(metrics),
      epollFd_(::epoll_create1(EPOLL_CLOEXEC)),
      wakeFd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)),
      running_(false),
      activeSessions_(0),
      nextSessionId_(1) {
    if (!epollFd_.valid()) {
        throw epollError("epoll_create1 failed");
    }
    if (!wakeFd_.valid()) {
        throw epollError("eventfd failed");
    }
    registerFd(wakeFd_.get(), EPOLLIN);
}

Worker::~Worker() {
    stop();
    join();
}

void Worker::start() {
    if (running_.exchange(true)) {
        return;
    }
    thread_ = std::thread(&Worker::run, this);
}

void Worker::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    const std::uint64_t signal = 1;
    const ssize_t ignored = ::write(wakeFd_.get(), &signal, sizeof(signal));
    (void)ignored;
}

void Worker::join() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

//主线程只在这里短暂持锁投递 fd；eventfd用于把阻塞在epoll_wait的Worker唤醒。
bool Worker::enqueue(int clientFd, const std::string& address) {
    if (!running_.load()) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        const std::size_t queueLimit = static_cast<std::size_t>(config_.backlog) * 4U;
        if (queue_.size() >= queueLimit) {
            return false;
        }
        AcceptedClient client = { clientFd, address };
        queue_.push(client);
    }

    const std::uint64_t signal = 1;
    const ssize_t result = ::write(wakeFd_.get(), &signal, sizeof(signal));
    return result == static_cast<ssize_t>(sizeof(signal)) || errno == EAGAIN;
}

//Worker主循环：处理 eventfd、新会话 I/O、延迟关闭和周期超时扫描。
void Worker::run() {
    FG_LOG_INFO("worker=%zu started", index_);
    std::vector<epoll_event> events(config_.maxEvents);
    std::chrono::steady_clock::time_point lastSweep = std::chrono::steady_clock::now();

    while (running_.load()) {
        const int count = ::epoll_wait(epollFd_.get(), &events[0], static_cast<int>(events.size()), 1000);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            FG_LOG_ERROR("worker=%zu epoll_wait failed: %s", index_, std::strerror(errno));
            break;
        }

        for (int i = 0; i < count; ++i) {
            const int fd = events[static_cast<std::size_t>(i)].data.fd;
            if (fd == wakeFd_.get()) {
                std::uint64_t value = 0;
                while (::read(wakeFd_.get(), &value, sizeof(value)) > 0) {}
                drainQueue();
                continue;
            }

            std::unordered_map<int, std::shared_ptr<ProxySession> >::iterator found = fdToSession_.find(fd);
            if (found == fdToSession_.end() || found->second->closing()) {
                continue;
            }

            const std::shared_ptr<ProxySession> session = found->second;
            if (!session->handleEvent(fd, events[static_cast<std::size_t>(i)].events, metrics_)) {
                session->markClosing();
            } else {
                try {
                    updateInterests(session);
                } catch (const std::exception& error) {
                    FG_LOG_WARN("worker=%zu session=%llu epoll update failed: %s",
                                index_, static_cast<unsigned long long>(session->id()), error.what());
                    session->markClosing();
                }
            }
        }

        //先收集closing会话，再统一删除，避免同一epoll批次中的另一个fd事件访问已释放对象。
        std::vector<std::shared_ptr<ProxySession> > closing;
        for (std::unordered_map<std::uint64_t, std::shared_ptr<ProxySession> >::const_iterator it = sessions_.begin();
             it != sessions_.end(); ++it) {
            if (it->second->closing()) {
                closing.push_back(it->second);
            }
        }
        for (std::size_t i = 0; i < closing.size(); ++i) {
            closeSession(closing[i], "connection completed or failed");
        }

        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        if (now - lastSweep >= std::chrono::seconds(1)) {
            sweepIdleSessions();
            lastSweep = now;
        }
    }

    drainQueue();
    while (!queue_.empty()) {
        ::close(queue_.front().fd);
        queue_.pop();
    }

    std::vector<std::shared_ptr<ProxySession> > remaining;
    for (std::unordered_map<std::uint64_t, std::shared_ptr<ProxySession> >::const_iterator it = sessions_.begin();
         it != sessions_.end(); ++it) {
        remaining.push_back(it->second);
    }
    for (std::size_t i = 0; i < remaining.size(); ++i) {
        closeSession(remaining[i], "worker stopping");
    }
    FG_LOG_INFO("worker=%zu stopped", index_);
}

//交换到局部队列后立即释放互斥锁，耗时的后端连接创建不会阻塞Acceptor入队。
void Worker::drainQueue() {
    std::queue<AcceptedClient> local;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        local.swap(queue_);
    }

    while (!local.empty()) {
        AcceptedClient client = local.front();
        local.pop();
        if (!running_.load()) {
            ::close(client.fd);
            continue;
        }
        createSession(client);
    }
}

//会话创建顺序：选后端 -> 非阻塞 connect -> 构造会话 -> 注册两个 fd -> 发布到映射。
void Worker::createSession(const AcceptedClient& client) {
    BackendLease backend = backendPool_.acquire();
    if (!backend.valid()) {
        FG_LOG_WARN("worker=%zu rejected client=%s because no backend is healthy", index_, client.address.c_str());
        metrics_.onRejected();
        ::close(client.fd);
        return;
    }

    ConnectResult result;
    try {
        result = connectNonBlocking(backend->endpoint());
    } catch (const std::exception& error) {
        // 业务连接失败只计入指标，不直接修改后端健康状态。
        // 后端摘除由主动健康检查负责，防止瞬时资源压力导致整个节点被误判为故障。
        backend->recordConnectFailure();
        metrics_.onConnectFailure();
        metrics_.onRejected();
        FG_LOG_WARN("worker=%zu backend=%s connect failed: %s", index_, backend->name().c_str(), error.what());
        ::close(client.fd);
        return;
    }

    const std::uint64_t sessionId = nextSessionId_++;
    std::shared_ptr<ProxySession> session = std::make_shared<ProxySession>(
        sessionId, client.fd, result.fd, result.connecting, std::move(backend), config_.bufferSize, client.address);

    try {
        registerFd(session->clientFd(), session->clientEvents());
        registerFd(session->upstreamFd(), session->upstreamEvents());
    } catch (const std::exception& error) {
        unregisterFd(session->clientFd());
        unregisterFd(session->upstreamFd());
        metrics_.onRejected();
        FG_LOG_ERROR("worker=%zu cannot register session=%llu: %s",
                     index_, static_cast<unsigned long long>(sessionId), error.what());
        return;
    }

    fdToSession_[session->clientFd()] = session;
    fdToSession_[session->upstreamFd()] = session;
    sessions_[sessionId] = session;
    activeSessions_.fetch_add(1);
    metrics_.onOpened();

    FG_LOG_DEBUG("worker=%zu session=%llu client=%s backend=%s(%s)",
                 index_, static_cast<unsigned long long>(sessionId), client.address.c_str(),
                 session->backend().name().c_str(), session->backend().endpoint().text.c_str());
}

void Worker::updateInterests(const std::shared_ptr<ProxySession>& session) {
    modifyFd(session->clientFd(), session->clientEvents());
    modifyFd(session->upstreamFd(), session->upstreamEvents());
}

//先从epoll和索引中移除，再释放 sessions_ 中的shared_ptr所有权。
void Worker::closeSession(const std::shared_ptr<ProxySession>& session, const char* reason) {
    const std::uint64_t id = session->id();
    if (sessions_.find(id) == sessions_.end()) {
        return;
    }

    unregisterFd(session->clientFd());
    unregisterFd(session->upstreamFd());
    fdToSession_.erase(session->clientFd());
    fdToSession_.erase(session->upstreamFd());
    sessions_.erase(id);
    activeSessions_.fetch_sub(1);
    metrics_.onClosed();

    FG_LOG_DEBUG("worker=%zu session=%llu closed: %s", index_, static_cast<unsigned long long>(id), reason);
}

//统一扫描连接建立超时和空闲超时；扫描周期为1秒，避免每个会话单独创建定时器。
void Worker::sweepIdleSessions() {
    std::vector<std::shared_ptr<ProxySession> > expired;
    const std::chrono::seconds idleTimeout(config_.idleTimeoutSeconds);
    const std::chrono::milliseconds connectTimeout(config_.connectTimeoutMs);

    for (std::unordered_map<std::uint64_t, std::shared_ptr<ProxySession> >::const_iterator it = sessions_.begin();
         it != sessions_.end(); ++it) {
        if (it->second->connectTimedOut(connectTimeout)) {
            //连接超时可能是瞬时拥塞，不在数据面直接摘除后端。
            //HealthChecker 会通过独立探测决定节点是否进入UNHEALTHY状态。
            it->second->backend().recordConnectFailure();
            metrics_.onConnectFailure();
            expired.push_back(it->second);
        } else if (it->second->idleFor(idleTimeout)) {
            expired.push_back(it->second);
        }
    }

    for (std::size_t i = 0; i < expired.size(); ++i) {
        closeSession(expired[i], "timeout");
    }
}

void Worker::registerFd(int fd, std::uint32_t events) {
    epoll_event event;
    std::memset(&event, 0, sizeof(event));
    event.data.fd = fd;
    event.events = events;
    if (::epoll_ctl(epollFd_.get(), EPOLL_CTL_ADD, fd, &event) < 0) {
        throw epollError("epoll_ctl(ADD) failed");
    }
}

void Worker::modifyFd(int fd, std::uint32_t events) {
    epoll_event event;
    std::memset(&event, 0, sizeof(event));
    event.data.fd = fd;
    event.events = events;
    if (::epoll_ctl(epollFd_.get(), EPOLL_CTL_MOD, fd, &event) < 0) {
        throw epollError("epoll_ctl(MOD) failed");
    }
}

void Worker::unregisterFd(int fd) {
    if (fd >= 0) {
        ::epoll_ctl(epollFd_.get(), EPOLL_CTL_DEL, fd, NULL);
    }
}

}
