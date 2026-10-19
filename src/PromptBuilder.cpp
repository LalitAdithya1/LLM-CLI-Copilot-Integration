
#include "PromptBuilder.hpp"

#include <cctype>
#include <string>

namespace {

bool isWhitespaceOnly(const std::string& text) {
    for (unsigned char ch : text) {
        if (!std::isspace(ch)) {
            return false;
        }
    }

    return true;
}

} // namespace

bool PromptBuilder::isValidInstruction(
    const std::string& instruction
) const {
    return !isWhitespaceOnly(instruction);
}

bool PromptBuilder::isValidLanguage(
    const std::string& language
) const {
    return !isWhitespaceOnly(language);
}

bool PromptBuilder::isValidSourceCode(
    const std::string& sourceCode
) const {
    return !isWhitespaceOnly(sourceCode);
}

bool PromptBuilder::buildGenerationPrompt(
    const std::string& instruction,
    const std::string& language,
    std::string& prompt
) const {
    prompt.clear();

    if (!isValidInstruction(instruction) ||
        !isValidLanguage(language)) {
        return false;
    }

    prompt =
        "You are an expert software developer.\n"
        "Generate code according to the requirements below.\n\n"
        "Programming language:\n" + language + "\n\n"
        "User instructions:\n" + instruction + "\n\n"
        "Return the generated code and nothing else.";

    return true;
}

bool PromptBuilder::buildRefactoringPrompt(
    const std::string& instruction,
    const std::string& language,
    const std::string& sourceCode,
    std::string& prompt
) const {
    prompt.clear();

    if (!isValidInstruction(instruction) ||
        !isValidLanguage(language) ||
        !isValidSourceCode(sourceCode)) {
        return false;
    }

    prompt =
        "You are an expert software developer.\n"
        "Refactor the provided source code according to the "
        "user's requirements.\n"
        "Preserve the existing behavior unless the instructions "
        "explicitly require a behavior change.\n\n"
        "Programming language:\n" + language + "\n\n"
        "User instructions:\n" + instruction + "\n\n"
        "Source code:\n"
        "```" + language + "\n" + sourceCode + "\n```\n\n"
        "Return the refactored code and nothing else.";

    return true;
}
