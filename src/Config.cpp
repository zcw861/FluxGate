//Config.cpp：实现严格的 INI 配置解析、默认值和参数范围校验。
#include "fluxgate/Config.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>

namespace fluxgate {
namespace {

//去除行首行尾空白，便于容忍常见INI排版。
std::string trim(const std::string& value) {
    std::string::const_iterator begin = value.begin();
    while (begin != value.end() && std::isspace(static_cast<unsigned char>(*begin)) != 0) {
        ++begin;
    }

    std::string::const_iterator end = value.end();
    while (end != begin && std::isspace(static_cast<unsigned char>(*(end - 1))) != 0) {
        --end;
    }
    return std::string(begin, end);
}

//节名和键名统一转为小写，使配置关键字大小写不敏感。
std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

//严格解析十进制整数：必须完整消费字符串并处于配置允许范围。
long parseLong(const std::string& value, const std::string& key, long minimum, long maximum) {
    std::size_t consumed = 0;
    long result = 0;
    try {
        result = std::stol(value, &consumed, 10);
    } catch (const std::exception&) {
        throw std::runtime_error("invalid integer for '" + key + "': " + value);
    }
    if (consumed != value.size() || result < minimum || result > maximum) {
        throw std::runtime_error("value out of range for '" + key + "': " + value);
    }
    return result;
}

//将[server]的键映射到 AppConfig；未知键直接报错，避免拼写错误被静默忽略。
void assignServerValue(AppConfig& config, const std::string& key, const std::string& value) {
    if (key == "listen_host") {
        config.listenHost = value;
    } else if (key == "listen_port") {
        config.listenPort = static_cast<std::uint16_t>(parseLong(value, key, 1, 65535));
    } else if (key == "workers") {
        config.workers = static_cast<std::size_t>(parseLong(value, key, 1, 128));
    } else if (key == "backlog") {
        config.backlog = static_cast<int>(parseLong(value, key, 16, 65535));
    } else if (key == "max_events") {
        config.maxEvents = static_cast<std::size_t>(parseLong(value, key, 16, 65536));
    } else if (key == "buffer_size") {
        config.bufferSize = static_cast<std::size_t>(parseLong(value, key, 4096, 16 * 1024 * 1024));
    } else if (key == "connect_timeout_ms") {
        config.connectTimeoutMs = static_cast<int>(parseLong(value, key, 50, 60000));
    } else if (key == "idle_timeout_seconds") {
        config.idleTimeoutSeconds = static_cast<int>(parseLong(value, key, 1, 86400));
    } else if (key == "stats_interval_seconds") {
        config.statsIntervalSeconds = static_cast<int>(parseLong(value, key, 0, 3600));
    } else if (key == "log_level") {
        config.logLevel = lower(value);
    } else {
        throw std::runtime_error("unknown key in [server]: " + key);
    }
}

//解析[health_check]参数，并限制探测周期、超时和状态阈值。
void assignHealthValue(AppConfig& config, const std::string& key, const std::string& value) {
    if (key == "interval_ms") {
        config.healthCheckIntervalMs = static_cast<int>(parseLong(value, key, 200, 600000));
    } else if (key == "timeout_ms") {
        config.healthCheckTimeoutMs = static_cast<int>(parseLong(value, key, 50, 60000));
    } else if (key == "failure_threshold") {
        config.healthCheckFailureThreshold = static_cast<std::size_t>(parseLong(value, key, 1, 100));
    } else if (key == "success_threshold") {
        config.healthCheckSuccessThreshold = static_cast<std::size_t>(parseLong(value, key, 1, 100));
    } else {
        throw std::runtime_error("unknown key in [health_check]: " + key);
    }
}

}

//默认值可直接用于开发环境，同时为缺省配置提供明确行为。
AppConfig::AppConfig()
    : listenHost("0.0.0.0"),
      listenPort(8080),
      workers(4),
      backlog(1024),
      maxEvents(2048),
      bufferSize(64 * 1024),
      connectTimeoutMs(3000),
      idleTimeoutSeconds(60),
      statsIntervalSeconds(10),
      healthCheckIntervalMs(3000),
      healthCheckTimeoutMs(800),
      healthCheckFailureThreshold(3),
      healthCheckSuccessThreshold(2),
      logLevel("info") {}

//按行解析配置；遇到新 section 时先提交上一段backend，文件结束后再提交最后一段。
AppConfig ConfigLoader::load(const std::string& path) {
    std::ifstream input(path.c_str());
    if (!input) {
        throw std::runtime_error("cannot open config file: " + path);
    }

    AppConfig config;
    std::string section;
    BackendConfig currentBackend;
    bool insideBackend = false;
    bool backendHasHost = false;
    bool backendHasPort = false;

    //统一完成后端段校验，防止切换section或到达EOF时遗漏最后一个节点。
    const auto finishBackend = [&]() {
        if (!insideBackend) {
            return;
        }
        if (currentBackend.name.empty() || !backendHasHost || !backendHasPort) {
            throw std::runtime_error("backend section requires name, host and port");
        }
        config.backends.push_back(currentBackend);
    };

    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        //#和;之后内容作为注释移除。
        const std::size_t comment = line.find_first_of("#;");
        if (comment != std::string::npos) {
            line.erase(comment);
        }
        line = trim(line);
        if (line.empty()) {
            continue;
        }

        if (line.front() == '[' && line.back() == ']') {
            finishBackend();
            insideBackend = false;
            backendHasHost = false;
            backendHasPort = false;
            currentBackend = BackendConfig();
            currentBackend.port = 0;
            currentBackend.weight = 1;

            section = trim(line.substr(1, line.size() - 2));
            const std::string normalized = lower(section);
            if (normalized == "server" || normalized == "health_check") {
                section = normalized;
            } else if (normalized.compare(0, 8, "backend ") == 0) {
                currentBackend.name = trim(section.substr(8));
                if (currentBackend.name.empty()) {
                    throw std::runtime_error("empty backend name at line " + std::to_string(lineNumber));
                }
                section = "backend";
                insideBackend = true;
            } else {
                throw std::runtime_error("unknown section at line " + std::to_string(lineNumber) + ": " + section);
            }
            continue;
        }

        const std::size_t separator = line.find('=');
        if (separator == std::string::npos) {
            throw std::runtime_error("expected key=value at line " + std::to_string(lineNumber));
        }
        const std::string key = lower(trim(line.substr(0, separator)));
        const std::string value = trim(line.substr(separator + 1));
        if (key.empty() || value.empty()) {
            throw std::runtime_error("empty key or value at line " + std::to_string(lineNumber));
        }

        if (section == "server") {
            assignServerValue(config, key, value);
        } else if (section == "health_check") {
            assignHealthValue(config, key, value);
        } else if (section == "backend" && insideBackend) {
            if (key == "host") {
                currentBackend.host = value;
                backendHasHost = true;
            } else if (key == "port") {
                currentBackend.port = static_cast<std::uint16_t>(parseLong(value, key, 1, 65535));
                backendHasPort = true;
            } else if (key == "weight") {
                currentBackend.weight = static_cast<std::size_t>(parseLong(value, key, 1, 1000));
            } else {
                throw std::runtime_error("unknown backend key at line " + std::to_string(lineNumber) + ": " + key);
            }
        } else {
            throw std::runtime_error("key appears before a valid section at line " + std::to_string(lineNumber));
        }
    }

    finishBackend();

    if (config.backends.empty()) {
        throw std::runtime_error("at least one backend is required");
    }
    //超时必须短于探测周期，否则多个探测周期会相互重叠。
    if (config.healthCheckTimeoutMs >= config.healthCheckIntervalMs) {
        throw std::runtime_error("health_check.timeout_ms must be smaller than interval_ms");
    }

    std::map<std::string, bool> names;
    for (std::size_t i = 0; i < config.backends.size(); ++i) {
        if (names[config.backends[i].name]) {
            throw std::runtime_error("duplicate backend name: " + config.backends[i].name);
        }
        names[config.backends[i].name] = true;
    }
    return config;
}

}
