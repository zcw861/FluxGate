#ifndef FLUXGATE_UNIQUEFD_H
#define FLUXGATE_UNIQUEFD_H

#include <unistd.h>
#include "fluxgate/NonCopyable.h"

namespace fluxgate {

/**
 * @brief 使用 RAII 独占管理一个 Linux 文件描述符。
 *
 * 对象不可拷贝但可移动。析构时自动 close()，确保正常返回、异常和中途失败路径都不会泄漏 Socket、
 * epoll 或 eventfd。release() 用于显式转交所有权，reset() 用于替换或提前关闭。
 */
class UniqueFd : private NonCopyable {
public:
    explicit UniqueFd(int fd = -1) : fd_(fd) {}

    UniqueFd(UniqueFd&& other) : fd_(other.release()) {}

    UniqueFd& operator=(UniqueFd&& other) {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    ~UniqueFd() { reset(); }

    int get() const { return fd_; }
    bool valid() const { return fd_ >= 0; }

    /** @brief 放弃管理并返回 fd；调用者从此负责关闭。 */
    int release() {
        const int value = fd_;
        fd_ = -1;
        return value;
    }

    /** @brief 关闭旧 fd 后接管新 fd，默认参数表示只关闭不接管。 */
    void reset(int fd = -1) {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = fd;
    }

private:
    int fd_;
};

}

#endif
