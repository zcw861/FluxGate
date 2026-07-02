#ifndef FLUXGATE_BACKEND_H
#define FLUXGATE_BACKEND_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include "fluxgate/Config.h"
#include "fluxgate/Net.h"

namespace fluxgate {

/**
 * @brief 表示一个可被负载均衡器选择的后端服务节点。
 *
 * Backend 保存节点的静态配置、解析后的网络地址以及运行期统计信息。
 * 健康检查线程会更新 healthy_，多个 Worker 会并发读取连接计数，因此运行期字段使用原子变量。
 */
class Backend {
public:
    /**
     * @brief 根据配置创建后端，并在启动阶段解析主机名和端口。
     * @throws std::runtime_error 当地址无法解析时抛出异常，使配置错误在启动阶段暴露。
     */
    explicit Backend(const BackendConfig& config);

    // 以下访问器返回后端的静态配置，不涉及锁竞争。
    const std::string& name() const { return config_.name; }
    const std::string& host() const { return config_.host; }
    std::uint16_t port() const { return config_.port; }
    std::size_t weight() const { return config_.weight; }
    const ResolvedEndpoint& endpoint() const { return endpoint_; }

    /** @brief 查询或更新健康检查结果。 */
    bool healthy() const { return healthy_.load(); }
    void setHealthy(bool value) { healthy_.store(value); }

    /** @brief 返回当前被代理会话占用的连接数。 */
    std::size_t activeConnections() const { return activeConnections_.load(); }

    /** @brief 新会话选中该节点时增加活动连接数。 */
    void acquire();

    /** @brief 会话销毁时减少活动连接数。 */
    void release();

    /** @brief 记录一次成功或失败的业务连接建立。 */
    void recordConnectSuccess();
    void recordConnectFailure();

    std::uint64_t totalConnections() const { return totalConnections_.load(); }
    std::uint64_t failedConnections() const { return failedConnections_.load(); }

private:
    BackendConfig config_;                         //节点名称、地址、端口和调度权重。
    ResolvedEndpoint endpoint_;                    //getaddrinfo 解析后的 sockaddr，连接时可直接复用。
    std::atomic<bool> healthy_;                    //是否允许参与新连接调度。
    std::atomic<std::size_t> activeConnections_;   //当前正在使用该节点的代理会话数。
    std::atomic<std::uint64_t> totalConnections_;  //成功建立到该节点的累计次数。
    std::atomic<std::uint64_t> failedConnections_; //建立到该节点失败的累计次数。
};

}

#endif
