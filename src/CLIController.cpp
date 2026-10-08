#include "CLIController.hpp"

#include "FileManager.hpp"

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
        << "  refactor   Read and prepare a source file for refactoring\n"
        << "  help       Display this help message\n\n"
        << "Options:\n"
        << "  -h, --help Display this help message\n";
}

void displayUsageError(const std::string& message) {
    std::cerr << "Error: " << message << "\n\n";
    std::cerr
        << "Run 'codeagent help' for usage instructions.\n";
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

        std::cout
            << "Generation request recognized.\n"
            << "Instruction: ";

        for (int i = 2; i < argc; ++i) {
            if (i > 2) {
                std::cout << ' ';
            }

            std::cout << argv[i];
        }

        std::cout << "\n\n"
                  << "Code generation is not implemented yet.\n";

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

        std::cout
            << "Refactoring request recognized.\n"
            << "File: " << filePath << "\n"
            << "Instruction: ";

        for (int i = 3; i < argc; ++i) {
            if (i > 3) {
                std::cout << ' ';
            }

            std::cout << argv[i];
        }

        std::cout << "\n\n";

        // --------------------------------------------------------
        // Module 2 integration
        // --------------------------------------------------------

        FileManager fileManager;

        std::string sourceCode;

        if (!fileManager.readFile(
                filePath,
                sourceCode)) {

            std::cerr
                << "Refactoring cancelled because "
                << "the source file could not be read.\n";

            return 1;
        }

        std::cout
            << "Source file read successfully.\n"
            << "Source size: "
            << sourceCode.size()
            << " bytes.\n\n"
            << "Code refactoring is not implemented yet.\n";

        return 0;
    }

    // ------------------------------------------------------------
    // UNKNOWN COMMAND
    // ------------------------------------------------------------

    displayUsageError(
        "Unknown command: " + command
    );

    return 1;
}