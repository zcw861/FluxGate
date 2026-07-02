//Backend.cpp：实现单个后端节点的地址解析、健康状态和连接统计。
#include "fluxgate/Backend.h"

namespace fluxgate {

//构造阶段完成 DNS/IP 解析，后续每次连接无需重复调用 getaddrinfo。
Backend::Backend(const BackendConfig& config)
    : config_(config),
      endpoint_(resolveTcpEndpoint(config.host, config.port)),
      healthy_(true),
      activeConnections_(0),
      totalConnections_(0),
      failedConnections_(0) {}

//新会话获得BackendLease时调用。原子加法允许多个 Worker 并发更新。
void Backend::acquire() {
    activeConnections_.fetch_add(1);
}

//会话结束时释放租约。防御性判断避免异常路径下计数发生无符号下溢。
void Backend::release() {
    const std::size_t current = activeConnections_.load();
    if (current > 0) {
        activeConnections_.fetch_sub(1);
    }
}

void Backend::recordConnectSuccess() {
    totalConnections_.fetch_add(1);
}

void Backend::recordConnectFailure() {
    failedConnections_.fetch_add(1);
}

}
