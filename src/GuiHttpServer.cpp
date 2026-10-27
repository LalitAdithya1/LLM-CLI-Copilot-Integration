#include "GuiHttpServer.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr const char* kLoopbackAddress = "127.0.0.1";
constexpr const char* kJsonContentType = "application/json; charset=utf-8";
constexpr std::size_t kMaxRequestBodyBytes = 1024 * 1024;

using Json = nlohmann::json;

bool isWhitespaceOnly(const std::string& value) {
    for (unsigned char character : value) {
        if (!std::isspace(character)) {
            return false;
        }
    }
    return true;
}

bool isJsonContentType(const std::string& contentType) {
    const auto parameterStart = contentType.find(';');
    std::string mediaType = contentType.substr(0, parameterStart);
    const auto first = mediaType.find_first_not_of(" \t");
    if (first == std::string::npos) {
        return false;
    }
    const auto last = mediaType.find_last_not_of(" \t");
    mediaType = mediaType.substr(first, last - first + 1);
    std::transform(
        mediaType.begin(),
        mediaType.end(),
        mediaType.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        }
    );
    return mediaType == "application/json";
}

void setJsonError(
    httplib::Response& response,
    int status,
    const char* code,
    const char* message
) {
    response.status = status;
    response.set_content(
        Json{{"error", {{"code", code}, {"message", message}}}}.dump(),
        kJsonContentType
    );
}

bool parseGenerationRequest(
    const std::string& requestBody,
    std::string& instruction,
    std::string& language,
    httplib::Response& response
) {
    Json body;
    try {
        body = Json::parse(requestBody);
    } catch (const Json::exception&) {
        setJsonError(
            response,
            400,
            "INVALID_JSON",
            "Request body must contain valid JSON."
        );
        return false;
    }

    if (!body.is_object() || body.size() != 2 ||
        !body.contains("instruction") ||
        !body.contains("language") ||
        !body["instruction"].is_string() ||
        !body["language"].is_string()) {
        setJsonError(
            response,
            400,
            "INVALID_REQUEST",
            "Request must contain only string instruction and language fields."
        );
        return false;
    }

    instruction = body["instruction"].get<std::string>();
    language = body["language"].get<std::string>();
    if (isWhitespaceOnly(instruction)) {
        setJsonError(
            response,
            400,
            "INVALID_INSTRUCTION",
            "Instruction cannot be empty."
        );
        return false;
    }
    if (isWhitespaceOnly(language)) {
        setJsonError(
            response,
            400,
            "INVALID_LANGUAGE",
            "Language cannot be empty."
        );
        return false;
    }
    return true;
}

const std::vector<std::string>& supportedExtensions() {
    static const std::vector<std::string> extensions = {
        ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"
    };
    return extensions;
}

} // namespace

class GuiHttpServer::Impl {
public:
    explicit Impl(
        int requestedPort,
        std::filesystem::path launchWorkingDirectory,
        GuiHttpServer::GenerationHandler generationHandler
    )
        : requestedPort_(requestedPort),
          launchWorkingDirectory_(resolveLaunchDirectory(
              std::move(launchWorkingDirectory)
          )),
          activeOutputDirectory_(launchWorkingDirectory_),
          generationHandler_(std::move(generationHandler)) {
        server_.Get("/api/health", [](const httplib::Request&, httplib::Response& response) {
            response.set_content(
                nlohmann::json{{"status", "ok"}}.dump(),
                kJsonContentType
            );
        });

        server_.Get("/api/config", [this](const httplib::Request&, httplib::Response& response) {
            const nlohmann::json config = {
                {"supportedExtensions", supportedExtensions()},
                {"launchDirectory", launchWorkingDirectory_.string()},
                {"activeOutputDirectory", activeOutputDirectory_.string()}
            };
            response.set_content(config.dump(), kJsonContentType);
        });

        server_.Post("/api/generate", [this](const httplib::Request& request, httplib::Response& response) {
            if (!isJsonContentType(request.get_header_value("Content-Type"))) {
                setJsonError(
                    response,
                    415,
                    "UNSUPPORTED_MEDIA_TYPE",
                    "Content-Type must be application/json."
                );
                return;
            }

            std::string instruction;
            std::string language;
            if (!parseGenerationRequest(
                    request.body,
                    instruction,
                    language,
                    response)) {
                return;
            }
            if (!generationHandler_) {
                setJsonError(
                    response,
                    503,
                    "GENERATION_UNAVAILABLE",
                    "Code generation is unavailable."
                );
                return;
            }

            std::string generatedCode;
            try {
                if (!generationHandler_(instruction, language, generatedCode) ||
                    isWhitespaceOnly(generatedCode)) {
                    setJsonError(
                        response,
                        502,
                        "GENERATION_FAILED",
                        "Code generation failed."
                    );
                    return;
                }
            } catch (...) {
                setJsonError(
                    response,
                    502,
                    "GENERATION_FAILED",
                    "Code generation failed."
                );
                return;
            }

            try {
                response.set_content(
                    Json{
                        {"generatedCode", generatedCode},
                        {"language", language}
                    }.dump(),
                    kJsonContentType
                );
            } catch (...) {
                setJsonError(
                    response,
                    502,
                    "GENERATION_FAILED",
                    "Code generation failed."
                );
            }
        });

        server_.set_payload_max_length(kMaxRequestBodyBytes);
        server_.set_error_handler([](const httplib::Request&, httplib::Response& response) {
            if (response.status == 413) {
                setJsonError(
                    response,
                    413,
                    "PAYLOAD_TOO_LARGE",
                    "Request body exceeds the 1 MiB limit."
                );
            }
        });

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
    static std::filesystem::path resolveLaunchDirectory(
        std::filesystem::path launchWorkingDirectory
    ) {
        if (launchWorkingDirectory.empty()) {
            launchWorkingDirectory = std::filesystem::current_path();
        }
        return std::filesystem::absolute(launchWorkingDirectory)
            .lexically_normal();
    }

    enum class State {
        Ready,
        Running,
        Failed,
        Stopped
    };

    httplib::Server server_;
    const int requestedPort_;
    const std::filesystem::path launchWorkingDirectory_;
    std::filesystem::path activeOutputDirectory_;
    const GuiHttpServer::GenerationHandler generationHandler_;
    std::atomic<int> boundPort_{0};
    State state_ = State::Ready;
    std::mutex lifecycleMutex_;
    std::thread serverThread_;
};

GuiHttpServer::GuiHttpServer(int port)
    : GuiHttpServer(port, std::filesystem::current_path(), {}) {}

GuiHttpServer::GuiHttpServer(
    int port,
    std::filesystem::path launchWorkingDirectory
)
    : GuiHttpServer(
          port,
          std::move(launchWorkingDirectory),
          {}
      ) {}

GuiHttpServer::GuiHttpServer(
    int port,
    std::filesystem::path launchWorkingDirectory,
    GenerationHandler generationHandler
)
    : impl_(std::make_unique<Impl>(
          port,
          std::move(launchWorkingDirectory),
          std::move(generationHandler)
      )) {}

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
