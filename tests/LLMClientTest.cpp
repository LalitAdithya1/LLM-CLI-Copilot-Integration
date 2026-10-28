
#include "LLMClient.hpp"
#include "CodeAgentError.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {

constexpr const char* kTestApiKey = "test-api-key";

void setTestApiKey() {
    setenv("GEMINI_API_KEY", kTestApiKey, 1);
}

void clearTestApiKey() {
    unsetenv("GEMINI_API_KEY");
}

std::string validGeminiResponse() {
    return R"({
        "candidates": [{
            "content": {
                "parts": [{
                    "text": "int add(int a, int b) { return a + b; }"
                }]
            }
        }]
    })";
}

} // namespace

class LLMClientTest : public ::testing::Test {
protected:
    std::string response;

    void SetUp() override {
        setTestApiKey();
    }

    void TearDown() override {
        clearTestApiKey();
    }
};

TEST_F(LLMClientTest, RejectsEmptyPrompt) {
    LLMClient client;

    response = "old response";

    EXPECT_FALSE(client.generateContent("", response));
    EXPECT_TRUE(response.empty());
}

TEST_F(LLMClientTest, RejectsWhitespaceOnlyPrompt) {
    LLMClient client;

    EXPECT_FALSE(client.generateContent("  \t\n", response));
    EXPECT_TRUE(response.empty());
}

TEST_F(LLMClientTest, RejectsMissingApiKey) {
    clearTestApiKey();

    bool transportCalled = false;

    LLMClient client(
        [&transportCalled](
            const std::string&,
            const std::string&,
            const std::string&,
            std::string&,
            long&
        ) {
            transportCalled = true;
            return true;
        }
    );

    EXPECT_FALSE(
        client.generateContent("Generate C++ code", response)
    );

    EXPECT_FALSE(transportCalled);
    EXPECT_TRUE(response.empty());
}

TEST_F(LLMClientTest, SendsRequestAndExtractsGeneratedText) {
    bool transportCalled = false;

    LLMClient client(
        [&transportCalled](
            const std::string& url,
            const std::string& apiKey,
            const std::string& requestBody,
            std::string& responseBody,
            long& httpStatus
        ) {
            transportCalled = true;

            EXPECT_NE(
                url.find("models/gemini-3.5-flash:generateContent"),
                std::string::npos
            );

            EXPECT_EQ(apiKey, kTestApiKey);

            const json request = json::parse(requestBody);

            EXPECT_EQ(
                request["contents"][0]["parts"][0]["text"],
                "Generate a C++ addition function"
            );

            responseBody = validGeminiResponse();
            httpStatus = 200;

            return true;
        }
    );

    ASSERT_TRUE(client.generateContent(
        "Generate a C++ addition function",
        response
    ));

    EXPECT_TRUE(transportCalled);
    EXPECT_EQ(
        response,
        "int add(int a, int b) { return a + b; }"
    );
}

TEST_F(LLMClientTest, RejectsNetworkFailure) {
    LLMClient client(
        [](
            const std::string&,
            const std::string&,
            const std::string&,
            std::string&,
            long&
        ) {
            return false;
        }
    );

    EXPECT_FALSE(
        client.generateContent("Generate code", response)
    );

    EXPECT_TRUE(response.empty());
}

TEST_F(LLMClientTest, RejectsHttpError) {
    LLMClient client(
        [](
            const std::string&,
            const std::string&,
            const std::string&,
            std::string& responseBody,
            long& httpStatus
        ) {
            responseBody = R"({"error":{"message":"Unauthorized"}})";
            httpStatus = 401;

            return true;
        }
    );

    EXPECT_FALSE(
        client.generateContent("Generate code", response)
    );

    EXPECT_TRUE(response.empty());
}

TEST_F(LLMClientTest, KeepsHttpStatusButDoesNotLogRawProviderErrorDetails) {
    LLMClient client(
        [](
            const std::string&,
            const std::string&,
            const std::string&,
            std::string& responseBody,
            long& httpStatus
        ) {
            responseBody =
                R"({"error":{"message":"provider detail leaked-secret-token"}})";
            httpStatus = 401;
            return true;
        }
    );

    testing::internal::CaptureStderr();
    EXPECT_FALSE(client.generateContent("Generate code", response));
    const std::string diagnostic = testing::internal::GetCapturedStderr();

    EXPECT_NE(diagnostic.find("401"), std::string::npos);
    EXPECT_NE(diagnostic.find("provider error"), std::string::npos);
    EXPECT_EQ(diagnostic.find("leaked-secret-token"), std::string::npos);
}

TEST_F(LLMClientTest, RejectsMalformedJson) {
    LLMClient client(
        [](
            const std::string&,
            const std::string&,
            const std::string&,
            std::string& responseBody,
            long& httpStatus
        ) {
            responseBody = "not valid JSON";
            httpStatus = 200;

            return true;
        }
    );

    EXPECT_FALSE(
        client.generateContent("Generate code", response)
    );

    EXPECT_TRUE(response.empty());
}

TEST_F(LLMClientTest, RejectsProviderResponsesOverTwoMiB) {
    const std::string oversizedText(2 * 1024 * 1024 + 1, 'x');
    const std::string oversizedResponse = json{
        {"candidates", json::array({json{{"content", json{{"parts", json::array({json{{"text", oversizedText}}})}}}}})}
    }.dump();
    LLMClient client(
        [oversizedResponse](const std::string&, const std::string&, const std::string&,
                            std::string& responseBody, long& status) {
            responseBody = oversizedResponse;
            status = 200;
            return true;
        }
    );

    EXPECT_FALSE(client.generateContent("Generate code", response));
    EXPECT_TRUE(response.empty());
}

TEST_F(LLMClientTest, RejectsResponseWithoutCandidates) {
    LLMClient client(
        [](
            const std::string&,
            const std::string&,
            const std::string&,
            std::string& responseBody,
            long& httpStatus
        ) {
            responseBody = R"({"candidates":[]})";
            httpStatus = 200;

            return true;
        }
    );

    EXPECT_FALSE(
        client.generateContent("Generate code", response)
    );

    EXPECT_TRUE(response.empty());
}

TEST_F(LLMClientTest, RejectsResponseWithoutGeneratedText) {
    LLMClient client(
        [](
            const std::string&,
            const std::string&,
            const std::string&,
            std::string& responseBody,
            long& httpStatus
        ) {
            responseBody = R"({
                "candidates": [{
                    "content": {
                        "parts": [{"text": "   "}]
                    }
                }]
            })";

            httpStatus = 200;
            return true;
        }
    );

    EXPECT_FALSE(
        client.generateContent("Generate code", response)
    );

    EXPECT_TRUE(response.empty());
}

TEST_F(LLMClientTest, RejectsPartialOutputAndLeavesResponseEmpty) {
    LLMClient client(
        [](
            const std::string&,
            const std::string&,
            const std::string&,
            std::string& responseBody,
            long& httpStatus
        ) {
            responseBody = R"({
                "candidates": [{
                    "finishReason": "MAX_TOKENS",
                    "content": {
                        "parts": [{"text": "partial generated text"}]
                    }
                }]
            })";

            httpStatus = 200;
            return true;
        }
    );

    response = "old response";

    testing::internal::CaptureStderr();
    EXPECT_FALSE(client.generateContent("Generate code", response));
    const std::string diagnostic = testing::internal::GetCapturedStderr();
    EXPECT_TRUE(response.empty());
    EXPECT_NE(diagnostic.find("token limit"), std::string::npos);
}

TEST_F(LLMClientTest, RefactoringRequiresExplicitCompleteFinishReason) {
    LLMClient client(
        [](
            const std::string&,
            const std::string&,
            const std::string&,
            std::string& responseBody,
            long& httpStatus
        ) {
            responseBody = validGeminiResponse();
            httpStatus = 200;
            return true;
        }
    );

    response = "old response";
    EXPECT_FALSE(client.generateCompleteContent("Refactor code", response));
    EXPECT_TRUE(response.empty());
}

TEST_F(LLMClientTest, AcceptsExplicitlyCompleteRefactoringResponse) {
    LLMClient client(
        [](
            const std::string&,
            const std::string&,
            const std::string&,
            std::string& responseBody,
            long& httpStatus
        ) {
            responseBody = R"({"candidates":[{"finishReason":"STOP","content":{"parts":[{"text":"complete code"}]}}]})";
            httpStatus = 200;
            return true;
        }
    );

    ASSERT_TRUE(client.generateCompleteContent("Refactor code", response));
    EXPECT_EQ(response, "complete code");
}

TEST_F(LLMClientTest, RefactoringRejectsBlockedResponse) {
    LLMClient client(
        [](
            const std::string&,
            const std::string&,
            const std::string&,
            std::string& responseBody,
            long& httpStatus
        ) {
            responseBody = R"({"candidates":[{"finishReason":"SAFETY","content":{"parts":[{"text":"blocked code"}]}}]})";
            httpStatus = 200;
            return true;
        }
    );

    EXPECT_FALSE(client.generateCompleteContent("Refactor code", response));
    EXPECT_TRUE(response.empty());
}

TEST_F(LLMClientTest, RefactoringRejectsIncompleteResponse) {
    LLMClient client(
        [](
            const std::string&,
            const std::string&,
            const std::string&,
            std::string& responseBody,
            long& httpStatus
        ) {
            responseBody = R"({"candidates":[{"finishReason":"MAX_TOKENS","content":{"parts":[{"text":"partial code"}]}}]})";
            httpStatus = 200;
            return true;
        }
    );

    EXPECT_FALSE(client.generateCompleteContent("Refactor code", response));
    EXPECT_TRUE(response.empty());
}

TEST_F(LLMClientTest, SupportsCustomModel) {
    LLMClient client(
        [](
            const std::string& url,
            const std::string&,
            const std::string&,
            std::string& responseBody,
            long& httpStatus
        ) {
            EXPECT_NE(
                url.find("models/custom-model:generateContent"),
                std::string::npos
            );

            responseBody = validGeminiResponse();
            httpStatus = 200;

            return true;
        },
        "custom-model"
    );

    ASSERT_TRUE(client.generateContent("Generate code", response));
    EXPECT_FALSE(response.empty());
}

TEST_F(LLMClientTest, RequestsAndReturnsThoughtsOnlyWhenAsked) {
    std::vector<json> requests;
    LLMClient client(
        [&requests](
            const std::string&,
            const std::string&,
            const std::string& requestBody,
            std::string& responseBody,
            long& httpStatus
        ) {
            requests.push_back(json::parse(requestBody));
            responseBody = R"({"candidates":[{"finishReason":"STOP","content":{"parts":[)"
                           R"({"text":"Plan it.","thought":true},{"text":"int x;"}]}}]})";
            httpStatus = 200;
            return true;
        }
    );

    std::string thoughts = "stale";
    ASSERT_TRUE(client.generateCompleteContent("Declare x", response, thoughts));
    EXPECT_EQ(response, "int x;");
    EXPECT_EQ(thoughts, "Plan it.");

    ASSERT_TRUE(client.generateContent("Declare x", response));
    EXPECT_EQ(response, "int x;");

    ASSERT_EQ(requests.size(), 2u);
    EXPECT_EQ(requests[0]["generationConfig"]["thinkingConfig"]["includeThoughts"], true);
    EXPECT_FALSE(requests[1].contains("generationConfig"));
}

// --------------------------------------------------------------------------
// Typed errors from generate()
// --------------------------------------------------------------------------

namespace {

// A transport that answers every request with status and body.
LLMClient::HttpTransport respondWith(long status, std::string body) {
    return [status, body](const std::string&, const std::string&, const std::string&,
                          std::string& responseBody, long& httpStatus) {
        responseBody = body;
        httpStatus = status;
        return true;
    };
}

std::string googleError(int code, const char* status, const std::string& details) {
    return R"({"error":{"code":)" + std::to_string(code) +
           R"(,"message":"free text from the provider","status":")" + status +
           R"(","details":[)" + details + "]}}";
}

const std::string kApiKeyInvalid =
    R"({"@type":"type.googleapis.com/google.rpc.ErrorInfo","reason":"API_KEY_INVALID"})";

std::string quotaFailure(const char* quotaId) {
    return std::string(R"({"@type":"type.googleapis.com/google.rpc.QuotaFailure","violations":[{"quotaId":")") +
           quotaId + R"("}]})";
}

const std::string kRetryIn30Seconds =
    R"({"@type":"type.googleapis.com/google.rpc.RetryInfo","retryDelay":"30s"})";

} // namespace

TEST_F(LLMClientTest, MissingApiKeyThrowsConfigurationErrorWithoutCallingGemini) {
    clearTestApiKey();
    bool transportCalled = false;
    LLMClient client([&transportCalled](const std::string&, const std::string&, const std::string&,
                                        std::string&, long&) {
        transportCalled = true;
        return true;
    });

    try {
        client.generate("Generate code", {});
        FAIL() << "expected ConfigurationError";
    } catch (const ConfigurationError& error) {
        EXPECT_STREQ(error.what(),
            "Configuration error: GEMINI_API_KEY is not configured. Set it in your environment or .env file.");
    }
    EXPECT_FALSE(transportCalled);
}

TEST_F(LLMClientTest, InvalidApiKeyIsAnAuthenticationFailureEvenAsHttp400) {
    // Gemini reports a bad key as 400 INVALID_ARGUMENT with reason API_KEY_INVALID.
    LLMClient client(respondWith(400, googleError(400, "INVALID_ARGUMENT", kApiKeyInvalid)));
    try {
        client.generate("Generate code", {});
        FAIL() << "expected AuthenticationError";
    } catch (const AuthenticationError& error) {
        EXPECT_STREQ(error.what(),
            "Authentication failed: Gemini rejected the configured credentials. Verify the API key.");
        EXPECT_NE(error.diagnostic().find("API_KEY_INVALID"), std::string::npos);
        EXPECT_EQ(error.diagnostic().find("free text"), std::string::npos);
    }

    EXPECT_THROW(LLMClient(respondWith(401, "{}")).generate("x", {}), AuthenticationError);
    EXPECT_THROW(LLMClient(respondWith(403, googleError(403, "PERMISSION_DENIED", "")))
                     .generate("x", {}),
                 AuthenticationError);
}

TEST_F(LLMClientTest, Http429IsARateLimitUnlessADailyQuotaIsExhausted) {
    try {
        LLMClient(respondWith(429, googleError(429, "RESOURCE_EXHAUSTED",
            quotaFailure("GenerateRequestsPerMinutePerProjectPerModel") + "," + kRetryIn30Seconds)))
            .generate("x", {});
        FAIL() << "expected RateLimitError";
    } catch (const RateLimitError& error) {
        EXPECT_FALSE(error.quotaExhausted());
        EXPECT_STREQ(error.code(), "PROVIDER_RATE_LIMITED");
        EXPECT_STREQ(error.what(),
            "Request rate limited. Wait briefly before retrying. Gemini suggests waiting 30s.");
    }

    // No details at all: still only a rate limit, never assumed to be quota.
    try {
        LLMClient(respondWith(429, "not json")).generate("x", {});
        FAIL() << "expected RateLimitError";
    } catch (const RateLimitError& error) {
        EXPECT_FALSE(error.quotaExhausted());
        EXPECT_FALSE(error.retryDelay().has_value());
    }

    try {
        LLMClient(respondWith(429, googleError(429, "RESOURCE_EXHAUSTED",
            quotaFailure("GenerateRequestsPerDayPerProjectPerModel-FreeTier"))))
            .generate("x", {});
        FAIL() << "expected RateLimitError";
    } catch (const RateLimitError& error) {
        EXPECT_TRUE(error.quotaExhausted());
        EXPECT_STREQ(error.code(), "PROVIDER_QUOTA_EXHAUSTED");
    }
}

TEST_F(LLMClientTest, ServerErrorsTimeoutsAndUnknownModelsHaveTheirOwnErrors) {
    EXPECT_THROW(LLMClient(respondWith(503, googleError(503, "UNAVAILABLE", ""))).generate("x", {}),
                 ProviderUnavailableError);
    EXPECT_THROW(LLMClient(respondWith(504, googleError(504, "DEADLINE_EXCEEDED", ""))).generate("x", {}),
                 TimeoutError);
    try {
        LLMClient(respondWith(404, googleError(404, "NOT_FOUND", "")), "gemini-missing").generate("x", {});
        FAIL() << "expected ProviderError";
    } catch (const ProviderError& error) {
        EXPECT_STREQ(error.code(), "PROVIDER_ERROR");
        EXPECT_NE(std::string(error.what()).find("gemini-missing"), std::string::npos);
    }
}

TEST_F(LLMClientTest, TransportExceptionsPropagateAndFailuresBecomeNetworkErrors) {
    LLMClient timingOut([](const std::string&, const std::string&, const std::string&,
                           std::string&, long&) -> bool {
        throw TimeoutError("libcurl error 28: Timeout was reached");
    });
    try {
        timingOut.generate("x", {});
        FAIL() << "expected TimeoutError";
    } catch (const NetworkError& error) {
        // Caught through its base class, but still the timeout.
        EXPECT_STREQ(error.code(), "PROVIDER_TIMEOUT");
        EXPECT_STREQ(error.what(), "Request timed out. Retry when the service is reachable.");
    }

    LLMClient unreachable([](const std::string&, const std::string&, const std::string&,
                             std::string&, long&) { return false; });
    try {
        unreachable.generate("x", {});
        FAIL() << "expected NetworkError";
    } catch (const NetworkError& error) {
        EXPECT_STREQ(error.what(), "Network error: Unable to connect to Gemini. Check your connection.");
    }

    // The bool API reports the same failure on standard error.
    testing::internal::CaptureStderr();
    EXPECT_FALSE(timingOut.generateContent("x", response));
    EXPECT_NE(testing::internal::GetCapturedStderr().find("[PROVIDER_TIMEOUT]"), std::string::npos);
}

TEST_F(LLMClientTest, UnusableResponsesThrowProviderResponseErrorsByKind) {
    const auto kindOf = [](const LLMClient& client, bool requireCompletion) {
        try {
            client.generate("x", {requireCompletion, false});
        } catch (const ProviderResponseError& error) {
            return error.kind();
        }
        ADD_FAILURE() << "expected ProviderResponseError";
        return ProviderResponseError::Kind::Invalid;
    };
    const std::string blocked =
        R"({"candidates":[{"finishReason":"SAFETY","content":{"parts":[{"text":"x"}]}}]})";
    const std::string truncated =
        R"({"candidates":[{"finishReason":"MAX_TOKENS","content":{"parts":[{"text":"x"}]}}]})";

    EXPECT_EQ(kindOf(LLMClient(respondWith(200, "not valid JSON")), false),
              ProviderResponseError::Kind::Invalid);
    EXPECT_EQ(kindOf(LLMClient(respondWith(200, R"({"candidates":[]})")), false),
              ProviderResponseError::Kind::Invalid);
    EXPECT_EQ(kindOf(LLMClient(respondWith(200, blocked)), false),
              ProviderResponseError::Kind::Blocked);
    EXPECT_EQ(kindOf(LLMClient(respondWith(200, truncated)), false),
              ProviderResponseError::Kind::Incomplete);
    // Without a finishReason a refactoring is not confirmed to be complete.
    EXPECT_EQ(kindOf(LLMClient(respondWith(200, validGeminiResponse())), true),
              ProviderResponseError::Kind::Incomplete);
}

TEST_F(LLMClientTest, ErrorsNeverContainTheApiKeyEvenWhenTheProviderEchoesIt) {
    setenv("GEMINI_API_KEY", "AIzaSyTESTKEY000000000000000000000000", 1);
    LLMClient client(respondWith(400, googleError(400, "INVALID_ARGUMENT", kApiKeyInvalid) +
        " AIzaSyTESTKEY000000000000000000000000"));
    try {
        client.generate("x", {});
        FAIL() << "expected AuthenticationError";
    } catch (const CodeAgentError& error) {
        EXPECT_EQ(std::string(error.what()).find("AIzaSy"), std::string::npos);
        EXPECT_EQ(error.diagnostic().find("AIzaSy"), std::string::npos);
    }
}

TEST_F(LLMClientTest, InvalidPromptIsAProgrammingErrorNotAProviderError) {
    LLMClient client(respondWith(200, validGeminiResponse()));
    EXPECT_THROW(client.generate("  \n", {}), std::invalid_argument);
}
