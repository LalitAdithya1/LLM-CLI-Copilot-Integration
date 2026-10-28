
#include "CLIController.hpp"

#include "EnvLoader.hpp"
#include "FileManager.hpp"
#include "GuiHttpServer.hpp"
#include "PromptBuilder.hpp"
#include "LLMClient.hpp"
#include "UnifiedDiff.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

volatile std::sig_atomic_t guiStopRequested = 0;
constexpr int kDefaultGuiPort = 8080;

void requestGuiStop(int) {
    guiStopRequested = 1;
}

bool isSetEnvironmentValue(const char* value) {
    if (value == nullptr) {
        return false;
    }
    for (const char* character = value; *character != '\0'; ++character) {
        if (!std::isspace(static_cast<unsigned char>(*character))) {
            return true;
        }
    }
    return false;
}

// CODEAGENT_GUI_PORT overrides the default port, e.g. when 8080 is taken.
bool resolveGuiPort(int& port, std::ostream& error) {
    const char* value = std::getenv("CODEAGENT_GUI_PORT");
    if (!isSetEnvironmentValue(value)) {
        port = kDefaultGuiPort;
        return true;
    }
    const std::string text(value);
    const bool digitsOnly = text.size() <= 5 && std::all_of(
        text.begin(), text.end(),
        [](unsigned char character) { return std::isdigit(character) != 0; }
    );
    port = digitsOnly ? std::stoi(text) : 0;
    if (port < 1 || port > 65535) {
        error << "Error: CODEAGENT_GUI_PORT must be a port number from 1 to 65535.\n";
        return false;
    }
    return true;
}

bool containsFrontend(const std::filesystem::path& directory) {
    std::error_code ignored;
    return std::filesystem::is_regular_file(directory / "index.html", ignored);
}

// Finds the frontend independently of the working directory: an explicit
// CODEAGENT_FRONTEND_DIR, then locations relative to the executable (build
// tree or installed share/codeagent), then the source tree it was built from.
std::filesystem::path findFrontendDirectory(std::ostream& error) {
    const char* configured = std::getenv("CODEAGENT_FRONTEND_DIR");
    if (isSetEnvironmentValue(configured)) {
        if (containsFrontend(configured)) {
            return configured;
        }
        error << "Warning: CODEAGENT_FRONTEND_DIR does not contain index.html; "
              << "searching the default locations.\n";
    }

    std::vector<std::filesystem::path> candidates;
    std::error_code executableError;
    const auto executable =
        std::filesystem::read_symlink("/proc/self/exe", executableError);
    if (!executableError) {
        const auto directory = executable.parent_path();
        candidates.push_back(directory / "frontend");
        candidates.push_back(directory.parent_path() / "frontend");
        candidates.push_back(directory.parent_path() / "share/codeagent/frontend");
    }
#ifdef CODEAGENT_FRONTEND_SOURCE_DIR
    candidates.emplace_back(CODEAGENT_FRONTEND_SOURCE_DIR);
#endif
    for (const auto& candidate : candidates) {
        if (containsFrontend(candidate)) {
            return candidate.lexically_normal();
        }
    }
    return {};
}

int runGuiCommand(std::ostream& output, std::ostream& error) {
    int port = kDefaultGuiPort;
    if (!resolveGuiPort(port, error)) {
        return 1;
    }

    // Report a missing key at startup instead of only on the first request.
    // The key itself is never printed or sent to the browser.
    loadEnvFile(".env");
    if (!isSetEnvironmentValue(std::getenv("GEMINI_API_KEY"))) {
        error << "Warning: GEMINI_API_KEY is not set. Add it to a .env file in "
              << "the launch directory or export it; generation and "
              << "refactoring will fail until it is configured.\n";
    }

    GuiHttpServer::GenerationHandler generationHandler =
        [](const std::string& instruction,
           const std::string& language,
           std::string& generatedCode,
           std::string& thoughts) {
            PromptBuilder promptBuilder;
            std::string prompt;
            if (!promptBuilder.buildGenerationPrompt(
                    instruction,
                    language,
                    prompt)) {
                return false;
            }

            LLMClient client;
            return client.generateContent(prompt, generatedCode, thoughts);
        };
    GuiHttpServer::RefactoringHandler refactoringHandler =
        [](const std::string& instruction,
           const std::string& language,
           const std::string& sourceCode,
           std::string& refactoredCode,
           std::string& thoughts) {
            PromptBuilder promptBuilder;
            std::string prompt;
            if (!promptBuilder.buildRefactoringPrompt(
                    instruction,
                    language,
                    sourceCode,
                    prompt)) {
                return false;
            }

            LLMClient client;
            return client.generateCompleteContent(prompt, refactoredCode, thoughts);
        };
    GuiHttpServer server(
        port,
        std::filesystem::current_path(),
        std::move(generationHandler),
        std::move(refactoringHandler)
    );
    const auto frontendDirectory = findFrontendDirectory(error);
    const bool servingFrontend =
        !frontendDirectory.empty() && server.serveFrontend(frontendDirectory);
    if (!servingFrontend) {
        error << "Warning: Frontend files were not found; only the API is "
              << "available. Set CODEAGENT_FRONTEND_DIR to the frontend "
              << "directory that contains index.html.\n";
    }
    if (!server.start()) {
        error << "Error: Could not start the GUI server on 127.0.0.1:"
              << port << ". The port may be in use by another program "
              << "(is another 'codeagent gui' running?). Stop it or choose "
              << "another port, e.g. CODEAGENT_GUI_PORT=8081 codeagent gui\n";
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

    const std::string url =
        "http://127.0.0.1:" + std::to_string(server.port()) + "/";
    output << "CodeAgent GUI running at " << url << "\n"
           << "Health endpoint: " << url << "api/health\n";
    if (servingFrontend) {
        output << "Serving frontend from " << frontendDirectory.string() << "\n";
        if (isSetEnvironmentValue(std::getenv("CODEAGENT_NO_BROWSER"))) {
            output << "Open " << url << " in your browser.\n";
        } else if (launchDefaultBrowser(url)) {
            output << "Opening your default browser...\n";
        } else {
            output << "Could not open a browser automatically. Open "
                   << url << " in your browser.\n";
        }
    }
    output << "Press Ctrl+C to stop.\n";
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
        << "  gui        Start the browser GUI on 127.0.0.1 and open it (Ctrl+C to stop)\n"
        << "  generate   Generate code from an instruction\n"
        << "  refactor   Refactor a source file using AI\n"
        << "  help       Display this help message\n\n"
        << "Options:\n"
        << "  -h, --help Display this help message\n\n"
        << "Environment:\n"
        << "  GEMINI_API_KEY          Gemini API key (or set it in ./.env)\n"
        << "  CODEAGENT_GUI_PORT      GUI port (default 8080)\n"
        << "  CODEAGENT_NO_BROWSER    Set to 1 to skip opening a browser\n"
        << "  CODEAGENT_FRONTEND_DIR  Directory containing the frontend index.html\n";
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
