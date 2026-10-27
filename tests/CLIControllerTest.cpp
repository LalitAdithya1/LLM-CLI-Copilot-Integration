#include "CLIController.hpp"
#include "LLMClient.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>()
    );
}

class ScopedApiKey {
public:
    ScopedApiKey() {
        const char* existing = std::getenv("GEMINI_API_KEY");
        if (existing != nullptr) {
            hadPreviousValue_ = true;
            previousValue_ = existing;
        }
        setenv("GEMINI_API_KEY", "offline-test-key", 1);
    }

    ~ScopedApiKey() {
        if (hadPreviousValue_) {
            setenv("GEMINI_API_KEY", previousValue_.c_str(), 1);
        } else {
            unsetenv("GEMINI_API_KEY");
        }
    }

private:
    bool hadPreviousValue_ = false;
    std::string previousValue_;
};

class CLIControllerTest : public ::testing::Test {
protected:
    void SetUp() override {
        originalDirectory_ = std::filesystem::current_path();
        testDirectory_ = std::filesystem::temp_directory_path() /
            ("codeagent_cli_test_" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()
             ) + "_" + std::to_string(++testNumber_));
        std::filesystem::create_directories(testDirectory_);
        std::filesystem::current_path(testDirectory_);
    }

    void TearDown() override {
        std::filesystem::current_path(originalDirectory_);
        std::error_code error;
        std::filesystem::remove_all(testDirectory_, error);
    }

    int runRefactor(
        const std::string& source,
        const std::string& proposal,
        const std::string& inputText,
        std::ostringstream& output,
        std::ostringstream& error,
        bool llmSucceeds = true
    ) {
        std::ofstream("source.cpp", std::ios::binary) << source;
        std::istringstream input(inputText);

        CLIController controller(
            [&proposal, llmSucceeds](
                const std::string& prompt,
                std::string& response,
                bool requireExplicitCompletion
            ) {
                EXPECT_NE(prompt.find("Source code:"), std::string::npos);
                EXPECT_TRUE(requireExplicitCompletion);
                response = proposal;
                return llmSucceeds;
            },
            input,
            output,
            error
        );

        std::vector<std::string> arguments = {
            "codeagent", "refactor", "source.cpp", "improve it"
        };
        std::vector<char*> argv;
        for (std::string& argument : arguments) {
            argv.push_back(argument.data());
        }
        return controller.run(static_cast<int>(argv.size()), argv.data());
    }

    int runRefactorWithGeminiResponse(
        const std::string& original,
        const std::string& geminiResponse,
        std::ostringstream& output,
        std::ostringstream& error
    ) {
        ScopedApiKey apiKey;
        std::ofstream("source.cpp", std::ios::binary) << original;
        std::istringstream input("y\n");
        LLMClient client(
            [&geminiResponse](
                const std::string&,
                const std::string&,
                const std::string&,
                std::string& responseBody,
                long& httpStatus
            ) {
                responseBody = geminiResponse;
                httpStatus = 200;
                return true;
            }
        );

        CLIController controller(
            [&client](
                const std::string& prompt,
                std::string& response,
                bool requireExplicitCompletion
            ) {
                EXPECT_TRUE(requireExplicitCompletion);
                return client.generateCompleteContent(prompt, response);
            },
            input,
            output,
            error
        );
        std::vector<std::string> arguments = {
            "codeagent", "refactor", "source.cpp", "improve it"
        };
        std::vector<char*> argv;
        for (std::string& argument : arguments) {
            argv.push_back(argument.data());
        }
        return controller.run(static_cast<int>(argv.size()), argv.data());
    }

    std::filesystem::path originalDirectory_;
    std::filesystem::path testDirectory_;
    static unsigned int testNumber_;
};

unsigned int CLIControllerTest::testNumber_ = 0;

TEST_F(CLIControllerTest, AppliesOnlyAfterApprovalAndFileManagerSafeguards) {
    const std::string original = "int value() { return 1; }\n";
    const std::string proposed = "int value() { return 2; }\n";
    std::istringstream input("y\nyes\ny\ny\n");
    std::ostringstream output;
    std::ostringstream error;
    bool completionWasRequired = false;
    std::ofstream("source.cpp", std::ios::binary) << original;

    CLIController controller(
        [&proposed, &completionWasRequired](
            const std::string&,
            std::string& response,
            bool requireExplicitCompletion
        ) {
            completionWasRequired = requireExplicitCompletion;
            response = proposed;
            return true;
        },
        input,
        output,
        error
    );
    std::vector<std::string> arguments = {
        "codeagent", "refactor", "source.cpp", "improve it"
    };
    std::vector<char*> argv;
    for (std::string& argument : arguments) {
        argv.push_back(argument.data());
    }

    ASSERT_EQ(controller.run(static_cast<int>(argv.size()), argv.data()), 0);
    EXPECT_TRUE(completionWasRequired);
    EXPECT_EQ(readFile("source.cpp"), proposed);
    EXPECT_NE(output.str().find("-int value() { return 1; }"), std::string::npos);
    EXPECT_NE(output.str().find("+int value() { return 2; }"), std::string::npos);
    EXPECT_NE(output.str().find("Refactoring applied successfully."), std::string::npos);
    EXPECT_TRUE(error.str().empty());
}

TEST_F(CLIControllerTest, RejectionNeverWritesProposal) {
    const std::string original = "int value() { return 1; }\n";
    const std::string proposed = "int value() { return 2; }\n";
    std::ostringstream output;
    std::ostringstream error;

    ASSERT_EQ(runRefactor(original, proposed, "y\nno\n", output, error), 0);
    EXPECT_EQ(readFile("source.cpp"), original);
    EXPECT_NE(output.str().find("Refactoring rejected"), std::string::npos);
}

TEST_F(CLIControllerTest, InvalidApprovalNeverWritesProposal) {
    const std::string original = "int value() { return 1; }\n";
    const std::string proposed = "int value() { return 2; }\n";
    std::ostringstream output;
    std::ostringstream error;

    EXPECT_EQ(runRefactor(original, proposed, "y\nmaybe\n", output, error), 1);
    EXPECT_EQ(readFile("source.cpp"), original);
    EXPECT_NE(error.str().find("Invalid approval input"), std::string::npos);
}

TEST_F(CLIControllerTest, IdenticalProposalDoesNotAskForApprovalOrWrite) {
    const std::string original = "int value() { return 1; }\n";
    std::ostringstream output;
    std::ostringstream error;

    EXPECT_EQ(runRefactor(original, original, "y\n", output, error), 0);
    EXPECT_EQ(readFile("source.cpp"), original);
    EXPECT_NE(output.str().find("No changes to apply."), std::string::npos);
    EXPECT_EQ(output.str().find("Apply these changes?"), std::string::npos);
}

TEST_F(CLIControllerTest, LLMFailureNeverWritesProposal) {
    const std::string original = "int value() { return 1; }\n";
    std::ostringstream output;
    std::ostringstream error;

    EXPECT_EQ(
        runRefactor(original, "partial proposal", "y\n", output, error, false),
        1
    );
    EXPECT_EQ(readFile("source.cpp"), original);
    EXPECT_EQ(output.str().find("Apply these changes?"), std::string::npos);
}

TEST_F(CLIControllerTest, BlockedGeminiResponseNeverReachesApprovalOrWrite) {
    const std::string original = "int value() { return 1; }\n";
    const std::string blockedResponse =
        R"({"candidates":[{"finishReason":"SAFETY","content":{"parts":[{"text":"blocked proposal"}]}}]})";
    std::ostringstream output;
    std::ostringstream error;

    EXPECT_EQ(
        runRefactorWithGeminiResponse(original, blockedResponse, output, error),
        1
    );
    EXPECT_EQ(readFile("source.cpp"), original);
    EXPECT_EQ(output.str().find("Apply these changes?"), std::string::npos);
    EXPECT_NE(error.str().find("Refactoring failed"), std::string::npos);
}

TEST_F(CLIControllerTest, IncompleteGeminiResponseNeverReachesApprovalOrWrite) {
    const std::string original = "int value() { return 1; }\n";
    const std::string incompleteResponse =
        R"({"candidates":[{"finishReason":"MAX_TOKENS","content":{"parts":[{"text":"partial proposal"}]}}]})";
    std::ostringstream output;
    std::ostringstream error;

    EXPECT_EQ(
        runRefactorWithGeminiResponse(original, incompleteResponse, output, error),
        1
    );
    EXPECT_EQ(readFile("source.cpp"), original);
    EXPECT_EQ(output.str().find("Apply these changes?"), std::string::npos);
    EXPECT_NE(error.str().find("Refactoring failed"), std::string::npos);
}

TEST_F(CLIControllerTest, WriteFailurePreservesOriginalFile) {
    const std::string original = "int value() { return 1; }\n";
    const std::string proposed = "int value() { return 2; }\n";
    std::ofstream("source.cpp", std::ios::binary) << original;
    std::filesystem::create_directory("source.cpp.codeagent.tmp");
    std::istringstream input("y\ny\ny\ny\n");
    std::ostringstream output;
    std::ostringstream error;
    CLIController controller(
        [&proposed](const std::string&, std::string& response, bool) {
            response = proposed;
            return true;
        },
        input,
        output,
        error
    );
    std::vector<std::string> arguments = {
        "codeagent", "refactor", "source.cpp", "improve it"
    };
    std::vector<char*> argv;
    for (std::string& argument : arguments) {
        argv.push_back(argument.data());
    }

    EXPECT_EQ(controller.run(static_cast<int>(argv.size()), argv.data()), 1);
    EXPECT_EQ(readFile("source.cpp"), original);
    EXPECT_NE(error.str().find("could not be saved"), std::string::npos);
}

TEST_F(CLIControllerTest, HelpAndGenerateRemainAvailable) {
    std::istringstream input;
    std::ostringstream output;
    std::ostringstream error;
    bool completionRequiredForGenerate = true;
    CLIController controller(
        [&completionRequiredForGenerate](
            const std::string&,
            std::string& response,
            bool requireExplicitCompletion
        ) {
            completionRequiredForGenerate = requireExplicitCompletion;
            response = "generated code";
            return true;
        },
        input,
        output,
        error
    );
    std::vector<std::string> helpArguments = {"codeagent", "help"};
    std::vector<char*> helpArgv;
    for (std::string& argument : helpArguments) {
        helpArgv.push_back(argument.data());
    }
    ASSERT_EQ(controller.run(static_cast<int>(helpArgv.size()), helpArgv.data()), 0);
    EXPECT_NE(output.str().find("Usage:"), std::string::npos);

    output.str("");
    output.clear();
    std::vector<std::string> generateArguments = {
        "codeagent", "generate", "write code"
    };
    std::vector<char*> generateArgv;
    for (std::string& argument : generateArguments) {
        generateArgv.push_back(argument.data());
    }
    ASSERT_EQ(controller.run(
        static_cast<int>(generateArgv.size()), generateArgv.data()), 0);
    EXPECT_FALSE(completionRequiredForGenerate);
    EXPECT_NE(output.str().find("generated code"), std::string::npos);
    EXPECT_TRUE(error.str().empty());
}

} // namespace
