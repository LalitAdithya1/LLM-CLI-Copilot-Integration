#include "CodeAgentError.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>

namespace {

// Exceptions are copied while they propagate, so copying must not throw.
static_assert(std::is_nothrow_copy_constructible_v<ConfigurationError>);
static_assert(std::is_nothrow_copy_constructible_v<TimeoutError>);
static_assert(std::is_nothrow_copy_constructible_v<RateLimitError>);
static_assert(std::is_nothrow_copy_constructible_v<ProviderResponseError>);
static_assert(std::is_nothrow_copy_constructible_v<FileError>);
static_assert(std::is_nothrow_copy_constructible_v<FileConflictError>);
static_assert(std::is_abstract_v<CodeAgentError>);

class ScopedApiKey {
public:
    explicit ScopedApiKey(const char* value) { setenv("GEMINI_API_KEY", value, 1); }
    ~ScopedApiKey() { unsetenv("GEMINI_API_KEY"); }
};

} // namespace

TEST(CodeAgentErrorTest, TimeoutIsCaughtAsEveryBaseAndReportsItsOwnCode) {
    try {
        throw TimeoutError("libcurl error 28");
    } catch (const NetworkError& error) {
        // Virtual dispatch through a base reference picks the derived values.
        const CodeAgentError& base = error;
        EXPECT_STREQ(base.code(), "PROVIDER_TIMEOUT");
        EXPECT_EQ(base.httpStatus(), 504);
        EXPECT_STREQ(base.what(), "Request timed out. Retry when the service is reachable.");
        EXPECT_EQ(base.diagnostic(), "libcurl error 28");
    }

    EXPECT_THROW(throw TimeoutError("x"), ProviderError);
    EXPECT_THROW(throw TimeoutError("x"), std::runtime_error);
    EXPECT_THROW(throw NetworkError("x"), CodeAgentError);
}

TEST(CodeAgentErrorTest, MissingSettingNamesTheVariableAndTheNextStep) {
    const ConfigurationError error = ConfigurationError::missingSetting("GEMINI_API_KEY");
    EXPECT_STREQ(error.what(),
        "Configuration error: GEMINI_API_KEY is not configured. Set it in your environment or .env file.");
    EXPECT_STREQ(error.code(), "CONFIGURATION_ERROR");
    EXPECT_EQ(error.httpStatus(), 503);
}

TEST(CodeAgentErrorTest, RateLimitAndExhaustedQuotaAreDifferentFailures) {
    const RateLimitError limited(false, std::string("30s"), "HTTP 429");
    EXPECT_STREQ(limited.code(), "PROVIDER_RATE_LIMITED");
    EXPECT_EQ(limited.httpStatus(), 429);
    EXPECT_FALSE(limited.quotaExhausted());
    EXPECT_STREQ(limited.what(),
        "Request rate limited. Wait briefly before retrying. Gemini suggests waiting 30s.");

    const RateLimitError exhausted(true, std::nullopt, "HTTP 429");
    EXPECT_STREQ(exhausted.code(), "PROVIDER_QUOTA_EXHAUSTED");
    EXPECT_NE(std::string(exhausted.what()).find("quota exhausted"), std::string::npos);
}

TEST(CodeAgentErrorTest, FileSystemErrorsMapToTheMatchingSubclass) {
    const std::filesystem::path path = "/tmp/example.cpp";

    try {
        throwFileSystemError(FileOperation::Write, path,
                             std::make_error_code(std::errc::permission_denied));
        FAIL() << "expected an exception";
    } catch (const FilePermissionError& error) {
        EXPECT_STREQ(error.code(), "PERMISSION_DENIED");
        EXPECT_EQ(error.httpStatus(), 403);
        EXPECT_EQ(error.path(), path);
        EXPECT_EQ(error.errorCode(), std::errc::permission_denied);
        EXPECT_EQ(std::string(error.what()).rfind(
            "Permission denied: CodeAgent cannot access this file.", 0), 0u);
    }

    EXPECT_THROW(
        throwFileSystemError(FileOperation::Read, path,
                             std::make_error_code(std::errc::read_only_file_system)),
        FilePermissionError);
    EXPECT_THROW(
        throwFileSystemError(FileOperation::Read, path,
                             std::make_error_code(std::errc::no_such_file_or_directory)),
        FileNotFoundError);

    try {
        throwFileSystemError(FileOperation::Write, path, std::make_error_code(std::errc::io_error));
        FAIL() << "expected an exception";
    } catch (const FileError& error) {
        EXPECT_STREQ(error.code(), "SAVE_FAILED");
        EXPECT_EQ(error.httpStatus(), 500);
        EXPECT_NE(error.diagnostic().find("write /tmp/example.cpp"), std::string::npos);
    }
}

TEST(CodeAgentErrorTest, SaveConflictTellsTheUserWhatToDo) {
    const FileConflictError changed("/tmp/a.cpp", true);
    EXPECT_STREQ(changed.what(), "The file changed externally. Reload it or save to another path.");
    EXPECT_STREQ(changed.code(), "FILE_CHANGED_ON_DISK");
    EXPECT_EQ(changed.httpStatus(), 409);
    EXPECT_FALSE(changed.errorCode());
}

TEST(CodeAgentErrorTest, DiagnosticsNeverKeepSecrets) {
    const ScopedApiKey key("configured-secret-value-123");
    const ProviderError error(
        "Gemini rejected the request.",
        "url ...?key=abc123&alt=json header x-goog-api-key: configured-secret-value-123 "
        "other AIzaSyA1234567890abcdefghijklmnopqrstu");

    EXPECT_EQ(error.diagnostic().find("configured-secret-value-123"), std::string::npos);
    EXPECT_EQ(error.diagnostic().find("abc123"), std::string::npos);
    EXPECT_EQ(error.diagnostic().find("AIzaSy"), std::string::npos);
    EXPECT_NE(error.diagnostic().find("[REDACTED]"), std::string::npos);
    EXPECT_STREQ(error.what(), "Gemini rejected the request.");
}

TEST(CodeAgentErrorTest, UnexpectedExceptionsAreLoggedWithoutSecrets) {
    const ScopedApiKey key("configured-secret-value-123");
    std::ostringstream log;
    try {
        throw std::runtime_error("failed with configured-secret-value-123");
    } catch (...) {
        logUnexpectedException(log, "code generation");
    }
    EXPECT_NE(log.str().find("Unexpected error during code generation"), std::string::npos);
    EXPECT_EQ(log.str().find("configured-secret-value-123"), std::string::npos);

    std::ostringstream typed;
    try {
        throw AuthenticationError("HTTP 401");
    } catch (...) {
        logUnexpectedException(typed, "code generation");
    }
    EXPECT_EQ(typed.str(),
        "Error [PROVIDER_AUTH_FAILED]: Authentication failed: Gemini rejected the configured "
        "credentials. Verify the API key. (details: HTTP 401)\n");
}
