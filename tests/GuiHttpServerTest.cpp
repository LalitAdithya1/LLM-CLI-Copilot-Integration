#include "GuiHttpServer.hpp"
#include "CodeAgentError.hpp"
#include "UnifiedDiff.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <sys/stat.h>
#include <unistd.h>

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

std::string ownOrigin(const GuiHttpServer& server) {
    return "http://127.0.0.1:" + std::to_string(server.port());
}

// Obtains the session token the way the page does: by redeeming a bootstrap
// code at /api/session.
std::string sessionToken(GuiHttpServer& server) {
    httplib::Client client("127.0.0.1", server.port());
    const auto response = client.Post(
        "/api/session", httplib::Headers{{"Origin", ownOrigin(server)}},
        nlohmann::json{{"bootstrap", server.issueBootstrapCode()}}.dump(), "application/json");
    if (!response || response->status != 200) return {};
    return nlohmann::json::parse(response->body).at("token").get<std::string>();
}

httplib::Headers authHeaders(GuiHttpServer& server) {
    return {{GuiHttpServer::kSessionTokenHeader, sessionToken(server)}};
}

// A client that sends the session token with every request.
httplib::Client apiClient(GuiHttpServer& server) {
    httplib::Client client("127.0.0.1", server.port());
    client.set_default_headers(authHeaders(server));
    return client;
}
} // namespace

TEST(GuiHttpServerTest, StartsOnAnAvailableLoopbackPortAndStops) {
    GuiHttpServer server(0);

    ASSERT_TRUE(server.start());
    EXPECT_TRUE(server.isRunning());
    EXPECT_GT(server.port(), 0);

    auto client = apiClient(server);
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
    auto client = apiClient(owner);
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

    auto client = apiClient(server);
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

TEST(GuiHttpServerTest, DevelopmentOriginsAreRejectedUnlessExplicitlyConfigured) {
    GuiHttpServer server(0);
    ASSERT_TRUE(server.start());
    EXPECT_FALSE(server.allowDevelopmentOrigin("http://127.0.0.1:5500"))
        << "the origin policy cannot change while running";
    auto client = apiClient(server);
    for (const auto& origin : {"http://127.0.0.1:5500", "http://localhost:5500"}) {
        const auto response = client.Get("/api/health", httplib::Headers{{"Origin", origin}});
        ASSERT_TRUE(response);
        EXPECT_EQ(response->status, 403);
        EXPECT_TRUE(response->get_header_value("Access-Control-Allow-Origin").empty());
    }
    server.stop();
}

TEST(GuiHttpServerTest, AllowsOnlyTheExactConfiguredDevelopmentOrigin) {
    GuiHttpServer server(0);
    for (const auto& invalid : {"*", "http://example.com:5500", "https://127.0.0.1:5500",
                                "http://127.0.0.1", "http://127.0.0.1:5500/", "http://127.0.0.1:0",
                                "http://127.0.0.1:99999", "null"}) {
        EXPECT_FALSE(server.allowDevelopmentOrigin(invalid)) << invalid;
    }
    ASSERT_TRUE(server.allowDevelopmentOrigin("http://127.0.0.1:5500"));
    ASSERT_TRUE(server.start());
    auto client = apiClient(server);
    const auto allowed = client.Get("/api/health", httplib::Headers{{"Origin", "http://127.0.0.1:5500"}});
    ASSERT_TRUE(allowed);
    EXPECT_EQ(allowed->status, 200);
    EXPECT_EQ(allowed->get_header_value("Access-Control-Allow-Origin"), "http://127.0.0.1:5500");
    EXPECT_EQ(allowed->get_header_value("Vary"), "Origin");
    const auto other = client.Get("/api/health", httplib::Headers{{"Origin", "http://localhost:5500"}});
    ASSERT_TRUE(other);
    EXPECT_EQ(other->status, 403);
    server.stop();
}

TEST(GuiHttpServerTest, HandlesAllowedJsonPostPreflightWithoutTheToken) {
    GuiHttpServer server(0);
    ASSERT_TRUE(server.allowDevelopmentOrigin("http://127.0.0.1:5500"));
    ASSERT_TRUE(server.start());
    // Browsers cannot attach the token to a preflight, so none is sent here.
    httplib::Client client("127.0.0.1", server.port());
    httplib::Headers headers{
        {"Origin", "http://127.0.0.1:5500"},
        {"Access-Control-Request-Method", "POST"},
        {"Access-Control-Request-Headers", "content-type, x-codeagent-token"}
    };
    const auto response = client.Options("/api/save", headers);
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 204);
    EXPECT_TRUE(response->body.empty());
    EXPECT_EQ(response->get_header_value("Access-Control-Allow-Origin"),
              "http://127.0.0.1:5500");
    EXPECT_EQ(response->get_header_value("Access-Control-Allow-Methods"),
              "GET, POST, OPTIONS");
    EXPECT_EQ(response->get_header_value("Access-Control-Allow-Headers"),
              "Content-Type, X-CodeAgent-Token");
    server.stop();
}

TEST(GuiHttpServerTest, RejectsForeignOriginAndUnexpectedHost) {
    GuiHttpServer server(0);
    ASSERT_TRUE(server.start());
    auto client = apiClient(server);
    httplib::Headers foreign{{"Origin", "http://evil.example"}};
    const auto originResponse = client.Get("/api/health", foreign);
    ASSERT_TRUE(originResponse);
    EXPECT_EQ(originResponse->status, 403);
    EXPECT_TRUE(originResponse->get_header_value("Access-Control-Allow-Origin").empty());
    EXPECT_EQ(nlohmann::json::parse(originResponse->body).at("error").at("code"),
              "ORIGIN_NOT_ALLOWED");

    httplib::Headers foreignPreflight{
        {"Origin", "http://evil.example"},
        {"Access-Control-Request-Method", "POST"},
        {"Access-Control-Request-Headers", "content-type"}
    };
    const auto preflightResponse = client.Options("/api/save", foreignPreflight);
    ASSERT_TRUE(preflightResponse);
    EXPECT_EQ(preflightResponse->status, 403);
    EXPECT_TRUE(preflightResponse->get_header_value("Access-Control-Allow-Origin").empty());

    httplib::Headers unexpectedHost{{"Host", "127.0.0.1:1"}};
    const auto hostResponse = client.Get("/api/health", unexpectedHost);
    ASSERT_TRUE(hostResponse);
    EXPECT_EQ(hostResponse->status, 400);
    EXPECT_EQ(nlohmann::json::parse(hostResponse->body).at("error").at("code"),
              "INVALID_HOST");

    httplib::Headers localHost{{"Host", "localhost:" + std::to_string(server.port())}};
    const auto localHostResponse = client.Get("/api/health", localHost);
    ASSERT_TRUE(localHostResponse);
    EXPECT_EQ(localHostResponse->status, 200);
    server.stop();
}

TEST(GuiHttpServerTest, AllowsItsOwnLoopbackOriginForTheBundledFrontend) {
    GuiHttpServer server(0);
    ASSERT_TRUE(server.start());
    auto client = apiClient(server);
    const std::string port = std::to_string(server.port());
    for (const auto& origin : {"http://127.0.0.1:" + port, "http://localhost:" + port}) {
        httplib::Headers headers{{"Origin", origin}};
        const auto response = client.Post("/api/diff", headers,
            R"({"originalCode":"a\n","proposedCode":"b\n"})", "application/json");
        ASSERT_TRUE(response);
        EXPECT_EQ(response->status, 200);
        EXPECT_EQ(response->get_header_value("Access-Control-Allow-Origin"), origin);
    }
    const std::string otherPort = std::to_string(server.port() == 65535 ? 1 : server.port() + 1);
    for (const auto& origin : {"http://127.0.0.1:" + otherPort, "https://127.0.0.1:" + port,
                               "http://0.0.0.0:" + port}) {
        httplib::Headers headers{{"Origin", origin}};
        const auto response = client.Get("/api/health", headers);
        ASSERT_TRUE(response);
        EXPECT_EQ(response->status, 403);
    }
    server.stop();
}

TEST(GuiHttpServerTest, ServesFrontendFilesWithSecurityHeadersAlongsideTheApi) {
    const auto directory = std::filesystem::temp_directory_path() /
        ("codeagent-frontend-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory / "js");
    std::ofstream(directory / "index.html") << "<!doctype html><title>CodeAgent</title>";
    std::ofstream(directory / "js/app.js") << "console.log('ok');";
    std::ofstream(directory.parent_path() / (directory.filename().string() + "-secret.txt"))
        << "secret";

    GuiHttpServer missing(0);
    EXPECT_FALSE(missing.serveFrontend(directory / "does-not-exist"));

    GuiHttpServer server(0);
    ASSERT_TRUE(server.serveFrontend(directory));
    ASSERT_TRUE(server.start());
    EXPECT_FALSE(server.serveFrontend(directory));
    auto client = apiClient(server);

    const auto index = client.Get("/");
    ASSERT_TRUE(index);
    EXPECT_EQ(index->status, 200);
    EXPECT_NE(index->body.find("CodeAgent"), std::string::npos);
    EXPECT_NE(index->get_header_value("Content-Type").find("text/html"), std::string::npos);
    EXPECT_EQ(index->get_header_value("X-Content-Type-Options"), "nosniff");
    EXPECT_NE(index->get_header_value("Content-Security-Policy").find("default-src 'self'"),
              std::string::npos);

    const auto script = client.Get("/js/app.js");
    ASSERT_TRUE(script);
    EXPECT_EQ(script->status, 200);
    EXPECT_EQ(script->get_header_value("Content-Type"), "text/javascript");

    const auto traversal = client.Get(
        "/../" + directory.filename().string() + "-secret.txt");
    ASSERT_TRUE(traversal);
    EXPECT_NE(traversal->status, 200);
    EXPECT_EQ(traversal->body.find("secret"), std::string::npos);

    const auto health = client.Get("/api/health");
    ASSERT_TRUE(health);
    EXPECT_EQ(health->status, 200);
    EXPECT_EQ(nlohmann::json::parse(health->body).at("status"), "ok");

    const auto missingApi = client.Get("/api/missing");
    ASSERT_TRUE(missingApi);
    EXPECT_EQ(missingApi->status, 404);
    EXPECT_EQ(nlohmann::json::parse(missingApi->body).at("error").at("code"), "NOT_FOUND");

    server.stop();
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::filesystem::remove(
        directory.parent_path() / (directory.filename().string() + "-secret.txt"), ignored);
}

TEST(GuiHttpServerTest, RejectsNullAndMalformedOrigins) {
    GuiHttpServer server(0);
    ASSERT_TRUE(server.start());
    auto client = apiClient(server);
    for (const auto& origin : {std::string("null"), std::string("http://127.0.0.1:5500/path")}) {
        httplib::Headers headers{{"Origin", origin}};
        const auto response = client.Get("/api/health", headers);
        ASSERT_TRUE(response);
        EXPECT_EQ(response->status, 403);
        EXPECT_TRUE(response->get_header_value("Access-Control-Allow-Origin").empty());
    }
    server.stop();
}

TEST(GuiHttpServerTest, RejectsMissingAndDuplicateHostHeaders) {
    GuiHttpServer server(0);
    ASSERT_TRUE(server.start());
    auto client = apiClient(server);

    // An empty explicit Host value is sent as the sole Host header by cpp-httplib.
    const auto missing = client.Get("/api/health", httplib::Headers{{"Host", ""}});
    const httplib::Headers duplicate{
        {"Host", "127.0.0.1:" + std::to_string(server.port())},
        {"Host", "localhost:" + std::to_string(server.port())}
    };
    const auto duplicated = client.Get("/api/health", duplicate);

    ASSERT_TRUE(missing);
    ASSERT_TRUE(duplicated);
    EXPECT_EQ(missing->status, 400);
    EXPECT_EQ(duplicated->status, 400);
    EXPECT_EQ(nlohmann::json::parse(missing->body).at("error").at("code"), "INVALID_HOST");
    EXPECT_EQ(nlohmann::json::parse(duplicated->body).at("error").at("code"), "INVALID_HOST");
    server.stop();
}

TEST(GuiHttpServerTest, RejectsDuplicateOriginAndMalformedPreflights) {
    GuiHttpServer server(0);
    ASSERT_TRUE(server.start());
    auto client = apiClient(server);
    const auto expectedOrigin = ownOrigin(server);
    httplib::Headers duplicateOrigin{{"Origin", expectedOrigin}, {"Origin", expectedOrigin}};
    const auto duplicate = client.Get("/api/health", duplicateOrigin);
    ASSERT_TRUE(duplicate);
    EXPECT_EQ(duplicate->status, 403);
    EXPECT_TRUE(duplicate->get_header_value("Access-Control-Allow-Origin").empty());

    const auto unsupportedMethod = client.Options("/api/refactor", httplib::Headers{
        {"Origin", expectedOrigin}, {"Access-Control-Request-Method", "DELETE"}
    });
    const auto duplicateMethod = client.Options("/api/refactor", httplib::Headers{
        {"Origin", expectedOrigin},
        {"Access-Control-Request-Method", "POST"},
        {"Access-Control-Request-Method", "GET"}
    });
    const auto malformedMethod = client.Options("/api/refactor", httplib::Headers{
        {"Origin", expectedOrigin}, {"Access-Control-Request-Method", "POST, GET"}
    });
    const auto unsupportedHeaders = client.Options("/api/refactor", httplib::Headers{
        {"Origin", expectedOrigin},
        {"Access-Control-Request-Method", "POST"},
        {"Access-Control-Request-Headers", "content-type, authorization"}
    });
    const auto duplicateTokenHeader = client.Options("/api/refactor", httplib::Headers{
        {"Origin", expectedOrigin},
        {"Access-Control-Request-Method", "POST"},
        {"Access-Control-Request-Headers", "x-codeagent-token, x-codeagent-token"}
    });
    for (const auto* response : {&unsupportedMethod, &duplicateMethod, &malformedMethod, &unsupportedHeaders,
                                 &duplicateTokenHeader}) {
        ASSERT_TRUE(*response);
        EXPECT_EQ((*response)->status, 403);
        EXPECT_TRUE((*response)->get_header_value("Access-Control-Allow-Origin").empty());
        EXPECT_EQ(nlohmann::json::parse((*response)->body).at("error").at("code"), "INVALID_PREFLIGHT");
    }
    server.stop();
}

TEST(GuiHttpServerTest, ConfigReturnsOnlySafeDirectories) {
    const std::filesystem::path launchDirectory =
        std::filesystem::current_path() / "gui-launch-directory";
    GuiHttpServer server(0, launchDirectory);
    ASSERT_TRUE(server.start());

    auto client = apiClient(server);
    const auto response = client.Get("/api/config");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_NE(response->get_header_value("Content-Type").find("application/json"),
              std::string::npos);
    const auto body = nlohmann::json::parse(response->body);
    ASSERT_TRUE(body.is_object());
    EXPECT_EQ(body.at("launchDirectory"), launchDirectory.string());
    EXPECT_EQ(body.at("activeOutputDirectory"), launchDirectory.string());
    EXPECT_EQ(body.size(), 2u);
    EXPECT_FALSE(body.contains("supportedExtensions"));
    EXPECT_FALSE(body.contains("apiKey"));
    EXPECT_FALSE(body.contains("GEMINI_API_KEY"));
    EXPECT_FALSE(body.contains("environment"));

    server.stop();
}

TEST(GuiHttpServerTest, UnknownRouteStillReturnsNotFound) {
    GuiHttpServer server(0);
    ASSERT_TRUE(server.start());

    auto client = apiClient(server);
    const auto response = client.Get("/");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 404);

    server.stop();
}

TEST(GuiHttpServerTest, UnknownApiRoutesAndUnsupportedMethodsReturnJsonErrors) {
    GuiHttpServer server(0);
    ASSERT_TRUE(server.start());
    auto client = apiClient(server);
    const auto unknown = client.Get("/api/not-a-route");
    const auto apiRoot = client.Get("/api");
    const auto unsupported = client.Put("/api/health", httplib::Headers{});
    ASSERT_TRUE(unknown);
    ASSERT_TRUE(apiRoot);
    ASSERT_TRUE(unsupported);
    EXPECT_EQ(unknown->status, 404);
    EXPECT_EQ(nlohmann::json::parse(unknown->body).at("error").at("code"), "NOT_FOUND");
    EXPECT_EQ(apiRoot->status, 404);
    EXPECT_EQ(nlohmann::json::parse(apiRoot->body).at("error").at("code"), "NOT_FOUND");
    EXPECT_EQ(unsupported->status, 404);
    EXPECT_EQ(nlohmann::json::parse(unsupported->body).at("error").at("code"), "NOT_FOUND");
    server.stop();
}

TEST(GuiHttpServerTest, GenerationWithoutHandlerReturnsServiceUnavailable) {
    GuiHttpServer server(0);
    ASSERT_TRUE(server.start());

    auto http = apiClient(server);
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
                   std::string& generatedCode,
                   std::string& thoughts) {
                generationCalls_.fetch_add(1);
                if (instruction == "throw") {
                    throw std::runtime_error("provider detail with secret");
                }
                if (!generationSucceeds_.load()) {
                    return false;
                }
                generatedCode = "generated: " + instruction + " (" + language + ")";
                thoughts = "thinking about " + instruction;
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
        return apiClient(*server_);
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
    EXPECT_EQ(body.size(), 3u);
    EXPECT_EQ(body.at("generatedCode"), "generated: write a greeting (C++)");
    EXPECT_EQ(body.at("language"), "C++");
    EXPECT_EQ(body.at("thoughts"), "thinking about write a greeting");
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
                   std::string& refactoredCode,
                   std::string&) {
                handlerCalls_.fetch_add(1);
                if (instruction == "wait") {
                    std::unique_lock<std::mutex> lock(providerWaitMutex_);
                    providerEntered_ = true;
                    providerWaitCondition_.notify_all();
                    providerWaitCondition_.wait(lock, [this] { return releaseProvider_; });
                }
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
        ASSERT_TRUE(server_->setClock([this] {
            return std::chrono::steady_clock::time_point{} + std::chrono::minutes(minutes_.load());
        }));
        ASSERT_TRUE(server_->start());
    }

    void TearDown() override {
        if (server_) {
            server_->stop();
        }
    }

    httplib::Client client() const {
        return apiClient(*server_);
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
    // The server's clock, in minutes; tests advance it instead of sleeping.
    std::atomic<long long> minutes_{0};
    std::mutex providerWaitMutex_;
    std::condition_variable providerWaitCondition_;
    bool providerEntered_ = false;
    bool releaseProvider_ = false;
};

TEST(GuiHttpServerTest, RefactoringWithoutHandlerReturnsServiceUnavailable) {
    GuiHttpServer server(
        0,
        std::filesystem::current_path(),
        GuiHttpServer::GenerationHandler{},
        GuiHttpServer::RefactoringHandler{}
    );
    ASSERT_TRUE(server.start());

    auto http = apiClient(server);
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
    ASSERT_EQ(body.size(), 6u);
    EXPECT_EQ(body.at("thoughts"), "");
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

TEST_F(GuiHttpServerRefactoringTest, PreflightDoesNotInvokeRefactorHandler) {
    auto http = client();
    const auto response = http.Options("/api/refactor", httplib::Headers{
        {"Origin", ownOrigin(*server_)},
        {"Access-Control-Request-Method", "POST"},
        {"Access-Control-Request-Headers", "content-type"}
    });
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 204);
    EXPECT_EQ(handlerCalls_.load(), 0);
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

TEST_F(GuiHttpServerProposalReviewTest, BoundsProposalRetentionWhenNoSafeEvictionExists) {
    for (std::size_t index = 0; index < 128; ++index) {
        ASSERT_FALSE(createProposal().empty()) << "proposal index " << index;
    }
    auto http = client();
    const auto full = http.Post("/api/refactor", requestBody(), "application/json");
    ASSERT_TRUE(full);
    EXPECT_EQ(full->status, 503);
    EXPECT_EQ(nlohmann::json::parse(full->body).at("error").at("code"), "PROPOSAL_CAPACITY");
    EXPECT_EQ(handlerCalls_.load(), 128);
}

TEST_F(GuiHttpServerProposalReviewTest, FailedRefactorPreservesTerminalProposalAtCapacity) {
    auto http = client();
    std::string rejectedId;
    for (std::size_t index = 0; index < 128; ++index) {
        const auto id = createProposal();
        ASSERT_FALSE(id.empty());
        if (index == 0) rejectedId = id;
    }
    const auto rejected = http.Post("/api/changes/" + rejectedId + "/reject");
    ASSERT_TRUE(rejected);
    ASSERT_EQ(rejected->status, 200);

    const auto failed = http.Post("/api/refactor", requestBody("int x;", "throw"), "application/json");
    ASSERT_TRUE(failed);
    EXPECT_EQ(failed->status, 502);
    const auto stillAvailable = http.Post("/api/diff", nlohmann::json{{"proposalId", rejectedId}}.dump(), "application/json");
    ASSERT_TRUE(stillAvailable);
    EXPECT_EQ(stillAvailable->status, 200);
    EXPECT_EQ(createProposal(), "prop-129");
}

TEST_F(GuiHttpServerProposalReviewTest, ConcurrentRefactorsCannotOverbookLastCapacitySlot) {
    std::string terminalId;
    for (std::size_t index = 0; index < 128; ++index) {
        const auto id = createProposal();
        ASSERT_FALSE(id.empty());
        if (index == 0) terminalId = id;
    }
    auto http = client();
    const auto rejected = http.Post("/api/changes/" + terminalId + "/reject");
    ASSERT_TRUE(rejected);
    ASSERT_EQ(rejected->status, 200);

    std::optional<httplib::Result> firstResult;
    std::thread firstRequest([&] {
        auto http = client();
        firstResult = http.Post("/api/refactor", requestBody("int x;", "wait"), "application/json");
    });
    bool providerEntered = false;
    {
        std::unique_lock<std::mutex> lock(providerWaitMutex_);
        providerEntered = providerWaitCondition_.wait_for(lock, std::chrono::seconds(3), [this] {
            return providerEntered_;
        });
    }
    if (!providerEntered) {
        {
            std::lock_guard<std::mutex> lock(providerWaitMutex_);
            releaseProvider_ = true;
        }
        providerWaitCondition_.notify_all();
        firstRequest.join();
        FAIL() << "refactor handler did not enter within the timeout";
    }

    const auto denied = http.Post("/api/refactor", requestBody(), "application/json");
    ASSERT_TRUE(denied);
    EXPECT_EQ(denied->status, 503);
    EXPECT_EQ(handlerCalls_.load(), 129);

    {
        std::lock_guard<std::mutex> lock(providerWaitMutex_);
        releaseProvider_ = true;
    }
    providerWaitCondition_.notify_all();
    firstRequest.join();
    ASSERT_TRUE(firstResult.has_value());
    ASSERT_TRUE(*firstResult);
    EXPECT_EQ((*firstResult)->status, 200);
    EXPECT_EQ(nlohmann::json::parse((*firstResult)->body).at("proposalId"), "prop-129");
    const auto evicted = http.Post("/api/diff", nlohmann::json{{"proposalId", terminalId}}.dump(), "application/json");
    ASSERT_TRUE(evicted);
    EXPECT_EQ(evicted->status, 404);
}

TEST_F(GuiHttpServerProposalReviewTest, EvictsOldestRejectedProposalAndKeepsPendingWork) {
    auto http = client();
    std::string rejectedId;
    std::string lastPendingId;
    for (std::size_t index = 0; index < 128; ++index) {
        const std::string id = createProposal();
        ASSERT_FALSE(id.empty()) << "proposal index " << index;
        if (index == 0) rejectedId = id;
        lastPendingId = id;
    }
    ASSERT_EQ(lastPendingId, "prop-128");
    const auto full = http.Post("/api/refactor", requestBody(), "application/json");
    ASSERT_TRUE(full);
    ASSERT_EQ(full->status, 503);
    const auto rejection = http.Post("/api/changes/" + rejectedId + "/reject");
    ASSERT_TRUE(rejection);
    ASSERT_EQ(rejection->status, 200);

    const std::string nextId = createProposal();
    EXPECT_EQ(nextId, "prop-129");
    const auto evictedDiff = http.Post("/api/diff", nlohmann::json{{"proposalId", rejectedId}}.dump(), "application/json");
    const auto pendingDiff = http.Post("/api/diff", nlohmann::json{{"proposalId", lastPendingId}}.dump(), "application/json");
    ASSERT_TRUE(evictedDiff);
    ASSERT_TRUE(pendingDiff);
    EXPECT_EQ(evictedDiff->status, 404);
    EXPECT_EQ(pendingDiff->status, 200);
}

TEST_F(GuiHttpServerProposalReviewTest, ApprovedProposalsAreNeverEvictedAtCapacity) {
    auto http = client();
    std::string firstId;
    for (std::size_t index = 0; index < 128; ++index) {
        const std::string id = createProposal();
        ASSERT_FALSE(id.empty()) << "proposal index " << index;
        if (index == 0) firstId = id;
        const auto decision = http.Post("/api/changes/" + id + "/approve");
        ASSERT_TRUE(decision);
        ASSERT_EQ(decision->status, 200);
    }
    const auto full = http.Post("/api/refactor", requestBody(), "application/json");
    const auto retained = http.Post("/api/diff", nlohmann::json{{"proposalId", firstId}}.dump(), "application/json");
    ASSERT_TRUE(full);
    ASSERT_TRUE(retained);
    EXPECT_EQ(full->status, 503);
    EXPECT_EQ(retained->status, 200);
}

TEST_F(GuiHttpServerProposalReviewTest, MarkAppliedOnlyMovesApprovedProposalsOnce) {
    auto http = client();
    const auto code = [](const httplib::Result& response) {
        return nlohmann::json::parse(response->body).at("error").at("code").get<std::string>();
    };

    // A pending proposal cannot be marked applied, and stays decidable.
    const std::string id = createProposal();
    const auto early = http.Post("/api/changes/" + id + "/mark-applied");
    ASSERT_TRUE(early);
    EXPECT_EQ(early->status, 409);
    EXPECT_EQ(code(early), "INVALID_PROPOSAL_STATE");
    ASSERT_EQ(http.Post("/api/changes/" + id + "/approve")->status, 200);

    const auto withBody = http.Post("/api/changes/" + id + "/mark-applied", "{}", "application/json");
    ASSERT_TRUE(withBody);
    EXPECT_EQ(withBody->status, 400);

    const auto applied = http.Post("/api/changes/" + id + "/mark-applied");
    ASSERT_TRUE(applied);
    ASSERT_EQ(applied->status, 200);
    EXPECT_EQ(nlohmann::json::parse(applied->body),
              nlohmann::json({{"proposalId", id}, {"status", "APPLIED"}}));

    for (const std::string action : {"mark-applied", "approve", "reject"}) {
        const auto repeated = http.Post("/api/changes/" + id + "/" + action);
        ASSERT_TRUE(repeated);
        EXPECT_EQ(repeated->status, 409) << action;
        EXPECT_EQ(code(repeated), "INVALID_PROPOSAL_STATE") << action;
    }

    // Marking applied writes nothing, and an applied proposal cannot be
    // saved through the proposal save path afterwards.
    const std::string fileName = "mark-applied-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()) + ".cpp";
    const auto save = http.Post("/api/save",
        nlohmann::json{{"proposalId", id}, {"fileName", fileName}}.dump(), "application/json");
    ASSERT_TRUE(save);
    EXPECT_EQ(save->status, 409);
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::current_path() / fileName));
    const auto diff = http.Post("/api/diff", nlohmann::json{{"proposalId", id}}.dump(), "application/json");
    ASSERT_TRUE(diff);
    EXPECT_EQ(diff->status, 200);

    const std::string rejectedId = createProposal();
    ASSERT_EQ(http.Post("/api/changes/" + rejectedId + "/reject")->status, 200);
    const auto rejected = http.Post("/api/changes/" + rejectedId + "/mark-applied");
    ASSERT_TRUE(rejected);
    EXPECT_EQ(rejected->status, 409);

    const auto unknown = http.Post("/api/changes/prop-999/mark-applied");
    ASSERT_TRUE(unknown);
    EXPECT_EQ(unknown->status, 404);

    const std::string foreignId = createProposal();
    ASSERT_EQ(http.Post("/api/changes/" + foreignId + "/approve")->status, 200);
    const auto foreign = http.Post("/api/changes/" + foreignId + "/mark-applied",
        httplib::Headers{{"Origin", "http://evil.example"}}, "", "");
    ASSERT_TRUE(foreign);
    EXPECT_EQ(foreign->status, 403);
    ASSERT_EQ(http.Post("/api/changes/" + foreignId + "/mark-applied")->status, 200)
        << "the refused request did not change the state";
}

TEST_F(GuiHttpServerProposalReviewTest, CapacityRecoversAfterApprovedProposalsAreMarkedApplied) {
    auto http = client();
    std::vector<std::string> ids;
    for (std::size_t index = 0; index < 128; ++index) {
        ids.push_back(createProposal());
        ASSERT_FALSE(ids.back().empty()) << "proposal index " << index;
        ASSERT_EQ(http.Post("/api/changes/" + ids.back() + "/approve")->status, 200);
    }
    const auto full = http.Post("/api/refactor", requestBody(), "application/json");
    ASSERT_TRUE(full);
    ASSERT_EQ(full->status, 503);
    EXPECT_EQ(handlerCalls_.load(), 128);

    // Marking the two oldest proposals applied frees exactly two slots.
    ASSERT_EQ(http.Post("/api/changes/" + ids[0] + "/mark-applied")->status, 200);
    ASSERT_EQ(http.Post("/api/changes/" + ids[1] + "/mark-applied")->status, 200);
    EXPECT_EQ(createProposal(), "prop-129");
    EXPECT_EQ(createProposal(), "prop-130");
    const auto stillFull = http.Post("/api/refactor", requestBody(), "application/json");
    ASSERT_TRUE(stillFull);
    EXPECT_EQ(stillFull->status, 503);

    const auto diffStatus = [&http](const std::string& proposalId) {
        return http.Post("/api/diff", nlohmann::json{{"proposalId", proposalId}}.dump(),
                         "application/json")->status;
    };
    EXPECT_EQ(diffStatus(ids[0]), 404) << "applied proposals are evicted first";
    EXPECT_EQ(diffStatus(ids[1]), 404);
    EXPECT_EQ(diffStatus(ids[2]), 200) << "approved proposals are kept";
    EXPECT_EQ(diffStatus("prop-129"), 200) << "the new pending proposal is kept";
}

TEST_F(GuiHttpServerProposalReviewTest, OriginChecksCoverEmptyBodyApprovalAndRejection) {
    httplib::Headers foreign{{"Origin", "http://evil.example"}};
    httplib::Headers allowed{{"Origin", ownOrigin(*server_)}};
    auto http = client();

    const std::string approvalId = createProposal();
    ASSERT_FALSE(approvalId.empty());
    const auto deniedApproval = http.Post("/api/changes/" + approvalId + "/approve", foreign);
    ASSERT_TRUE(deniedApproval);
    EXPECT_EQ(deniedApproval->status, 403);
    EXPECT_TRUE(deniedApproval->get_header_value("Access-Control-Allow-Origin").empty());
    const auto approved = http.Post("/api/changes/" + approvalId + "/approve", allowed);
    ASSERT_TRUE(approved);
    EXPECT_EQ(approved->status, 200);

    const std::string rejectionId = createProposal();
    ASSERT_FALSE(rejectionId.empty());
    const auto deniedRejection = http.Post("/api/changes/" + rejectionId + "/reject", foreign);
    ASSERT_TRUE(deniedRejection);
    EXPECT_EQ(deniedRejection->status, 403);
    EXPECT_TRUE(deniedRejection->get_header_value("Access-Control-Allow-Origin").empty());
    const auto rejected = http.Post("/api/changes/" + rejectionId + "/reject", allowed);
    ASSERT_TRUE(rejected);
    EXPECT_EQ(rejected->status, 200);
}

class GuiHttpServerSaveTest : public ::testing::Test {
protected:
    void SetUp() override {
        directory_ = std::filesystem::temp_directory_path() / ("codeagent-save-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ASSERT_TRUE(std::filesystem::create_directory(directory_));
        server_ = std::make_unique<GuiHttpServer>(0, directory_, GuiHttpServer::GenerationHandler{},
            [](const std::string&, const std::string&, const std::string& source, std::string& result, std::string&) { result = "saved: " + source; return true; });
        ASSERT_TRUE(server_->start());
    }
    void TearDown() override { if (server_) server_->stop(); std::error_code e; std::filesystem::remove_all(directory_, e); }
    httplib::Client client() const { return apiClient(*server_); }
    std::string approvedProposal() {
        auto http = client();
        auto created = http.Post("/api/refactor", R"({"sourceCode":"base","instruction":"edit","language":"C++","editorRevision":"r1"})", "application/json");
        if (!created || created->status != 200) return {};
        auto id = nlohmann::json::parse(created->body).at("proposalId").get<std::string>();
        auto approved = http.Post("/api/changes/" + id + "/approve");
        return approved && approved->status == 200 ? id : std::string{};
    }
    std::filesystem::path directory_;
    std::unique_ptr<GuiHttpServer> server_;
};

TEST_F(GuiHttpServerSaveTest, RejectsForeignOriginForSaveButAllowsOriginlessLocalClient) {
    auto http = client();
    httplib::Headers foreign{{"Origin", "http://evil.example"}};
    const std::string body = R"({"fileName":"origin-check.cpp","content":"safe","overwriteConfirmed":false})";
    const auto denied = http.Post("/api/save", foreign, body, "application/json");
    ASSERT_TRUE(denied);
    EXPECT_EQ(denied->status, 403);
    EXPECT_FALSE(std::filesystem::exists(directory_ / "origin-check.cpp"));

    const auto allowed = http.Post("/api/save", body, "application/json");
    ASSERT_TRUE(allowed);
    EXPECT_EQ(allowed->status, 200);
    EXPECT_TRUE(std::filesystem::exists(directory_ / "origin-check.cpp"));
}

TEST_F(GuiHttpServerSaveTest, RejectsForeignOriginBeforeChangingOutputDirectory) {
    auto http = client();
    const auto alternateDirectory = directory_ / "alternate";
    ASSERT_TRUE(std::filesystem::create_directory(alternateDirectory));
    httplib::Headers foreign{{"Origin", "http://evil.example"}};
    const auto denied = http.Post("/api/output/select-directory", foreign,
                                  nlohmann::json{{"directoryPath", alternateDirectory.string()}}.dump(),
                                  "application/json");
    ASSERT_TRUE(denied);
    EXPECT_EQ(denied->status, 403);
    const auto config = http.Get("/api/config");
    ASSERT_TRUE(config);
    EXPECT_EQ(nlohmann::json::parse(config->body).at("activeOutputDirectory"), directory_.string());
}

TEST_F(GuiHttpServerSaveTest, SavesGeneratedCodeExplicitlyAndRequiresOverwriteConfirmation) {
    auto http = client();
    auto generated = http.Post("/api/generate", R"({"instruction":"x","language":"C++"})", "application/json");
    ASSERT_TRUE(generated); EXPECT_EQ(generated->status, 503);
    EXPECT_FALSE(std::filesystem::exists(directory_ / "main.cpp"));
    auto first = http.Post("/api/save", R"({"fileName":"main.cpp","content":"one","overwriteConfirmed":false})", "application/json");
    ASSERT_TRUE(first); EXPECT_EQ(first->status, 200);
    auto denied = http.Post("/api/save", R"({"fileName":"main.cpp","content":"two","overwriteConfirmed":false})", "application/json");
    ASSERT_TRUE(denied); EXPECT_EQ(denied->status, 409);
    std::ifstream oldFile(directory_ / "main.cpp"); std::string old; oldFile >> old; EXPECT_EQ(old, "one");
    auto replaced = http.Post("/api/save", R"({"fileName":"main.cpp","content":"two","overwriteConfirmed":true})", "application/json");
    ASSERT_TRUE(replaced); EXPECT_EQ(replaced->status, 200);
}

TEST_F(GuiHttpServerSaveTest, SavesTextOfAnyFormatUnderTheExactNameGiven) {
    auto http = client();
    for (const std::string name : {"notes.txt", "page.html", "settings.toml", ".env.example", "README"}) {
        const std::string content = "line one\r\n\tü ✓\n";
        auto saved = http.Post("/api/save", nlohmann::json{{"fileName", name}, {"content", content},
            {"overwriteConfirmed", false}}.dump(), "application/json");
        ASSERT_TRUE(saved) << name;
        ASSERT_EQ(saved->status, 200) << name << ": " << saved->body;
        std::ifstream file(directory_ / name, std::ios::binary);
        EXPECT_EQ(std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()), content) << name;
        EXPECT_FALSE(std::filesystem::exists(directory_ / (name + ".cpp"))) << name;
    }
    auto id = approvedProposal(); ASSERT_FALSE(id.empty());
    auto proposal = http.Post("/api/save", nlohmann::json{{"proposalId",id},{"fileName","proposal.yaml"}}.dump(), "application/json");
    ASSERT_TRUE(proposal); EXPECT_EQ(proposal->status, 200);
    EXPECT_TRUE(std::filesystem::exists(directory_ / "proposal.yaml"));
}

TEST_F(GuiHttpServerSaveTest, SavesOnlyApprovedProposalAndPreventsDuplicateApplication) {
    auto http = client();
    auto pending = http.Post("/api/refactor", R"({"sourceCode":"base","instruction":"edit","language":"C++","editorRevision":"r"})", "application/json");
    ASSERT_TRUE(pending);
    auto pendingId = nlohmann::json::parse(pending->body).at("proposalId").get<std::string>();
    auto denied = http.Post("/api/save", nlohmann::json{{"proposalId",pendingId},{"fileName","pending.cpp"}}.dump(), "application/json");
    ASSERT_TRUE(denied); EXPECT_EQ(denied->status, 409);
    auto unknown = http.Post("/api/save", R"({"proposalId":"missing","fileName":"none.cpp"})", "application/json");
    ASSERT_TRUE(unknown); EXPECT_EQ(unknown->status, 404);
    auto id = approvedProposal(); ASSERT_FALSE(id.empty());
    auto saved = http.Post("/api/save", nlohmann::json{{"proposalId",id},{"fileName","proposal.cpp"}}.dump(), "application/json");
    ASSERT_TRUE(saved); EXPECT_EQ(saved->status, 200);
    auto repeated = http.Post("/api/save", nlohmann::json{{"proposalId",id},{"fileName","duplicate.cpp"}}.dump(), "application/json");
    ASSERT_TRUE(repeated); EXPECT_EQ(repeated->status, 409);
    EXPECT_FALSE(std::filesystem::exists(directory_ / "duplicate.cpp"));
}

TEST_F(GuiHttpServerSaveTest, RejectsMalformedRequestsUnsupportedMediaUnsafeNamesAndOversizedBodies) {
    auto http = client();
    for (const std::string body : {"{bad", "{}", R"({"fileName":"../x.cpp","content":"x","overwriteConfirmed":false})", R"({"fileName":"/tmp/x.cpp","content":"x","overwriteConfirmed":false})", R"({"fileName":".env","content":"x","overwriteConfirmed":false})", R"({"fileName":".env.local","content":"x","overwriteConfirmed":false})", R"({"fileName":"x.cpp","content":"x","overwriteConfirmed":false,"extra":true})"}) {
        auto response = http.Post("/api/save", body, "application/json"); ASSERT_TRUE(response); EXPECT_EQ(response->status, 400);
    }
    auto media = http.Post("/api/save", "{}", "text/plain"); ASSERT_TRUE(media); EXPECT_EQ(media->status, 415);
    auto large = http.Post("/api/save", std::string(1024 * 1024 + 32, 'x'), "application/json"); ASSERT_TRUE(large); EXPECT_EQ(large->status, 413);
}

TEST_F(GuiHttpServerSaveTest, FailedProposalSaveRemainsRetryableAndConcurrentSaveAppliesOnce) {
    auto id = approvedProposal(); ASSERT_FALSE(id.empty());
    std::filesystem::create_directory(directory_ / "retry.cpp");
    auto http = client();
    auto failed = http.Post("/api/save", nlohmann::json{{"proposalId",id},{"fileName","retry.cpp"}}.dump(), "application/json");
    ASSERT_TRUE(failed); EXPECT_EQ(failed->status, 422);
    std::filesystem::remove(directory_ / "retry.cpp");
    std::atomic<int> ready{0}; std::atomic<bool> go{false}; int results[2] = {};
    auto save = [&](int index) { auto c = apiClient(*server_); ready++; while (!go) std::this_thread::yield(); auto r = c.Post("/api/save", nlohmann::json{{"proposalId",id},{"fileName","retry.cpp"}}.dump(), "application/json"); results[index] = r ? r->status : -1; };
    std::thread a(save,0), b(save,1); while (ready != 2) std::this_thread::yield(); go = true; a.join(); b.join();
    EXPECT_TRUE((results[0] == 200 && results[1] == 409) || (results[0] == 409 && results[1] == 200));
}

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
        auto http = apiClient(*server_);
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
        auto http = apiClient(*server_);
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

    auto http = apiClient(server);
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
        auto http = apiClient(server);
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
    // Any extension is a valid source file now, so a missing file is used: it
    // is neither a regular file nor a directory.
    const auto invalidPath = repositoryRoot() / "docs/missing-selection.md";
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
    auto http = apiClient(server);

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
    auto http = apiClient(server);

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
            auto client = apiClient(server);
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

TEST(NativeDialogPathTest, TranslatesWindowsPathsToWslPaths) {
    EXPECT_EQ(translateWindowsPathForWsl(R"(D:\LLM-CLI-Copilot-Integration\src)"),
              "/mnt/d/LLM-CLI-Copilot-Integration/src");
    EXPECT_EQ(translateWindowsPathForWsl("c:/Users/me/a.cpp"), "/mnt/c/Users/me/a.cpp");
    EXPECT_EQ(translateWindowsPathForWsl(R"(E:\)"), "/mnt/e/");
    EXPECT_EQ(translateWindowsPathForWsl(R"(\\wsl.localhost\Ubuntu\home\me\x.cpp)"),
              "/home/me/x.cpp");
    EXPECT_EQ(translateWindowsPathForWsl(R"(\\wsl$\Ubuntu\tmp)"), "/tmp");
    EXPECT_EQ(translateWindowsPathForWsl("/home/me/x.cpp"), "/home/me/x.cpp");
    EXPECT_EQ(translateWindowsPathForWsl("relative/x.cpp"), "relative/x.cpp");
    EXPECT_EQ(translateWindowsPathForWsl("C:relative"), "C:relative");
    EXPECT_EQ(translateWindowsPathForWsl(R"(\\fileserver\share\x.cpp)"),
              R"(\\fileserver\share\x.cpp)");
}

class GuiHttpServerDocumentTest : public ::testing::Test {
protected:
    void SetUp() override {
        directory_ = std::filesystem::temp_directory_path() / ("codeagent-doc-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(directory_);
        source_ = directory_ / "insertion_sort.cpp";
        write(source_, "int original;\n");
        server_ = std::make_unique<GuiHttpServer>(0, directory_);
        ASSERT_TRUE(server_->start());
        http_ = std::make_unique<httplib::Client>("127.0.0.1", server_->port());
        http_->set_default_headers(authHeaders(*server_));
    }

    void TearDown() override {
        if (server_) server_->stop();
        std::error_code ignored;
        std::filesystem::permissions(directory_, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::add, ignored);
        std::filesystem::remove_all(directory_, ignored);
    }

    static void write(const std::filesystem::path& path, const std::string& content) {
        std::ofstream(path, std::ios::binary) << content;
    }

    static std::string read(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    httplib::Result post(const std::string& path, const nlohmann::json& body) {
        return http_->Post(path, body.dump(), "application/json");
    }

    std::filesystem::path directory_;
    std::filesystem::path source_;
    std::unique_ptr<GuiHttpServer> server_;
    std::unique_ptr<httplib::Client> http_;
};

TEST_F(GuiHttpServerDocumentTest, OpensAndSavesTextFilesOfAnyExtensionExactly) {
    const std::vector<std::pair<std::string, std::string>> files = {
        {"config.yaml", "key: value\r\nname: \"ü ✓ 中文\"\r\n"},
        {"query.sql", "SELECT 1;\n\t-- trailing spaces   \n"},
        {"Makefile", "all:\n\techo ok\n"},
        {".env.example", "GEMINI_API_KEY=\n"},
        {"data.custom-ext", "no trailing newline"},
        {"empty.log", ""},
    };
    for (const auto& [name, original] : files) {
        const auto path = directory_ / name;
        write(path, original);
        const auto opened = post("/api/files/read", {{"path", path.string()}});
        ASSERT_TRUE(opened) << name;
        ASSERT_EQ(opened->status, 200) << name << ": " << opened->body;
        const auto document = nlohmann::json::parse(opened->body);
        EXPECT_EQ(document.at("content"), original) << name;
        EXPECT_EQ(document.at("path"), std::filesystem::canonical(path).string()) << name;

        const std::string edited = original + "\r\nedited ✓\n";
        const auto saved = post("/api/files/write", {
            {"path", document.at("path")}, {"content", edited},
            {"baseRevision", document.at("revision")}, {"overwriteConfirmed", false}});
        ASSERT_TRUE(saved) << name;
        ASSERT_EQ(saved->status, 200) << name << ": " << saved->body;
        EXPECT_EQ(read(path), edited) << name;

        // External changes are still detected for these files.
        write(path, "changed elsewhere\n");
        const auto conflict = post("/api/files/write", {
            {"path", document.at("path")}, {"content", "lost?\n"},
            {"baseRevision", nlohmann::json::parse(saved->body).at("revision")},
            {"overwriteConfirmed", false}});
        ASSERT_TRUE(conflict) << name;
        EXPECT_EQ(conflict->status, 409) << name;
        EXPECT_EQ(read(path), "changed elsewhere\n") << name;
    }
}

TEST_F(GuiHttpServerDocumentTest, OpensEditsAndSavesBackToTheSameFile) {
    const auto opened = post("/api/files/read", {{"path", source_.string()}});
    ASSERT_TRUE(opened);
    ASSERT_EQ(opened->status, 200);
    const auto document = nlohmann::json::parse(opened->body);
    EXPECT_EQ(document.size(), 3u);
    EXPECT_EQ(document.at("path"), std::filesystem::canonical(source_).string());
    EXPECT_EQ(document.at("content"), "int original;\n");
    const std::string revision = document.at("revision");
    EXPECT_EQ(revision.rfind("fnv1a64:", 0), 0u);

    const auto saved = post("/api/files/write", {
        {"path", document.at("path")}, {"content", "int edited;\n"},
        {"baseRevision", revision}, {"overwriteConfirmed", false}});
    ASSERT_TRUE(saved);
    ASSERT_EQ(saved->status, 200);
    const auto result = nlohmann::json::parse(saved->body);
    EXPECT_EQ(result.at("status"), "SAVED");
    EXPECT_EQ(result.at("savedPath"), document.at("path"));
    EXPECT_NE(result.at("revision"), revision);
    EXPECT_EQ(read(source_), "int edited;\n");
    for (const auto& entry : std::filesystem::directory_iterator(directory_)) {
        EXPECT_EQ(entry.path().filename().string().find(".tmp-"), std::string::npos);
    }

    // A second save based on the returned revision needs no confirmation.
    const auto again = post("/api/files/write", {
        {"path", document.at("path")}, {"content", "int again;\n"},
        {"baseRevision", result.at("revision")}, {"overwriteConfirmed", false}});
    ASSERT_TRUE(again);
    EXPECT_EQ(again->status, 200);
    EXPECT_EQ(read(source_), "int again;\n");
}

TEST_F(GuiHttpServerDocumentTest, ExternalChangesRequireConfirmationBeforeOverwrite) {
    const auto opened = post("/api/files/read", {{"path", source_.string()}});
    ASSERT_TRUE(opened);
    const auto document = nlohmann::json::parse(opened->body);
    write(source_, "int changedInVsCode;\n");

    nlohmann::json body = {
        {"path", document.at("path")}, {"content", "int fromBrowser;\n"},
        {"baseRevision", document.at("revision")}, {"overwriteConfirmed", false}};
    const auto conflict = post("/api/files/write", body);
    ASSERT_TRUE(conflict);
    EXPECT_EQ(conflict->status, 409);
    EXPECT_EQ(nlohmann::json::parse(conflict->body).at("error").at("code"), "FILE_CHANGED_ON_DISK");
    EXPECT_EQ(read(source_), "int changedInVsCode;\n");

    body["overwriteConfirmed"] = true;
    const auto confirmed = post("/api/files/write", body);
    ASSERT_TRUE(confirmed);
    EXPECT_EQ(confirmed->status, 200);
    EXPECT_EQ(read(source_), "int fromBrowser;\n");

    std::filesystem::remove(source_);
    body["overwriteConfirmed"] = false;
    const auto deleted = post("/api/files/write", body);
    ASSERT_TRUE(deleted);
    EXPECT_EQ(deleted->status, 409);
    EXPECT_FALSE(std::filesystem::exists(source_));
}

TEST_F(GuiHttpServerDocumentTest, RefusesToWriteFilesThatWereNotOpened) {
    const auto other = directory_ / "other.cpp";
    write(other, "int keep;\n");
    for (const auto& path : {other.string(), (directory_ / "new.cpp").string(),
                             (directory_ / ".." / source_.filename()).string()}) {
        const auto response = post("/api/files/write", {
            {"path", path}, {"content", "int bad;\n"},
            {"baseRevision", "fnv1a64:0000000000000000"}, {"overwriteConfirmed", true}});
        ASSERT_TRUE(response);
        EXPECT_EQ(response->status, 403);
        EXPECT_EQ(nlohmann::json::parse(response->body).at("error").at("code"), "FILE_NOT_OPENED");
    }
    EXPECT_EQ(read(other), "int keep;\n");
    EXPECT_FALSE(std::filesystem::exists(directory_ / "new.cpp"));

    const auto malformed = http_->Post("/api/files/write",
        R"({"path":"x.cpp","content":"x"})", "application/json");
    ASSERT_TRUE(malformed);
    EXPECT_EQ(malformed->status, 400);
}

TEST_F(GuiHttpServerDocumentTest, RejectsUnsupportedLargeAndNonUtf8Files) {
    write(directory_ / ".env", "GEMINI_API_KEY=secret\n");
    write(directory_ / "big.txt", std::string(512 * 1024 + 1, 'x'));
    write(directory_ / "binary.cpp", std::string("\xff\xfe\x00", 3));
    write(directory_ / "nul.json", std::string("{\"a\":\x00}", 7));
    write(directory_ / "latin1.csv", "caf\xe9,1\n");
    std::filesystem::create_symlink(directory_ / "missing.txt", directory_ / "dangling.md");
    std::filesystem::create_symlink(directory_ / ".env", directory_ / "env-link.txt");

    const std::vector<std::pair<std::string, std::string>> cases = {
        {(directory_ / ".env").string(), "INVALID_SOURCE_FILE"},
        {(directory_ / "env-link.txt").string(), "INVALID_SOURCE_FILE"},
        {(directory_ / "missing.cpp").string(), "INVALID_SOURCE_FILE"},
        {directory_.string(), "INVALID_SOURCE_FILE"},
        {(directory_ / "dangling.md").string(), "INVALID_SOURCE_FILE"},
        {(directory_ / "big.txt").string(), "FILE_TOO_LARGE"},
        {(directory_ / "binary.cpp").string(), "UNSUPPORTED_ENCODING"},
        {(directory_ / "nul.json").string(), "UNSUPPORTED_ENCODING"},
        {(directory_ / "latin1.csv").string(), "UNSUPPORTED_ENCODING"},
    };
    for (const auto& [path, code] : cases) {
        const auto response = post("/api/files/read", {{"path", path}});
        ASSERT_TRUE(response) << path;
        EXPECT_GE(response->status, 400) << path;
        EXPECT_EQ(nlohmann::json::parse(response->body).at("error").at("code"), code) << path;
    }
    const auto extraField = post("/api/files/read", {{"path", source_.string()}, {"x", 1}});
    ASSERT_TRUE(extraField);
    EXPECT_EQ(extraField->status, 400);
}

TEST_F(GuiHttpServerDocumentTest, SavingBackKeepsTheFilePermissions) {
    std::filesystem::permissions(source_, std::filesystem::perms::owner_read |
        std::filesystem::perms::owner_write | std::filesystem::perms::group_read);
    const auto document = nlohmann::json::parse(
        post("/api/files/read", {{"path", source_.string()}})->body);
    const auto saved = post("/api/files/write", {
        {"path", document.at("path")}, {"content", "int edited;\n"},
        {"baseRevision", document.at("revision")}, {"overwriteConfirmed", false}});
    ASSERT_TRUE(saved);
    ASSERT_EQ(saved->status, 200);
    EXPECT_EQ(std::filesystem::status(source_).permissions() & std::filesystem::perms::mask,
              std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                  std::filesystem::perms::group_read);
}

TEST_F(GuiHttpServerDocumentTest, OutputDirectoryMustBeAnExistingWritableDirectory) {
    const auto readOnly = directory_ / "read-only";
    std::filesystem::create_directories(readOnly);
    std::filesystem::permissions(readOnly, std::filesystem::perms::owner_read |
                                               std::filesystem::perms::owner_exec);
    const auto response = post("/api/output/select-directory", {{"directoryPath", readOnly.string()}});
    ASSERT_TRUE(response);
    if (::geteuid() != 0) {
        EXPECT_EQ(response->status, 422);
        EXPECT_EQ(nlohmann::json::parse(response->body).at("error").at("code"),
                  "OUTPUT_DIRECTORY_NOT_WRITABLE");
    }
    const auto file = post("/api/output/select-directory", {{"directoryPath", source_.string()}});
    ASSERT_TRUE(file);
    EXPECT_EQ(file->status, 422);
    EXPECT_EQ(nlohmann::json::parse(file->body).at("error").at("code"), "INVALID_OUTPUT_DIRECTORY");
    const auto config = http_->Get("/api/config");
    ASSERT_TRUE(config);
    EXPECT_EQ(nlohmann::json::parse(config->body).at("activeOutputDirectory"), directory_.string());
}

// --------------------------------------------------------------------------
// Session authentication
// --------------------------------------------------------------------------

namespace {
std::string errorCode(const httplib::Result& response) {
    return nlohmann::json::parse(response->body).at("error").at("code").get<std::string>();
}

std::string cookieFrom(const httplib::Result& response) {
    const std::string header = response->get_header_value("Set-Cookie");
    return header.substr(0, header.find(';'));
}
} // namespace

TEST(GuiHttpServerAuthTest, EveryApiRouteRejectsMissingOrInvalidTokensWithoutSideEffects) {
    const auto directory = std::filesystem::temp_directory_path() / ("codeagent-auth-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    const auto source = directory / "a.cpp";
    std::ofstream(source) << "int a;\n";
    std::atomic<int> providerCalls{0};
    std::atomic<int> dialogCalls{0};
    GuiHttpServer server(
        0, directory,
        [&](const std::string&, const std::string&, std::string& code, std::string&) {
            providerCalls++; code = "x"; return true;
        },
        [&](const std::string&, const std::string&, const std::string&, std::string& code, std::string&) {
            providerCalls++; code = "y"; return true;
        },
        std::make_shared<ScriptedNativeDialogProvider>([&](NativeDialogKind) {
            dialogCalls++; return NativeDialogResult{NativeDialogStatus::Cancelled, {}};
        }));
    ASSERT_TRUE(server.start());
    const std::string token = sessionToken(server);
    ASSERT_EQ(token.size(), 64u);

    // An authenticated proposal, so decisions on a real ID are covered too.
    auto authenticated = apiClient(server);
    const auto created = authenticated.Post("/api/refactor", nlohmann::json{
        {"sourceCode", "int a;"}, {"instruction", "x"}, {"language", "C++"}, {"editorRevision", "r"}
    }.dump(), "application/json");
    ASSERT_TRUE(created);
    ASSERT_EQ(created->status, 200);
    const std::string proposalId = nlohmann::json::parse(created->body).at("proposalId");
    providerCalls = 0;

    const std::vector<std::pair<std::string, nlohmann::json>> posts = {
        {"/api/generate", {{"instruction", "x"}, {"language", "C++"}}},
        {"/api/refactor", {{"sourceCode", "int a;"}, {"instruction", "x"}, {"language", "C++"}, {"editorRevision", "r"}}},
        {"/api/diff", {{"proposalId", proposalId}}},
        {"/api/save", {{"fileName", "new.cpp"}, {"content", "int b;\n"}, {"overwriteConfirmed", true}}},
        {"/api/files/read", {{"path", source.string()}}},
        {"/api/files/write", {{"path", source.string()}, {"content", "int b;\n"}, {"baseRevision", "x"}, {"overwriteConfirmed", true}}},
        {"/api/files/open-dialog", nlohmann::json::object()},
        {"/api/output/select-directory", nlohmann::json::object()},
    };
    const std::vector<httplib::Headers> badHeaders = {
        {},
        {{GuiHttpServer::kSessionTokenHeader, "wrong"}},
        {{GuiHttpServer::kSessionTokenHeader, std::string(64, '0')}},
        {{GuiHttpServer::kSessionTokenHeader, token + "0"}},
        {{GuiHttpServer::kSessionTokenHeader, token.substr(1)}},
        {{GuiHttpServer::kSessionTokenHeader, token}, {GuiHttpServer::kSessionTokenHeader, token}},
        {{"Authorization", "Bearer " + token}},
    };
    httplib::Client raw("127.0.0.1", server.port());
    for (std::size_t index = 0; index < badHeaders.size(); ++index) {
        const auto& headers = badHeaders[index];
        std::vector<httplib::Result> responses;
        responses.push_back(raw.Get("/api/health", headers));
        responses.push_back(raw.Get("/api/config", headers));
        responses.push_back(raw.Post("/api/changes/" + proposalId + "/approve", headers));
        responses.push_back(raw.Post("/api/changes/" + proposalId + "/reject", headers));
        for (const auto& [path, body] : posts) {
            responses.push_back(raw.Post(path, headers, body.dump(), "application/json"));
        }
        for (const auto& response : responses) {
            ASSERT_TRUE(response) << "header set " << index;
            EXPECT_EQ(response->status, 401) << "header set " << index;
            EXPECT_EQ(errorCode(response), "UNAUTHORIZED");
            EXPECT_EQ(response->body.find(token), std::string::npos);
        }
    }
    EXPECT_EQ(providerCalls.load(), 0);
    EXPECT_EQ(dialogCalls.load(), 0);
    EXPECT_FALSE(std::filesystem::exists(directory / "new.cpp"));
    std::ifstream input(source);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(input), {}), "int a;\n");

    // The proposal is still pending: the rejected decisions changed nothing.
    const auto approved = authenticated.Post("/api/changes/" + proposalId + "/approve");
    ASSERT_TRUE(approved);
    EXPECT_EQ(approved->status, 200);
    const auto health = authenticated.Get("/api/health");
    ASSERT_TRUE(health);
    EXPECT_EQ(health->status, 200);
    const auto config = authenticated.Get("/api/config");
    ASSERT_TRUE(config);
    EXPECT_EQ(config->status, 200);
    EXPECT_EQ(config->body.find(token), std::string::npos);
    server.stop();
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

TEST(GuiHttpServerSessionTest, BootstrapCodesAreRandomSingleUseAndNeedThePageOrigin) {
    GuiHttpServer server(0);
    ASSERT_TRUE(server.start());
    httplib::Client raw("127.0.0.1", server.port());
    const std::string code = server.issueBootstrapCode();
    EXPECT_EQ(code.size(), 64u);
    EXPECT_NE(code, server.issueBootstrapCode());
    const auto body = nlohmann::json{{"bootstrap", code}}.dump();

    const auto withoutOrigin = raw.Post("/api/session", body, "application/json");
    ASSERT_TRUE(withoutOrigin);
    EXPECT_EQ(withoutOrigin->status, 403);
    EXPECT_EQ(errorCode(withoutOrigin), "ORIGIN_REQUIRED");
    const auto foreign = raw.Post("/api/session", httplib::Headers{{"Origin", "http://evil.example"}},
                                  body, "application/json");
    ASSERT_TRUE(foreign);
    EXPECT_EQ(foreign->status, 403);
    const auto devOrigin = raw.Post("/api/session", httplib::Headers{{"Origin", "http://127.0.0.1:5500"}},
                                    body, "application/json");
    ASSERT_TRUE(devOrigin);
    EXPECT_EQ(devOrigin->status, 403);
    const httplib::Headers page{{"Origin", ownOrigin(server)}};
    const auto wrong = raw.Post("/api/session", page,
                                nlohmann::json{{"bootstrap", std::string(64, 'a')}}.dump(), "application/json");
    ASSERT_TRUE(wrong);
    EXPECT_EQ(wrong->status, 401);
    EXPECT_EQ(errorCode(wrong), "INVALID_BOOTSTRAP");

    // The rejected attempts did not use up the code.
    const auto exchanged = raw.Post("/api/session", page, body, "application/json");
    ASSERT_TRUE(exchanged);
    ASSERT_EQ(exchanged->status, 200);
    EXPECT_EQ(exchanged->get_header_value("Cache-Control"), "no-store");
    const std::string token = nlohmann::json::parse(exchanged->body).at("token");
    EXPECT_EQ(token.size(), 64u);
    const std::string setCookie = exchanged->get_header_value("Set-Cookie");
    EXPECT_NE(setCookie.find("HttpOnly"), std::string::npos);
    EXPECT_NE(setCookie.find("SameSite=Strict"), std::string::npos);
    EXPECT_NE(setCookie.find("Path=/api/session"), std::string::npos);
    EXPECT_EQ(setCookie.find(token), std::string::npos) << "the cookie is not the API token";

    const auto reused = raw.Post("/api/session", page, body, "application/json");
    ASSERT_TRUE(reused);
    EXPECT_EQ(reused->status, 401);
    EXPECT_EQ(errorCode(reused), "INVALID_BOOTSTRAP");

    const auto health = raw.Get("/api/health", httplib::Headers{{GuiHttpServer::kSessionTokenHeader, token}});
    ASSERT_TRUE(health);
    EXPECT_EQ(health->status, 200);
    EXPECT_EQ(health->body.find(token), std::string::npos);

    GuiHttpServer other(0);
    ASSERT_TRUE(other.start());
    EXPECT_NE(sessionToken(other), token) << "each server has its own token";
    const auto crossServer = raw.Get("/api/health",
        httplib::Headers{{GuiHttpServer::kSessionTokenHeader, sessionToken(other)}});
    ASSERT_TRUE(crossServer);
    EXPECT_EQ(crossServer->status, 401);
    other.stop();
    server.stop();
}

TEST(GuiHttpServerSessionTest, BootstrapCodesExpireAfterFiveMinutesAndAreBounded) {
    std::atomic<long long> seconds{0};
    GuiHttpServer server(0);
    ASSERT_TRUE(server.setClock([&] {
        return std::chrono::steady_clock::time_point{} + std::chrono::seconds(seconds.load());
    }));
    ASSERT_TRUE(server.start());
    EXPECT_FALSE(server.setClock([] { return std::chrono::steady_clock::now(); }));
    httplib::Client raw("127.0.0.1", server.port());
    const httplib::Headers page{{"Origin", ownOrigin(server)}};
    const auto exchange = [&](const std::string& code) {
        return raw.Post("/api/session", page, nlohmann::json{{"bootstrap", code}}.dump(), "application/json");
    };

    const std::string expiring = server.issueBootstrapCode();
    const std::string fresh = server.issueBootstrapCode();
    seconds = 5 * 60;
    const auto expired = exchange(expiring);
    ASSERT_TRUE(expired);
    EXPECT_EQ(expired->status, 401);

    seconds = 4 * 60 + 59;  // Clocks are monotonic in practice; this checks the bound.
    const auto stillValid = exchange(fresh);
    ASSERT_TRUE(stillValid);
    EXPECT_EQ(stillValid->status, 200);

    // Only the 8 newest unused codes are kept.
    std::vector<std::string> codes;
    for (int index = 0; index < 9; ++index) codes.push_back(server.issueBootstrapCode());
    const auto dropped = exchange(codes.front());
    ASSERT_TRUE(dropped);
    EXPECT_EQ(dropped->status, 401);
    const auto newest = exchange(codes.back());
    ASSERT_TRUE(newest);
    EXPECT_EQ(newest->status, 200);
    server.stop();
}

TEST(GuiHttpServerSessionTest, SessionCookieRestoresTheTokenAfterAReload) {
    GuiHttpServer server(0);
    ASSERT_TRUE(server.start());
    httplib::Client raw("127.0.0.1", server.port());
    const std::string origin = ownOrigin(server);
    const auto exchanged = raw.Post("/api/session", httplib::Headers{{"Origin", origin}},
        nlohmann::json{{"bootstrap", server.issueBootstrapCode()}}.dump(), "application/json");
    ASSERT_TRUE(exchanged);
    ASSERT_EQ(exchanged->status, 200);
    const std::string token = nlohmann::json::parse(exchanged->body).at("token");
    const std::string cookie = cookieFrom(exchanged);
    EXPECT_EQ(cookie.rfind("codeagent_session_" + std::to_string(server.port()) + "=", 0), 0u);

    const auto refresh = [&](const httplib::Headers& headers) {
        return raw.Post("/api/session", headers, "{}", "application/json");
    };
    const auto reloaded = refresh({{"Origin", origin}, {"Cookie", "other=1; " + cookie}});
    ASSERT_TRUE(reloaded);
    ASSERT_EQ(reloaded->status, 200);
    EXPECT_EQ(nlohmann::json::parse(reloaded->body).at("token"), token);

    const std::string cookieValue = cookie.substr(cookie.find('=') + 1);
    for (const auto& headers : std::vector<httplib::Headers>{
             {{"Origin", origin}},
             {{"Origin", origin}, {"Cookie", cookie + "0"}},
             {{"Origin", origin}, {"Cookie", "codeagent_session_1=" + cookieValue}},
             {{"Origin", origin}, {"Cookie", GuiHttpServer::kSessionTokenHeader + std::string("=") + token}},
             {{"Origin", "http://evil.example"}, {"Cookie", cookie}},
             {{"Cookie", cookie}},
         }) {
        const auto response = refresh(headers);
        ASSERT_TRUE(response);
        EXPECT_GE(response->status, 401);
        EXPECT_EQ(response->body.find(token), std::string::npos);
    }
    // The cookie alone never authenticates an API request.
    const auto cookieOnly = raw.Get("/api/health", httplib::Headers{{"Cookie", cookie}});
    ASSERT_TRUE(cookieOnly);
    EXPECT_EQ(cookieOnly->status, 401);
    server.stop();
}

TEST(GuiHttpServerSessionTest, ConfiguredDevelopmentOriginCanStartASession) {
    GuiHttpServer server(0);
    ASSERT_TRUE(server.allowDevelopmentOrigin("http://localhost:5500"));
    ASSERT_TRUE(server.start());
    httplib::Client raw("127.0.0.1", server.port());
    const auto exchanged = raw.Post("/api/session", httplib::Headers{{"Origin", "http://localhost:5500"}},
        nlohmann::json{{"bootstrap", server.issueBootstrapCode()}}.dump(), "application/json");
    ASSERT_TRUE(exchanged);
    EXPECT_EQ(exchanged->status, 200);
    EXPECT_EQ(exchanged->get_header_value("Access-Control-Allow-Origin"), "http://localhost:5500");
    EXPECT_EQ(exchanged->get_header_value("Access-Control-Allow-Credentials"), "true");
    server.stop();
}

// --------------------------------------------------------------------------
// Proposal expiry
// --------------------------------------------------------------------------

namespace {
int diffStatus(httplib::Client& http, const std::string& proposalId) {
    const auto response = http.Post("/api/diff", nlohmann::json{{"proposalId", proposalId}}.dump(),
                                    "application/json");
    return response ? response->status : -1;
}
} // namespace

TEST_F(GuiHttpServerProposalReviewTest, IdleProposalsExpireOnlyWhenTheirSlotIsNeeded) {
    for (std::size_t index = 0; index < 128; ++index) {
        ASSERT_FALSE(createProposal().empty()) << index;
    }
    auto http = client();
    minutes_ = 29;
    const auto tooEarly = http.Post("/api/refactor", requestBody(), "application/json");
    ASSERT_TRUE(tooEarly);
    EXPECT_EQ(tooEarly->status, 503);
    EXPECT_EQ(nlohmann::json::parse(tooEarly->body).at("error").at("code"), "PROPOSAL_CAPACITY");

    // Using prop-1 keeps it alive.
    EXPECT_EQ(diffStatus(http, "prop-1"), 200);
    minutes_ = 30;
    EXPECT_EQ(createProposal(), "prop-129");
    EXPECT_EQ(diffStatus(http, "prop-1"), 200);
    EXPECT_EQ(diffStatus(http, "prop-2"), 404) << "the longest-idle proposal was evicted";
    EXPECT_EQ(diffStatus(http, "prop-3"), 200) << "only one slot was freed";
    // Until evicted, an idle proposal is still fully usable.
    const auto approved = http.Post("/api/changes/prop-3/approve");
    ASSERT_TRUE(approved);
    EXPECT_EQ(approved->status, 200);
}

TEST_F(GuiHttpServerProposalReviewTest, FinishedProposalsAreEvictedBeforeIdleOnes) {
    for (std::size_t index = 0; index < 128; ++index) {
        ASSERT_FALSE(createProposal().empty()) << index;
    }
    auto http = client();
    const auto rejected = http.Post("/api/changes/prop-100/reject");
    ASSERT_TRUE(rejected);
    ASSERT_EQ(rejected->status, 200);
    minutes_ = 45;
    EXPECT_EQ(createProposal(), "prop-129");
    EXPECT_EQ(diffStatus(http, "prop-100"), 404);
    EXPECT_EQ(diffStatus(http, "prop-1"), 200);
}

TEST_F(GuiHttpServerProposalReviewTest, ReservedIdleSlotIsKeptIfUsedWhileTheProviderRuns) {
    for (std::size_t index = 0; index < 128; ++index) {
        ASSERT_FALSE(createProposal().empty()) << index;
    }
    minutes_ = 31;
    httplib::Result pending;
    std::thread request([&] {
        auto http = client();
        pending = http.Post("/api/refactor", requestBody("int y;", "wait"), "application/json");
    });
    {
        std::unique_lock<std::mutex> lock(providerWaitMutex_);
        providerWaitCondition_.wait(lock, [this] { return providerEntered_; });
    }
    // prop-1 was reserved for eviction; approving it marks it in use again.
    auto http = client();
    const auto approved = http.Post("/api/changes/prop-1/approve");
    ASSERT_TRUE(approved);
    EXPECT_EQ(approved->status, 200);
    {
        std::lock_guard<std::mutex> lock(providerWaitMutex_);
        releaseProvider_ = true;
    }
    providerWaitCondition_.notify_all();
    request.join();
    ASSERT_TRUE(pending);
    EXPECT_EQ(pending->status, 200);
    EXPECT_EQ(diffStatus(http, "prop-1"), 200) << "an in-use proposal is never evicted";
    EXPECT_EQ(diffStatus(http, "prop-2"), 404) << "another idle proposal was evicted instead";
}

TEST_F(GuiHttpServerProposalReviewTest, CapacityErrorWhenEveryReservedSlotBecameActive) {
    for (std::size_t index = 0; index < 128; ++index) {
        ASSERT_FALSE(createProposal().empty()) << index;
    }
    minutes_ = 31;
    httplib::Result pending;
    std::thread request([&] {
        auto http = client();
        pending = http.Post("/api/refactor", requestBody("int y;", "wait"), "application/json");
    });
    {
        std::unique_lock<std::mutex> lock(providerWaitMutex_);
        providerWaitCondition_.wait(lock, [this] { return providerEntered_; });
    }
    auto http = client();
    for (int index = 1; index <= 128; ++index) {
        EXPECT_EQ(diffStatus(http, "prop-" + std::to_string(index)), 200);
    }
    {
        std::lock_guard<std::mutex> lock(providerWaitMutex_);
        releaseProvider_ = true;
    }
    providerWaitCondition_.notify_all();
    request.join();
    ASSERT_TRUE(pending);
    EXPECT_EQ(pending->status, 503);
    EXPECT_EQ(nlohmann::json::parse(pending->body).at("error").at("code"), "PROPOSAL_CAPACITY");
    for (int index = 1; index <= 128; ++index) {
        EXPECT_EQ(diffStatus(http, "prop-" + std::to_string(index)), 200);
    }
}

// --------------------------------------------------------------------------
// Opened files and file safety
// --------------------------------------------------------------------------

TEST_F(GuiHttpServerDocumentTest, ForgetsTheLeastRecentlyUsedOpenedFileFirst) {
    std::map<std::string, std::string> revisions;
    const auto open = [&](const std::string& name) {
        write(directory_ / name, "int " + name.substr(0, 1) + ";\n");
        const auto response = post("/api/files/read", {{"path", (directory_ / name).string()}});
        ASSERT_TRUE(response);
        ASSERT_EQ(response->status, 200) << name;
        revisions[name] = nlohmann::json::parse(response->body).at("revision");
    };
    const auto save = [&](const std::string& name) {
        const auto response = post("/api/files/write", {
            {"path", (directory_ / name).string()}, {"content", "int saved;\n"},
            {"baseRevision", revisions[name]}, {"overwriteConfirmed", true}});
        return response ? response->status : -1;
    };
    // Opened first, but alphabetically last.
    open("z.cpp");
    open("y.cpp");
    for (int index = 0; index < 254; ++index) {
        char name[16];
        std::snprintf(name, sizeof name, "a%03d.cpp", index);
        open(name);
    }
    // 256 files are remembered. Saving z.cpp makes it recently used.
    ASSERT_EQ(save("z.cpp"), 200);
    open("m.cpp");
    EXPECT_EQ(save("y.cpp"), 403) << "the least recently used file is forgotten";
    EXPECT_EQ(save("z.cpp"), 200);
    EXPECT_EQ(save("a000.cpp"), 200) << "not the alphabetically first file";
    EXPECT_EQ(save("m.cpp"), 200);
}

TEST_F(GuiHttpServerDocumentTest, SavingBackDropsSetuidAndSetgidBits) {
    namespace fs = std::filesystem;
    const auto requested = fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                           fs::perms::set_uid | fs::perms::set_gid;
    fs::permissions(source_, requested);
    if ((fs::status(source_).permissions() & (fs::perms::set_uid | fs::perms::set_gid)) == fs::perms::none) {
        GTEST_SKIP() << "this filesystem does not keep setuid/setgid bits";
    }
    const auto document = nlohmann::json::parse(post("/api/files/read", {{"path", source_.string()}})->body);
    const auto saved = post("/api/files/write", {
        {"path", document.at("path")}, {"content", "int edited;\n"},
        {"baseRevision", document.at("revision")}, {"overwriteConfirmed", false}});
    ASSERT_TRUE(saved);
    ASSERT_EQ(saved->status, 200);
    EXPECT_EQ(fs::status(source_).permissions() & fs::perms::mask,
              fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec);
    EXPECT_EQ(read(source_), "int edited;\n");
}

TEST_F(GuiHttpServerDocumentTest, ReadRefusesFifosWithoutBlocking) {
    const auto fifo = directory_ / "pipe.cpp";
    ASSERT_EQ(::mkfifo(fifo.c_str(), 0600), 0);
    const auto response = post("/api/files/read", {{"path", fifo.string()}});
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 422);
    EXPECT_EQ(nlohmann::json::parse(response->body).at("error").at("code"), "INVALID_SOURCE_FILE");
}

// --------------------------------------------------------------------------
// Typed errors at the HTTP boundary
// --------------------------------------------------------------------------

namespace {
struct ThrownError {
    std::function<void()> raise;
    int status;
    const char* code;
    const char* message;
    const char* diagnostic;
};

const std::vector<ThrownError>& providerFailures() {
    static const std::vector<ThrownError> failures = {
        {[] { throw ConfigurationError::missingSetting("GEMINI_API_KEY"); }, 503,
         "CONFIGURATION_ERROR",
         "Configuration error: GEMINI_API_KEY is not configured. Set it in your environment or .env file.",
         "GEMINI_API_KEY is unset or empty"},
        {[] { throw AuthenticationError("Gemini HTTP 400; provider error reason API_KEY_INVALID"); },
         502, "PROVIDER_AUTH_FAILED",
         "Authentication failed: Gemini rejected the configured credentials. Verify the API key.",
         "API_KEY_INVALID"},
        {[] { throw RateLimitError(false, std::nullopt, "Gemini HTTP 429; provider error"); }, 429,
         "PROVIDER_RATE_LIMITED", "Request rate limited. Wait briefly before retrying.", "HTTP 429"},
        {[] { throw NetworkError("libcurl error 7: Couldn't connect to server"); }, 502,
         "PROVIDER_UNREACHABLE", "Network error: Unable to connect to Gemini. Check your connection.",
         "libcurl"},
        {[] { throw TimeoutError("libcurl error 28: Timeout was reached"); }, 504,
         "PROVIDER_TIMEOUT", "Request timed out. Retry when the service is reachable.", "libcurl"},
        {[] { throw ProviderResponseError(ProviderResponseError::Kind::Invalid, "malformed JSON"); },
         502, "PROVIDER_RESPONSE_INVALID",
         "Gemini returned a response CodeAgent could not use. Retry the request.", "malformed"},
    };
    return failures;
}
} // namespace

TEST(GuiHttpServerErrorTest, GenerationErrorsReachTheBrowserAsStatusCodeAndSafeMessage) {
    for (const auto& failure : providerFailures()) {
        SCOPED_TRACE(failure.code);
        GuiHttpServer server(0, std::filesystem::current_path(),
            [&failure](const std::string&, const std::string&, std::string&, std::string&) -> bool {
                failure.raise();
                return true;
            });
        ASSERT_TRUE(server.start());
        auto http = apiClient(server);
        const auto response = http.Post("/api/generate",
            R"({"instruction":"Write main","language":"C++"})", "application/json");
        ASSERT_TRUE(response);
        EXPECT_EQ(response->status, failure.status);
        const auto error = nlohmann::json::parse(response->body).at("error");
        EXPECT_EQ(error.at("code"), failure.code);
        EXPECT_EQ(error.at("message"), failure.message);
        // Diagnostics stay in the terminal log.
        EXPECT_EQ(response->body.find(failure.diagnostic), std::string::npos);
        server.stop();
    }
}

TEST(GuiHttpServerErrorTest, RefactoringErrorsAreReportedAndCreateNoProposal) {
    std::atomic<bool> fail{true};
    GuiHttpServer server(0, std::filesystem::current_path(), {},
        [&fail](const std::string&, const std::string&, const std::string&,
                std::string& refactored, std::string&) {
            if (fail.load()) throw TimeoutError("libcurl error 28");
            refactored = "int refactored;\n";
            return true;
        });
    ASSERT_TRUE(server.start());
    auto http = apiClient(server);
    const std::string body =
        R"({"sourceCode":"int x;\n","instruction":"Rename","language":"C++","editorRevision":"r1"})";

    const auto failed = http.Post("/api/refactor", body, "application/json");
    ASSERT_TRUE(failed);
    EXPECT_EQ(failed->status, 504);
    EXPECT_EQ(nlohmann::json::parse(failed->body).at("error").at("code"), "PROVIDER_TIMEOUT");

    // The failure consumed no proposal ID: a retry gets the first one.
    fail = false;
    const auto retried = http.Post("/api/refactor", body, "application/json");
    ASSERT_TRUE(retried);
    ASSERT_EQ(retried->status, 200);
    EXPECT_EQ(nlohmann::json::parse(retried->body).at("proposalId"), "prop-1");
    server.stop();
}

TEST_F(GuiHttpServerDocumentTest, SaveConflictMessageTellsTheUserWhatToDo) {
    const auto opened = post("/api/files/read", {{"path", source_.string()}});
    ASSERT_TRUE(opened);
    const auto document = nlohmann::json::parse(opened->body);
    write(source_, "int changedElsewhere;\n");
    const auto conflict = post("/api/files/write", {
        {"path", document.at("path")}, {"content", "int mine;\n"},
        {"baseRevision", document.at("revision")}, {"overwriteConfirmed", false}});
    ASSERT_TRUE(conflict);
    EXPECT_EQ(conflict->status, 409);
    EXPECT_EQ(nlohmann::json::parse(conflict->body).at("error").at("message"),
              "The file changed externally. Reload it or save to another path.");
}

TEST_F(GuiHttpServerDocumentTest, PermissionErrorsAreReportedAsPermissionDenied) {
    if (::geteuid() == 0) GTEST_SKIP() << "root ignores file permissions";
    const auto opened = post("/api/files/read", {{"path", source_.string()}});
    ASSERT_TRUE(opened);
    const auto document = nlohmann::json::parse(opened->body);

    // Without write permission on the folder, the temporary file for the
    // atomic save cannot be created.
    std::filesystem::permissions(directory_, std::filesystem::perms::owner_read |
                                             std::filesystem::perms::owner_exec);
    const auto saved = post("/api/files/write", {
        {"path", document.at("path")}, {"content", "int edited;\n"},
        {"baseRevision", document.at("revision")}, {"overwriteConfirmed", false}});
    std::filesystem::permissions(directory_, std::filesystem::perms::owner_all);
    ASSERT_TRUE(saved);
    EXPECT_EQ(saved->status, 403);
    const auto error = nlohmann::json::parse(saved->body).at("error");
    EXPECT_EQ(error.at("code"), "PERMISSION_DENIED");
    EXPECT_EQ(error.at("message").get<std::string>().rfind(
        "Permission denied: CodeAgent cannot access this file.", 0), 0u);
    EXPECT_EQ(read(source_), "int original;\n");

    std::filesystem::permissions(source_, std::filesystem::perms::none);
    const auto unreadable = post("/api/files/read", {{"path", source_.string()}});
    std::filesystem::permissions(source_, std::filesystem::perms::owner_read |
                                          std::filesystem::perms::owner_write);
    ASSERT_TRUE(unreadable);
    EXPECT_EQ(unreadable->status, 403);
    EXPECT_EQ(nlohmann::json::parse(unreadable->body).at("error").at("code"), "PERMISSION_DENIED");
}
