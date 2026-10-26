#include "GuiHttpServer.hpp"

#include <httplib.h>

#include <atomic>
#include <mutex>
#include <system_error>
#include <thread>

namespace {

constexpr const char* kLoopbackAddress = "127.0.0.1";

} // namespace

class GuiHttpServer::Impl {
public:
    explicit Impl(int requestedPort)
        : requestedPort_(requestedPort) {
        server_.set_socket_options([](socket_t socket) {
#ifdef _WIN32
            httplib::set_socket_opt(
                socket,
                SOL_SOCKET,
                SO_EXCLUSIVEADDRUSE,
                1
            );
#else
            httplib::set_socket_opt(
                socket,
                SOL_SOCKET,
                SO_REUSEADDR,
                1
            );
#endif
        });
    }

    ~Impl() {
        stop();
    }

    bool start() {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);

        if (state_ != State::Ready) {
            return false;
        }

        state_ = State::Failed;
        if (requestedPort_ < 0 || requestedPort_ > 65535) {
            return false;
        }

        if (requestedPort_ == 0) {
            const int selectedPort =
                server_.bind_to_any_port(kLoopbackAddress);
            if (selectedPort <= 0) {
                server_.stop();
                return false;
            }
            boundPort_ = selectedPort;
        } else {
            if (!server_.bind_to_port(kLoopbackAddress, requestedPort_)) {
                server_.stop();
                return false;
            }
            boundPort_ = requestedPort_;
        }

        try {
            serverThread_ = std::thread([this] {
                server_.listen_after_bind();
            });
        } catch (const std::system_error&) {
            server_.stop();
            boundPort_ = 0;
            return false;
        }

        // cpp-httplib v0.58.0 provides this synchronization point. It waits
        // until listen_after_bind() is running or reports decommissioning.
        server_.wait_until_ready();
        if (!server_.is_running()) {
            server_.stop();
            serverThread_.join();
            boundPort_ = 0;
            return false;
        }

        state_ = State::Running;
        return true;
    }

    void stop() {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        if (serverThread_.joinable()) {
            server_.stop();
            serverThread_.join();
        }

        if (state_ == State::Ready || state_ == State::Running) {
            state_ = State::Stopped;
        }
    }

    bool isRunning() const {
        return server_.is_running();
    }

    int port() const {
        return boundPort_;
    }

private:
    enum class State {
        Ready,
        Running,
        Failed,
        Stopped
    };

    httplib::Server server_;
    const int requestedPort_;
    std::atomic<int> boundPort_{0};
    State state_ = State::Ready;
    std::mutex lifecycleMutex_;
    std::thread serverThread_;
};

GuiHttpServer::GuiHttpServer(int port)
    : impl_(std::make_unique<Impl>(port)) {}

GuiHttpServer::~GuiHttpServer() = default;

bool GuiHttpServer::start() {
    return impl_->start();
}

void GuiHttpServer::stop() {
    impl_->stop();
}

bool GuiHttpServer::isRunning() const {
    return impl_->isRunning();
}

int GuiHttpServer::port() const {
    return impl_->port();
}
