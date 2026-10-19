#ifndef PROMPT_BUILDER_HPP
#define PROMPT_BUILDER_HPP

#include <string>

class PromptBuilder {
public:
    bool buildGenerationPrompt(
        const std::string& instruction,
        const std::string& language,
        std::string& prompt
    ) const;

    bool buildRefactoringPrompt(
        const std::string& instruction,
        const std::string& language,
        const std::string& sourceCode,
        std::string& prompt
    ) const;

private:
    bool isValidInstruction(
        const std::string& instruction
    ) const;

    bool isValidLanguage(
        const std::string& language
    ) const;

    bool isValidSourceCode(
        const std::string& sourceCode
    ) const;
};

#endif
