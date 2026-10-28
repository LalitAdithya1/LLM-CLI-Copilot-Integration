#include "GuiHttpServer.hpp"
#include "CodeAgentError.hpp"
#include "UnifiedDiff.hpp"

#include <iostream>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <system_error>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt")
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <fstream>
#include <iterator>

namespace {

constexpr const char* kLoopbackAddress = "127.0.0.1";
constexpr const char* kJsonContentType = "application/json; charset=utf-8";
constexpr std::size_t kMaxRequestBodyBytes = 1024 * 1024;
constexpr std::size_t kMaximumRetainedProposals = 128;
// A pending or approved proposal untouched for this long may be evicted when
// a new proposal needs its slot. It stays usable until then.
constexpr std::chrono::minutes kProposalIdleExpiry{30};
constexpr std::chrono::minutes kBootstrapCodeLifetime{5};
constexpr std::size_t kMaximumBootstrapCodes = 8;
constexpr std::size_t kSecretBytes = 32;
constexpr const char* kSessionPath = "/api/session";

using Json = nlohmann::json;

// Reads cryptographically secure random bytes from the operating system.
bool fillRandomBytes(unsigned char* buffer, std::size_t size) {
#ifdef _WIN32
    return BCRYPT_SUCCESS(BCryptGenRandom(
        nullptr, buffer, static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
#else
    std::size_t filled = 0;
    while (filled < size) {
        const auto count = ::getrandom(buffer + filled, size - filled, 0);
        if (count < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        filled += static_cast<std::size_t>(count);
    }
    return true;
#endif
}

// Returns kSecretBytes random bytes as lowercase hex, or an empty string if
// the system random source failed.
std::string randomSecret() {
    unsigned char bytes[kSecretBytes];
    if (!fillRandomBytes(bytes, sizeof bytes)) return {};
    std::ostringstream hex;
    hex << std::hex << std::setfill('0');
    for (unsigned char byte : bytes) hex << std::setw(2) << static_cast<int>(byte);
    return hex.str();
}

// Compares secrets without stopping at the first difference. Secrets have a
// fixed, public length, so only a length mismatch returns early.
bool constantTimeEquals(const std::string& actual, const std::string& expected) {
    if (actual.size() != expected.size() || expected.empty()) return false;
    unsigned char difference = 0;
    for (std::size_t index = 0; index < expected.size(); ++index) {
        difference |= static_cast<unsigned char>(actual[index] ^ expected[index]);
    }
    return difference == 0;
}

// Only exact http loopback origins with a port may be configured.
bool isLoopbackOrigin(const std::string& origin) {
    for (const std::string prefix : {"http://127.0.0.1:", "http://localhost:"}) {
        if (origin.compare(0, prefix.size(), prefix) != 0) continue;
        const std::string port = origin.substr(prefix.size());
        if (port.empty() || port.size() > 5 ||
            !std::all_of(port.begin(), port.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
            return false;
        }
        const int value = std::stoi(port);
        return value >= 1 && value <= 65535 && port[0] != '0';
    }
    return false;
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

// Reports a CodeAgentError through its virtual code() and httpStatus(), so
// one handler covers every error type. The safe message goes to the browser;
// the sanitized diagnostic only to the terminal.
void respondWithError(httplib::Response& response, const CodeAgentError& error) {
    logError(std::cerr, error);
    setJsonError(response, error.httpStatus(), error.code(), error.what());
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

// A preflight may announce Content-Type and the session-token header, each
// at most once, and nothing else.
bool isAllowedPreflightHeaderList(const std::string& value) {
    std::size_t start = 0;
    bool sawContentType = false;
    bool sawToken = false;
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
        if (token == "content-type" && !sawContentType) {
            sawContentType = true;
        } else if (token == "x-codeagent-token" && !sawToken) {
            sawToken = true;
        } else {
            return false;
        }
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

// Writes through a temporary file and an atomic rename. The system calls
// report failures as error codes; writeFileSafely turns them into exceptions.
bool tryWriteFileAtomically(const std::filesystem::path& target, const std::string& content,
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
    // A replaced file keeps its read/write/execute bits instead of the
    // temporary 0600, but never setuid, setgid, or sticky bits.
    if (exists) (void)::fchmod(fd, static_cast<mode_t>(status.permissions()) & 0777);
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

// Throws FilePermissionError, FileNotFoundError, or FileError when the file
// cannot be written. errorCode() is std::errc::file_exists when the target
// appeared although overwrite is false.
void writeFileSafely(const std::filesystem::path& target, const std::string& content,
                     bool overwrite) {
    std::error_code error;
    if (!tryWriteFileAtomically(target, content, overwrite, error)) {
        throwFileSystemError(FileOperation::Write, target, error);
    }
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

bool isWritableDirectory(const std::filesystem::path& directory) {
#ifdef _WIN32
    (void)directory;
    return true;
#else
    return ::access(directory.c_str(), W_OK | X_OK) == 0;
#endif
}

bool hasSupportedExtension(const std::filesystem::path& path) {
    const auto& extensions = supportedExtensions();
    return std::find(extensions.begin(), extensions.end(), path.extension().string()) !=
           extensions.end();
}

enum class ReadResult { Ok, Failed, TooLarge };

// Reads at most maximumBytes from a regular file. The size is enforced while
// reading, so a file that grows after an earlier size check cannot make the
// read unbounded, and a path swapped for a FIFO or device is refused without
// blocking.
#ifndef _WIN32
// Owns a POSIX file descriptor and closes it when it goes out of scope, also
// when reading throws (for example std::bad_alloc while appending).
class FileDescriptor {
public:
    explicit FileDescriptor(int fd) noexcept : fd_(fd) {}
    ~FileDescriptor() {
        if (fd_ >= 0) ::close(fd_);
    }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    int get() const noexcept { return fd_; }
    bool valid() const noexcept { return fd_ >= 0; }

private:
    int fd_;
};
#endif

// Reads at most maximumBytes from a regular file. The size is enforced while
// reading, so a file that grows after an earlier size check cannot make the
// read unbounded, and a path swapped for a FIFO or device is refused without
// blocking. On Failed, error (when given) holds the operating-system cause.
ReadResult readRegularFile(const std::filesystem::path& path, std::uintmax_t maximumBytes,
                           std::string& content, std::error_code* error = nullptr) {
    content.clear();
    std::error_code ignored;
    std::error_code& failure = error != nullptr ? *error : ignored;
    failure.clear();
#ifdef _WIN32
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        failure = std::make_error_code(std::errc::permission_denied);
        return ReadResult::Failed;
    }
    char buffer[64 * 1024];
    while (input) {
        input.read(buffer, sizeof buffer);
        content.append(buffer, static_cast<std::size_t>(input.gcount()));
        if (content.size() > maximumBytes) return ReadResult::TooLarge;
    }
    if (input.bad()) {
        failure = std::make_error_code(std::errc::io_error);
        return ReadResult::Failed;
    }
    return ReadResult::Ok;
#else
    const FileDescriptor fd(::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    if (!fd.valid()) {
        failure = std::error_code(errno, std::generic_category());
        return ReadResult::Failed;
    }
    struct stat info {};
    if (::fstat(fd.get(), &info) != 0) {
        failure = std::error_code(errno, std::generic_category());
        return ReadResult::Failed;
    }
    if (!S_ISREG(info.st_mode)) {
        failure = std::make_error_code(std::errc::invalid_argument);
        return ReadResult::Failed;
    }
    char buffer[64 * 1024];
    for (;;) {
        const auto count = ::read(fd.get(), buffer, sizeof buffer);
        if (count < 0) {
            if (errno == EINTR) continue;
            failure = std::error_code(errno, std::generic_category());
            return ReadResult::Failed;
        }
        if (count == 0) return ReadResult::Ok;
        content.append(buffer, static_cast<std::size_t>(count));
        if (content.size() > maximumBytes) return ReadResult::TooLarge;
    }
#endif
}

// Source files opened in the editor stay well under the 1 MiB request limit,
// so their edited contents can still be sent back to be saved or refactored.
constexpr std::uintmax_t kMaximumEditorFileBytes = 512 * 1024;
constexpr std::size_t kMaximumOpenedDocuments = 256;

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
        // Updated by every request that uses the proposal.
        std::chrono::steady_clock::time_point lastActivity{};
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
                                         : createNativeDialogProvider()),
          sessionToken_(randomSecret()),
          refreshSecret_(randomSecret()) {
        // Exchanges a single-use bootstrap code, or the session cookie set by
        // an earlier exchange (e.g. after a page reload), for the session
        // token. This is the only API route that works without the token.
        server_.Post(kSessionPath, [this](const httplib::Request& request, httplib::Response& response) {
            response.set_header("Cache-Control", "no-store");
            if (!isJsonContentType(request.get_header_value("Content-Type"))) {
                setJsonError(response, 415, "UNSUPPORTED_MEDIA_TYPE", "Content-Type must be application/json.");
                return;
            }
            std::optional<std::string> bootstrapCode;
            try {
                const Json body = Json::parse(request.body);
                if (!body.is_object() || body.size() > 1 ||
                    (body.size() == 1 && (!body.contains("bootstrap") || !body["bootstrap"].is_string()))) {
                    setJsonError(response, 400, "INVALID_REQUEST", "Request must be an empty object or contain one bootstrap string.");
                    return;
                }
                if (body.size() == 1) bootstrapCode = body["bootstrap"].get<std::string>();
            } catch (const Json::exception&) {
                setJsonError(response, 400, "INVALID_JSON", "Request body must contain valid JSON.");
                return;
            }

            if (bootstrapCode) {
                if (!redeemBootstrapCode(*bootstrapCode)) {
                    setJsonError(response, 401, "INVALID_BOOTSTRAP",
                        "This startup link is invalid, expired, or already used. Restart codeagent gui to get a new link.");
                    return;
                }
                // HttpOnly and limited to this path, so page scripts cannot
                // read it and it is sent nowhere else.
                response.set_header("Set-Cookie", sessionCookieName() + "=" + refreshSecret_ +
                    "; Path=" + kSessionPath + "; HttpOnly; SameSite=Strict");
            } else if (!hasValidSessionCookie(request)) {
                setJsonError(response, 401, "SESSION_REQUIRED",
                    "This page is not connected. Open the link that codeagent gui opened or printed.");
                return;
            }
            response.set_content(Json{{"token", sessionToken_}}.dump(), kJsonContentType);
        });

        server_.Get("/api/health", [](const httplib::Request&, httplib::Response& response) {
            response.set_content(
                nlohmann::json{{"status", "ok"}}.dump(),
                kJsonContentType
            );
        });

        server_.Get("/api/config", [this](const httplib::Request&, httplib::Response& response) {
            std::lock_guard<std::mutex> lock(configMutex_);
            const nlohmann::json config = {
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
                logUnexpectedException(std::cerr, "the native file dialog");
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

        // Opens a source file chosen by the user (dialog result or typed path)
        // for editing. Its canonical path is remembered so that the editor can
        // later save back to exactly that file and no other.
        server_.Post("/api/files/read", [this](const httplib::Request& request, httplib::Response& response) {
            if (!isJsonContentType(request.get_header_value("Content-Type"))) {
                setJsonError(response, 415, "UNSUPPORTED_MEDIA_TYPE", "Content-Type must be application/json.");
                return;
            }
            std::string requestedPath;
            try {
                const Json body = Json::parse(request.body);
                if (!body.is_object() || body.size() != 1 || !body.contains("path") ||
                    !body["path"].is_string()) {
                    setJsonError(response, 400, "INVALID_REQUEST", "Request must contain only a string path field.");
                    return;
                }
                requestedPath = body["path"].get<std::string>();
            } catch (const Json::exception&) {
                setJsonError(response, 400, "INVALID_JSON", "Request body must contain valid JSON.");
                return;
            }
            if (isWhitespaceOnly(requestedPath)) {
                setJsonError(response, 400, "INVALID_REQUEST", "path cannot be empty.");
                return;
            }

            std::filesystem::path selectedPath;
            std::error_code error;
            std::filesystem::path canonicalPath;
            if (resolveSelectedSourcePath(toHostPath(requestedPath), selectedPath)) {
                canonicalPath = std::filesystem::canonical(selectedPath, error);
            }
            if (canonicalPath.empty() || error || !hasSupportedExtension(canonicalPath)) {
                setJsonError(response, 422, "INVALID_SOURCE_FILE", "Path is not an existing file with a supported source extension.");
                return;
            }
            const auto size = std::filesystem::file_size(canonicalPath, error);
            if (error || size > kMaximumEditorFileBytes) {
                setJsonError(response, 413, "FILE_TOO_LARGE", "Source files larger than 512 KiB cannot be opened in the editor.");
                return;
            }
            std::string content;
            try {
                std::error_code readError;
                const ReadResult read =
                    readRegularFile(canonicalPath, kMaximumEditorFileBytes, content, &readError);
                if (read == ReadResult::TooLarge) {
                    setJsonError(response, 413, "FILE_TOO_LARGE", "Source files larger than 512 KiB cannot be opened in the editor.");
                    return;
                }
                if (read != ReadResult::Ok) {
                    throwFileSystemError(FileOperation::Read, canonicalPath, readError);
                }
            } catch (const FileError& error) {
                respondWithError(response, error);
                return;
            }
            std::string serializedResponse;
            try {
                serializedResponse = Json{
                    {"path", canonicalPath.string()},
                    {"content", content},
                    {"revision", hashEditorContent(content)}
                }.dump();
            } catch (const Json::exception&) {
                setJsonError(response, 422, "UNSUPPORTED_ENCODING", "The file is not valid UTF-8 text.");
                return;
            }
            rememberOpenedDocument(canonicalPath.string());
            response.set_content(std::move(serializedResponse), kJsonContentType);
        });

        // Saves editor content back to a file opened through /api/files/read.
        // baseRevision is the revision returned when the file was opened or last
        // saved; if the file has changed on disk since, overwriting needs
        // explicit confirmation.
        server_.Post("/api/files/write", [this](const httplib::Request& request, httplib::Response& response) {
            if (!isJsonContentType(request.get_header_value("Content-Type"))) {
                setJsonError(response, 415, "UNSUPPORTED_MEDIA_TYPE", "Content-Type must be application/json.");
                return;
            }
            std::string path;
            std::string content;
            std::string baseRevision;
            bool overwriteConfirmed = false;
            try {
                const Json body = Json::parse(request.body);
                if (!body.is_object() || body.size() != 4 ||
                    !body.contains("path") || !body["path"].is_string() ||
                    !body.contains("content") || !body["content"].is_string() ||
                    !body.contains("baseRevision") || !body["baseRevision"].is_string() ||
                    !body.contains("overwriteConfirmed") || !body["overwriteConfirmed"].is_boolean()) {
                    setJsonError(response, 400, "INVALID_REQUEST", "Request must contain only string path, content, and baseRevision fields and a boolean overwriteConfirmed field.");
                    return;
                }
                path = body["path"].get<std::string>();
                content = body["content"].get<std::string>();
                baseRevision = body["baseRevision"].get<std::string>();
                overwriteConfirmed = body["overwriteConfirmed"].get<bool>();
            } catch (const Json::exception&) {
                setJsonError(response, 400, "INVALID_JSON", "Request body must contain valid JSON.");
                return;
            }
            {
                std::lock_guard<std::mutex> lock(documentsMutex_);
                if (openedDocumentIndex_.count(path) == 0) {
                    setJsonError(response, 403, "FILE_NOT_OPENED", "Only files opened in this session can be saved in place. Use Save As to choose a destination.");
                    return;
                }
            }
            const std::filesystem::path target(path);
            if (!hasSupportedExtension(target)) {
                setJsonError(response, 400, "INVALID_FILENAME", "Only supported source files can be saved.");
                return;
            }

            std::lock_guard<std::mutex> saveLock(saveMutex_);
            std::error_code error;
            const auto state = std::filesystem::symlink_status(target, error);
            const bool exists = !error && state.type() != std::filesystem::file_type::not_found;
            if (exists && (std::filesystem::is_symlink(state) || !std::filesystem::is_regular_file(state))) {
                setJsonError(response, 422, "INVALID_TARGET", "Target must be a regular file and cannot be a symbolic link.");
                return;
            }
            std::string current;
            const bool unchanged = exists &&
                readRegularFile(target, kMaximumEditorFileBytes, current) == ReadResult::Ok &&
                hashEditorContent(current) == baseRevision;
            try {
                if (!unchanged && !overwriteConfirmed) {
                    throw FileConflictError(target, exists);
                }
                writeFileSafely(target, content, true);
            } catch (const FileError& error) {
                // FileConflictError (409), FilePermissionError (403),
                // FileNotFoundError (404), or another FileError (500).
                respondWithError(response, error);
                return;
            }
            rememberOpenedDocument(path);
            response.set_content(Json{
                {"savedPath", target.string()},
                {"status", "SAVED"},
                {"revision", hashEditorContent(content)}
            }.dump(), kJsonContentType);
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
                    selectedPath = toHostPath(path);
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
                    logUnexpectedException(std::cerr, "the native directory dialog");
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
            if (!isWritableDirectory(normalizedPath)) {
                setJsonError(response, 422, "OUTPUT_DIRECTORY_NOT_WRITABLE", "Selected directory is not writable; check its permissions.");
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
            std::string thoughts;
            try {
                if (!generationHandler_(instruction, language, generatedCode, thoughts) ||
                    isWhitespaceOnly(generatedCode)) {
                    setJsonError(
                        response,
                        502,
                        "GENERATION_FAILED",
                        "Code generation failed."
                    );
                    return;
                }
            } catch (const CodeAgentError& error) {
                respondWithError(response, error);
                return;
            } catch (...) {
                logUnexpectedException(std::cerr, "code generation");
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
                        {"language", language},
                        {"thoughts", thoughts}
                    }.dump(),
                    kJsonContentType
                );
            } catch (...) {
                logUnexpectedException(std::cerr, "serializing generated code");
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
                // proposalLock stays held until the write finishes, so the
                // proposal cannot be evicted while it is being saved.
                proposal->second.lastActivity = clock_();
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
            try {
                writeFileSafely(target, code, save.overwriteConfirmed);
            } catch (const FileError& failure) {
                // The file appeared after the existence check above.
                if (failure.errorCode() == std::errc::file_exists) {
                    setJsonError(response, 409, "OVERWRITE_CONFIRMATION_REQUIRED", "Set overwriteConfirmed to true to replace the existing file.");
                } else {
                    respondWithError(response, failure);
                }
                return;
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
            std::string thoughts;
            try {
                if (!refactoringHandler_(
                        refactoringRequest.instruction,
                        refactoringRequest.language,
                        refactoringRequest.sourceCode,
                        proposedCode,
                        thoughts) ||
                    isWhitespaceOnly(proposedCode)) {
                    setJsonError(
                        response,
                        502,
                        "REFACTOR_FAILED",
                        "Code refactoring failed or returned no complete code."
                    );
                    return;
                }
            } catch (const CodeAgentError& error) {
                respondWithError(response, error);
                return;
            } catch (...) {
                logUnexpectedException(std::cerr, "code refactoring");
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
                const auto now = clock_();
                // The reserved slot may have been used again while the
                // provider ran; it is only evicted if it is still evictable.
                std::optional<std::string> evictionId = reservation.evictionId;
                if (evictionId) {
                    const auto eviction = proposals_.find(*evictionId);
                    if (eviction == proposals_.end() || !isEvictable(eviction->second, now)) {
                        evictionId = findEvictionCandidate(now);
                    }
                    if (!evictionId) {
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
                    {"baseEditorHash", baseEditorHash},
                    {"thoughts", thoughts}
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
                        proposalSequence,
                        now
                    }
                );
                auto stagedNode = staged.extract(proposalId);
                if (evictionId) {
                    auto evictedNode = proposals_.extract(*evictionId);
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
                logUnexpectedException(std::cerr, "storing a refactoring proposal");
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
                proposal->second.lastActivity = clock_();
                originalCode = proposal->second.originalCode;
                proposedCode = proposal->second.proposedCode;
            }

            try {
                response.set_content(
                    Json{{"diff", makeUnifiedDiff(originalCode, proposedCode)}}.dump(),
                    kJsonContentType
                );
            } catch (...) {
                logUnexpectedException(std::cerr, "diff generation");
                setJsonError(
                    response,
                    500,
                    "DIFF_FAILED",
                    "Diff could not be generated."
                );
            }
        });

        // approve/reject decide a pending proposal. mark-applied records that
        // the client put an approved proposal into its editor; it writes no
        // file and makes the proposal evictable, like a saved one.
        server_.Post(R"(/api/changes/([^/]+)/(approve|reject|mark-applied))",
            [this](const httplib::Request& request, httplib::Response& response) {
                if (!request.body.empty()) {
                    setJsonError(
                        response,
                        400,
                        "INVALID_REQUEST",
                        "Proposal state requests must have an empty body."
                    );
                    return;
                }

                const std::string proposalId = request.matches[1].str();
                const std::string action = request.matches[2].str();
                const bool markApplied = action == "mark-applied";
                const ProposalStatus requiredStatus = markApplied
                    ? ProposalStatus::Approved
                    : ProposalStatus::Pending;
                const ProposalStatus nextStatus = markApplied
                    ? ProposalStatus::Applied
                    : action == "approve"
                        ? ProposalStatus::Approved
                        : ProposalStatus::Rejected;
                const char* statusText = markApplied
                    ? "APPLIED"
                    : action == "approve"
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
                    if (proposal->second.status != requiredStatus) {
                        setJsonError(
                            response,
                            409,
                            "INVALID_PROPOSAL_STATE",
                            markApplied
                                ? "Only approved proposals can be marked as applied."
                                : "Only pending proposals can be approved or rejected."
                        );
                        return;
                    }
                    try {
                        serializedResponse = Json{
                            {"proposalId", proposalId},
                            {"status", statusText}
                        }.dump();
                    } catch (...) {
                        logUnexpectedException(std::cerr, "updating proposal state");
                        setJsonError(
                            response,
                            500,
                            "STATE_UPDATE_FAILED",
                            "Proposal state could not be updated."
                        );
                        return;
                    }
                    proposal->second.status = nextStatus;
                    proposal->second.lastActivity = clock_();
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
                    !isAllowedOrigin(origin)) {
                    setJsonError(response, 403, "ORIGIN_NOT_ALLOWED", "Request origin is not allowed.");
                    return httplib::Server::HandlerResponse::Handled;
                }
            }
            const auto allowOrigin = [&] {
                if (!hasOrigin) return;
                response.set_header("Access-Control-Allow-Origin", origin);
                response.set_header("Access-Control-Allow-Credentials", "true");
            };

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
                // Browsers never send custom headers such as the session token
                // on a preflight, so a preflight cannot be authenticated. It
                // only permits the actual request, which needs the token, and
                // never reaches a route handler.
                allowOrigin();
                response.status = 204;
                response.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
                response.set_header("Access-Control-Allow-Headers",
                    std::string("Content-Type, ") + GuiHttpServer::kSessionTokenHeader);
                response.set_header("Access-Control-Max-Age", "600");
                return httplib::Server::HandlerResponse::Handled;
            }

            // Lets an allowed page read API responses, including errors.
            allowOrigin();
            if (request.path == kSessionPath) {
                // The exchange is only for pages, which always send Origin
                // with a POST.
                if (!hasOrigin) {
                    setJsonError(response, 403, "ORIGIN_REQUIRED", "Session requests must come from the CodeAgent page.");
                    return httplib::Server::HandlerResponse::Handled;
                }
                return httplib::Server::HandlerResponse::Unhandled;
            }
            std::string token;
            if (!hasSingleHeader(request, GuiHttpServer::kSessionTokenHeader, token) ||
                !constantTimeEquals(token, sessionToken_)) {
                setJsonError(response, 401, "UNAUTHORIZED",
                    "Missing or invalid session token. Reload the page or open the link printed by codeagent gui.");
                return httplib::Server::HandlerResponse::Handled;
            }
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
        // Last line of defence for exceptions that escape a route handler: the
        // server keeps running, and the request gets a JSON error.
        server_.set_exception_handler([](const httplib::Request& request,
                                         httplib::Response& response,
                                         std::exception_ptr exception) {
            try {
                if (exception) std::rethrow_exception(exception);
            } catch (const CodeAgentError& error) {
                if (isApiPath(request.path)) {
                    respondWithError(response, error);
                    return;
                }
                logError(std::cerr, error);
            } catch (...) {
                logUnexpectedException(std::cerr, "an API request");
            }
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
        // Never serve the API without authentication.
        if (sessionToken_.empty() || refreshSecret_.empty()) {
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

    std::string issueBootstrapCode() {
        std::string code = randomSecret();
        if (code.empty()) return code;
        std::lock_guard<std::mutex> lock(sessionMutex_);
        const auto now = clock_();
        bootstrapCodes_.erase(
            std::remove_if(bootstrapCodes_.begin(), bootstrapCodes_.end(),
                           [now](const BootstrapCode& entry) { return entry.expires <= now; }),
            bootstrapCodes_.end());
        if (bootstrapCodes_.size() >= kMaximumBootstrapCodes) {
            bootstrapCodes_.erase(bootstrapCodes_.begin());
        }
        bootstrapCodes_.push_back({code, now + kBootstrapCodeLifetime});
        return code;
    }

    bool allowDevelopmentOrigin(const std::string& origin) {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        if (state_ != State::Ready || !isLoopbackOrigin(origin)) return false;
        developmentOrigin_ = origin;
        return true;
    }

    bool setClock(GuiHttpServer::Clock clock) {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        if (state_ != State::Ready || !clock) return false;
        clock_ = std::move(clock);
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
    // Count in-flight creation leases with stored proposals so parallel provider
    // calls cannot claim the same final slot. A terminal candidate is pinned
    // until commit or lease destruction; provider calls run without this mutex.
    bool reserveProposalCapacity(ProposalCapacityReservation& reservation,
                                 httplib::Response& response) {
        std::lock_guard<std::mutex> lock(proposalMutex_);
        std::optional<std::string> evictionId;
        if (proposals_.size() + proposalReservations_ >= kMaximumRetainedProposals) {
            evictionId = findEvictionCandidate(clock_());
            if (!evictionId) {
                setJsonError(response, 503, "PROPOSAL_CAPACITY", "Proposal capacity is full; resolve or save existing proposals before creating another.");
                return false;
            }
        }
        if (evictionId) reservedEvictions_.insert(*evictionId);
        reservation.evictionId = std::move(evictionId);
        ++proposalReservations_;
        reservation.active = true;
        return true;
    }

    // Rejected and applied proposals are finished. Pending and approved ones
    // are abandoned once idle for kProposalIdleExpiry. Callers hold
    // proposalMutex_, which every request using a proposal also holds while
    // it reads or changes it, so an in-progress save is never evicted.
    static bool isEvictable(const PendingProposal& proposal,
                            std::chrono::steady_clock::time_point now) {
        if (proposal.status == ProposalStatus::Rejected ||
            proposal.status == ProposalStatus::Applied) {
            return true;
        }
        return now - proposal.lastActivity >= kProposalIdleExpiry;
    }

    // Prefers the oldest finished proposal, then the longest-idle abandoned
    // one. Slots already reserved by another request are skipped.
    std::optional<std::string> findEvictionCandidate(std::chrono::steady_clock::time_point now) const {
        auto best = proposals_.end();
        const auto rank = [&](const PendingProposal& proposal) {
            const bool finished = proposal.status == ProposalStatus::Rejected ||
                                  proposal.status == ProposalStatus::Applied;
            return std::make_tuple(finished ? 0 : 1,
                                   finished ? std::chrono::steady_clock::time_point{} : proposal.lastActivity,
                                   proposal.sequence);
        };
        for (auto proposal = proposals_.begin(); proposal != proposals_.end(); ++proposal) {
            if (reservedEvictions_.count(proposal->first) != 0 ||
                !isEvictable(proposal->second, now)) {
                continue;
            }
            if (best == proposals_.end() || rank(proposal->second) < rank(best->second)) {
                best = proposal;
            }
        }
        if (best == proposals_.end()) return std::nullopt;
        return best->first;
    }

    void releaseProposalCapacity(ProposalCapacityReservation& reservation) {
        std::lock_guard<std::mutex> lock(proposalMutex_);
        if (reservation.evictionId) reservedEvictions_.erase(*reservation.evictionId);
        if (proposalReservations_ > 0) --proposalReservations_;
        reservation.active = false;
    }

    // The page served by this server, and an explicitly configured
    // development origin; nothing else.
    bool isAllowedOrigin(const std::string& origin) const {
        const std::string port = ":" + std::to_string(boundPort_);
        return origin == "http://127.0.0.1" + port || origin == "http://localhost" + port ||
               (!developmentOrigin_.empty() && origin == developmentOrigin_);
    }

    bool redeemBootstrapCode(const std::string& code) {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        const auto now = clock_();
        for (auto entry = bootstrapCodes_.begin(); entry != bootstrapCodes_.end(); ++entry) {
            if (constantTimeEquals(code, entry->code)) {
                const bool valid = entry->expires > now;
                bootstrapCodes_.erase(entry);
                return valid;
            }
        }
        return false;
    }

    // One cookie name per port, so two servers do not overwrite each other's
    // cookie (cookies are not separated by port).
    std::string sessionCookieName() const {
        return "codeagent_session_" + std::to_string(boundPort_);
    }

    bool hasValidSessionCookie(const httplib::Request& request) const {
        const std::string prefix = sessionCookieName() + "=";
        const auto cookies = request.headers.equal_range("Cookie");
        for (auto header = cookies.first; header != cookies.second; ++header) {
            const std::string& value = header->second;
            std::size_t start = 0;
            while (start < value.size()) {
                auto end = value.find(';', start);
                if (end == std::string::npos) end = value.size();
                std::string cookie = value.substr(start, end - start);
                const auto first = cookie.find_first_not_of(' ');
                cookie = first == std::string::npos ? "" : cookie.substr(first);
                if (cookie.compare(0, prefix.size(), prefix) == 0 &&
                    constantTimeEquals(cookie.substr(prefix.size()), refreshSecret_)) {
                    return true;
                }
                start = end + 1;
            }
        }
        return false;
    }

    void rememberOpenedDocument(const std::string& path) {
        std::lock_guard<std::mutex> lock(documentsMutex_);
        const auto known = openedDocumentIndex_.find(path);
        if (known != openedDocumentIndex_.end()) {
            openedDocuments_.splice(openedDocuments_.end(), openedDocuments_, known->second);
            return;
        }
        if (openedDocuments_.size() >= kMaximumOpenedDocuments) {
            openedDocumentIndex_.erase(openedDocuments_.front());
            openedDocuments_.pop_front();
        }
        openedDocuments_.push_back(path);
        openedDocumentIndex_.emplace(path, std::prev(openedDocuments_.end()));
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
    std::mutex documentsMutex_;
    // Canonical paths the user opened for editing; the only in-place save
    // targets. Ordered from least to most recently opened or saved; the
    // least recently used path is forgotten when the limit is reached.
    std::list<std::string> openedDocuments_;
    std::map<std::string, std::list<std::string>::iterator> openedDocumentIndex_;
    std::map<std::string, PendingProposal> proposals_;
    std::unordered_set<std::string> reservedEvictions_;
    std::size_t proposalReservations_ = 0;
    struct BootstrapCode {
        std::string code;
        std::chrono::steady_clock::time_point expires;
    };
    // Kept in memory only; never logged or returned except by /api/session.
    const std::string sessionToken_;
    const std::string refreshSecret_;
    std::mutex sessionMutex_;
    std::vector<BootstrapCode> bootstrapCodes_;
    std::string developmentOrigin_;
    GuiHttpServer::Clock clock_ = [] { return std::chrono::steady_clock::now(); };
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

std::string GuiHttpServer::issueBootstrapCode() {
    return impl_->issueBootstrapCode();
}

bool GuiHttpServer::allowDevelopmentOrigin(const std::string& origin) {
    return impl_->allowDevelopmentOrigin(origin);
}

bool GuiHttpServer::setClock(Clock clock) {
    return impl_->setClock(std::move(clock));
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
