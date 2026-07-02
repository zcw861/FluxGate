//BackendPool.cpp：实现后端 RAII 租约和加权最少连接调度。
#include "fluxgate/BackendPool.h"

#include <limits>
#include <stdexcept>

namespace fluxgate {

//构造租约即占用后端，析构时自动归还，形成异常安全的计数管理。
BackendLease::BackendLease(const std::shared_ptr<Backend>& backend) : backend_(backend) {
    if (backend_) {
        backend_->acquire();
    }
}

//移动构造只转移所有权，不重复增加 activeConnections。
BackendLease::BackendLease(BackendLease&& other) : backend_(other.backend_) {
    other.backend_.reset();
}

BackendLease& BackendLease::operator=(BackendLease&& other) {
    if (this != &other) {
        reset();
        backend_ = other.backend_;
        other.backend_.reset();
    }
    return *this;
}

BackendLease::~BackendLease() {
    reset();
}

void BackendLease::reset() {
    if (backend_) {
        backend_->release();
        backend_.reset();
    }
}

//将配置转为长期存活的Backend对象；空池无法提供服务，因此直接拒绝启动。
BackendPool::BackendPool(const std::vector<BackendConfig>& configs) : tieBreaker_(0) {
    if (configs.empty()) {
        throw std::runtime_error("backend pool cannot be empty");
    }
    for (std::size_t i = 0; i < configs.size(); ++i) {
        backends_.push_back(std::make_shared<Backend>(configs[i]));
    }
}

//从健康节点中选择 active/weight 最小者，并在同一临界区内创建租约。
BackendLease BackendPool::acquire() {
    //选择与占用必须在同一临界区完成，避免并发接入时多个工作线程同时命中同一节点。
    std::lock_guard<std::mutex> lock(selectionMutex_);
    std::shared_ptr<Backend> selected;
    const std::size_t start = tieBreaker_.fetch_add(1) % backends_.size();

    for (std::size_t offset = 0; offset < backends_.size(); ++offset) {
        const std::size_t index = (start + offset) % backends_.size();
        const std::shared_ptr<Backend>& candidate = backends_[index];
        if (!candidate->healthy()) {
            continue;
        }
        if (!selected) {
            selected = candidate;
            continue;
        }

        //比较 active/weight，避免浮点运算导致边界误差。
        const std::size_t left = candidate->activeConnections() * selected->weight();
        const std::size_t right = selected->activeConnections() * candidate->weight();
        if (left < right) {
            selected = candidate;
        }
    }

    return BackendLease(selected);
}

bool BackendPool::hasHealthyBackend() const {
    for (std::size_t i = 0; i < backends_.size(); ++i) {
        if (backends_[i]->healthy()) {
            return true;
        }
    }
    return false;
}

}
