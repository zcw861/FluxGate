#ifndef FLUXGATE_BACKENDPOOL_H
#define FLUXGATE_BACKENDPOOL_H

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>
#include "fluxgate/Backend.h"
#include "fluxgate/Config.h"
#include "fluxgate/NonCopyable.h"

namespace fluxgate {

/**
 * @brief 后端节点的 RAII 占用令牌。
 *
 * 创建令牌时自动增加 Backend::activeConnections，令牌析构或 reset 时自动减少。
 * 这样即使会话创建中途抛出异常，也不会遗漏连接计数回滚。
 */
class BackendLease : private NonCopyable {
public:
    explicit BackendLease(const std::shared_ptr<Backend>& backend);
    BackendLease(BackendLease&& other);
    BackendLease& operator=(BackendLease&& other);
    ~BackendLease();

    Backend* operator->() const { return backend_.get(); }
    Backend& operator*() const { return *backend_; }
    std::shared_ptr<Backend> shared() const { return backend_; }
    bool valid() const { return static_cast<bool>(backend_); }

    /** @brief 提前释放节点占用，并清空令牌。 */
    void reset();

private:
    std::shared_ptr<Backend> backend_; // 保证会话存活期间后端对象不会被销毁。
};

/**
 * @brief 保存全部后端，并执行加权最少连接选择。
 *
 * 调度分数为 active_connections / weight。比较时使用交叉乘法避免浮点误差；
 * 相同分数下使用轮转起点，防止总是偏向配置中的第一个节点。
 */
class BackendPool : private NonCopyable {
public:
    explicit BackendPool(const std::vector<BackendConfig>& configs);

    /**
     * @brief 选择一个健康后端并返回占用令牌。
     * @return 没有健康节点时返回无效 BackendLease。
     */
    BackendLease acquire();

    /** @brief 返回后端列表，供健康检查和指标输出只读遍历。 */
    const std::vector<std::shared_ptr<Backend> >& all() const { return backends_; }

    /** @brief 判断当前是否至少存在一个可调度节点。 */
    bool hasHealthyBackend() const;

private:
    std::vector<std::shared_ptr<Backend> > backends_; //配置中定义的全部节点。
    std::atomic<std::size_t> tieBreaker_;             //同分节点选择的轮转起点。
    std::mutex selectionMutex_;                       //保证“选择节点+增加计数”是一个原子步骤。
};

}

#endif
