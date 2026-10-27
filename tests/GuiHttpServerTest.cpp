#include "GuiHttpServer.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

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
