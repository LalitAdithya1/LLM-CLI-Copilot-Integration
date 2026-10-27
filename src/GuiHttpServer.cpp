#include "GuiHttpServer.hpp"
#include "UnifiedDiff.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <system_error>
#include <thread>
#include <unordered_map>
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

struct RefactoringRequest {
    std::string sourceCode;
    std::string instruction;
    std::string language;
    std::string editorRevision;
};

bool parseRefactoringRequest(
    const std::string& requestBody,
    RefactoringRequest& request,
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

    if (!body.is_object() || body.size() != 4 ||
        !body.contains("sourceCode") || !body["sourceCode"].is_string() ||
        !body.contains("instruction") || !body["instruction"].is_string() ||
        !body.contains("language") || !body["language"].is_string() ||
        !body.contains("editorRevision") || !body["editorRevision"].is_string()) {
        setJsonError(
            response,
            400,
            "INVALID_REQUEST",
            "Request must contain only string sourceCode, instruction, language, and editorRevision fields."
        );
        return false;
    }

    request.sourceCode = body["sourceCode"].get<std::string>();
    request.instruction = body["instruction"].get<std::string>();
    request.language = body["language"].get<std::string>();
    request.editorRevision = body["editorRevision"].get<std::string>();

    if (isWhitespaceOnly(request.sourceCode)) {
        setJsonError(response, 400, "INVALID_SOURCE_CODE", "Source code cannot be empty.");
        return false;
    }
    if (isWhitespaceOnly(request.instruction)) {
        setJsonError(response, 400, "INVALID_INSTRUCTION", "Instruction cannot be empty.");
        return false;
    }
    if (isWhitespaceOnly(request.language)) {
        setJsonError(response, 400, "INVALID_LANGUAGE", "Language cannot be empty.");
        return false;
    }
    if (isWhitespaceOnly(request.editorRevision)) {
        setJsonError(response, 400, "INVALID_EDITOR_REVISION", "Editor revision cannot be empty.");
        return false;
    }
    return true;
}

std::string hashEditorContent(const std::string& sourceCode) {
    std::uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned char byte : sourceCode) {
        hash ^= byte;
        hash *= UINT64_C(1099511628211);
    }

    std::ostringstream output;
    output << "fnv1a64:" << std::hex << std::nouppercase
           << std::setw(16) << std::setfill('0') << hash;
    return output.str();
}

struct DiffRequest {
    std::string proposalId;
    std::string originalCode;
    std::string proposedCode;
    bool usesProposal = false;
};

bool parseDiffRequest(
    const std::string& requestBody,
    DiffRequest& request,
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

    if (!body.is_object()) {
        setJsonError(
            response,
            400,
            "INVALID_REQUEST",
            "Request must contain a proposalId or originalCode and proposedCode."
        );
        return false;
    }

    if (body.size() == 1 && body.contains("proposalId") &&
        body["proposalId"].is_string()) {
        request.proposalId = body["proposalId"].get<std::string>();
        if (isWhitespaceOnly(request.proposalId)) {
            setJsonError(
                response,
                400,
                "INVALID_REQUEST",
                "proposalId cannot be empty."
            );
            return false;
        }
        request.usesProposal = true;
        return true;
    }

    if (body.size() == 2 && body.contains("originalCode") &&
        body["originalCode"].is_string() &&
        body.contains("proposedCode") && body["proposedCode"].is_string()) {
        request.originalCode = body["originalCode"].get<std::string>();
        request.proposedCode = body["proposedCode"].get<std::string>();
        return true;
    }

    setJsonError(
        response,
        400,
        "INVALID_REQUEST",
        "Request must contain exactly proposalId or originalCode and proposedCode string fields."
    );
    return false;
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
        GuiHttpServer::GenerationHandler generationHandler,
        GuiHttpServer::RefactoringHandler refactoringHandler
    )
        : requestedPort_(requestedPort),
          launchWorkingDirectory_(resolveLaunchDirectory(
              std::move(launchWorkingDirectory)
          )),
          activeOutputDirectory_(launchWorkingDirectory_),
          generationHandler_(std::move(generationHandler)),
          refactoringHandler_(std::move(refactoringHandler)) {
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

        server_.Post("/api/refactor", [this](const httplib::Request& request, httplib::Response& response) {
            if (!isJsonContentType(request.get_header_value("Content-Type"))) {
                setJsonError(
                    response,
                    415,
                    "UNSUPPORTED_MEDIA_TYPE",
                    "Content-Type must be application/json."
                );
                return;
            }

            RefactoringRequest refactoringRequest;
            if (!parseRefactoringRequest(
                    request.body,
                    refactoringRequest,
                    response)) {
                return;
            }
            if (!refactoringHandler_) {
                setJsonError(
                    response,
                    503,
                    "REFACTOR_UNAVAILABLE",
                    "Code refactoring is unavailable."
                );
                return;
            }

            std::string proposedCode;
            try {
                if (!refactoringHandler_(
                        refactoringRequest.instruction,
                        refactoringRequest.language,
                        refactoringRequest.sourceCode,
                        proposedCode) ||
                    isWhitespaceOnly(proposedCode)) {
                    setJsonError(
                        response,
                        502,
                        "REFACTOR_FAILED",
                        "Code refactoring failed or returned no complete code."
                    );
                    return;
                }
            } catch (...) {
                setJsonError(
                    response,
                    502,
                    "REFACTOR_FAILED",
                    "Code refactoring failed."
                );
                return;
            }

            const std::string baseEditorHash =
                hashEditorContent(refactoringRequest.sourceCode);
            std::string proposalId;
            std::string serializedResponse;
            try {
                std::lock_guard<std::mutex> lock(proposalMutex_);
                proposalId = "prop-" + std::to_string(nextProposalId_++);
                const Json result = {
                    {"proposalId", proposalId},
                    {"status", "PENDING"},
                    {"originalCode", refactoringRequest.sourceCode},
                    {"proposedCode", proposedCode},
                    {"baseEditorHash", baseEditorHash}
                };
                serializedResponse = result.dump();
                proposals_.emplace(
                    proposalId,
                    PendingProposal{
                        refactoringRequest.sourceCode,
                        proposedCode,
                        refactoringRequest.editorRevision,
                        baseEditorHash,
                        ProposalStatus::Pending
                    }
                );
            } catch (...) {
                setJsonError(
                    response,
                    502,
                    "REFACTOR_FAILED",
                    "Code refactoring result could not be prepared."
                );
                return;
            }

            response.set_content(std::move(serializedResponse), kJsonContentType);
        });

        server_.Post("/api/diff", [this](const httplib::Request& request, httplib::Response& response) {
            if (!isJsonContentType(request.get_header_value("Content-Type"))) {
                setJsonError(
                    response,
                    415,
                    "UNSUPPORTED_MEDIA_TYPE",
                    "Content-Type must be application/json."
                );
                return;
            }

            DiffRequest diffRequest;
            if (!parseDiffRequest(request.body, diffRequest, response)) {
                return;
            }

            std::string originalCode = std::move(diffRequest.originalCode);
            std::string proposedCode = std::move(diffRequest.proposedCode);
            if (diffRequest.usesProposal) {
                std::lock_guard<std::mutex> lock(proposalMutex_);
                const auto proposal = proposals_.find(diffRequest.proposalId);
                if (proposal == proposals_.end()) {
                    setJsonError(
                        response,
                        404,
                        "PROPOSAL_NOT_FOUND",
                        "Proposal was not found."
                    );
                    return;
                }
                originalCode = proposal->second.originalCode;
                proposedCode = proposal->second.proposedCode;
            }

            try {
                response.set_content(
                    Json{{"diff", makeUnifiedDiff(originalCode, proposedCode)}}.dump(),
                    kJsonContentType
                );
            } catch (...) {
                setJsonError(
                    response,
                    500,
                    "DIFF_FAILED",
                    "Diff could not be generated."
                );
            }
        });

        server_.Post(R"(/api/changes/([^/]+)/(approve|reject))",
            [this](const httplib::Request& request, httplib::Response& response) {
                if (!request.body.empty()) {
                    setJsonError(
                        response,
                        400,
                        "INVALID_REQUEST",
                        "Approval and rejection requests must have an empty body."
                    );
                    return;
                }

                const std::string proposalId = request.matches[1].str();
                const std::string action = request.matches[2].str();
                const ProposalStatus nextStatus = action == "approve"
                    ? ProposalStatus::Approved
                    : ProposalStatus::Rejected;
                const char* statusText = action == "approve"
                    ? "APPROVED"
                    : "REJECTED";

                std::string serializedResponse;
                {
                    std::lock_guard<std::mutex> lock(proposalMutex_);
                    const auto proposal = proposals_.find(proposalId);
                    if (proposal == proposals_.end()) {
                        setJsonError(
                            response,
                            404,
                            "PROPOSAL_NOT_FOUND",
                            "Proposal was not found."
                        );
                        return;
                    }
                    if (proposal->second.status != ProposalStatus::Pending) {
                        setJsonError(
                            response,
                            409,
                            "INVALID_PROPOSAL_STATE",
                            "Only pending proposals can be approved or rejected."
                        );
                        return;
                    }
                    try {
                        serializedResponse = Json{
                            {"proposalId", proposalId},
                            {"status", statusText}
                        }.dump();
                    } catch (...) {
                        setJsonError(
                            response,
                            500,
                            "STATE_UPDATE_FAILED",
                            "Proposal state could not be updated."
                        );
                        return;
                    }
                    proposal->second.status = nextStatus;
                }

                response.set_content(std::move(serializedResponse), kJsonContentType);
            }
        );

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
    enum class ProposalStatus {
        Pending,
        Approved,
        Rejected
    };

    struct PendingProposal {
        std::string originalCode;
        std::string proposedCode;
        std::string editorRevision;
        std::string baseEditorHash;
        ProposalStatus status = ProposalStatus::Pending;
    };

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
    const GuiHttpServer::RefactoringHandler refactoringHandler_;
    std::mutex proposalMutex_;
    std::unordered_map<std::string, PendingProposal> proposals_;
    std::uint64_t nextProposalId_ = 1;
    std::atomic<int> boundPort_{0};
    State state_ = State::Ready;
    std::mutex lifecycleMutex_;
    std::thread serverThread_;
};

GuiHttpServer::GuiHttpServer(int port)
    : GuiHttpServer(port, std::filesystem::current_path(), {}, {}) {}

GuiHttpServer::GuiHttpServer(
    int port,
    std::filesystem::path launchWorkingDirectory
)
    : GuiHttpServer(
          port,
          std::move(launchWorkingDirectory),
          {},
          {}
      ) {}

GuiHttpServer::GuiHttpServer(
    int port,
    std::filesystem::path launchWorkingDirectory,
    GenerationHandler generationHandler
)
    : GuiHttpServer(
          port,
          std::move(launchWorkingDirectory),
          std::move(generationHandler),
          {}
      ) {}

GuiHttpServer::GuiHttpServer(
    int port,
    std::filesystem::path launchWorkingDirectory,
    GenerationHandler generationHandler,
    RefactoringHandler refactoringHandler
)
    : impl_(std::make_unique<Impl>(
          port,
          std::move(launchWorkingDirectory),
          std::move(generationHandler),
          std::move(refactoringHandler)
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
