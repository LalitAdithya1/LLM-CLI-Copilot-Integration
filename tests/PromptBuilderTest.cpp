
#include "PromptBuilder.hpp"

#include <gtest/gtest.h>

#include <string>

class PromptBuilderTest : public ::testing::Test {
protected:
    PromptBuilder builder;
    std::string prompt;
};

TEST_F(PromptBuilderTest, BuildsGenerationPrompt) {
    const bool result = builder.buildGenerationPrompt(
        "Write a function that adds two integers.",
        "C++",
        prompt
    );

    ASSERT_TRUE(result);
    EXPECT_FALSE(prompt.empty());
    EXPECT_NE(prompt.find("C++"), std::string::npos);
    EXPECT_NE(
        prompt.find("Write a function that adds two integers."),
        std::string::npos
    );
}

TEST_F(PromptBuilderTest, BuildsRefactoringPrompt) {
    const bool result = builder.buildRefactoringPrompt(
        "Improve readability",
        "C++",
        "int add(int a,int b){return a+b;}",
        prompt
    );

    ASSERT_TRUE(result);
    EXPECT_NE(prompt.find("Improve readability"), std::string::npos);
    EXPECT_NE(prompt.find("C++"), std::string::npos);
    EXPECT_NE(
        prompt.find("int add(int a,int b){return a+b;}"),
        std::string::npos
    );
}

TEST_F(PromptBuilderTest, RejectsEmptyGenerationInstruction) {
    prompt = "old prompt";

    EXPECT_FALSE(
        builder.buildGenerationPrompt("", "C++", prompt)
    );

    EXPECT_TRUE(prompt.empty());
}

TEST_F(PromptBuilderTest, RejectsWhitespaceOnlyInstruction) {
    EXPECT_FALSE(
        builder.buildGenerationPrompt("   \t\n", "C++", prompt)
    );

    EXPECT_TRUE(prompt.empty());
}

TEST_F(PromptBuilderTest, RejectsEmptyLanguage) {
    EXPECT_FALSE(
        builder.buildGenerationPrompt("Write a function", "", prompt)
    );

    EXPECT_TRUE(prompt.empty());
}

TEST_F(PromptBuilderTest, RejectsEmptyRefactoringSourceCode) {
    EXPECT_FALSE(
        builder.buildRefactoringPrompt(
            "Improve readability",
            "C++",
            "   \n",
            prompt
        )
    );

    EXPECT_TRUE(prompt.empty());
}

TEST_F(PromptBuilderTest, GenerationOutputIsDeterministic) {
    std::string firstPrompt;
    std::string secondPrompt;

    ASSERT_TRUE(builder.buildGenerationPrompt(
        "Write a calculator",
        "C++",
        firstPrompt
    ));

    ASSERT_TRUE(builder.buildGenerationPrompt(
        "Write a calculator",
        "C++",
        secondPrompt
    ));

    EXPECT_EQ(firstPrompt, secondPrompt);
}

TEST_F(PromptBuilderTest, GenerationAndRefactoringPromptsDiffer) {
    std::string generationPrompt;
    std::string refactoringPrompt;

    ASSERT_TRUE(builder.buildGenerationPrompt(
        "Improve this code",
        "C++",
        generationPrompt
    ));

    ASSERT_TRUE(builder.buildRefactoringPrompt(
        "Improve this code",
        "C++",
        "int main() { return 0; }",
        refactoringPrompt
    ));

    EXPECT_NE(generationPrompt, refactoringPrompt);
}
