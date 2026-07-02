//main.cpp：负责参数解析、监听接入、Worker调度、指标线程和优雅退出。
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include "fluxgate/BackendPool.h"
#include "fluxgate/Config.h"
#include "fluxgate/HealthChecker.h"
#include "fluxgate/Logger.h"
#include "fluxgate/Metrics.h"
#include "fluxgate/Net.h"
#include "fluxgate/UniqueFd.h"
#include "fluxgate/Worker.h"

namespace {

//信号处理器只能安全修改sig_atomic_t标志，复杂清理留给主循环执行。
volatile std::sig_atomic_t stopRequested = 0;

void signalHandler(int) {
    stopRequested = 1;
}

//SIGINT/SIGTERM请求优雅退出；忽略SIGPIPE，实际写错误由send()返回值处理。
void installSignals() {
    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    action.sa_handler = signalHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    if (::sigaction(SIGINT, &action, NULL) < 0 || ::sigaction(SIGTERM, &action, NULL) < 0) {
        throw std::runtime_error("sigaction failed");
    }
    ::signal(SIGPIPE, SIG_IGN);
}

void printUsage(const char* program) {
    std::cout << "Usage: " << program << " [-c config] [--check] [--version]\n";
}

struct Options {
    std::string configPath;
    bool checkOnly;
    bool showVersion;

    Options() : configPath("config/fluxgate.conf"), checkOnly(false), showVersion(false) {}
};

//解析命令行，并对未知参数立即报错。
Options parseOptions(int argc, char* argv[]) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if ((argument == "-c" || argument == "--config") && i + 1 < argc) {
            options.configPath = argv[++i];
        } else if (argument == "--check") {
            options.checkOnly = true;
        } else if (argument == "--version" || argument == "-v") {
            options.showVersion = true;
        } else if (argument == "--help" || argument == "-h") {
            printUsage(argv[0]);
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + argument);
        }
    }
    return options;
}

//从轮转起点开始寻找活动会话数最少的 Worker，兼顾负载与同分公平性。
std::size_t selectWorker(const std::vector<std::unique_ptr<fluxgate::Worker> >& workers, std::size_t cursor) {
    std::size_t selected = cursor % workers.size();
    std::size_t minimum = workers[selected]->activeSessions();
    for (std::size_t i = 1; i < workers.size(); ++i) {
        const std::size_t index = (cursor + i) % workers.size();
        const std::size_t active = workers[index]->activeSessions();
        if (active < minimum) {
            selected = index;
            minimum = active;
        }
    }
    return selected;
}

}

//进程入口按“配置 -> 资源初始化 -> 启动线程 -> accept 循环 -> 反向停止”的顺序管理生命周期。
int main(int argc, char* argv[]) {
    try {
        const Options options = parseOptions(argc, argv);
        if (options.showVersion) {
            std::cout << "FluxGate 1.2.0\n";
            return 0;
        }

        const fluxgate::AppConfig config = fluxgate::ConfigLoader::load(options.configPath);
        fluxgate::Logger::instance().setLevel(fluxgate::Logger::parseLevel(config.logLevel));
        fluxgate::BackendPool backendPool(config.backends);

        if (options.checkOnly) {
            std::cout << "configuration is valid; resolved " << backendPool.all().size() << " backend(s)\n";
            return 0;
        }

        installSignals();
        fluxgate::UniqueFd listenFd(fluxgate::createListenSocket(config.listenHost, config.listenPort, config.backlog));
        fluxgate::Metrics metrics;
        fluxgate::HealthChecker healthChecker(backendPool,
                                                 config.healthCheckIntervalMs,
                                                 config.healthCheckTimeoutMs,
                                                 config.healthCheckFailureThreshold,
                                                 config.healthCheckSuccessThreshold);

        std::vector<std::unique_ptr<fluxgate::Worker> > workers;
        workers.reserve(config.workers);
        for (std::size_t i = 0; i < config.workers; ++i) {
            workers.push_back(std::unique_ptr<fluxgate::Worker>(new fluxgate::Worker(i, config, backendPool, metrics)));
            workers.back()->start();
        }
        healthChecker.start();

        //指标线程只负责周期读取原子计数和打印日志，不参与业务控制。
        std::atomic<bool> statsRunning(true);
        std::thread statsThread;
        if (config.statsIntervalSeconds > 0) {
            statsThread = std::thread([&]() {
                while (statsRunning.load()) {
                    for (int elapsed = 0; elapsed < config.statsIntervalSeconds * 10 && statsRunning.load(); ++elapsed) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                    if (!statsRunning.load()) {
                        break;
                    }
                    FG_LOG_INFO("metrics %s", metrics.snapshot().c_str());
                    const std::vector<std::shared_ptr<fluxgate::Backend> >& backends = backendPool.all();
                    for (std::size_t i = 0; i < backends.size(); ++i) {
                        FG_LOG_INFO("backend=%s healthy=%s active=%zu total=%llu failed=%llu",
                                    backends[i]->name().c_str(), backends[i]->healthy() ? "yes" : "no",
                                    backends[i]->activeConnections(),
                                    static_cast<unsigned long long>(backends[i]->totalConnections()),
                                    static_cast<unsigned long long>(backends[i]->failedConnections()));
                    }
                }
            });
        }

        FG_LOG_INFO("FluxGate listening on %s:%u with %zu worker(s) and %zu backend(s)",
                    config.listenHost.c_str(), static_cast<unsigned int>(config.listenPort),
                    config.workers, config.backends.size());

        pollfd listener;
        listener.fd = listenFd.get();
        listener.events = POLLIN;
        listener.revents = 0;
        std::size_t dispatchCursor = 0;

        //主线程使用 poll 等待监听 Socket，500ms 超时用于定期观察退出标志。
        while (stopRequested == 0) {
            const int ready = ::poll(&listener, 1, 500);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::runtime_error(std::string("poll failed: ") + std::strerror(errno));
            }
            if (ready == 0 || (listener.revents & POLLIN) == 0) {
                continue;
            }

            //一次可读事件可能包含多个已完成连接，循环 accept4 直到 EAGAIN。
            for (;;) {
                sockaddr_storage address;
                std::memset(&address, 0, sizeof(address));
                socklen_t length = sizeof(address);
                const int clientFd = ::accept4(listenFd.get(), reinterpret_cast<sockaddr*>(&address), &length,
                                               SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (clientFd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    if (errno == EINTR) {
                        continue;
                    }
                    FG_LOG_WARN("accept4 failed: %s", std::strerror(errno));
                    break;
                }

                metrics.onAccepted();
                try {
                    fluxgate::setTcpNoDelay(clientFd);
                } catch (const std::exception& error) {
                    FG_LOG_WARN("cannot configure client socket: %s", error.what());
                    ::close(clientFd);
                    metrics.onRejected();
                    continue;
                }

                const std::size_t workerIndex = selectWorker(workers, dispatchCursor++);
                if (!workers[workerIndex]->enqueue(clientFd, fluxgate::peerAddress(address))) {
                    ::close(clientFd);
                    metrics.onRejected();
                }
            }
        }

        //按依赖关系停止：先阻止健康检查和Worker继续工作，再join，最后停止指标线程。
        FG_LOG_INFO("shutdown requested");
        healthChecker.stop();
        for (std::size_t i = 0; i < workers.size(); ++i) {
            workers[i]->stop();
        }
        healthChecker.join();
        for (std::size_t i = 0; i < workers.size(); ++i) {
            workers[i]->join();
        }

        statsRunning.store(false);
        if (statsThread.joinable()) {
            statsThread.join();
        }
        FG_LOG_INFO("FluxGate stopped; final metrics %s", metrics.snapshot().c_str());
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fatal: " << error.what() << '\n';
        return 1;
    }
}
