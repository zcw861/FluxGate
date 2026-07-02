#ifndef FLUXGATE_WORKER_H
#define FLUXGATE_WORKER_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "fluxgate/BackendPool.h"
#include "fluxgate/Config.h"
#include "fluxgate/Metrics.h"
#include "fluxgate/NonCopyable.h"
#include "fluxgate/ProxySession.h"
#include "fluxgate/UniqueFd.h"

namespace fluxgate {

/** @brief Acceptor 交给 Worker 的新客户端连接。fd 所有权随入队操作转移。 */
struct AcceptedClient {
    int fd;
    std::string address;
};

/**
 * @brief 一个独立的 Worker Reactor。
 *
 * 每个 Worker 拥有一个线程、一个 epoll、一个 eventfd 和自己的全部 ProxySession。
 * 主线程通过线程安全队列投递新连接，再写 eventfd 唤醒 Worker。会话建立后只在该 Worker
 * 中处理，因此数据转发热路径无需为会话状态加锁。
 */
class Worker : private NonCopyable {
public:
    Worker(std::size_t index, const AppConfig& config, BackendPool& backendPool, Metrics& metrics);
    ~Worker();

    void start(); //启动事件循环线程。
    void stop();  //设置停止标志并写eventfd立即唤醒线程。
    void join();  //等待线程完整清理队列和活动会话后退出。

    /**
     * @brief 将客户端连接交给 Worker。
     * @return true 表示 Worker 接管 fd；false 表示调用者仍需关闭 fd。
     */
    bool enqueue(int clientFd, const std::string& address);

    /** @brief 返回当前活动会话数，供 Acceptor 选择负载较低的 Worker。 */
    std::size_t activeSessions() const { return activeSessions_.load(); }

private:
    void run(); //epoll事件循环和退出清理流程。
    void drainQueue(); //把跨线程队列快速交换到本地，然后逐个创建会话。
    void createSession(const AcceptedClient& client); //选择后端、发起连接并注册两个Socket。
    void updateInterests(const std::shared_ptr<ProxySession>& session); //根据会话状态修改epoll兴趣集合。
    void closeSession(const std::shared_ptr<ProxySession>& session, const char* reason); //延迟统一删除会话。
    void sweepIdleSessions(); //每秒检查连接建立超时和会话空闲超时。
    void registerFd(int fd, std::uint32_t events);
    void modifyFd(int fd, std::uint32_t events);
    void unregisterFd(int fd);

    std::size_t index_;                  //Worker 编号，用于日志定位。
    AppConfig config_;                   //Worker 使用的配置快照，运行期间不修改。
    BackendPool& backendPool_;           //全局后端池，仅新建会话时执行短临界区选择。
    Metrics& metrics_;                   //全局原子指标。
    UniqueFd epollFd_;                   //本 Worker 独占的 epoll 实例。
    UniqueFd wakeFd_;                    //主线程通知新连接或停止事件的 eventfd。
    std::atomic<bool> running_;
    std::thread thread_;
    std::mutex queueMutex_;              //只保护跨线程的新连接队列。
    std::queue<AcceptedClient> queue_;
    std::unordered_map<int, std::shared_ptr<ProxySession> > fdToSession_; // 根据 epoll 返回 fd 定位会话。
    std::unordered_map<std::uint64_t, std::shared_ptr<ProxySession> > sessions_; // 会话所有权和遍历入口。
    std::atomic<std::size_t> activeSessions_;
    std::uint64_t nextSessionId_;        //仅由 Worker 线程访问，无需原子变量。
};

}

#endif
