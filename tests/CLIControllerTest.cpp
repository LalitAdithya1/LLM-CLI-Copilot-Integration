#include "CLIController.hpp"
#include "CodeAgentError.hpp"
#include "GuiHttpServer.hpp"
#include "LLMClient.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
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

TEST_F(CLIControllerTest, RefactorsTextFilesOfAnyExtensionInTheirOwnFormat) {
    const std::vector<std::tuple<std::string, std::string, std::string>> files = {
        {"config.yaml", "key: 1\r\nname: \"ü\"\r\n", "yaml"},
        {"script.PY", "def f():\n\treturn 1\n", "py"},
        {"Makefile", "all:\n\techo ok\n", "plain text"},
        {"header.hpp", "int value();\n", "C++"},
    };
    for (const auto& [name, original, language] : files) {
        const std::string proposed = original + "# changed\n";
        std::ofstream(name, std::ios::binary) << original;
        std::istringstream input("y\nyes\ny\ny\n");
        std::ostringstream output;
        std::ostringstream error;
        std::string sentPrompt;

        CLIController controller(
            [&proposed, &sentPrompt](const std::string& prompt, std::string& response, bool) {
                sentPrompt = prompt;
                response = proposed;
                return true;
            },
            input, output, error);
        std::vector<std::string> arguments = {"codeagent", "refactor", name, "improve it"};
        std::vector<char*> argv;
        for (std::string& argument : arguments) {
            argv.push_back(argument.data());
        }

        ASSERT_EQ(controller.run(static_cast<int>(argv.size()), argv.data()), 0)
            << name << ": " << error.str();
        EXPECT_NE(sentPrompt.find("Programming language:\n" + language + "\n"), std::string::npos)
            << name;
        EXPECT_NE(sentPrompt.find(original), std::string::npos) << name;
        EXPECT_EQ(readFile(name), proposed) << name;
        EXPECT_FALSE(std::filesystem::exists(name + ".cpp")) << name;
    }
}

TEST_F(CLIControllerTest, BinaryFilesAreNeverSentForRefactoring) {
    const std::string binary("\x7f" "ELF\x02\x01\x00\x00", 8);
    std::ofstream("program.bin", std::ios::binary) << binary;
    std::istringstream input("y\n");
    std::ostringstream output;
    std::ostringstream error;
    bool called = false;
    CLIController controller(
        [&called](const std::string&, std::string&, bool) { called = true; return true; },
        input, output, error);
    std::vector<std::string> arguments = {"codeagent", "refactor", "program.bin", "improve it"};
    std::vector<char*> argv;
    for (std::string& argument : arguments) {
        argv.push_back(argument.data());
    }

    EXPECT_EQ(controller.run(static_cast<int>(argv.size()), argv.data()), 1);
    EXPECT_FALSE(called);
    EXPECT_NE(error.str().find("is binary or not UTF-8 text"), std::string::npos);
    EXPECT_EQ(readFile("program.bin"), binary);
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
    EXPECT_NE(output.str().find("codeagent gui"), std::string::npos);

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

TEST_F(CLIControllerTest, GuiCommandDispatchesToInjectedCommand) {
    std::istringstream input;
    std::ostringstream output;
    std::ostringstream error;
    bool called = false;
    CLIController controller(
        [](const std::string&, std::string&, bool) { return false; },
        input,
        output,
        error,
        [&called](std::ostream& commandOutput, std::ostream& commandError) {
            called = true;
            GuiHttpServer server(0);
            if (!server.start()) {
                commandError << "Could not start GUI server.\n";
                return 1;
            }
            commandOutput << "GUI backend listening at http://127.0.0.1:"
                          << server.port() << '\n';
            server.stop();
            return 0;
        }
    );
    std::vector<std::string> arguments = {"codeagent", "gui"};
    std::vector<char*> argv;
    for (std::string& argument : arguments) {
        argv.push_back(argument.data());
    }

    EXPECT_EQ(controller.run(static_cast<int>(argv.size()), argv.data()), 0);
    EXPECT_TRUE(called);
    EXPECT_NE(output.str().find("GUI backend listening at http://127.0.0.1:"),
              std::string::npos);
    EXPECT_TRUE(error.str().empty());
}

TEST_F(CLIControllerTest, GuiCommandRejectsExtraArguments) {
    std::istringstream input;
    std::ostringstream output;
    std::ostringstream error;
    bool commandCalled = false;
    CLIController controller(
        [](const std::string&, std::string&, bool) { return false; },
        input,
        output,
        error,
        [&commandCalled](std::ostream&, std::ostream&) {
            commandCalled = true;
            return 0;
        }
    );
    std::vector<std::string> arguments = {"codeagent", "gui", "unexpected"};
    std::vector<char*> argv;
    for (std::string& argument : arguments) {
        argv.push_back(argument.data());
    }

    EXPECT_EQ(controller.run(static_cast<int>(argv.size()), argv.data()), 1);
    EXPECT_FALSE(commandCalled);
    EXPECT_NE(error.str().find("does not accept arguments"), std::string::npos);
}

// --------------------------------------------------------------------------
// Application-level error boundary
// --------------------------------------------------------------------------

int runWithArguments(CLIController& controller, std::vector<std::string> arguments) {
    std::vector<char*> argv;
    for (std::string& argument : arguments) {
        argv.push_back(argument.data());
    }
    return controller.run(static_cast<int>(argv.size()), argv.data());
}

TEST_F(CLIControllerTest, ProviderExceptionsAreReportedWithTheirActionableMessage) {
    const std::string original = "int value() { return 1; }\n";
    std::ofstream("source.cpp", std::ios::binary) << original;
    std::istringstream input("y\n");
    std::ostringstream output;
    std::ostringstream error;
    CLIController controller(
        [](const std::string&, std::string&, bool) -> bool {
            throw RateLimitError(false, std::nullopt, "Gemini HTTP 429; provider error");
        },
        input, output, error);

    EXPECT_EQ(runWithArguments(controller, {"codeagent", "refactor", "source.cpp", "improve it"}), 1);
    EXPECT_NE(error.str().find("Request rate limited. Wait briefly before retrying."),
              std::string::npos);
    EXPECT_NE(error.str().find("Details: Gemini HTTP 429"), std::string::npos);
    EXPECT_EQ(output.str().find("Apply these changes?"), std::string::npos);
    EXPECT_EQ(readFile("source.cpp"), original);
}

TEST_F(CLIControllerTest, MissingApiKeyIsReportedAsAConfigurationError) {
    const char* existing = std::getenv("GEMINI_API_KEY");
    const std::string previous = existing != nullptr ? existing : "";
    unsetenv("GEMINI_API_KEY");
    std::istringstream input;
    std::ostringstream output;
    std::ostringstream error;
    // The default LLM call uses the real client, which fails before any
    // network request because the key is missing.
    CLIController controller(
        [](const std::string& prompt, std::string& response, bool) {
            response = LLMClient([](const std::string&, const std::string&, const std::string&,
                                    std::string&, long&) -> bool {
                ADD_FAILURE() << "no request may be sent without a key";
                return false;
            }).generate(prompt, {}).text;
            return true;
        },
        input, output, error);

    EXPECT_EQ(runWithArguments(controller, {"codeagent", "generate", "write main"}), 1);
    EXPECT_NE(error.str().find(
        "Configuration error: GEMINI_API_KEY is not configured. Set it in your environment or .env file."),
        std::string::npos);
    if (existing != nullptr) setenv("GEMINI_API_KEY", previous.c_str(), 1);
}

TEST_F(CLIControllerTest, UnexpectedExceptionsEndTheCommandInsteadOfTheProgram) {
    std::istringstream input;
    std::ostringstream output;
    std::ostringstream error;
    CLIController controller(
        [](const std::string&, std::string&, bool) -> bool {
            throw std::runtime_error("internal invariant broken");
        },
        input, output, error);

    EXPECT_EQ(runWithArguments(controller, {"codeagent", "generate", "write main"}), 1);
    EXPECT_NE(error.str().find("An unexpected internal error occurred (internal invariant broken)"),
              std::string::npos);
}

TEST_F(CLIControllerTest, InvalidGuiPortIsAConfigurationError) {
    setenv("CODEAGENT_GUI_PORT", "99999", 1);
    std::istringstream input;
    std::ostringstream output;
    std::ostringstream error;
    // No GUI command is injected, so the real one runs and rejects the port
    // before starting a server.
    CLIController controller(
        [](const std::string&, std::string&, bool) { return false; },
        input, output, error);

    EXPECT_EQ(runWithArguments(controller, {"codeagent", "gui"}), 1);
    unsetenv("CODEAGENT_GUI_PORT");
    EXPECT_NE(error.str().find(
        "Configuration error: CODEAGENT_GUI_PORT must be a port number from 1 to 65535."),
        std::string::npos);
    EXPECT_EQ(output.str().find("GUI running"), std::string::npos);
}

// --------------------------------------------------------------------------
// generate: offering to save the generated code
// --------------------------------------------------------------------------

constexpr const char* kGeneratedCode = "int main() { return 0; }\n";

int runGenerate(
    const std::string& inputText,
    std::ostringstream& output,
    std::ostringstream& error,
    bool generationSucceeds = true
) {
    std::istringstream input(inputText);
    CLIController controller(
        [generationSucceeds](const std::string&, std::string& response, bool) {
            response = generationSucceeds ? kGeneratedCode : "";
            return generationSucceeds;
        },
        input, output, error);
    return runWithArguments(controller, {"codeagent", "generate", "write main"});
}

std::size_t filesInCurrentDirectory() {
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(".")) {
        (void)entry;
        ++count;
    }
    return count;
}

TEST_F(CLIControllerTest, DecliningToSaveWritesNothing) {
    std::ostringstream output;
    std::ostringstream error;

    EXPECT_EQ(runGenerate("n\n", output, error), 0);
    EXPECT_NE(output.str().find(
        "Do you want to save the generated code to a file? (y/n): "), std::string::npos);
    EXPECT_NE(output.str().find("The generated code was not saved."), std::string::npos);
    EXPECT_EQ(output.str().find("Enter the output file path:"), std::string::npos);
    EXPECT_EQ(filesInCurrentDirectory(), 0u);
    EXPECT_TRUE(error.str().empty());
}

TEST_F(CLIControllerTest, AcceptingSavesTheExactGeneratedCodeToTheGivenPath) {
    std::ostringstream output;
    std::ostringstream error;

    EXPECT_EQ(runGenerate("y\n  generated.cpp \n", output, error), 0);
    EXPECT_EQ(readFile("generated.cpp"), kGeneratedCode);
    const auto expectedPath =
        std::filesystem::absolute("generated.cpp").lexically_normal().string();
    EXPECT_NE(output.str().find("Generated code saved successfully to: " + expectedPath),
              std::string::npos);
    EXPECT_EQ(output.str().find("wants permission"), std::string::npos);
    EXPECT_TRUE(error.str().empty());
}

TEST_F(CLIControllerTest, EmptyAndInvalidPathsAreExplainedAndAskedAgain) {
    std::ostringstream output;
    std::ostringstream error;

    EXPECT_EQ(runGenerate("y\n\n.env\nmissing/main.cpp\n\"generated.cpp\"\n", output, error), 0);
    EXPECT_NE(error.str().find("Output file path cannot be empty."), std::string::npos);
    EXPECT_NE(error.str().find("environment file that can hold API keys"), std::string::npos);
    EXPECT_NE(error.str().find("does not exist. Create it first or choose another path."),
              std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(".env"));
    EXPECT_FALSE(std::filesystem::exists("missing"));
    EXPECT_EQ(readFile("generated.cpp"), kGeneratedCode);
}

TEST_F(CLIControllerTest, GeneratedTextIsSavedUnderTheExactNameGiven) {
    for (const std::string name : {"notes.txt", "settings.toml", ".env.example", "README"}) {
        std::ostringstream output;
        std::ostringstream error;
        EXPECT_EQ(runGenerate("y\n" + name + "\n", output, error), 0) << name;
        EXPECT_EQ(readFile(name), kGeneratedCode) << name;
        EXPECT_FALSE(std::filesystem::exists(name + ".cpp")) << name;
        EXPECT_TRUE(error.str().empty()) << name << ": " << error.str();
    }
}

TEST_F(CLIControllerTest, AnExistingFileIsReplacedOnlyAfterConfirmation) {
    std::ofstream("existing.cpp", std::ios::binary) << "int old;\n";
    std::ostringstream output;
    std::ostringstream error;

    // Declining keeps the file and lets the user choose another path.
    EXPECT_EQ(runGenerate("y\nexisting.cpp\nn\nother.cpp\n", output, error), 0);
    EXPECT_NE(output.str().find("Overwrite this file? [y/N]"), std::string::npos);
    EXPECT_NE(output.str().find("Enter a different path"), std::string::npos);
    EXPECT_EQ(readFile("existing.cpp"), "int old;\n");
    EXPECT_EQ(readFile("other.cpp"), kGeneratedCode);

    std::ostringstream confirmedOutput;
    std::ostringstream confirmedError;
    EXPECT_EQ(runGenerate("y\nexisting.cpp\ny\n", confirmedOutput, confirmedError), 0);
    EXPECT_EQ(readFile("existing.cpp"), kGeneratedCode);
    EXPECT_NE(confirmedOutput.str().find("Generated code saved successfully to:"),
              std::string::npos);
}

TEST_F(CLIControllerTest, AFailedWriteIsReportedAndNeverClaimedAsSaved) {
    // A directory at the temporary path makes the write itself fail.
    std::filesystem::create_directory("generated.cpp.codeagent.tmp");
    std::ostringstream output;
    std::ostringstream error;

    EXPECT_EQ(runGenerate("y\ngenerated.cpp\n", output, error), 1);
    EXPECT_NE(error.str().find("Error: Could not write"), std::string::npos);
    EXPECT_NE(error.str().find("Error: The generated code was not saved."), std::string::npos);
    EXPECT_EQ(output.str().find("saved successfully"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists("generated.cpp"));
}

TEST_F(CLIControllerTest, FailedGenerationNeverOffersToSave) {
    std::ostringstream output;
    std::ostringstream error;

    EXPECT_EQ(runGenerate("y\ngenerated.cpp\n", output, error, false), 1);
    EXPECT_EQ(output.str().find("Do you want to save"), std::string::npos);
    EXPECT_NE(error.str().find("Code generation failed"), std::string::npos);
    EXPECT_EQ(filesInCurrentDirectory(), 0u);
}

TEST_F(CLIControllerTest, EndOfInputOrAnUnclearAnswerSavesNothing) {
    std::ostringstream output;
    std::ostringstream error;
    EXPECT_EQ(runGenerate("y\n", output, error), 0);
    EXPECT_NE(output.str().find("No path received; the generated code was not saved."),
              std::string::npos);

    std::ostringstream unclearOutput;
    std::ostringstream unclearError;
    EXPECT_EQ(runGenerate("maybe\n", unclearOutput, unclearError), 0);
    EXPECT_NE(unclearOutput.str().find("Please answer y or n."), std::string::npos);
    EXPECT_NE(unclearOutput.str().find("No answer received; the generated code was not saved."),
              std::string::npos);
    EXPECT_EQ(filesInCurrentDirectory(), 0u);
}

TEST_F(CLIControllerTest, FencedGeminiOutputIsSavedWithoutTheFence) {
    ScopedApiKey apiKey;
    const std::string fenced =
        R"({"candidates":[{"finishReason":"STOP","content":{"parts":[{"text":"```cpp\nint main() { return 0; }\n```"}]}}]})";
    std::istringstream input("y\ngenerated.cpp\n");
    std::ostringstream output;
    std::ostringstream error;
    CLIController controller(
        [&fenced](const std::string& prompt, std::string& response, bool) {
            response = LLMClient([&fenced](const std::string&, const std::string&,
                                           const std::string&, std::string& body, long& status) {
                body = fenced;
                status = 200;
                return true;
            }).generate(prompt, {}).text;
            return true;
        },
        input, output, error);

    EXPECT_EQ(runWithArguments(controller, {"codeagent", "generate", "write main"}), 0);
    const std::string saved = readFile("generated.cpp");
    EXPECT_EQ(saved.find("```"), std::string::npos);
    EXPECT_EQ(saved.rfind("int main() { return 0; }", 0), 0u);
}

} // namespace
