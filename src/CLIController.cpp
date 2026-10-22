
#include "CLIController.hpp"

#include "FileManager.hpp"
#include "PromptBuilder.hpp"
#include "LLMClient.hpp"

#include <iostream>
#include <string>

namespace {

void displayHelp() {
    std::cout
        << "CodingCLI - AI Coding Assistant\n\n"
        << "Usage:\n"
        << "  codeagent help\n"
        << "  codeagent generate \"<instruction>\"\n"
        << "  codeagent refactor <file-path> \"<instruction>\"\n\n"
        << "Commands:\n"
        << "  generate   Generate code from an instruction\n"
        << "  refactor   Refactor a source file using AI\n"
        << "  help       Display this help message\n\n"
        << "Options:\n"
        << "  -h, --help Display this help message\n";
}

void displayUsageError(const std::string& message) {
    std::cerr << "Error: " << message << "\n\n";
    std::cerr << "Run 'codeagent help' for usage instructions.\n";
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

int CLIController::run(int argc, char* argv[]) {
    if (argc < 2) {
        displayUsageError("No command provided.");
        return 1;
    }

    const std::string command = argv[1];

    // ------------------------------------------------------------
    // HELP
    // ------------------------------------------------------------

    if (command == "help" ||
        command == "--help" ||
        command == "-h") {
        displayHelp();
        return 0;
    }

    // ------------------------------------------------------------
    // GENERATE
    // ------------------------------------------------------------

    if (command == "generate") {
        if (argc < 3) {
            displayUsageError(
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
            std::cerr
                << "Error: Could not build generation prompt.\n";
            return 1;
        }

        LLMClient llmClient;
        std::string generatedCode;

        std::cout
            << "Sending generation request to Gemini...\n";

        if (!llmClient.generateContent(prompt, generatedCode)) {
            std::cerr
                << "Error: Code generation failed. Check your "
                << ".env API key, internet connection, and Gemini "
                << "API configuration.\n";
            return 1;
        }

        std::cout
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
            displayUsageError(
                "The refactor command requires a file path "
                "and an instruction."
            );
            return 1;
        }

        const std::string filePath = argv[2];
        const std::string instruction =
            joinArguments(argc, argv, 3);

        FileManager fileManager;
        std::string sourceCode;

        if (!fileManager.readFile(filePath, sourceCode)) {
            std::cerr
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
            std::cerr
                << "Error: Could not build refactoring prompt.\n";
            return 1;
        }

        LLMClient llmClient;
        std::string refactoredCode;

        std::cout
            << "Sending refactoring request to Gemini...\n";

        if (!llmClient.generateContent(prompt, refactoredCode)) {
            std::cerr
                << "Error: Refactoring failed. Check your "
                << ".env API key, internet connection, and Gemini "
                << "API configuration.\n";
            return 1;
        }

        std::cout
            << "\nRefactored code:\n\n"
            << refactoredCode
            << '\n';

        return 0;
    }

    // ------------------------------------------------------------
    // UNKNOWN COMMAND
    // ------------------------------------------------------------

    displayUsageError("Unknown command: " + command);
    return 1;
}
