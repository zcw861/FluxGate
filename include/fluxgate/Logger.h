#ifndef FLUXGATE_LOGGER_H
#define FLUXGATE_LOGGER_H

#include <cstdarg>
#include <mutex>
#include <string>
#include "fluxgate/NonCopyable.h"

namespace fluxgate {

/** @brief 日志严重级别，数值越大表示越严重。 */
enum class LogLevel {
    Debug = 0,
    Info,
    Warn,
    Error
};

/**
 * @brief 进程级线程安全日志器。
 *
 * 采用单例保证所有线程使用相同日志级别和输出锁；每条日志包含时间、级别、文件名和行号。
 */
class Logger : private NonCopyable {
public:
    static Logger& instance();

    void setLevel(LogLevel level);
    LogLevel level() const;

    /** @brief printf 风格日志接口，低于当前阈值的日志会被丢弃。 */
    void log(LogLevel level, const char* file, int line, const char* format, ...);

    /** @brief 将配置中的字符串转换为 LogLevel。 */
    static LogLevel parseLevel(const std::string& value);

private:
    Logger();
    void vlog(LogLevel level, const char* file, int line, const char* format, va_list arguments);

    mutable std::mutex mutex_; //同时保护level_和完整的一次日志输出，防止多线程交叉打印。
    LogLevel level_;
};

}

//宏自动注入调用点文件名与行号，业务代码只需要提供格式字符串和参数。
#define FG_LOG_DEBUG(...) ::fluxgate::Logger::instance().log(::fluxgate::LogLevel::Debug, __FILE__, __LINE__, __VA_ARGS__)
#define FG_LOG_INFO(...)  ::fluxgate::Logger::instance().log(::fluxgate::LogLevel::Info,  __FILE__, __LINE__, __VA_ARGS__)
#define FG_LOG_WARN(...)  ::fluxgate::Logger::instance().log(::fluxgate::LogLevel::Warn,  __FILE__, __LINE__, __VA_ARGS__)
#define FG_LOG_ERROR(...) ::fluxgate::Logger::instance().log(::fluxgate::LogLevel::Error, __FILE__, __LINE__, __VA_ARGS__)

#endif
