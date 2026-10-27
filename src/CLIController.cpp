
#include "CLIController.hpp"

#include "FileManager.hpp"
#include "GuiHttpServer.hpp"
#include "PromptBuilder.hpp"
#include "LLMClient.hpp"
#include "UnifiedDiff.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

namespace {

volatile std::sig_atomic_t guiStopRequested = 0;

void requestGuiStop(int) {
    guiStopRequested = 1;
}

int runGuiCommand(std::ostream& output, std::ostream& error) {
    GuiHttpServer::GenerationHandler generationHandler =
        [](const std::string& instruction,
           const std::string& language,
           std::string& generatedCode) {
            PromptBuilder promptBuilder;
            std::string prompt;
            if (!promptBuilder.buildGenerationPrompt(
                    instruction,
                    language,
                    prompt)) {
                return false;
            }

            LLMClient client;
            return client.generateContent(prompt, generatedCode);
        };
    GuiHttpServer server(
        8080,
        std::filesystem::current_path(),
        std::move(generationHandler)
    );
    if (!server.start()) {
        error << "Error: Could not start the GUI server on 127.0.0.1:"
              << (server.port() == 0 ? 8080 : server.port())
              << ". The port may be unavailable or already in use.\n";
        return 1;
    }

    guiStopRequested = 0;
    const auto previousInterruptHandler =
        std::signal(SIGINT, requestGuiStop);
    if (previousInterruptHandler == SIG_ERR) {
        server.stop();
        error << "Error: Could not install the interrupt handler for the GUI server.\n";
        return 1;
    }

#ifdef SIGTERM
    const auto previousTerminateHandler =
        std::signal(SIGTERM, requestGuiStop);
    if (previousTerminateHandler == SIG_ERR) {
        std::signal(SIGINT, previousInterruptHandler);
        server.stop();
        error << "Error: Could not install the termination handler for the GUI server.\n";
        return 1;
    }
#endif

    output << "GUI backend listening at http://127.0.0.1:"
           << server.port() << "\n"
           << "Health endpoint: http://127.0.0.1:"
           << server.port() << "/api/health\n"
           << "Press Ctrl+C to stop.\n";
    output.flush();

    while (guiStopRequested == 0 && server.isRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    server.stop();
    std::signal(SIGINT, previousInterruptHandler);
#ifdef SIGTERM
    std::signal(SIGTERM, previousTerminateHandler);
#endif
    guiStopRequested = 0;
    output << "GUI backend stopped.\n";
    return 0;
}

void displayHelp(std::ostream& output) {
    output
        << "CodingCLI - AI Coding Assistant\n\n"
        << "Usage:\n"
        << "  codeagent help\n"
        << "  codeagent gui\n"
        << "  codeagent generate \"<instruction>\"\n"
        << "  codeagent refactor <file-path> \"<instruction>\"\n\n"
        << "Commands:\n"
        << "  gui        Start the local GUI backend (Ctrl+C to stop)\n"
        << "  generate   Generate code from an instruction\n"
        << "  refactor   Refactor a source file using AI\n"
        << "  help       Display this help message\n\n"
        << "Options:\n"
        << "  -h, --help Display this help message\n";
}

void displayUsageError(std::ostream& error, const std::string& message) {
    error << "Error: " << message << "\n\n";
    error << "Run 'codeagent help' for usage instructions.\n";
}

bool isApproval(const std::string& response) {
    std::string normalized;
    const auto first = response.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return false;
    }
    const auto last = response.find_last_not_of(" \t\r\n");
    normalized = response.substr(first, last - first + 1);
    std::transform(
        normalized.begin(), normalized.end(), normalized.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); }
    );
    return normalized == "y" || normalized == "yes";
}

bool isRejection(const std::string& response) {
    std::string normalized;
    const auto first = response.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return false;
    }
    const auto last = response.find_last_not_of(" \t\r\n");
    normalized = response.substr(first, last - first + 1);
    std::transform(
        normalized.begin(), normalized.end(), normalized.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); }
    );
    return normalized == "n" || normalized == "no";
}

std::string joinArguments(
    int argc,
    char* argv[],
    int startIndex
) {
    std::string result;

    for (int i = startIndex; i < argc; ++i) {
        if (!result.empty()) {
            result += ' ';
        }

        result += argv[i];
    }

    return result;
}

} // namespace

CLIController::CLIController()
    : CLIController(
          [](const std::string& prompt,
             std::string& response,
             bool requireExplicitCompletion) {
              LLMClient client;
              return requireExplicitCompletion
                  ? client.generateCompleteContent(prompt, response)
                  : client.generateContent(prompt, response);
          },
          std::cin,
          std::cout,
          std::cerr) {
}

CLIController::CLIController(
    LLMCall llmCall,
    std::istream& input,
    std::ostream& output,
    std::ostream& error,
    GuiCommand guiCommand
)
    : llmCall_(std::move(llmCall)),
      input_(input),
      output_(output),
      error_(error),
      guiCommand_(std::move(guiCommand)) {
    if (!guiCommand_) {
        guiCommand_ = runGuiCommand;
    }
}

int CLIController::run(int argc, char* argv[]) {
    if (argc < 2) {
        displayUsageError(error_, "No command provided.");
        return 1;
    }

    const std::string command = argv[1];

    // ------------------------------------------------------------
    // HELP
    // ------------------------------------------------------------

    if (command == "help" ||
        command == "--help" ||
        command == "-h") {
        displayHelp(output_);
        return 0;
    }

    // ------------------------------------------------------------
    // GUI
    // ------------------------------------------------------------

    if (command == "gui") {
        if (argc != 2) {
            displayUsageError(error_, "The gui command does not accept arguments.");
            return 1;
        }
        return guiCommand_(output_, error_);
    }

    // ------------------------------------------------------------
    // GENERATE
    // ------------------------------------------------------------

    if (command == "generate") {
        if (argc < 3) {
            displayUsageError(error_,
                "The generate command requires an instruction."
            );
            return 1;
        }

        const std::string instruction =
            joinArguments(argc, argv, 2);

        const std::string language = "C++";

        PromptBuilder promptBuilder;
        std::string prompt;

        if (!promptBuilder.buildGenerationPrompt(
                instruction,
                language,
                prompt)) {
            error_
                << "Error: Could not build generation prompt.\n";
            return 1;
        }

        std::string generatedCode;

        output_
            << "Sending generation request to Gemini...\n";

        if (!llmCall_(prompt, generatedCode, false)) {
            error_
                << "Error: Code generation failed. Check your "
                << ".env API key, internet connection, and Gemini "
                << "API configuration.\n";
            return 1;
        }

        output_
            << "\nGenerated code:\n\n"
            << generatedCode
            << '\n';

        return 0;
    }

    // ------------------------------------------------------------
    // REFACTOR
    // ------------------------------------------------------------

    if (command == "refactor") {
        if (argc < 4) {
            displayUsageError(error_,
                "The refactor command requires a file path "
                "and an instruction."
            );
            return 1;
        }

        const std::string filePath = argv[2];
        const std::string instruction =
            joinArguments(argc, argv, 3);

        FileManager fileManager(input_, output_, error_);
        std::string sourceCode;

        if (!fileManager.readFile(filePath, sourceCode)) {
            error_
                << "Refactoring cancelled because the source file "
                << "could not be read.\n";
            return 1;
        }

        PromptBuilder promptBuilder;
        std::string prompt;
        const std::string language = "C++";

        if (!promptBuilder.buildRefactoringPrompt(
                instruction,
                language,
                sourceCode,
                prompt)) {
            error_
                << "Error: Could not build refactoring prompt.\n";
            return 1;
        }

        std::string refactoredCode;

        output_
            << "Sending refactoring request to Gemini...\n";

        if (!llmCall_(prompt, refactoredCode, true)) {
            error_
                << "Error: Refactoring failed. Check your "
                << ".env API key, internet connection, and Gemini "
                << "API configuration.\n";
            return 1;
        }

        output_ << "\nRefactoring diff:\n"
                << makeUnifiedDiff(sourceCode, refactoredCode);

        if (sourceCode == refactoredCode) {
            output_ << "No changes to apply.\n";
            return 0;
        }

        output_ << "\nApply these changes? [y/N]: ";
        std::string approval;
        if (!std::getline(input_, approval)) {
            error_
                << "Refactoring was not approved; source file was not modified.\n";
            return 1;
        }

        if (!isApproval(approval)) {
            if (isRejection(approval)) {
                output_
                    << "Refactoring rejected; source file was not modified.\n";
                return 0;
            }
            error_
                << "Invalid approval input; source file was not modified.\n";
            return 1;
        }

        if (!fileManager.writeFile(filePath, refactoredCode)) {
            error_
                << "Refactoring was approved but could not be saved.\n";
            return 1;
        }

        output_ << "Refactoring applied successfully.\n";

        return 0;
    }

    // ------------------------------------------------------------
    // UNKNOWN COMMAND
    // ------------------------------------------------------------

    displayUsageError(error_, "Unknown command: " + command);
    return 1;
}
