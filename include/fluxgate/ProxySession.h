#ifndef FLUXGATE_PROXYSESSION_H
#define FLUXGATE_PROXYSESSION_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include "fluxgate/BackendPool.h"
#include "fluxgate/ByteBuffer.h"
#include "fluxgate/Metrics.h"
#include "fluxgate/NonCopyable.h"
#include "fluxgate/UniqueFd.h"

namespace fluxgate {

/**
 * @brief 管理一条“客户端 Socket <-> 后端 Socket”的完整代理会话。
 *
 * ProxySession 不创建线程，只由所属 Worker 的 epoll 线程访问。它负责：
 * 1. 完成非阻塞后端连接；
 * 2. 在两个方向读取、缓存和发送数据；
 * 3. 根据缓冲区容量动态生成 epoll 关注事件，实现背压；
 * 4. 正确处理 TCP 半关闭、连接超时和空闲超时；
 * 5. 在析构时通过 UniqueFd 和 BackendLease 自动释放资源。
 */
class ProxySession : private NonCopyable {
public:
    ProxySession(std::uint64_t id,
                 int clientFd,
                 int upstreamFd,
                 bool upstreamConnecting,
                 BackendLease backend,
                 std::size_t bufferSize,
                 const std::string& clientAddress);
    ~ProxySession();

    std::uint64_t id() const { return id_; }
    int clientFd() const { return clientFd_.get(); }
    int upstreamFd() const { return upstreamFd_.get(); }
    const std::string& clientAddress() const { return clientAddress_; }
    Backend& backend() { return *backend_; }
    const Backend& backend() const { return *backend_; }

    /**
     * @brief 处理某个 Socket 的一组 epoll 事件。
     * @param fd 触发事件的客户端或后端文件描述符。
     * @param events epoll 返回的事件位集合。
     * @param metrics 用于累计连接失败和双向字节数。
     * @return true 表示会话仍可继续；false 表示 Worker 应在本轮事件结束后关闭会话。
     */
    bool handleEvent(int fd, std::uint32_t events, Metrics& metrics);

    /** @brief 根据缓冲区和半关闭状态计算客户端 Socket 当前应关注的 epoll 事件。 */
    std::uint32_t clientEvents() const;

    /** @brief 根据连接阶段和缓冲区状态计算后端 Socket 当前应关注的 epoll 事件。 */
    std::uint32_t upstreamEvents() const;

    bool idleFor(std::chrono::seconds timeout) const;
    bool connectTimedOut(std::chrono::milliseconds timeout) const;
    void markClosing() { closing_ = true; }
    bool closing() const { return closing_; }

private:
    /** @brief 单次非阻塞读写操作的结果，用于区分正常阻塞、EOF 和真实错误。 */
    enum class IoResult {
        Progress,   //本次至少读写了一个字节。
        WouldBlock, //当前无数据或发送缓冲区已满，等待下一次epoll事件。
        Closed,     //recv/send返回0，对端已结束该方向。
        Error       //出现不可恢复错误，应关闭会话。
    };

    bool finishUpstreamConnect(Metrics& metrics); // 用 SO_ERROR 确认 EINPROGRESS 连接结果。
    IoResult readInto(int fd, ByteBuffer& buffer, Metrics& metrics, bool clientSide);
    IoResult writeFrom(int fd, ByteBuffer& buffer);
    void updateHalfClose(); //在一侧读 EOF 且缓存排空后，关闭另一侧写方向。
    bool completed() const; //判断响应已结束且无待发送数据。
    void touch();           //更新最近一次成功I/O时间。

    std::uint64_t id_;                 //Worker 内唯一会话编号，用于日志和映射索引。
    UniqueFd clientFd_;                //客户端连接，析构时自动关闭。
    UniqueFd upstreamFd_;              //选中后端的连接，析构时自动关闭。
    bool upstreamConnecting_;          //后端 connect 是否仍处于 EINPROGRESS 阶段。
    BackendLease backend_;             //被选中节点的 RAII 活动连接计数令牌。
    ByteBuffer clientToUpstream_;      //客户端已读、尚未写入后端的数据。
    ByteBuffer upstreamToClient_;      //后端已读、尚未写回客户端的数据。
    bool clientReadClosed_;            //客户端是否已发送 FIN/EOF。
    bool upstreamReadClosed_;          //后端是否已发送 FIN/EOF。
    bool clientWriteShutdown_;         //是否已对客户端执行 shutdown(SHUT_WR)。
    bool upstreamWriteShutdown_;       //是否已对后端执行 shutdown(SHUT_WR)。
    bool closing_;                     //延迟删除标记，避免同一批 epoll 事件中的 fd 复用问题。
    std::string clientAddress_;        //日志使用的客户端 ip:port。
    std::chrono::steady_clock::time_point createdAt_;    //用于判断后端连接超时。
    std::chrono::steady_clock::time_point lastActivity_; //用于清理空闲连接。
};

}
#endif
