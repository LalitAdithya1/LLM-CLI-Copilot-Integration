
#include "CLIController.hpp"

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
        << "  refactor   Propose changes to an existing source file\n"
        << "  help       Display this help message\n\n"
        << "Options:\n"
        << "  -h, --help Display this help message\n";
}

void displayUsageError(const std::string& message) {
    std::cerr << "Error: " << message << "\n\n";
    std::cerr << "Run 'codeagent help' for usage instructions.\n";
}

} // namespace

int CLIController::run(int argc, char* argv[]) {
    if (argc < 2) {
        displayUsageError("No command provided.");
        return 1;
    }

    const std::string command = argv[1];

    if (command == "help" ||
        command== "--help" ||
        command == "-h") {
        displayHelp();
        return 0;
    }

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

        std::cout << "\n\n"
                  << "Code refactoring is not implemented yet.\n";

        return 0;
    }

    displayUsageError("Unknown command: " + command);
    return 1;
}