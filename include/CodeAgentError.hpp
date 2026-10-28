#ifndef CODE_AGENT_ERROR_HPP
#define CODE_AGENT_ERROR_HPP

#include <exception>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>

// Base of every failure that CodeAgent reports to its user.
//
// what() is a short, actionable message that is safe to print in the
// terminal or return to the browser: it never contains API keys, request
// headers, .env contents, or free text from the provider. diagnostic() holds
// technical detail for the terminal log; it is redacted on construction.
//
// code() and httpStatus() let each boundary report the error without knowing
// its concrete type: the GUI server turns them into its JSON error response.
class CodeAgentError : public std::runtime_error {
public:
    virtual const char* code() const noexcept = 0;
    virtual int httpStatus() const noexcept = 0;

    const std::string& diagnostic() const noexcept;

protected:
    CodeAgentError(const std::string& userMessage, std::string diagnostic);

private:
    // Shared so that copying the exception, which happens while it is thrown
    // and caught, cannot itself throw.
    std::shared_ptr<const std::string> diagnostic_;
};

// Missing or invalid settings, such as GEMINI_API_KEY or CODEAGENT_GUI_PORT.
class ConfigurationError final : public CodeAgentError {
public:
    explicit ConfigurationError(const std::string& userMessage, std::string diagnostic = {});
    const char* code() const noexcept override;
    int httpStatus() const noexcept override;

    // "Configuration error: <name> is not configured. Set it in your
    // environment or .env file." searchedLocations, when given, lists the
    // .env files that were checked; it only goes into the diagnostic.
    static ConfigurationError missingSetting(const std::string& name,
                                             const std::string& searchedLocations = {});
};

// ---------------------------------------------------------------------------
// Gemini provider failures
// ---------------------------------------------------------------------------

// A request that Gemini refused or could not serve. Used as is for failures
// without a more specific subclass, such as an unknown model.
class ProviderError : public CodeAgentError {
public:
    ProviderError(const std::string& userMessage, std::string diagnostic);
    const char* code() const noexcept override;
    int httpStatus() const noexcept override;
};

// Gemini rejected the API key (HTTP 401/403, or 400 with API_KEY_INVALID).
class AuthenticationError final : public ProviderError {
public:
    explicit AuthenticationError(std::string diagnostic);
    const char* code() const noexcept override;
};

// HTTP 429 / RESOURCE_EXHAUSTED. A short-term rate limit and an exhausted
// daily quota need different next steps, so they are told apart from the
// QuotaFailure details instead of assuming every 429 is a spent quota.
class RateLimitError final : public ProviderError {
public:
    RateLimitError(bool quotaExhausted, std::optional<std::string> retryDelay,
                   std::string diagnostic);
    const char* code() const noexcept override;
    int httpStatus() const noexcept override;

    bool quotaExhausted() const noexcept { return quotaExhausted_; }
    // As given by Gemini's RetryInfo, for example "30s".
    const std::optional<std::string>& retryDelay() const noexcept { return *retryDelay_; }

private:
    bool quotaExhausted_;
    // Members are shared, immutable values so that copying cannot throw.
    std::shared_ptr<const std::optional<std::string>> retryDelay_;
};

// Gemini answered with a 5xx status.
class ProviderUnavailableError final : public ProviderError {
public:
    explicit ProviderUnavailableError(std::string diagnostic);
    const char* code() const noexcept override;
    int httpStatus() const noexcept override;
};

// The request did not reach Gemini or no response came back.
class NetworkError : public ProviderError {
public:
    explicit NetworkError(std::string diagnostic);
    const char* code() const noexcept override;

protected:
    NetworkError(const std::string& userMessage, std::string diagnostic);
};

// A network failure caused by a deadline, so catching NetworkError also
// catches it.
class TimeoutError final : public NetworkError {
public:
    explicit TimeoutError(std::string diagnostic);
    const char* code() const noexcept override;
    int httpStatus() const noexcept override;
};

// Gemini answered, but the response cannot be used as code.
class ProviderResponseError final : public ProviderError {
public:
    enum class Kind {
        Invalid,     // malformed or missing content
        Blocked,     // safety or policy block
        Incomplete   // token limit, continuation, or unconfirmed completion
    };

    ProviderResponseError(Kind kind, std::string diagnostic);
    const char* code() const noexcept override;
    Kind kind() const noexcept { return kind_; }

private:
    Kind kind_;
};

// ---------------------------------------------------------------------------
// File failures
// ---------------------------------------------------------------------------

enum class FileOperation { Read, Write };

// A file could not be read or written. Subclasses cover the causes that need
// a different next step.
class FileError : public CodeAgentError {
public:
    FileError(FileOperation operation, std::filesystem::path path, std::error_code error);
    const char* code() const noexcept override;
    int httpStatus() const noexcept override;

    FileOperation operation() const noexcept { return operation_; }
    const std::filesystem::path& path() const noexcept { return *path_; }
    // The operating-system error, or an empty code for logical conflicts.
    const std::error_code& errorCode() const noexcept { return error_; }

protected:
    FileError(FileOperation operation, std::filesystem::path path, std::error_code error,
              const std::string& userMessage);

private:
    FileOperation operation_;
    // Shared so that copying the exception cannot throw.
    std::shared_ptr<const std::filesystem::path> path_;
    std::error_code error_;
};

class FilePermissionError final : public FileError {
public:
    FilePermissionError(FileOperation operation, std::filesystem::path path, std::error_code error);
    const char* code() const noexcept override;
    int httpStatus() const noexcept override;
};

class FileNotFoundError final : public FileError {
public:
    FileNotFoundError(FileOperation operation, std::filesystem::path path, std::error_code error);
    const char* code() const noexcept override;
    int httpStatus() const noexcept override;
};

// The file on disk is no longer the revision the editor was based on.
class FileConflictError final : public FileError {
public:
    FileConflictError(std::filesystem::path path, bool fileStillExists);
    const char* code() const noexcept override;
    int httpStatus() const noexcept override;
};

// Throws the FileError subclass that matches error.
[[noreturn]] void throwFileSystemError(FileOperation operation,
                                       const std::filesystem::path& path,
                                       std::error_code error);

// ---------------------------------------------------------------------------
// Reporting helpers
// ---------------------------------------------------------------------------

// Replaces the configured GEMINI_API_KEY, Google API keys, and key-carrying
// URL parameters or headers with [REDACTED].
std::string redactSecrets(std::string text);

// Writes "Error [CODE]: message (details: diagnostic)" to log.
void logError(std::ostream& log, const CodeAgentError& error);

// Logs the exception currently being handled, for catch (...) blocks at a
// boundary. context names what was being done, e.g. "code generation".
void logUnexpectedException(std::ostream& log, const char* context) noexcept;

#endif // CODE_AGENT_ERROR_HPP
