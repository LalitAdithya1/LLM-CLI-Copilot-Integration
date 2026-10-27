#include "GuiHttpServer.hpp"
#include "UnifiedDiff.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

constexpr const char* kLoopbackAddress = "127.0.0.1";
constexpr const char* kJsonContentType = "application/json; charset=utf-8";
constexpr const char* kAllowedGuiOrigin = "http://127.0.0.1:5500";
constexpr const char* kAllowedGuiLocalhostOrigin = "http://localhost:5500";
constexpr std::size_t kMaxRequestBodyBytes = 1024 * 1024;
constexpr std::size_t kMaximumRetainedProposals = 128;

using Json = nlohmann::json;

// The bundled frontend is served by this server, so its own loopback origin is
// allowed as well as the standalone development server on port 5500.
bool isAllowedGuiOrigin(const std::string& origin, int boundPort) {
    const std::string port = ":" + std::to_string(boundPort);
    return origin == kAllowedGuiOrigin || origin == kAllowedGuiLocalhostOrigin ||
           origin == "http://127.0.0.1" + port || origin == "http://localhost" + port;
}

const std::vector<std::string>& supportedExtensions();

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

bool isApiPath(const std::string& path) {
    return path == "/api" || path.compare(0, 5, "/api/") == 0;
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

bool hasSingleHeader(const httplib::Request& request, const std::string& name,
                     std::string& value) {
    auto range = request.headers.equal_range(name);
    if (range.first == range.second) return false;
    const auto first = range.first;
    ++range.first;
    if (range.first != range.second) return false;
    value = first->second;
    return true;
}

bool isAllowedPreflightHeaderList(const std::string& value) {
    std::size_t start = 0;
    bool sawContentType = false;
    while (start < value.size()) {
        const auto comma = value.find(',', start);
        auto token = value.substr(start, comma == std::string::npos ? comma : comma - start);
        const auto first = token.find_first_not_of(" \t");
        const auto last = token.find_last_not_of(" \t");
        if (first == std::string::npos) return false;
        token = token.substr(first, last - first + 1);
        std::transform(token.begin(), token.end(), token.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (token != "content-type" || sawContentType) return false;
        sawContentType = true;
        if (comma == std::string::npos) break;
        start = comma + 1;
        if (start == value.size()) return false;
    }
    return true;
}

struct SaveRequest {
    bool proposal = false;
    std::string proposalId;
    std::string fileName;
    std::string content;
    bool overwriteConfirmed = false;
};

bool parseSaveRequest(const std::string& requestBody, SaveRequest& request,
                      httplib::Response& response) {
    Json body;
    try { body = Json::parse(requestBody); }
    catch (const Json::exception&) {
        setJsonError(response, 400, "INVALID_JSON", "Request body must contain valid JSON."); return false;
    }
    if (!body.is_object()) {
        setJsonError(response, 400, "INVALID_REQUEST", "Request must be a generated-code or proposal save object."); return false;
    }
    if (body.contains("proposalId")) {
        if ((body.size() != 2 && body.size() != 3) || !body["proposalId"].is_string() ||
            !body.contains("fileName") || !body["fileName"].is_string() ||
            (body.size() == 3 && (!body.contains("overwriteConfirmed") || !body["overwriteConfirmed"].is_boolean()))) {
            setJsonError(response, 400, "INVALID_REQUEST", "Proposal save requires string proposalId and fileName fields, with optional boolean overwriteConfirmed."); return false;
        }
        request.proposal = true;
        request.proposalId = body["proposalId"].get<std::string>();
        request.fileName = body["fileName"].get<std::string>();
        request.overwriteConfirmed = body.contains("overwriteConfirmed") && body["overwriteConfirmed"].get<bool>();
        if (isWhitespaceOnly(request.proposalId)) {
            setJsonError(response, 400, "INVALID_REQUEST", "proposalId cannot be empty."); return false;
        }
    } else {
        if (body.size() != 3 || !body.contains("fileName") || !body["fileName"].is_string() ||
            !body.contains("content") || !body["content"].is_string() ||
            !body.contains("overwriteConfirmed") || !body["overwriteConfirmed"].is_boolean()) {
            setJsonError(response, 400, "INVALID_REQUEST", "Generated-code save requires only string fileName, string content, and boolean overwriteConfirmed fields."); return false;
        }
        request.fileName = body["fileName"].get<std::string>();
        request.content = body["content"].get<std::string>();
        request.overwriteConfirmed = body["overwriteConfirmed"].get<bool>();
    }
    if (isWhitespaceOnly(request.fileName) || (!request.proposal && isWhitespaceOnly(request.content))) {
        setJsonError(response, 400, "INVALID_REQUEST", "Required save values cannot be empty."); return false;
    }
    return true;
}

bool validSaveFileName(const std::string& name) {
    if (name.empty() || name == "." || name == ".." ||
        std::filesystem::path(name).filename().string() != name ||
        name.find('/') != std::string::npos || name.find('\\') != std::string::npos || name.find(':') != std::string::npos) return false;
    for (unsigned char c : name) if (c < 32 || c == 127) return false;
    const auto& extensions = supportedExtensions();
    return std::find(extensions.begin(), extensions.end(), std::filesystem::path(name).extension().string()) != extensions.end();
}

bool writeFileSafely(const std::filesystem::path& target, const std::string& content,
                     bool overwrite, std::error_code& error) {
    error.clear();
    std::error_code statusError;
    const auto status = std::filesystem::symlink_status(target, statusError);
    const bool exists = !statusError && status.type() != std::filesystem::file_type::not_found;
    if (statusError && statusError != std::errc::no_such_file_or_directory) { error = statusError; return false; }
    if (exists && (std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status))) {
        error = std::make_error_code(std::errc::invalid_argument); return false;
    }
    if (exists && !overwrite) { error = std::make_error_code(std::errc::file_exists); return false; }
    static std::atomic<std::uint64_t> sequence{0};
    std::filesystem::path temp;
#ifdef _WIN32
    HANDLE file = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 32 && file == INVALID_HANDLE_VALUE; ++attempt) {
        temp = target.parent_path() / (target.filename().string() + ".tmp-" + std::to_string(sequence.fetch_add(1)));
        file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (file == INVALID_HANDLE_VALUE) { error = std::error_code(GetLastError(), std::system_category()); return false; }
    std::size_t offset = 0;
    while (offset < content.size()) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(content.size() - offset, 1u << 20));
        if (!WriteFile(file, content.data() + offset, chunk, &written, nullptr) || written == 0) {
            const DWORD code = GetLastError(); error = std::error_code(code == ERROR_SUCCESS ? ERROR_WRITE_FAULT : code, std::system_category()); break;
        }
        offset += written;
    }
    if (!error && !FlushFileBuffers(file)) error = std::error_code(GetLastError(), std::system_category());
    CloseHandle(file);
#else
    int fd = -1;
    for (int attempt = 0; attempt < 32 && fd < 0; ++attempt) {
        temp = target.parent_path() / (target.filename().string() + ".tmp-" + std::to_string(sequence.fetch_add(1)));
        fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0 && errno != EEXIST) break;
    }
    if (fd < 0) { error = std::error_code(errno, std::generic_category()); return false; }
    std::size_t offset = 0;
    while (offset < content.size()) {
        const auto written = ::write(fd, content.data() + offset, content.size() - offset);
        if (written < 0) { if (errno == EINTR) continue; error = std::error_code(errno, std::generic_category()); break; }
        if (written == 0) { error = std::make_error_code(std::errc::io_error); break; }
        offset += static_cast<std::size_t>(written);
    }
    if (!error && ::fsync(fd) != 0) error = std::error_code(errno, std::generic_category());
    if (::close(fd) != 0 && !error) error = std::error_code(errno, std::generic_category());
#endif
    if (!error) {
#ifdef _WIN32
        const DWORD flags = MOVEFILE_WRITE_THROUGH | (exists ? MOVEFILE_REPLACE_EXISTING : 0);
        if (!MoveFileExW(temp.c_str(), target.c_str(), flags)) error = std::error_code(GetLastError(), std::system_category());
#else
        if (exists) std::filesystem::rename(temp, target, error);
        else if (::link(temp.c_str(), target.c_str()) != 0) error = std::error_code(errno, std::generic_category());
        else { std::error_code ignored; std::filesystem::remove(temp, ignored); }
#endif
    }
    if (error) { std::error_code ignored; std::filesystem::remove(temp, ignored); return false; }
    return true;
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

bool parseEmptyObject(
    const std::string& requestBody,
    httplib::Response& response
) {
    try {
        const Json body = Json::parse(requestBody);
        if (body.is_object() && body.empty()) {
            return true;
        }
    } catch (const Json::exception&) {
        setJsonError(response, 400, "INVALID_JSON", "Request body must contain valid JSON.");
        return false;
    }
    setJsonError(response, 400, "INVALID_REQUEST", "Request body must be an empty JSON object.");
    return false;
}

bool resolveSelectedSourcePath(
    const std::filesystem::path& selectedPath,
    std::filesystem::path& normalizedPath
) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(selectedPath, error) || error) {
        return false;
    }
    const std::string extension = selectedPath.extension().string();
    const auto& extensions = supportedExtensions();
    if (std::find(extensions.begin(), extensions.end(), extension) == extensions.end()) {
        return false;
    }
    normalizedPath = std::filesystem::absolute(selectedPath, error).lexically_normal();
    return !error;
}

bool resolveSelectedDirectoryPath(
    const std::filesystem::path& selectedPath,
    std::filesystem::path& normalizedPath
) {
    std::error_code error;
    if (!std::filesystem::is_directory(selectedPath, error) || error) {
        return false;
    }
    normalizedPath = std::filesystem::absolute(selectedPath, error).lexically_normal();
    return !error;
}

void setDialogError(
    httplib::Response& response,
    const NativeDialogStatus status,
    const char* dialogKind
) {
    switch (status) {
        case NativeDialogStatus::Unavailable:
            setJsonError(response, 503, "DIALOG_UNAVAILABLE", dialogKind);
            return;
        case NativeDialogStatus::TimedOut:
            setJsonError(response, 504, "DIALOG_TIMEOUT", dialogKind);
            return;
        case NativeDialogStatus::Failed:
            setJsonError(response, 500, "DIALOG_FAILED", dialogKind);
            return;
        case NativeDialogStatus::Selected:
        case NativeDialogStatus::Cancelled:
            break;
    }
    setJsonError(response, 500, "DIALOG_FAILED", dialogKind);
}

} // namespace

class GuiHttpServer::Impl {
    enum class ProposalStatus {
        Pending,
        Approved,
        Rejected,
        Applied
    };

    struct PendingProposal {
        std::string originalCode;
        std::string proposedCode;
        std::string editorRevision;
        std::string baseEditorHash;
        ProposalStatus status = ProposalStatus::Pending;
        std::uint64_t sequence = 0;
    };

    struct ProposalCapacityReservation {
        Impl* owner = nullptr;
        std::optional<std::string> evictionId;
        bool active = false;

        ProposalCapacityReservation() = default;
        explicit ProposalCapacityReservation(Impl* server) : owner(server) {}
        ProposalCapacityReservation(const ProposalCapacityReservation&) = delete;
        ProposalCapacityReservation& operator=(const ProposalCapacityReservation&) = delete;
        ~ProposalCapacityReservation() {
            if (active) owner->releaseProposalCapacity(*this);
        }
    };

public:
    explicit Impl(
        int requestedPort,
        std::filesystem::path launchWorkingDirectory,
        GuiHttpServer::GenerationHandler generationHandler,
        GuiHttpServer::RefactoringHandler refactoringHandler,
        std::shared_ptr<NativeDialogProvider> dialogProvider
    )
        : requestedPort_(requestedPort),
          launchWorkingDirectory_(resolveLaunchDirectory(
              std::move(launchWorkingDirectory)
          )),
          activeOutputDirectory_(launchWorkingDirectory_),
          generationHandler_(std::move(generationHandler)),
          refactoringHandler_(std::move(refactoringHandler)),
          dialogProvider_(dialogProvider ? std::move(dialogProvider)
                                         : createNativeDialogProvider()) {
        server_.Get("/api/health", [](const httplib::Request&, httplib::Response& response) {
            response.set_content(
                nlohmann::json{{"status", "ok"}}.dump(),
                kJsonContentType
            );
        });

        server_.Get("/api/config", [this](const httplib::Request&, httplib::Response& response) {
            std::lock_guard<std::mutex> lock(configMutex_);
            const nlohmann::json config = {
                {"supportedExtensions", supportedExtensions()},
                {"launchDirectory", launchWorkingDirectory_.string()},
                {"activeOutputDirectory", activeOutputDirectory_.string()}
            };
            response.set_content(config.dump(), kJsonContentType);
        });

        server_.Post("/api/files/open-dialog", [this](const httplib::Request& request, httplib::Response& response) {
            if (!isJsonContentType(request.get_header_value("Content-Type"))) {
                setJsonError(response, 415, "UNSUPPORTED_MEDIA_TYPE", "Content-Type must be application/json.");
                return;
            }
            if (!parseEmptyObject(request.body, response)) {
                return;
            }

            NativeDialogResult result;
            try {
                result = dialogProvider_->choose(NativeDialogKind::SourceFile);
            } catch (...) {
                setDialogError(response, NativeDialogStatus::Failed, "Native file dialog failed.");
                return;
            }
            if (result.status == NativeDialogStatus::Cancelled) {
                response.set_content(Json{{"status", "CANCELLED"}}.dump(), kJsonContentType);
                return;
            }
            if (result.status != NativeDialogStatus::Selected) {
                const char* message = result.status == NativeDialogStatus::Unavailable
                    ? "Native file dialog is unavailable."
                    : result.status == NativeDialogStatus::TimedOut
                        ? "Native file dialog timed out."
                        : "Native file dialog failed.";
                setDialogError(response, result.status, message);
                return;
            }

            std::filesystem::path selectedPath;
            if (!resolveSelectedSourcePath(result.selectedPath, selectedPath)) {
                setJsonError(response, 422, "INVALID_SOURCE_FILE", "Selected path is not a supported source file.");
                return;
            }
            response.set_content(
                Json{{"status", "SELECTED"}, {"selectedPath", selectedPath.string()}}.dump(),
                kJsonContentType
            );
        });

        server_.Post("/api/output/select-directory", [this](const httplib::Request& request, httplib::Response& response) {
            if (!isJsonContentType(request.get_header_value("Content-Type"))) {
                setJsonError(response, 415, "UNSUPPORTED_MEDIA_TYPE", "Content-Type must be application/json.");
                return;
            }

            bool useDialog = false;
            std::filesystem::path selectedPath;
            try {
                const Json body = Json::parse(request.body);
                if (!body.is_object()) {
                    setJsonError(response, 400, "INVALID_REQUEST", "Request must be an empty object or contain one directoryPath string.");
                    return;
                }
                if (body.empty()) {
                    useDialog = true;
                } else if (body.size() == 1 && body.contains("directoryPath") &&
                           body["directoryPath"].is_string()) {
                    const std::string path = body["directoryPath"].get<std::string>();
                    if (isWhitespaceOnly(path)) {
                        setJsonError(response, 400, "INVALID_DIRECTORY_PATH", "directoryPath cannot be empty.");
                        return;
                    }
                    selectedPath = std::filesystem::path(path);
                } else {
                    setJsonError(response, 400, "INVALID_REQUEST", "Request must be an empty object or contain one directoryPath string.");
                    return;
                }
            } catch (const Json::exception&) {
                setJsonError(response, 400, "INVALID_JSON", "Request body must contain valid JSON.");
                return;
            }

            if (useDialog) {
                NativeDialogResult result;
                try {
                    result = dialogProvider_->choose(NativeDialogKind::OutputDirectory);
                } catch (...) {
                    setDialogError(response, NativeDialogStatus::Failed, "Native directory dialog failed.");
                    return;
                }
                if (result.status == NativeDialogStatus::Cancelled) {
                    response.set_content(Json{{"status", "CANCELLED"}}.dump(), kJsonContentType);
                    return;
                }
                if (result.status != NativeDialogStatus::Selected) {
                    const char* message = result.status == NativeDialogStatus::Unavailable
                        ? "Native directory dialog is unavailable."
                        : result.status == NativeDialogStatus::TimedOut
                            ? "Native directory dialog timed out."
                            : "Native directory dialog failed.";
                    setDialogError(response, result.status, message);
                    return;
                }
                selectedPath = std::move(result.selectedPath);
            }

            std::filesystem::path normalizedPath;
            if (!resolveSelectedDirectoryPath(selectedPath, normalizedPath)) {
                setJsonError(response, 422, "INVALID_OUTPUT_DIRECTORY", "Selected path is not an existing directory.");
                return;
            }
            {
                std::lock_guard<std::mutex> lock(configMutex_);
                activeOutputDirectory_ = normalizedPath;
            }
            response.set_content(
                Json{{"status", "SELECTED"}, {"activeOutputDirectory", normalizedPath.string()}}.dump(),
                kJsonContentType
            );
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

        server_.Post("/api/save", [this](const httplib::Request& request, httplib::Response& response) {
            if (!isJsonContentType(request.get_header_value("Content-Type"))) {
                setJsonError(response, 415, "UNSUPPORTED_MEDIA_TYPE", "Content-Type must be application/json."); return;
            }
            SaveRequest save;
            if (!parseSaveRequest(request.body, save, response)) return;
            if (!validSaveFileName(save.fileName)) {
                setJsonError(response, 400, "INVALID_FILENAME", "fileName must be a supported source filename without a directory path."); return;
            }
            std::filesystem::path outputDirectory;
            { std::lock_guard<std::mutex> lock(configMutex_); outputDirectory = activeOutputDirectory_; }
            std::error_code error;
            const auto canonicalDirectory = std::filesystem::canonical(outputDirectory, error);
            if (error || !std::filesystem::is_directory(canonicalDirectory, error) || error) {
                setJsonError(response, 422, "INVALID_OUTPUT_DIRECTORY", "Output directory is unavailable."); return;
            }
            const auto target = canonicalDirectory / save.fileName;
            std::string code = std::move(save.content);
            std::unique_lock<std::mutex> proposalLock(proposalMutex_, std::defer_lock);
            if (save.proposal) {
                proposalLock.lock();
                const auto proposal = proposals_.find(save.proposalId);
                if (proposal == proposals_.end()) { setJsonError(response, 404, "PROPOSAL_NOT_FOUND", "Proposal was not found."); return; }
                if (proposal->second.status != ProposalStatus::Approved) { setJsonError(response, 409, "INVALID_PROPOSAL_STATE", "Only approved proposals can be saved."); return; }
                code = proposal->second.proposedCode;
            }
            std::lock_guard<std::mutex> saveLock(saveMutex_);
            const auto parent = std::filesystem::canonical(target.parent_path(), error);
            if (error || parent != canonicalDirectory) { setJsonError(response, 422, "INVALID_OUTPUT_DIRECTORY", "Output directory is unavailable."); return; }
            const auto state = std::filesystem::symlink_status(target, error);
            if (error && error != std::errc::no_such_file_or_directory) { setJsonError(response, 500, "SAVE_FAILED", "File could not be saved."); return; }
            const bool exists = !error && state.type() != std::filesystem::file_type::not_found;
            if (exists && (std::filesystem::is_symlink(state) || !std::filesystem::is_regular_file(state))) {
                setJsonError(response, 422, "INVALID_TARGET", "Target must be a regular file and cannot be a symbolic link."); return;
            }
            if (exists && !save.overwriteConfirmed) { setJsonError(response, 409, "OVERWRITE_CONFIRMATION_REQUIRED", "Set overwriteConfirmed to true to replace the existing file."); return; }
            if (!writeFileSafely(target, code, save.overwriteConfirmed, error)) {
                const bool confirmation = error == std::errc::file_exists;
                setJsonError(response, confirmation ? 409 : 500,
                    confirmation ? "OVERWRITE_CONFIRMATION_REQUIRED" : "SAVE_FAILED",
                    confirmation ? "Set overwriteConfirmed to true to replace the existing file." : "File could not be saved."); return;
            }
            if (save.proposal) proposals_.at(save.proposalId).status = ProposalStatus::Applied;
            response.set_content(Json{{"savedPath", target.string()}, {"status", "SAVED"}}.dump(), kJsonContentType);
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

            ProposalCapacityReservation reservation(this);
            if (!reserveProposalCapacity(reservation, response)) return;

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
                if (reservation.evictionId) {
                    const auto eviction = proposals_.find(*reservation.evictionId);
                    if (eviction == proposals_.end() ||
                        (eviction->second.status != ProposalStatus::Rejected &&
                         eviction->second.status != ProposalStatus::Applied)) {
                        setJsonError(response, 503, "PROPOSAL_CAPACITY", "Proposal capacity is full; resolve or save existing proposals before creating another.");
                        return;
                    }
                } else if (proposals_.size() >= kMaximumRetainedProposals) {
                    setJsonError(response, 503, "PROPOSAL_CAPACITY", "Proposal capacity is full; resolve or save existing proposals before creating another.");
                    return;
                }
                const std::uint64_t proposalSequence = nextProposalId_;
                proposalId = "prop-" + std::to_string(proposalSequence);
                const Json result = {
                    {"proposalId", proposalId},
                    {"status", "PENDING"},
                    {"originalCode", refactoringRequest.sourceCode},
                    {"proposedCode", proposedCode},
                    {"baseEditorHash", baseEditorHash}
                };
                serializedResponse = result.dump();
                std::map<std::string, PendingProposal> staged;
                staged.emplace(
                    proposalId,
                    PendingProposal{
                        refactoringRequest.sourceCode,
                        proposedCode,
                        refactoringRequest.editorRevision,
                        baseEditorHash,
                        ProposalStatus::Pending,
                        proposalSequence
                    }
                );
                auto stagedNode = staged.extract(proposalId);
                if (reservation.evictionId) {
                    auto evictedNode = proposals_.extract(*reservation.evictionId);
                    try {
                        const auto inserted = proposals_.insert(std::move(stagedNode));
                        if (!inserted.inserted) {
                            proposals_.insert(std::move(evictedNode));
                            setJsonError(response, 502, "REFACTOR_FAILED", "Code refactoring result could not be prepared.");
                            return;
                        }
                    } catch (...) {
                        if (!evictedNode.empty()) proposals_.insert(std::move(evictedNode));
                        throw;
                    }
                } else {
                    const auto inserted = proposals_.insert(std::move(stagedNode));
                    if (!inserted.inserted) {
                        setJsonError(response, 502, "REFACTOR_FAILED", "Code refactoring result could not be prepared.");
                        return;
                    }
                }
                ++nextProposalId_;
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

        server_.set_pre_routing_handler([this](const httplib::Request& request,
                                               httplib::Response& response) {
            if (!isApiPath(request.path)) {
                return httplib::Server::HandlerResponse::Unhandled;
            }

            const std::string expectedPort = ":" + std::to_string(boundPort_);
            std::string host;
            if (!hasSingleHeader(request, "Host", host) ||
                (host != std::string("127.0.0.1") + expectedPort &&
                 host != std::string("localhost") + expectedPort)) {
                setJsonError(response, 400, "INVALID_HOST", "Host is not allowed.");
                return httplib::Server::HandlerResponse::Handled;
            }

            response.set_header("Vary", "Origin");
            std::string origin;
            const auto originRange = request.headers.equal_range("Origin");
            const bool hasOrigin = originRange.first != originRange.second;
            if (hasOrigin) {
                if (!hasSingleHeader(request, "Origin", origin) ||
                    !isAllowedGuiOrigin(origin, boundPort_)) {
                    setJsonError(response, 403, "ORIGIN_NOT_ALLOWED", "Request origin is not allowed.");
                    return httplib::Server::HandlerResponse::Handled;
                }
            }

            if (request.method == "OPTIONS") {
                std::string requestedMethod;
                std::string requestedHeaders;
                if (!hasOrigin || !hasSingleHeader(request, "Access-Control-Request-Method", requestedMethod) ||
                    (requestedMethod != "GET" && requestedMethod != "POST") ||
                    (request.headers.count("Access-Control-Request-Headers") > 1) ||
                    (request.headers.count("Access-Control-Request-Headers") == 1 &&
                     (!hasSingleHeader(request, "Access-Control-Request-Headers", requestedHeaders) ||
                      !isAllowedPreflightHeaderList(requestedHeaders)))) {
                    setJsonError(response, 403, "INVALID_PREFLIGHT", "Preflight request is not allowed.");
                    return httplib::Server::HandlerResponse::Handled;
                }
                response.set_header("Access-Control-Allow-Origin", origin);
                response.status = 204;
                response.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
                response.set_header("Access-Control-Allow-Headers", "Content-Type");
                response.set_header("Access-Control-Max-Age", "600");
                return httplib::Server::HandlerResponse::Handled;
            }
            if (hasOrigin) response.set_header("Access-Control-Allow-Origin", origin);
            return httplib::Server::HandlerResponse::Unhandled;
        });

        server_.set_payload_max_length(kMaxRequestBodyBytes);
        server_.set_error_handler([](const httplib::Request& request, httplib::Response& response) {
            if (!isApiPath(request.path)) return;
            if (response.status == 413 && response.body.empty()) {
                setJsonError(
                    response,
                    413,
                    "PAYLOAD_TOO_LARGE",
                    "Request body exceeds the 1 MiB limit."
                );
            } else if (response.body.empty() && response.status == 404) {
                setJsonError(response, 404, "NOT_FOUND", "API endpoint was not found.");
            } else if (response.body.empty() && response.status == 405) {
                setJsonError(response, 405, "METHOD_NOT_ALLOWED", "HTTP method is not supported for this endpoint.");
            } else if (response.body.empty() && response.status >= 400) {
                setJsonError(response, response.status, "INVALID_HTTP_REQUEST", "The API request could not be processed.");
            }
        });
        server_.set_exception_handler([](const httplib::Request& request,
                                         httplib::Response& response,
                                         std::exception_ptr) {
            if (isApiPath(request.path)) {
                setJsonError(response, 500, "INTERNAL_ERROR", "The API request could not be completed.");
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

    bool serveFrontend(const std::filesystem::path& directory) {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        if (state_ != State::Ready) {
            return false;
        }
        // cpp-httplib rejects ".." segments and symlinks escaping the
        // directory. The policy keeps the page and its requests same-origin.
        return server_.set_mount_point("/", directory.string(), {
            {"X-Content-Type-Options", "nosniff"},
            {"Cache-Control", "no-store"},
            {"Referrer-Policy", "no-referrer"},
            {"Content-Security-Policy",
             "default-src 'self'; img-src 'self' data:; connect-src 'self'; "
             "object-src 'none'; base-uri 'none'; form-action 'none'; "
             "frame-ancestors 'none'"}
        });
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
    // Count in-flight creation leases with stored proposals so parallel provider
    // calls cannot claim the same final slot. A terminal candidate is pinned
    // until commit or lease destruction; provider calls run without this mutex.
    bool reserveProposalCapacity(ProposalCapacityReservation& reservation,
                                 httplib::Response& response) {
        std::lock_guard<std::mutex> lock(proposalMutex_);
        std::optional<std::string> evictionId;
        if (proposals_.size() + proposalReservations_ >= kMaximumRetainedProposals) {
            auto oldestTerminal = proposals_.end();
            for (auto proposal = proposals_.begin(); proposal != proposals_.end(); ++proposal) {
                const bool terminal = proposal->second.status == ProposalStatus::Rejected ||
                                      proposal->second.status == ProposalStatus::Applied;
                if (terminal && reservedEvictions_.count(proposal->first) == 0 &&
                    (oldestTerminal == proposals_.end() ||
                     proposal->second.sequence < oldestTerminal->second.sequence)) {
                    oldestTerminal = proposal;
                }
            }
            if (oldestTerminal == proposals_.end()) {
                setJsonError(response, 503, "PROPOSAL_CAPACITY", "Proposal capacity is full; resolve or save existing proposals before creating another.");
                return false;
            }
            evictionId = oldestTerminal->first;
        }
        if (evictionId) reservedEvictions_.insert(*evictionId);
        reservation.evictionId = std::move(evictionId);
        ++proposalReservations_;
        reservation.active = true;
        return true;
    }

    void releaseProposalCapacity(ProposalCapacityReservation& reservation) {
        std::lock_guard<std::mutex> lock(proposalMutex_);
        if (reservation.evictionId) reservedEvictions_.erase(*reservation.evictionId);
        if (proposalReservations_ > 0) --proposalReservations_;
        reservation.active = false;
    }

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
    const std::shared_ptr<NativeDialogProvider> dialogProvider_;
    std::mutex configMutex_;
    std::mutex proposalMutex_;
    std::mutex saveMutex_;
    std::map<std::string, PendingProposal> proposals_;
    std::unordered_set<std::string> reservedEvictions_;
    std::size_t proposalReservations_ = 0;
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
    : GuiHttpServer(
          port,
          std::move(launchWorkingDirectory),
          std::move(generationHandler),
          std::move(refactoringHandler),
          {}
      ) {}

GuiHttpServer::GuiHttpServer(
    int port,
    std::filesystem::path launchWorkingDirectory,
    GenerationHandler generationHandler,
    RefactoringHandler refactoringHandler,
    std::shared_ptr<NativeDialogProvider> dialogProvider
)
    : impl_(std::make_unique<Impl>(
          port,
          std::move(launchWorkingDirectory),
          std::move(generationHandler),
          std::move(refactoringHandler),
          std::move(dialogProvider)
      )) {}

GuiHttpServer::~GuiHttpServer() = default;

bool GuiHttpServer::start() {
    return impl_->start();
}

bool GuiHttpServer::serveFrontend(const std::filesystem::path& directory) {
    return impl_->serveFrontend(directory);
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
