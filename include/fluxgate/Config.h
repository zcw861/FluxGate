#ifndef FLUXGATE_CONFIG_H
#define FLUXGATE_CONFIG_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fluxgate {

/** @brief 单个后端节点的静态配置。 */
struct BackendConfig {
    std::string name;  //日志和指标中使用的唯一名称。
    std::string host;  //IPv4、IPv6 或可解析主机名。
    std::uint16_t port;
    std::size_t weight; //加权最少连接算法中的容量权重，必须大于 0。
};

/** @brief FluxGate 启动后使用的完整配置对象。 */
struct AppConfig {
    std::string listenHost;                  //代理监听地址。
    std::uint16_t listenPort;                //代理监听端口。
    std::size_t workers;                     //Worker Reactor线程数量。
    int backlog;                             //listen()半连接/全连接队列提示值。
    std::size_t maxEvents;                   //每个Worker单次epoll_wait最大事件数。
    std::size_t bufferSize;                  //每个转发方向的固定缓冲区容量。
    int connectTimeoutMs;                    //业务连接后端的超时时间。
    int idleTimeoutSeconds;                  //会话无读写活动后的清理时间。
    int statsIntervalSeconds;                //指标日志输出周期，0表示关闭周期输出。
    int healthCheckIntervalMs;               //主动探测周期。
    int healthCheckTimeoutMs;                //单次TCP探测超时。
    std::size_t healthCheckFailureThreshold; //连续失败多少次后摘除。
    std::size_t healthCheckSuccessThreshold; //连续成功多少次后恢复。
    std::string logLevel;                    //debug/info/warn/error。
    std::vector<BackendConfig> backends;     //至少包含一个后端节点。

    /** @brief 构造包含安全默认值的配置，文件中未出现的项目使用这些默认值。 */
    AppConfig();
};

/** @brief 严格读取并校验 INI 配置文件。 */
class ConfigLoader {
public:
    /**
     * @brief 解析配置文件并返回 AppConfig。
     * @throws std::runtime_error 文件不存在、字段未知、数值越界或后端配置不完整时抛出。
     */
    static AppConfig load(const std::string& path);
};

}

#endif
