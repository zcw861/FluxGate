// fluxgate_tests.cpp：验证配置解析、加权最少连接和不健康节点跳过逻辑。
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include "fluxgate/BackendPool.h"
#include "fluxgate/Config.h"

namespace {

//极简断言辅助函数：失败时抛异常，由 main 统一输出并返回非零状态。
void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

//验证INI中服务端、健康检查和后端字段均被正确读取。
void testConfigParser() {
    const std::string path = std::string(FLUXGATE_SOURCE_DIR) + "/tests/test.conf";
    const fluxgate::AppConfig config = fluxgate::ConfigLoader::load(path);
    require(config.listenHost == "127.0.0.1", "listen host was not parsed");
    require(config.listenPort == 18080, "listen port was not parsed");
    require(config.workers == 2U, "worker count was not parsed");
    require(config.bufferSize == 8192U, "buffer size was not parsed");
    require(config.healthCheckFailureThreshold == 3U, "health failure threshold was not parsed");
    require(config.healthCheckSuccessThreshold == 2U, "health success threshold was not parsed");
    require(config.backends.size() == 2U, "backend count was not parsed");
    require(config.backends[1].name == "second", "backend name was not parsed");
    require(config.backends[1].weight == 2U, "backend weight was not parsed");
}

//权重 1:2 的两个空闲节点获得 6 个租约后，期望分配为 2:4。
void testWeightedLeastConnections() {
    std::vector<fluxgate::BackendConfig> configs;
    fluxgate::BackendConfig first = { "first", "127.0.0.1", 19001, 1U };
    fluxgate::BackendConfig second = { "second", "127.0.0.1", 19002, 2U };
    configs.push_back(first);
    configs.push_back(second);

    fluxgate::BackendPool pool(configs);
    std::vector<fluxgate::BackendLease> leases;
    for (std::size_t i = 0; i < 6U; ++i) {
        leases.push_back(pool.acquire());
        require(leases.back().valid(), "pool returned an empty lease");
    }

    require(pool.all()[0]->activeConnections() == 2U, "weight=1 backend should receive two leases");
    require(pool.all()[1]->activeConnections() == 4U, "weight=2 backend should receive four leases");

    leases.clear();
    require(pool.all()[0]->activeConnections() == 0U, "lease did not release first backend");
    require(pool.all()[1]->activeConnections() == 0U, "lease did not release second backend");
}

//手工摘除第一个节点，确保调度器不会返回不健康后端。
void testUnhealthyBackendIsSkipped() {
    std::vector<fluxgate::BackendConfig> configs;
    fluxgate::BackendConfig first = { "first", "127.0.0.1", 19001, 1U };
    fluxgate::BackendConfig second = { "second", "127.0.0.1", 19002, 1U };
    configs.push_back(first);
    configs.push_back(second);

    fluxgate::BackendPool pool(configs);
    pool.all()[0]->setHealthy(false);
    fluxgate::BackendLease lease = pool.acquire();
    require(lease.valid(), "healthy backend was not selected");
    require(lease->name() == "second", "unhealthy backend was selected");
}

}

int main() {
    try {
        testConfigParser();
        testWeightedLeastConnections();
        testUnhealthyBackendIsSkipped();
        std::cout << "all FluxGate tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "test failure: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
