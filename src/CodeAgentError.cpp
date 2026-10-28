#include "CodeAgentError.hpp"

#include <cstdio>
#include <cstdlib>
#include <ostream>
#include <regex>
#include <typeinfo>
#include <utility>

namespace {

std::string replaceAll(std::string text, const std::string& secret) {
    if (secret.empty()) return text;
    for (auto position = text.find(secret); position != std::string::npos;
         position = text.find(secret, position)) {
        text.replace(position, secret.size(), "[REDACTED]");
        position += 10;
    }
    return text;
}

std::string rateLimitMessage(bool quotaExhausted, const std::optional<std::string>& retryDelay) {
    if (quotaExhausted) {
        return "Gemini quota exhausted: the daily request limit for this API key has been "
               "reached. Wait for the quota to reset or use a key with a higher quota.";
    }
    std::string message = "Request rate limited. Wait briefly before retrying.";
    if (retryDelay) message += " Gemini suggests waiting " + *retryDelay + ".";
    return message;
}

std::string responseMessage(ProviderResponseError::Kind kind) {
    switch (kind) {
    case ProviderResponseError::Kind::Blocked:
        return "Gemini blocked the response for safety or policy reasons. "
               "Rephrase the instruction and try again.";
    case ProviderResponseError::Kind::Incomplete:
        return "Gemini did not finish the response, so it was not used. "
               "Ask for a smaller change or retry.";
    case ProviderResponseError::Kind::Invalid:
        break;
    }
    return "Gemini returned a response CodeAgent could not use. Retry the request.";
}

const char* verb(FileOperation operation) {
    return operation == FileOperation::Read ? "read" : "write";
}

std::string describeFailure(FileOperation operation, const std::filesystem::path& path,
                            const std::error_code& error) {
    std::string text = std::string(verb(operation)) + " " + path.string();
    if (error) text += ": " + error.message();
    return text;
}

} // namespace

// ---------------------------------------------------------------------------

CodeAgentError::CodeAgentError(const std::string& userMessage, std::string diagnostic)
    : std::runtime_error(userMessage),
      diagnostic_(std::make_shared<const std::string>(redactSecrets(std::move(diagnostic)))) {
}

const std::string& CodeAgentError::diagnostic() const noexcept {
    return *diagnostic_;
}

ConfigurationError::ConfigurationError(const std::string& userMessage, std::string diagnostic)
    : CodeAgentError(userMessage, std::move(diagnostic)) {
}

const char* ConfigurationError::code() const noexcept { return "CONFIGURATION_ERROR"; }
int ConfigurationError::httpStatus() const noexcept { return 503; }

ConfigurationError ConfigurationError::missingSetting(const std::string& name) {
    return ConfigurationError(
        "Configuration error: " + name + " is not configured. "
        "Set it in your environment or .env file.",
        name + " is unset or empty");
}

ProviderError::ProviderError(const std::string& userMessage, std::string diagnostic)
    : CodeAgentError(userMessage, std::move(diagnostic)) {
}

const char* ProviderError::code() const noexcept { return "PROVIDER_ERROR"; }
int ProviderError::httpStatus() const noexcept { return 502; }

AuthenticationError::AuthenticationError(std::string diagnostic)
    : ProviderError(
          "Authentication failed: Gemini rejected the configured credentials. Verify the API key.",
          std::move(diagnostic)) {
}

const char* AuthenticationError::code() const noexcept { return "PROVIDER_AUTH_FAILED"; }

RateLimitError::RateLimitError(bool quotaExhausted, std::optional<std::string> retryDelay,
                               std::string diagnostic)
    : ProviderError(rateLimitMessage(quotaExhausted, retryDelay), std::move(diagnostic)),
      quotaExhausted_(quotaExhausted),
      retryDelay_(std::make_shared<const std::optional<std::string>>(std::move(retryDelay))) {
}

const char* RateLimitError::code() const noexcept {
    return quotaExhausted_ ? "PROVIDER_QUOTA_EXHAUSTED" : "PROVIDER_RATE_LIMITED";
}

int RateLimitError::httpStatus() const noexcept { return 429; }

ProviderUnavailableError::ProviderUnavailableError(std::string diagnostic)
    : ProviderError("Gemini is temporarily unavailable. Retry in a moment.", std::move(diagnostic)) {
}

const char* ProviderUnavailableError::code() const noexcept { return "PROVIDER_UNAVAILABLE"; }
int ProviderUnavailableError::httpStatus() const noexcept { return 503; }

NetworkError::NetworkError(std::string diagnostic)
    : NetworkError("Network error: Unable to connect to Gemini. Check your connection.",
                   std::move(diagnostic)) {
}

NetworkError::NetworkError(const std::string& userMessage, std::string diagnostic)
    : ProviderError(userMessage, std::move(diagnostic)) {
}

const char* NetworkError::code() const noexcept { return "PROVIDER_UNREACHABLE"; }

TimeoutError::TimeoutError(std::string diagnostic)
    : NetworkError("Request timed out. Retry when the service is reachable.", std::move(diagnostic)) {
}

const char* TimeoutError::code() const noexcept { return "PROVIDER_TIMEOUT"; }
int TimeoutError::httpStatus() const noexcept { return 504; }

ProviderResponseError::ProviderResponseError(Kind kind, std::string diagnostic)
    : ProviderError(responseMessage(kind), std::move(diagnostic)), kind_(kind) {
}

const char* ProviderResponseError::code() const noexcept {
    switch (kind_) {
    case Kind::Blocked: return "PROVIDER_RESPONSE_BLOCKED";
    case Kind::Incomplete: return "PROVIDER_RESPONSE_INCOMPLETE";
    case Kind::Invalid: break;
    }
    return "PROVIDER_RESPONSE_INVALID";
}

// ---------------------------------------------------------------------------

FileError::FileError(FileOperation operation, std::filesystem::path path, std::error_code error)
    : FileError(operation, std::move(path), error,
                operation == FileOperation::Read
                    ? "The file could not be read. Check that it exists and is a regular file."
                    : "The file could not be saved. Check the disk and the folder, or use Save As to choose another location.") {
}

FileError::FileError(FileOperation operation, std::filesystem::path path, std::error_code error,
                     const std::string& userMessage)
    : CodeAgentError(userMessage, describeFailure(operation, path, error)),
      operation_(operation),
      path_(std::make_shared<const std::filesystem::path>(std::move(path))),
      error_(error) {
}

const char* FileError::code() const noexcept {
    return operation_ == FileOperation::Read ? "READ_FAILED" : "SAVE_FAILED";
}

int FileError::httpStatus() const noexcept { return 500; }

FilePermissionError::FilePermissionError(FileOperation operation, std::filesystem::path path,
                                         std::error_code error)
    : FileError(operation, std::move(path), error,
                operation == FileOperation::Read
                    ? "Permission denied: CodeAgent cannot access this file. Check its permissions."
                    : "Permission denied: CodeAgent cannot access this file. Check its permissions, or use Save As to choose another location.") {
}

const char* FilePermissionError::code() const noexcept { return "PERMISSION_DENIED"; }
int FilePermissionError::httpStatus() const noexcept { return 403; }

FileNotFoundError::FileNotFoundError(FileOperation operation, std::filesystem::path path,
                                     std::error_code error)
    : FileError(operation, std::move(path), error,
                "The file or its folder no longer exists. Check the path, or use Save As to choose another location.") {
}

const char* FileNotFoundError::code() const noexcept { return "FILE_NOT_FOUND"; }
int FileNotFoundError::httpStatus() const noexcept { return 404; }

FileConflictError::FileConflictError(std::filesystem::path path, bool fileStillExists)
    : FileError(FileOperation::Write, std::move(path), {},
                fileStillExists
                    ? "The file changed externally. Reload it or save to another path."
                    : "The file no longer exists on disk. Save it again to recreate it, or save to another path.") {
}

const char* FileConflictError::code() const noexcept { return "FILE_CHANGED_ON_DISK"; }
int FileConflictError::httpStatus() const noexcept { return 409; }

void throwFileSystemError(FileOperation operation, const std::filesystem::path& path,
                          std::error_code error) {
    if (error == std::errc::permission_denied || error == std::errc::operation_not_permitted ||
        error == std::errc::read_only_file_system) {
        throw FilePermissionError(operation, path, error);
    }
    if (error == std::errc::no_such_file_or_directory) {
        throw FileNotFoundError(operation, path, error);
    }
    throw FileError(operation, path, error);
}

// ---------------------------------------------------------------------------

std::string redactSecrets(std::string text) {
    if (const char* apiKey = std::getenv("GEMINI_API_KEY")) {
        // Very short values would redact ordinary words; real keys are long.
        if (std::string(apiKey).size() >= 8) text = replaceAll(std::move(text), apiKey);
    }
    static const std::regex googleApiKey(R"(AIza[0-9A-Za-z_\-]{20,})");
    static const std::regex keyParameter(R"(([?&]key=)[^&\s"]+)", std::regex::icase);
    static const std::regex keyHeader(R"((x-goog-api-key:\s*)\S+)", std::regex::icase);
    static const std::regex bearer(R"((authorization:\s*bearer\s+)\S+)", std::regex::icase);
    text = std::regex_replace(text, googleApiKey, "[REDACTED]");
    text = std::regex_replace(text, keyParameter, "$1[REDACTED]");
    text = std::regex_replace(text, keyHeader, "$1[REDACTED]");
    return std::regex_replace(text, bearer, "$1[REDACTED]");
}

void logError(std::ostream& log, const CodeAgentError& error) {
    log << "Error [" << error.code() << "]: " << error.what();
    if (!error.diagnostic().empty()) log << " (details: " << error.diagnostic() << ')';
    log << '\n';
}

void logUnexpectedException(std::ostream& log, const char* context) noexcept {
    try {
        const std::exception_ptr current = std::current_exception();
        if (!current) {
            log << "Unexpected error during " << context << ".\n";
            return;
        }
        try {
            std::rethrow_exception(current);
        } catch (const CodeAgentError& error) {
            logError(log, error);
        } catch (const std::exception& error) {
            log << "Unexpected error during " << context << " (" << typeid(error).name()
                << "): " << redactSecrets(error.what()) << '\n';
        } catch (...) {
            log << "Unexpected non-standard exception during " << context << ".\n";
        }
    } catch (...) {
        // Logging must never turn a handled failure into a new one, so fall
        // back to C stdio, which does not throw.
        std::fputs("CodeAgent: an error occurred while logging another error.\n", stderr);
    }
}
