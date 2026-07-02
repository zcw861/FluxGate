#ifndef FLUXGATE_BYTEBUFFER_H
#define FLUXGATE_BYTEBUFFER_H

#include <algorithm>
#include <cstddef>
#include <vector>

namespace fluxgate {

/**
 * @brief 用于非阻塞 Socket 转发的固定容量字节缓冲区。
 *
 * 缓冲区通过 readIndex_ 和 writeIndex_ 区分“待发送数据”和“剩余可写空间”。
 * 不在数据热路径中扩容，可限制单会话内存；当尾部空间不足而头部已经释放时，makeWritable()
 * 会把未发送数据原地移动到起始位置，从而继续复用已有存储。
 */
class ByteBuffer {
public:
    explicit ByteBuffer(std::size_t capacity)
        : storage_(capacity), readIndex_(0), writeIndex_(0) {}

    /** @brief 当前等待发送的字节数。 */
    std::size_t readableBytes() const { return writeIndex_ - readIndex_; }

    /** @brief 当前可直接写入的尾部空间。 */
    std::size_t writableBytes() const { return storage_.size() - writeIndex_; }

    bool empty() const { return readableBytes() == 0; }

    /** @brief 返回待发送数据起始地址，供 send() 使用。 */
    const char* readData() const { return &storage_[readIndex_]; }

    /** @brief 返回可写区域起始地址，供 recv() 使用。 */
    char* writeData() { return &storage_[writeIndex_]; }

    /** @brief recv() 成功后推进写游标。调用者必须保证 bytes 不超过 writableBytes()。 */
    void hasWritten(std::size_t bytes) { writeIndex_ += bytes; }

    /**
     * @brief send() 成功后丢弃已经发送的数据。
     * 当全部数据发送完成时同时把两个游标归零，避免索引持续向后增长。
     */
    void consume(std::size_t bytes) {
        readIndex_ += bytes;
        if (readIndex_ == writeIndex_) {
            readIndex_ = 0;
            writeIndex_ = 0;
        }
    }

    /**
     * @brief 在尾部没有空间时压缩缓冲区。
     * 只移动尚未发送的数据，不改变其顺序；固定容量设计用于实现背压而不是无限扩容。
     */
    void makeWritable() {
        if (writableBytes() == 0 && readIndex_ > 0) {
            const std::size_t length = readableBytes();
            std::copy(storage_.begin() + static_cast<std::ptrdiff_t>(readIndex_),
                      storage_.begin() + static_cast<std::ptrdiff_t>(writeIndex_),
                      storage_.begin());
            readIndex_ = 0;
            writeIndex_ = length;
        }
    }

private:
    std::vector<char> storage_; //创建会话时一次性分配的底层存储。
    std::size_t readIndex_;     //第一个未发送字节的位置。
    std::size_t writeIndex_;    //下一个可写字节的位置。
};

}

#endif
