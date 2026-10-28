#include "EnvLoader.hpp"
#include "CodeAgentError.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

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

} // namespace
