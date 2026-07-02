//Logger.cpp：实现带级别、时间、源文件位置的线程安全标准错误日志。
#include "fluxgate/Logger.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <stdexcept>

namespace fluxgate {
namespace {

//将枚举映射为固定宽度文本，保持日志列对齐。
const char* levelName(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "UNKNOWN";
}

//去掉编译器传入的目录，只输出源文件名。
const char* baseName(const char* path) {
    const char* result = path;
    for (const char* current = path; *current != '\0'; ++current) {
        if (*current == '/') {
            result = current + 1;
        }
    }
    return result;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

} // namespace

Logger::Logger() : level_(LogLevel::Info) {}

// C++11 保证函数局部静态对象只初始化一次。
Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::setLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    level_ = level;
}

LogLevel Logger::level() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return level_;
}

void Logger::log(LogLevel level, const char* file, int line, const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    vlog(level, file, line, format, arguments);
    va_end(arguments);
}

// 单次加锁覆盖级别判断和整行输出，防止多个线程日志内容交叉。
void Logger::vlog(LogLevel level, const char* file, int line, const char* format, va_list arguments) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (static_cast<int>(level) < static_cast<int>(level_)) {
        return;
    }

    std::time_t now = std::time(NULL);
    std::tm localTime;
    localtime_r(&now, &localTime);

    char timestamp[32];
    std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &localTime);

    std::fprintf(stderr, "[%s] [%-5s] [%s:%d] ", timestamp, levelName(level), baseName(file), line);
    std::vfprintf(stderr, format, arguments);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

LogLevel Logger::parseLevel(const std::string& value) {
    const std::string normalized = lower(value);
    if (normalized == "debug") return LogLevel::Debug;
    if (normalized == "info") return LogLevel::Info;
    if (normalized == "warn" || normalized == "warning") return LogLevel::Warn;
    if (normalized == "error") return LogLevel::Error;
    throw std::runtime_error("unsupported log level: " + value);
}

}
