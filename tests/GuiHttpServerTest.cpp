#include "GuiHttpServer.hpp"
#include "UnifiedDiff.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

class ScriptedNativeDialogProvider final : public NativeDialogProvider {
public:
    explicit ScriptedNativeDialogProvider(
        std::function<NativeDialogResult(NativeDialogKind)> callback
    ) : callback_(std::move(callback)) {}

    NativeDialogResult choose(NativeDialogKind kind) override {
        return callback_(kind);
    }

private:
    std::function<NativeDialogResult(NativeDialogKind)> callback_;
};

namespace {
std::filesystem::path repositoryRoot() {
    return std::filesystem::path(__FILE__).parent_path().parent_path();
}
} // namespace

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

class GuiHttpServerProposalReviewTest : public GuiHttpServerRefactoringTest {
protected:
    std::string createProposal() {
        auto http = client();
        const auto response = http.Post(
            "/api/refactor",
            requestBody(),
            "application/json"
        );
        if (!response || response->status != 200) {
            return {};
        }
        return nlohmann::json::parse(response->body).at("proposalId").get<std::string>();
    }
};

TEST_F(GuiHttpServerProposalReviewTest, DiffsBuffersAndProposalWithoutWritingOrTransitioning) {
    std::set<std::filesystem::path> filesBefore;
    for (const auto& entry : std::filesystem::directory_iterator(
             std::filesystem::current_path())) {
        filesBefore.insert(entry.path().filename());
    }

    auto http = client();
    const std::string original = "int value = 1;\n";
    const std::string proposed = "int value = 2;\n";
    const auto bufferDiff = http.Post(
        "/api/diff",
        nlohmann::json{
            {"originalCode", original},
            {"proposedCode", proposed}
        }.dump(),
        "application/json"
    );
    ASSERT_TRUE(bufferDiff);
    ASSERT_EQ(bufferDiff->status, 200);
    EXPECT_EQ(
        nlohmann::json::parse(bufferDiff->body).at("diff"),
        makeUnifiedDiff(original, proposed)
    );

    const std::string proposalId = createProposal();
    ASSERT_FALSE(proposalId.empty());
    const auto proposalDiff = http.Post(
        "/api/diff",
        nlohmann::json{{"proposalId", proposalId}}.dump(),
        "application/json"
    );
    ASSERT_TRUE(proposalDiff);
    ASSERT_EQ(proposalDiff->status, 200);
    EXPECT_EQ(
        nlohmann::json::parse(proposalDiff->body).at("diff"),
        makeUnifiedDiff("int value = 1;", "refactored: int value = 1;")
    );

    const auto approval = http.Post(
        "/api/changes/" + proposalId + "/approve"
    );
    ASSERT_TRUE(approval);
    EXPECT_EQ(approval->status, 200);
    EXPECT_EQ(nlohmann::json::parse(approval->body).at("status"), "APPROVED");

    std::set<std::filesystem::path> filesAfter;
    for (const auto& entry : std::filesystem::directory_iterator(
             std::filesystem::current_path())) {
        filesAfter.insert(entry.path().filename());
    }
    EXPECT_EQ(filesAfter, filesBefore);
}

TEST_F(GuiHttpServerProposalReviewTest, UnknownProposalIdsReturnSafeNotFoundErrors) {
    auto http = client();
    const auto diff = http.Post(
        "/api/diff",
        R"({"proposalId":"prop-999999"})",
        "application/json"
    );
    const auto approval = http.Post("/api/changes/prop-999999/approve");

    ASSERT_TRUE(diff);
    ASSERT_TRUE(approval);
    EXPECT_EQ(diff->status, 404);
    EXPECT_EQ(approval->status, 404);
    EXPECT_EQ(nlohmann::json::parse(diff->body).at("error").at("code"),
              "PROPOSAL_NOT_FOUND");
    EXPECT_EQ(nlohmann::json::parse(approval->body).at("error").at("code"),
              "PROPOSAL_NOT_FOUND");
    EXPECT_EQ(diff->body.find("prop-999999"), std::string::npos);
    EXPECT_EQ(approval->body.find("prop-999999"), std::string::npos);
}

TEST_F(GuiHttpServerProposalReviewTest, RejectsInvalidDiffRequests) {
    auto http = client();
    const auto malformed = http.Post("/api/diff", "{not json", "application/json");
    const auto extraFields = http.Post(
        "/api/diff",
        R"({"proposalId":"prop-1","originalCode":"a","proposedCode":"b"})",
        "application/json"
    );
    const auto wrongTypes = http.Post(
        "/api/diff",
        R"({"originalCode":"a","proposedCode":2})",
        "application/json"
    );
    const auto wrongMediaType = http.Post("/api/diff", "{}", "text/plain");
    const auto actionBody = http.Post(
        "/api/changes/prop-1/approve",
        "{}",
        "application/json"
    );

    ASSERT_TRUE(malformed);
    ASSERT_TRUE(extraFields);
    ASSERT_TRUE(wrongTypes);
    ASSERT_TRUE(wrongMediaType);
    ASSERT_TRUE(actionBody);
    EXPECT_EQ(malformed->status, 400);
    EXPECT_EQ(extraFields->status, 400);
    EXPECT_EQ(wrongTypes->status, 400);
    EXPECT_EQ(wrongMediaType->status, 415);
    EXPECT_EQ(actionBody->status, 400);
    EXPECT_EQ(nlohmann::json::parse(malformed->body).at("error").at("code"),
              "INVALID_JSON");
    EXPECT_EQ(nlohmann::json::parse(extraFields->body).at("error").at("code"),
              "INVALID_REQUEST");
    EXPECT_EQ(nlohmann::json::parse(wrongTypes->body).at("error").at("code"),
              "INVALID_REQUEST");
}

TEST_F(GuiHttpServerProposalReviewTest, OnlyPendingProposalsCanTransitionOnce) {
    auto http = client();
    const std::string approvedId = createProposal();
    ASSERT_FALSE(approvedId.empty());
    const auto approved = http.Post("/api/changes/" + approvedId + "/approve");
    const auto repeatedApproval = http.Post("/api/changes/" + approvedId + "/approve");
    const auto rejectionAfterApproval = http.Post("/api/changes/" + approvedId + "/reject");

    ASSERT_TRUE(approved);
    ASSERT_TRUE(repeatedApproval);
    ASSERT_TRUE(rejectionAfterApproval);
    EXPECT_EQ(approved->status, 200);
    EXPECT_EQ(nlohmann::json::parse(approved->body).at("status"), "APPROVED");
    EXPECT_EQ(repeatedApproval->status, 409);
    EXPECT_EQ(rejectionAfterApproval->status, 409);
    EXPECT_EQ(nlohmann::json::parse(repeatedApproval->body).at("error").at("code"),
              "INVALID_PROPOSAL_STATE");

    const std::string rejectedId = createProposal();
    ASSERT_FALSE(rejectedId.empty());
    const auto rejected = http.Post("/api/changes/" + rejectedId + "/reject");
    const auto approvalAfterRejection = http.Post("/api/changes/" + rejectedId + "/approve");
    ASSERT_TRUE(rejected);
    ASSERT_TRUE(approvalAfterRejection);
    EXPECT_EQ(rejected->status, 200);
    EXPECT_EQ(nlohmann::json::parse(rejected->body).at("status"), "REJECTED");
    EXPECT_EQ(approvalAfterRejection->status, 409);
}

TEST_F(GuiHttpServerProposalReviewTest, ConcurrentApprovalAndRejectionHaveOneWinner) {
    const std::string proposalId = createProposal();
    ASSERT_FALSE(proposalId.empty());

    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    int approvalStatus = 0;
    int rejectionStatus = 0;
    std::string approvalResult;
    std::string rejectionResult;

    std::thread approvalThread([&] {
        httplib::Client http("127.0.0.1", server_->port());
        ready.fetch_add(1);
        while (!start.load()) {
            std::this_thread::yield();
        }
        const auto response = http.Post("/api/changes/" + proposalId + "/approve");
        approvalStatus = response ? response->status : -1;
        if (response) {
            approvalResult = response->body;
        }
    });
    std::thread rejectionThread([&] {
        httplib::Client http("127.0.0.1", server_->port());
        ready.fetch_add(1);
        while (!start.load()) {
            std::this_thread::yield();
        }
        const auto response = http.Post("/api/changes/" + proposalId + "/reject");
        rejectionStatus = response ? response->status : -1;
        if (response) {
            rejectionResult = response->body;
        }
    });

    while (ready.load() != 2) {
        std::this_thread::yield();
    }
    start = true;
    approvalThread.join();
    rejectionThread.join();

    EXPECT_TRUE(
        (approvalStatus == 200 && rejectionStatus == 409) ||
        (approvalStatus == 409 && rejectionStatus == 200)
    );
    const std::string& winningBody = approvalStatus == 200
        ? approvalResult
        : rejectionResult;
    const std::string winningStatus =
        nlohmann::json::parse(winningBody).at("status").get<std::string>();
    EXPECT_TRUE(winningStatus == "APPROVED" || winningStatus == "REJECTED");
}

TEST(GuiHttpServerDialogTest, FileSelectionReturnsPathOnlyAndDoesNotModifySource) {
    const auto sourcePath = repositoryRoot() / "src/main.cpp";
    ASSERT_TRUE(std::filesystem::is_regular_file(sourcePath));
    const auto initialSize = std::filesystem::file_size(sourcePath);
    const auto initialWriteTime = std::filesystem::last_write_time(sourcePath);
    auto provider = std::make_shared<ScriptedNativeDialogProvider>(
        [sourcePath](NativeDialogKind kind) {
            EXPECT_EQ(kind, NativeDialogKind::SourceFile);
            return NativeDialogResult{NativeDialogStatus::Selected, sourcePath};
        }
    );
    GuiHttpServer server(0, std::filesystem::current_path(), {}, {}, provider);
    ASSERT_TRUE(server.start());

    httplib::Client http("127.0.0.1", server.port());
    const auto response = http.Post("/api/files/open-dialog", "{}", "application/json");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    const auto body = nlohmann::json::parse(response->body);
    EXPECT_EQ(body, nlohmann::json({{"status", "SELECTED"},
                                   {"selectedPath", sourcePath.string()}}));
    EXPECT_FALSE(body.contains("contents"));
    EXPECT_EQ(std::filesystem::file_size(sourcePath), initialSize);
    EXPECT_EQ(std::filesystem::last_write_time(sourcePath), initialWriteTime);
    server.stop();
}

TEST(GuiHttpServerDialogTest, CancellationAndDialogFailuresAreSafeAndDoNotChangeConfig) {
    const std::vector<std::pair<NativeDialogResult, int>> outcomes = {
        {{NativeDialogStatus::Cancelled, {}}, 200},
        {{NativeDialogStatus::Unavailable, {}}, 503},
        {{NativeDialogStatus::TimedOut, {}}, 504},
        {{NativeDialogStatus::Failed, {}}, 500}
    };
    for (const auto& outcome : outcomes) {
        auto provider = std::make_shared<ScriptedNativeDialogProvider>(
            [result = outcome.first](NativeDialogKind) { return result; }
        );
        GuiHttpServer server(0, std::filesystem::current_path(), {}, {}, provider);
        ASSERT_TRUE(server.start());
        httplib::Client http("127.0.0.1", server.port());
        const auto response = http.Post(
            "/api/output/select-directory", "{}", "application/json"
        );
        ASSERT_TRUE(response);
        EXPECT_EQ(response->status, outcome.second);
        const auto body = nlohmann::json::parse(response->body);
        if (outcome.first.status == NativeDialogStatus::Cancelled) {
            EXPECT_EQ(body.at("status"), "CANCELLED");
        } else {
            EXPECT_EQ(body.at("error").at("code"),
                      outcome.first.status == NativeDialogStatus::Unavailable
                          ? "DIALOG_UNAVAILABLE"
                          : outcome.first.status == NativeDialogStatus::TimedOut
                                ? "DIALOG_TIMEOUT"
                                : "DIALOG_FAILED");
            EXPECT_NE(body.at("error").at("message").get<std::string>().find("Native directory dialog"),
                      std::string::npos);
            EXPECT_EQ(response->body.find("secret"), std::string::npos);
        }
        const auto config = http.Get("/api/config");
        ASSERT_TRUE(config);
        EXPECT_EQ(nlohmann::json::parse(config->body).at("activeOutputDirectory"),
                  std::filesystem::current_path().string());
        server.stop();
    }
}

TEST(GuiHttpServerDialogTest, RejectsInvalidSelectionsAndRequestsWithoutLeakingPaths) {
    const auto invalidPath = repositoryRoot() / "docs/gui-api-contract.md";
    auto provider = std::make_shared<ScriptedNativeDialogProvider>(
        [invalidPath](NativeDialogKind) {
            return NativeDialogResult{
                NativeDialogStatus::Selected,
                invalidPath
            };
        }
    );
    GuiHttpServer server(0, std::filesystem::current_path(), {}, {}, provider);
    ASSERT_TRUE(server.start());
    httplib::Client http("127.0.0.1", server.port());

    const auto invalidSource = http.Post("/api/files/open-dialog", "{}", "application/json");
    const auto invalidOutput = http.Post("/api/output/select-directory", "{}", "application/json");
    const auto invalidBody = http.Post("/api/files/open-dialog", "[]", "application/json");
    const auto wrongType = http.Post("/api/files/open-dialog", "{}", "text/plain");
    ASSERT_TRUE(invalidSource);
    ASSERT_TRUE(invalidOutput);
    ASSERT_TRUE(invalidBody);
    ASSERT_TRUE(wrongType);
    EXPECT_EQ(invalidSource->status, 422);
    EXPECT_EQ(nlohmann::json::parse(invalidSource->body).at("error").at("code"),
              "INVALID_SOURCE_FILE");
    EXPECT_EQ(invalidOutput->status, 422);
    EXPECT_EQ(nlohmann::json::parse(invalidOutput->body).at("error").at("code"),
              "INVALID_OUTPUT_DIRECTORY");
    EXPECT_EQ(invalidBody->status, 400);
    EXPECT_EQ(wrongType->status, 415);
    EXPECT_EQ(invalidSource->body.find(invalidPath.string()), std::string::npos);

    const auto missingOutput = http.Post(
        "/api/output/select-directory",
        R"({"directoryPath":"/path/that/does/not/exist"})",
        "application/json"
    );
    ASSERT_TRUE(missingOutput);
    EXPECT_EQ(missingOutput->status, 422);
    const auto config = http.Get("/api/config");
    ASSERT_TRUE(config);
    EXPECT_EQ(nlohmann::json::parse(config->body).at("activeOutputDirectory"),
              std::filesystem::current_path().string());
    server.stop();
}

TEST(GuiHttpServerDialogTest, UpdatesOutputDirectoryForManualAndConcurrentSelections) {
    const auto firstDirectory = repositoryRoot();
    const auto secondDirectory = repositoryRoot() / "include";
    ASSERT_TRUE(std::filesystem::is_directory(secondDirectory));
    std::atomic<unsigned int> next{0};
    auto provider = std::make_shared<ScriptedNativeDialogProvider>(
        [firstDirectory, secondDirectory, &next](NativeDialogKind kind) {
            EXPECT_EQ(kind, NativeDialogKind::OutputDirectory);
            return NativeDialogResult{
                NativeDialogStatus::Selected,
                next.fetch_add(1) % 2 == 0 ? firstDirectory : secondDirectory
            };
        }
    );
    GuiHttpServer server(0, firstDirectory, {}, {}, provider);
    ASSERT_TRUE(server.start());
    httplib::Client http("127.0.0.1", server.port());

    const auto manual = http.Post(
        "/api/output/select-directory",
        nlohmann::json{{"directoryPath", secondDirectory.string()}}.dump(),
        "application/json"
    );
    ASSERT_TRUE(manual);
    EXPECT_EQ(manual->status, 200);
    EXPECT_EQ(nlohmann::json::parse(manual->body).at("activeOutputDirectory"),
              secondDirectory.string());

    std::atomic<int> successes{0};
    std::vector<std::thread> requests;
    for (int index = 0; index < 8; ++index) {
        requests.emplace_back([&] {
            httplib::Client client("127.0.0.1", server.port());
            const auto response = client.Post(
                "/api/output/select-directory", "{}", "application/json"
            );
            if (response && response->status == 200) {
                ++successes;
            }
        });
    }
    for (auto& request : requests) {
        request.join();
    }
    EXPECT_EQ(successes.load(), 8);
    const auto config = http.Get("/api/config");
    ASSERT_TRUE(config);
    const auto activeDirectory =
        nlohmann::json::parse(config->body).at("activeOutputDirectory");
    EXPECT_TRUE(activeDirectory == firstDirectory.string() ||
                activeDirectory == secondDirectory.string());
    server.stop();
}
