
#include "LLMClient.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <string>

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

    EXPECT_FALSE(client.generateContent("Generate code", response));
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
