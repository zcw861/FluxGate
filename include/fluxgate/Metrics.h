#ifndef FLUXGATE_METRICS_H
#define FLUXGATE_METRICS_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace fluxgate {

/**
 * @brief 保存跨线程共享的轻量运行指标。
 *
 * 指标只使用原子加减，不在 I/O 热路径中引入互斥锁；snapshot() 生成适合日志输出的文本快照。
 */
class Metrics {
public:
    Metrics();

    void onAccepted();                          //Acceptor 成功 accept 一个客户端。
    void onOpened();                            //Worker 成功建立并注册一个代理会话。
    void onClosed();                            //一个代理会话完成或失败后被清理。
    void onRejected();                          //无健康后端、队列满或创建失败导致拒绝。
    void onConnectFailure();                    //业务连接后端失败或超时。
    void addClientToBackend(std::size_t bytes); //累计客户端到后端方向读取的字节。
    void addBackendToClient(std::size_t bytes); //累计后端到客户端方向读取的字节。

    /** @brief 返回当前全部指标的单行文本。 */
    std::string snapshot() const;

private:
    std::atomic<std::uint64_t> accepted_;
    std::atomic<std::uint64_t> active_;
    std::atomic<std::uint64_t> rejected_;
    std::atomic<std::uint64_t> connectFailures_;
    std::atomic<std::uint64_t> clientToBackendBytes_;
    std::atomic<std::uint64_t> backendToClientBytes_;
};

}

#endif
