#ifndef FLUXGATE_HEALTHCHECKER_H
#define FLUXGATE_HEALTHCHECKER_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>
#include <unordered_map>
#include "fluxgate/BackendPool.h"
#include "fluxgate/NonCopyable.h"

namespace fluxgate {

/**
 * @brief 在独立线程中执行后端 TCP 主动健康检查。
 *
 * 为避免一次瞬时失败导致节点反复上下线，健康状态带有迟滞：
 * 连续失败达到 failureThreshold_ 才摘除，连续成功达到 successThreshold_ 才恢复。
 */
class HealthChecker : private NonCopyable {
public:
    HealthChecker(BackendPool& pool,
                  int intervalMs,
                  int timeoutMs,
                  std::size_t failureThreshold,
                  std::size_t successThreshold);
    ~HealthChecker();

    /** @brief 启动健康检查线程；重复调用不会创建多个线程。 */
    void start();

    /** @brief 请求线程停止，并唤醒正在等待下次探测的条件变量。 */
    void stop();

    /** @brief 等待健康检查线程退出。 */
    void join();

private:
    /** @brief 保存每个节点当前连续成功和连续失败次数。 */
    struct ProbeState {
        std::size_t consecutiveFailures;
        std::size_t consecutiveSuccesses;

        ProbeState() : consecutiveFailures(0), consecutiveSuccesses(0) {}
    };

    void run();       //线程主循环：探测一次，然后按配置间隔等待。
    void checkOnce(); //对全部后端执行一轮带超时的 TCP Connect 探测。

    BackendPool& pool_;
    int intervalMs_;
    int timeoutMs_;
    std::size_t failureThreshold_;
    std::size_t successThreshold_;
    std::unordered_map<const Backend*, ProbeState> probeStates_; //Backend 地址在进程生命周期内稳定。
    std::atomic<bool> running_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable condition_; //用于可中断等待，保证 stop() 能快速退出。
};

}

#endif
