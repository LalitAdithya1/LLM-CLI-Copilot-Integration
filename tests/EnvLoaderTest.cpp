#include "EnvLoader.hpp"
#include "CodeAgentError.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

class EnvLoaderTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = std::filesystem::temp_directory_path() / ("codeagent-env-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        for (const char* key : kKeys) ::unsetenv(key);
    }

    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        for (const char* key : kKeys) ::unsetenv(key);
    }

    void write(const std::string& content) {
        std::ofstream(path_) << content;
    }

    static std::string value(const char* key) {
        const char* found = std::getenv(key);
        return found == nullptr ? "<unset>" : found;
    }

    static constexpr const char* kKeys[] = {
        "CODEAGENT_TEST_PLAIN", "CODEAGENT_TEST_DOUBLE", "CODEAGENT_TEST_SINGLE",
        "CODEAGENT_TEST_MISMATCHED", "CODEAGENT_TEST_INNER", "CODEAGENT_TEST_EXISTING",
        "CODEAGENT_TEST_EMPTY_QUOTES"
    };
    std::filesystem::path path_;
};

TEST_F(EnvLoaderTest, LoadsValuesAndStripsOneMatchingPairOfQuotes) {
    write("# comment\n"
          "CODEAGENT_TEST_PLAIN = plain value \n"
          "CODEAGENT_TEST_DOUBLE=\"double quoted\"\n"
          "CODEAGENT_TEST_SINGLE='single quoted'\n"
          "CODEAGENT_TEST_MISMATCHED=\"open only\n"
          "CODEAGENT_TEST_INNER=a\"b\"c\n"
          "CODEAGENT_TEST_EMPTY_QUOTES=\"\"\n"
          "not a pair\n");
    ASSERT_TRUE(loadEnvFile(path_.string()));
    EXPECT_EQ(value("CODEAGENT_TEST_PLAIN"), "plain value");
    EXPECT_EQ(value("CODEAGENT_TEST_DOUBLE"), "double quoted");
    EXPECT_EQ(value("CODEAGENT_TEST_SINGLE"), "single quoted");
    EXPECT_EQ(value("CODEAGENT_TEST_MISMATCHED"), "\"open only");
    EXPECT_EQ(value("CODEAGENT_TEST_INNER"), "a\"b\"c");
    EXPECT_EQ(value("CODEAGENT_TEST_EMPTY_QUOTES"), "");
}

TEST_F(EnvLoaderTest, KeepsVariablesAlreadyInTheEnvironment) {
    ::setenv("CODEAGENT_TEST_EXISTING", "from shell", 1);
    write("CODEAGENT_TEST_EXISTING=\"from file\"\n");
    ASSERT_TRUE(loadEnvFile(path_.string()));
    EXPECT_EQ(value("CODEAGENT_TEST_EXISTING"), "from shell");
}

TEST_F(EnvLoaderTest, ReportsAMissingFile) {
    EXPECT_FALSE(loadEnvFile((path_ / "missing").string()));
}

TEST_F(EnvLoaderTest, RequiredVariablesAreReturnedOrReportedAsConfigurationErrors) {
    ::setenv("CODEAGENT_TEST_PLAIN", "configured", 1);
    EXPECT_EQ(requireEnvironmentVariable("CODEAGENT_TEST_PLAIN"), "configured");

    for (const char* missing : {"", " 	"}) {
        ::setenv("CODEAGENT_TEST_PLAIN", missing, 1);
        try {
            requireEnvironmentVariable("CODEAGENT_TEST_PLAIN");
            ADD_FAILURE() << "expected ConfigurationError";
        } catch (const ConfigurationError& error) {
            EXPECT_STREQ(error.what(),
                "Configuration error: CODEAGENT_TEST_PLAIN is not configured. "
                "Set it in your environment or .env file.");
        }
    }
    ::unsetenv("CODEAGENT_TEST_PLAIN");
    EXPECT_THROW(requireEnvironmentVariable("CODEAGENT_TEST_PLAIN"), ConfigurationError);
}

// --------------------------------------------------------------------------
// Configuration file lookup
// --------------------------------------------------------------------------

// Sets or unsets an environment variable and restores it on destruction.
class ScopedVariable {
public:
    ScopedVariable(const char* name, const char* value) : name_(name) {
        if (const char* previous = std::getenv(name)) previous_ = previous;
        if (value != nullptr) ::setenv(name, value, 1);
        else ::unsetenv(name);
    }
    ~ScopedVariable() {
        if (previous_) ::setenv(name_.c_str(), previous_->c_str(), 1);
        else ::unsetenv(name_.c_str());
    }
    ScopedVariable(const ScopedVariable&) = delete;
    ScopedVariable& operator=(const ScopedVariable&) = delete;

private:
    std::string name_;
    std::optional<std::string> previous_;
};

TEST_F(EnvLoaderTest, CandidatesFollowTheDocumentedOrder) {
    const auto workingDotEnv = std::filesystem::current_path() / ".env";
    {
        ScopedVariable explicitFile("CODEAGENT_ENV_FILE", "/srv/codeagent/explicit.env");
        ScopedVariable configHome("XDG_CONFIG_HOME", "/srv/config");
        const std::vector<std::filesystem::path> expected = {
            "/srv/codeagent/explicit.env", workingDotEnv, "/srv/config/codeagent/.env"};
        EXPECT_EQ(configurationFileCandidates(), expected);
    }
    {
        // A relative XDG_CONFIG_HOME is invalid, so ~/.config is used.
        ScopedVariable explicitFile("CODEAGENT_ENV_FILE", nullptr);
        ScopedVariable configHome("XDG_CONFIG_HOME", "relative/config");
        ScopedVariable home("HOME", "/home/student");
        const std::vector<std::filesystem::path> expected = {
            workingDotEnv, "/home/student/.config/codeagent/.env"};
        EXPECT_EQ(configurationFileCandidates(), expected);
    }
}

TEST_F(EnvLoaderTest, EarlierFilesAndTheProcessEnvironmentWin) {
    const auto second = path_.string() + "-second";
    write("CODEAGENT_TEST_PLAIN=first\nCODEAGENT_TEST_DOUBLE=first\n");
    std::ofstream(second) << "CODEAGENT_TEST_PLAIN=second\nCODEAGENT_TEST_SINGLE=second\n";
    ::setenv("CODEAGENT_TEST_DOUBLE", "from shell", 1);
    std::ostringstream warnings;

    const auto loaded = loadConfigurationFiles(
        {path_, path_ / "not-there.env", second}, warnings);

    const std::vector<std::filesystem::path> expected = {path_, second};
    EXPECT_EQ(loaded, expected);
    EXPECT_EQ(value("CODEAGENT_TEST_PLAIN"), "first");
    EXPECT_EQ(value("CODEAGENT_TEST_SINGLE"), "second");
    EXPECT_EQ(value("CODEAGENT_TEST_DOUBLE"), "from shell");
    EXPECT_TRUE(warnings.str().empty()) << "missing optional files are normal";
    std::filesystem::remove(second);
}

TEST_F(EnvLoaderTest, AnUnreadableExplicitFileIsReported) {
    const auto missing = (path_ / "missing.env").string();
    ScopedVariable explicitFile("CODEAGENT_ENV_FILE", missing.c_str());
    std::ostringstream warnings;

    EXPECT_TRUE(loadConfigurationFiles({missing}, warnings).empty());
    EXPECT_NE(warnings.str().find("CODEAGENT_ENV_FILE is set to"), std::string::npos);
}

TEST_F(EnvLoaderTest, AMissingSettingNamesTheCheckedLocationsOnlyInTheDiagnostic) {
    ScopedVariable configHome("XDG_CONFIG_HOME", "/srv/config");
    try {
        requireEnvironmentVariable("CODEAGENT_TEST_PLAIN");
        FAIL() << "expected ConfigurationError";
    } catch (const ConfigurationError& error) {
        EXPECT_EQ(std::string(error.what()).find("/srv/config"), std::string::npos);
        EXPECT_NE(error.diagnostic().find("/srv/config/codeagent/.env"), std::string::npos);
    }
}

} // namespace
