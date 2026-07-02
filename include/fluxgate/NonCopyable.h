#ifndef FLUXGATE_NONCOPYABLE_H
#define FLUXGATE_NONCOPYABLE_H

namespace fluxgate {

/**
 * @brief 为资源拥有者提供统一的“禁止拷贝”语义。
 *
 * C++11 项目中通过私有声明拷贝构造和赋值操作，避免文件描述符、线程、锁等资源被浅拷贝后重复释放。
 */
class NonCopyable {
protected:
    NonCopyable() {}
    ~NonCopyable() {}

private:
    NonCopyable(const NonCopyable&);
    NonCopyable& operator=(const NonCopyable&);
};

}

#endif
