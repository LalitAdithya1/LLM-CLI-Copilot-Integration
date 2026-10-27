#include "GuiHttpServer.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

TEST(GuiHttpServerTest, StartsOnAnAvailableLoopbackPortAndStops) {
    GuiHttpServer server(0);

    ASSERT_TRUE(server.start());
    EXPECT_TRUE(server.isRunning());
    EXPECT_GT(server.port(), 0);

    httplib::Client client("127.0.0.1", server.port());
    const auto response = client.Get("/");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 404);

    server.stop();
    EXPECT_FALSE(server.isRunning());
    server.stop();
}

TEST(GuiHttpServerTest, RejectsInvalidPort) {
    GuiHttpServer server(65536);

    EXPECT_FALSE(server.start());
    EXPECT_FALSE(server.isRunning());
    EXPECT_EQ(server.port(), 0);
}

TEST(GuiHttpServerTest, RejectsNegativePortAndCannotBeRestarted) {
    GuiHttpServer server(-1);

    EXPECT_FALSE(server.start());
    EXPECT_FALSE(server.isRunning());
    EXPECT_EQ(server.port(), 0);
    EXPECT_FALSE(server.start());
}

TEST(GuiHttpServerTest, RejectsPortAlreadyHeldByAnotherServer) {
    GuiHttpServer owner(0);
    ASSERT_TRUE(owner.start());

    GuiHttpServer contender(owner.port());
    EXPECT_FALSE(contender.start());
    EXPECT_FALSE(contender.isRunning());
    EXPECT_EQ(contender.port(), 0);
    EXPECT_FALSE(contender.start());

    EXPECT_TRUE(owner.isRunning());
    httplib::Client client("127.0.0.1", owner.port());
    const auto response = client.Get("/");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 404);

    contender.stop();
    owner.stop();
}

TEST(GuiHttpServerTest, StopsWorkerWhenDestroyedWithoutExplicitStop) {
    {
        GuiHttpServer server(0);
        ASSERT_TRUE(server.start());
        EXPECT_TRUE(server.isRunning());
    }
    SUCCEED();
}

TEST(GuiHttpServerTest, DoesNotAllowStartingTwice) {
    GuiHttpServer server(0);

    ASSERT_TRUE(server.start());
    EXPECT_FALSE(server.start());
    server.stop();
}

TEST(GuiHttpServerTest, HealthEndpointReturnsJsonSuccess) {
    GuiHttpServer server(0);
    ASSERT_TRUE(server.start());

    httplib::Client client("127.0.0.1", server.port());
    const auto response = client.Get("/api/health");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_NE(response->get_header_value("Content-Type").find("application/json"),
              std::string::npos);
    const auto body = nlohmann::json::parse(response->body);
    ASSERT_TRUE(body.is_object());
    EXPECT_EQ(body.at("status"), "ok");

    server.stop();
}

TEST(GuiHttpServerTest, ConfigReturnsSafeDirectoriesAndSupportedExtensions) {
    const std::filesystem::path launchDirectory =
        std::filesystem::current_path() / "gui-launch-directory";
    GuiHttpServer server(0, launchDirectory);
    ASSERT_TRUE(server.start());

    httplib::Client client("127.0.0.1", server.port());
    const auto response = client.Get("/api/config");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_NE(response->get_header_value("Content-Type").find("application/json"),
              std::string::npos);
    const auto body = nlohmann::json::parse(response->body);
    ASSERT_TRUE(body.is_object());
    EXPECT_EQ(body.at("launchDirectory"), launchDirectory.string());
    EXPECT_EQ(body.at("activeOutputDirectory"), launchDirectory.string());
    EXPECT_EQ(
        body.at("supportedExtensions"),
        nlohmann::json({".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"})
    );
    EXPECT_EQ(body.size(), 3u);
    EXPECT_FALSE(body.contains("apiKey"));
    EXPECT_FALSE(body.contains("GEMINI_API_KEY"));
    EXPECT_FALSE(body.contains("environment"));

    server.stop();
}

TEST(GuiHttpServerTest, UnknownRouteStillReturnsNotFound) {
    GuiHttpServer server(0);
    ASSERT_TRUE(server.start());

    httplib::Client client("127.0.0.1", server.port());
    const auto response = client.Get("/");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 404);

    server.stop();
}

TEST(GuiHttpServerTest, GenerationWithoutHandlerReturnsServiceUnavailable) {
    GuiHttpServer server(0);
    ASSERT_TRUE(server.start());

    httplib::Client http("127.0.0.1", server.port());
    const auto response = http.Post(
        "/api/generate",
        R"({"instruction":"write a greeting","language":"C++"})",
        "application/json"
    );

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 503);
    EXPECT_EQ(
        nlohmann::json::parse(response->body).at("error").at("code"),
        "GENERATION_UNAVAILABLE"
    );

    server.stop();
}

class GuiHttpServerGenerationTest : public ::testing::Test {
protected:
    void SetUp() override {
        server_ = std::make_unique<GuiHttpServer>(
            0,
            std::filesystem::current_path(),
            [this](const std::string& instruction,
                   const std::string& language,
                   std::string& generatedCode) {
                generationCalls_.fetch_add(1);
                if (instruction == "throw") {
                    throw std::runtime_error("provider detail with secret");
                }
                if (!generationSucceeds_.load()) {
                    return false;
                }
                generatedCode = "generated: " + instruction + " (" + language + ")";
                return true;
            }
        );
        ASSERT_TRUE(server_->start());
    }

    void TearDown() override {
        if (server_) {
            server_->stop();
        }
    }

    httplib::Client client() const {
        return httplib::Client("127.0.0.1", server_->port());
    }

    std::unique_ptr<GuiHttpServer> server_;
    std::atomic<bool> generationSucceeds_{true};
    std::atomic<int> generationCalls_{0};
};

TEST_F(GuiHttpServerGenerationTest, GeneratesCodeAndReturnsContractFields) {
    std::set<std::filesystem::path> filesBefore;
    for (const auto& entry : std::filesystem::directory_iterator(
             std::filesystem::current_path())) {
        filesBefore.insert(entry.path().filename());
    }

    auto http = client();
    const auto response = http.Post(
        "/api/generate",
        R"({"instruction":"write a greeting","language":"C++"})",
        "application/json"
    );

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_NE(response->get_header_value("Content-Type").find("application/json"),
              std::string::npos);
    const auto body = nlohmann::json::parse(response->body);
    EXPECT_EQ(body.size(), 2u);
    EXPECT_EQ(body.at("generatedCode"), "generated: write a greeting (C++)");
    EXPECT_EQ(body.at("language"), "C++");
    EXPECT_FALSE(body.contains("savedPath"));
    EXPECT_EQ(generationCalls_.load(), 1);

    std::set<std::filesystem::path> filesAfter;
    for (const auto& entry : std::filesystem::directory_iterator(
             std::filesystem::current_path())) {
        filesAfter.insert(entry.path().filename());
    }
    EXPECT_EQ(filesAfter, filesBefore);
}

TEST_F(GuiHttpServerGenerationTest, RejectsMalformedJson) {
    auto http = client();
    const auto response = http.Post(
        "/api/generate",
        "{not json",
        "application/json"
    );

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 400);
    const auto body = nlohmann::json::parse(response->body);
    EXPECT_EQ(body.at("error").at("code"), "INVALID_JSON");
    EXPECT_EQ(generationCalls_.load(), 0);
}

TEST_F(GuiHttpServerGenerationTest, RejectsNonJsonContentType) {
    auto http = client();
    const auto response = http.Post(
        "/api/generate",
        R"({"instruction":"write a greeting","language":"C++"})",
        "text/plain"
    );

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 415);
    EXPECT_EQ(
        nlohmann::json::parse(response->body).at("error").at("code"),
        "UNSUPPORTED_MEDIA_TYPE"
    );
    EXPECT_EQ(generationCalls_.load(), 0);
}

TEST_F(GuiHttpServerGenerationTest, RejectsMissingInstruction) {
    auto http = client();
    const auto response = http.Post(
        "/api/generate",
        R"({"language":"C++"})",
        "application/json"
    );

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 400);
    const auto body = nlohmann::json::parse(response->body);
    EXPECT_EQ(body.at("error").at("code"), "INVALID_REQUEST");
    EXPECT_EQ(generationCalls_.load(), 0);
}

TEST_F(GuiHttpServerGenerationTest, RejectsWhitespaceOnlyInstructionAndLanguage) {
    auto http = client();
    const auto emptyInstruction = http.Post(
        "/api/generate",
        R"({"instruction":" \t\n","language":"C++"})",
        "application/json"
    );
    const auto emptyLanguage = http.Post(
        "/api/generate",
        R"({"instruction":"write a greeting","language":" \t\n"})",
        "application/json"
    );

    ASSERT_TRUE(emptyInstruction);
    ASSERT_TRUE(emptyLanguage);
    EXPECT_EQ(emptyInstruction->status, 400);
    EXPECT_EQ(emptyLanguage->status, 400);
    EXPECT_EQ(
        nlohmann::json::parse(emptyInstruction->body).at("error").at("code"),
        "INVALID_INSTRUCTION"
    );
    EXPECT_EQ(
        nlohmann::json::parse(emptyLanguage->body).at("error").at("code"),
        "INVALID_LANGUAGE"
    );
    EXPECT_EQ(generationCalls_.load(), 0);
}

TEST_F(GuiHttpServerGenerationTest, RejectsExtraFieldsIncludingCredentialFields) {
    auto http = client();
    const auto response = http.Post(
        "/api/generate",
        R"({"instruction":"write a greeting","language":"C++","apiKey":"must-not-be-accepted"})",
        "application/json"
    );

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 400);
    EXPECT_EQ(
        nlohmann::json::parse(response->body).at("error").at("code"),
        "INVALID_REQUEST"
    );
    EXPECT_EQ(response->body.find("must-not-be-accepted"), std::string::npos);
    EXPECT_EQ(generationCalls_.load(), 0);
}

TEST_F(GuiHttpServerGenerationTest, ReportsGenerationFailureWithoutExceptionDetails) {
    generationSucceeds_ = false;
    auto http = client();
    const auto providerFailure = http.Post(
        "/api/generate",
        R"({"instruction":"write a greeting","language":"C++"})",
        "application/json"
    );
    const auto thrownFailure = http.Post(
        "/api/generate",
        R"({"instruction":"throw","language":"C++"})",
        "application/json"
    );

    ASSERT_TRUE(providerFailure);
    ASSERT_TRUE(thrownFailure);
    EXPECT_EQ(providerFailure->status, 502);
    EXPECT_EQ(thrownFailure->status, 502);
    EXPECT_EQ(
        nlohmann::json::parse(providerFailure->body).at("error").at("code"),
        "GENERATION_FAILED"
    );
    EXPECT_EQ(
        nlohmann::json::parse(thrownFailure->body).at("error").at("code"),
        "GENERATION_FAILED"
    );
    EXPECT_EQ(thrownFailure->body.find("secret"), std::string::npos);
}

TEST_F(GuiHttpServerGenerationTest, RejectsRequestBodiesOverOneMiB) {
    auto http = client();
    const std::string oversizedBody =
        "{\"instruction\":\"" + std::string(1024 * 1024, 'x') +
        "\",\"language\":\"C++\"}";
    const auto response = http.Post(
        "/api/generate",
        oversizedBody,
        "application/json"
    );

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 413);
    EXPECT_NE(response->get_header_value("Content-Type").find("application/json"),
              std::string::npos);
    EXPECT_EQ(
        nlohmann::json::parse(response->body),
        nlohmann::json({
            {"error", {
                {"code", "PAYLOAD_TOO_LARGE"},
                {"message", "Request body exceeds the 1 MiB limit."}
            }}
        })
    );
    EXPECT_EQ(generationCalls_.load(), 0);
}

class GuiHttpServerRefactoringTest : public ::testing::Test {
protected:
    void SetUp() override {
        server_ = std::make_unique<GuiHttpServer>(
            0,
            std::filesystem::current_path(),
            GuiHttpServer::GenerationHandler{},
            [this](const std::string& instruction,
                   const std::string&,
                   const std::string& sourceCode,
                   std::string& refactoredCode) {
                handlerCalls_.fetch_add(1);
                if (instruction == "blocked" || instruction == "incomplete") {
                    return false;
                }
                if (instruction == "throw") {
                    throw std::runtime_error("provider detail with secret");
                }
                if (instruction == "empty result") {
                    refactoredCode = " \t\n";
                    return true;
                }
                refactoredCode = "refactored: " + sourceCode;
                return true;
            }
        );
        ASSERT_TRUE(server_->start());
    }

    void TearDown() override {
        if (server_) {
            server_->stop();
        }
    }

    httplib::Client client() const {
        return httplib::Client("127.0.0.1", server_->port());
    }

    static std::string requestBody(
        const std::string& sourceCode = "int value = 1;",
        const std::string& instruction = "improve readability",
        const std::string& language = "C++",
        const std::string& editorRevision = "revision-1"
    ) {
        return nlohmann::json{
            {"sourceCode", sourceCode},
            {"instruction", instruction},
            {"language", language},
            {"editorRevision", editorRevision}
        }.dump();
    }

    std::unique_ptr<GuiHttpServer> server_;
    std::atomic<int> handlerCalls_{0};
};

TEST(GuiHttpServerTest, RefactoringWithoutHandlerReturnsServiceUnavailable) {
    GuiHttpServer server(
        0,
        std::filesystem::current_path(),
        GuiHttpServer::GenerationHandler{},
        GuiHttpServer::RefactoringHandler{}
    );
    ASSERT_TRUE(server.start());

    httplib::Client http("127.0.0.1", server.port());
    const auto response = http.Post(
        "/api/refactor",
        R"({"sourceCode":"int x;","instruction":"change","language":"C++","editorRevision":"r1"})",
        "application/json"
    );

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 503);
    EXPECT_EQ(nlohmann::json::parse(response->body).at("error").at("code"),
              "REFACTOR_UNAVAILABLE");
    server.stop();
}

TEST_F(GuiHttpServerRefactoringTest, CreatesPendingProposalWithoutWritingFiles) {
    std::set<std::filesystem::path> filesBefore;
    for (const auto& entry : std::filesystem::directory_iterator(
             std::filesystem::current_path())) {
        filesBefore.insert(entry.path().filename());
    }

    auto http = client();
    const auto response = http.Post(
        "/api/refactor",
        requestBody("a"),
        "application/json"
    );

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_NE(response->get_header_value("Content-Type").find("application/json"),
              std::string::npos);
    const auto body = nlohmann::json::parse(response->body);
    ASSERT_EQ(body.size(), 5u);
    EXPECT_EQ(body.at("proposalId"), "prop-1");
    EXPECT_EQ(body.at("status"), "PENDING");
    EXPECT_EQ(body.at("originalCode"), "a");
    EXPECT_EQ(body.at("proposedCode"), "refactored: a");
    EXPECT_EQ(body.at("baseEditorHash"), "fnv1a64:af63dc4c8601ec8c");
    EXPECT_EQ(handlerCalls_.load(), 1);

    std::set<std::filesystem::path> filesAfter;
    for (const auto& entry : std::filesystem::directory_iterator(
             std::filesystem::current_path())) {
        filesAfter.insert(entry.path().filename());
    }
    EXPECT_EQ(filesAfter, filesBefore);
}

TEST_F(GuiHttpServerRefactoringTest, RejectsMalformedAndInvalidRequests) {
    auto http = client();
    const auto malformed = http.Post("/api/refactor", "{not json", "application/json");
    const auto missing = http.Post(
        "/api/refactor",
        R"({"sourceCode":"int x;","instruction":"change","language":"C++"})",
        "application/json"
    );
    auto extraRequest = nlohmann::json::parse(requestBody());
    extraRequest["apiKey"] = "must-not-be-accepted";
    const auto extra = http.Post(
        "/api/refactor",
        extraRequest.dump(),
        "application/json"
    );
    const auto emptySource = http.Post(
        "/api/refactor",
        requestBody(" \t\n"),
        "application/json"
    );
    const auto emptyInstruction = http.Post(
        "/api/refactor",
        requestBody("int x;", " \t\n"),
        "application/json"
    );
    const auto emptyLanguage = http.Post(
        "/api/refactor",
        requestBody("int x;", "change", " \t\n"),
        "application/json"
    );
    const auto emptyRevision = http.Post(
        "/api/refactor",
        requestBody("int x;", "change", "C++", " \t\n"),
        "application/json"
    );

    ASSERT_TRUE(malformed);
    ASSERT_TRUE(missing);
    ASSERT_TRUE(extra);
    ASSERT_TRUE(emptySource);
    ASSERT_TRUE(emptyInstruction);
    ASSERT_TRUE(emptyLanguage);
    ASSERT_TRUE(emptyRevision);
    EXPECT_EQ(malformed->status, 400);
    EXPECT_EQ(nlohmann::json::parse(malformed->body).at("error").at("code"), "INVALID_JSON");
    EXPECT_EQ(nlohmann::json::parse(missing->body).at("error").at("code"), "INVALID_REQUEST");
    EXPECT_EQ(nlohmann::json::parse(extra->body).at("error").at("code"), "INVALID_REQUEST");
    EXPECT_EQ(extra->body.find("must-not-be-accepted"), std::string::npos);
    EXPECT_EQ(nlohmann::json::parse(emptySource->body).at("error").at("code"), "INVALID_SOURCE_CODE");
    EXPECT_EQ(nlohmann::json::parse(emptyInstruction->body).at("error").at("code"), "INVALID_INSTRUCTION");
    EXPECT_EQ(nlohmann::json::parse(emptyLanguage->body).at("error").at("code"), "INVALID_LANGUAGE");
    EXPECT_EQ(nlohmann::json::parse(emptyRevision->body).at("error").at("code"), "INVALID_EDITOR_REVISION");
    EXPECT_EQ(handlerCalls_.load(), 0);
}

TEST_F(GuiHttpServerRefactoringTest, RejectsNonJsonContentType) {
    auto http = client();
    const auto response = http.Post(
        "/api/refactor",
        requestBody(),
        "text/plain"
    );

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 415);
    EXPECT_EQ(nlohmann::json::parse(response->body).at("error").at("code"),
              "UNSUPPORTED_MEDIA_TYPE");
    EXPECT_EQ(handlerCalls_.load(), 0);
}

TEST_F(GuiHttpServerRefactoringTest, RejectsOversizedRequestBody) {
    auto http = client();
    const std::string oversizedBody = requestBody(std::string(1024 * 1024, 'x'));
    const auto response = http.Post(
        "/api/refactor",
        oversizedBody,
        "application/json"
    );

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 413);
    EXPECT_EQ(nlohmann::json::parse(response->body).at("error").at("code"),
              "PAYLOAD_TOO_LARGE");
    EXPECT_EQ(handlerCalls_.load(), 0);
}

TEST_F(GuiHttpServerRefactoringTest, BlockedAndIncompleteResultsCreateNoProposal) {
    auto http = client();
    for (const char* instruction : {"blocked", "incomplete"}) {
        const auto response = http.Post(
            "/api/refactor",
            requestBody("int x;", instruction),
            "application/json"
        );
        ASSERT_TRUE(response);
        EXPECT_EQ(response->status, 502);
        EXPECT_EQ(nlohmann::json::parse(response->body).at("error").at("code"),
                  "REFACTOR_FAILED");
        EXPECT_EQ(response->body.find("PENDING"), std::string::npos);
    }

    const auto success = http.Post(
        "/api/refactor",
        requestBody(),
        "application/json"
    );
    ASSERT_TRUE(success);
    EXPECT_EQ(nlohmann::json::parse(success->body).at("proposalId"), "prop-1");
    EXPECT_EQ(handlerCalls_.load(), 3);
}

TEST_F(GuiHttpServerRefactoringTest, ProviderFailuresAndEmptyResultsReturnSafeErrors) {
    auto http = client();
    const auto thrownFailure = http.Post(
        "/api/refactor",
        requestBody("int x;", "throw"),
        "application/json"
    );
    const auto emptyResult = http.Post(
        "/api/refactor",
        requestBody("int x;", "empty result"),
        "application/json"
    );

    ASSERT_TRUE(thrownFailure);
    ASSERT_TRUE(emptyResult);
    EXPECT_EQ(thrownFailure->status, 502);
    EXPECT_EQ(emptyResult->status, 502);
    EXPECT_EQ(nlohmann::json::parse(thrownFailure->body).at("error").at("code"),
              "REFACTOR_FAILED");
    EXPECT_EQ(thrownFailure->body.find("secret"), std::string::npos);
    EXPECT_EQ(thrownFailure->body.find("provider detail"), std::string::npos);
    EXPECT_EQ(nlohmann::json::parse(emptyResult->body).at("error").at("code"),
              "REFACTOR_FAILED");
}
