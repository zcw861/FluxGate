//Metrics.cpp：实现无锁原子计数和日志文本快照。
#include "fluxgate/Metrics.h"

#include <sstream>

namespace fluxgate {

Metrics::Metrics()
    : accepted_(0),
      active_(0),
      rejected_(0),
      connectFailures_(0),
      clientToBackendBytes_(0),
      backendToClientBytes_(0) {}

//每个更新函数只执行原子操作，避免在高频I/O路径中使用互斥锁。
void Metrics::onAccepted() { accepted_.fetch_add(1); }
void Metrics::onOpened() { active_.fetch_add(1); }
void Metrics::onClosed() {
    const std::uint64_t current = active_.load();
    if (current > 0) active_.fetch_sub(1);
}
void Metrics::onRejected() { rejected_.fetch_add(1); }
void Metrics::onConnectFailure() { connectFailures_.fetch_add(1); }
void Metrics::addClientToBackend(std::size_t bytes) { clientToBackendBytes_.fetch_add(bytes); }
void Metrics::addBackendToClient(std::size_t bytes) { backendToClientBytes_.fetch_add(bytes); }

//读取各原子计数生成近似一致的观测快照，不阻塞业务线程。
std::string Metrics::snapshot() const {
    std::ostringstream stream;
    stream << "accepted=" << accepted_.load()
           << " active=" << active_.load()
           << " rejected=" << rejected_.load()
           << " connect_failures=" << connectFailures_.load()
           << " c2b_bytes=" << clientToBackendBytes_.load()
           << " b2c_bytes=" << backendToClientBytes_.load();
    return stream.str();
}

}
