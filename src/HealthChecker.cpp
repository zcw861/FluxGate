//HealthChecker.cpp：在独立线程中周期探测后端，并通过连续成功/失败阈值更新健康状态。
#include "fluxgate/HealthChecker.h"

#include <chrono>
#include "fluxgate/Logger.h"
#include "fluxgate/Net.h"

namespace fluxgate {

HealthChecker::HealthChecker(BackendPool& pool,
                             int intervalMs,
                             int timeoutMs,
                             std::size_t failureThreshold,
                             std::size_t successThreshold)
    : pool_(pool),
      intervalMs_(intervalMs),
      timeoutMs_(timeoutMs),
      failureThreshold_(failureThreshold),
      successThreshold_(successThreshold),
      running_(false) {}

HealthChecker::~HealthChecker() {
    stop();
    join();
}

//exchange(true) 使重复start()具有幂等性。
void HealthChecker::start() {
    if (running_.exchange(true)) {
        return;
    }
    thread_ = std::thread(&HealthChecker::run, this);
}

// notify_all 让 wait_for 不必等完整个 intervalMs_ 才能退出。
void HealthChecker::stop() {
    running_.store(false);
    condition_.notify_all();
}

void HealthChecker::join() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

//启动后立即检查一次；之后使用可中断条件变量等待下一周期。
void HealthChecker::run() {
    checkOnce();
    std::unique_lock<std::mutex> lock(mutex_);
    while (running_.load()) {
        condition_.wait_for(lock, std::chrono::milliseconds(intervalMs_), [this]() { return !running_.load(); });
        if (!running_.load()) {
            break;
        }
        lock.unlock();
        checkOnce();
        lock.lock();
    }
}

//每个后端一次独立 TCP 探测。状态只有跨过阈值时才改变，以降低抖动。
void HealthChecker::checkOnce() {
    const std::vector<std::shared_ptr<Backend> >& backends = pool_.all();
    for (std::size_t i = 0; i < backends.size(); ++i) {
        const std::shared_ptr<Backend>& backend = backends[i];
        ProbeState& state = probeStates_[backend.get()];
        const bool reachable = waitForConnect(backend->endpoint(), timeoutMs_);
        const bool previous = backend->healthy();

        if (reachable) {
            state.consecutiveFailures = 0;
            ++state.consecutiveSuccesses;
            if (!previous && state.consecutiveSuccesses >= successThreshold_) {
                backend->setHealthy(true);
            }
        } else {
            state.consecutiveSuccesses = 0;
            ++state.consecutiveFailures;
            if (previous && state.consecutiveFailures >= failureThreshold_) {
                backend->setHealthy(false);
            }
        }

        const bool current = backend->healthy();
        if (current != previous) {
            FG_LOG_WARN("backend %s (%s) changed state to %s after %zu consecutive %s probe(s)",
                        backend->name().c_str(),
                        backend->endpoint().text.c_str(),
                        current ? "HEALTHY" : "UNHEALTHY",
                        current ? state.consecutiveSuccesses : state.consecutiveFailures,
                        current ? "successful" : "failed");
        }
    }
}

}
