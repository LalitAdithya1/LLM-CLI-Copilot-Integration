#include "ResponseParser.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

namespace {

const std::string kValidGeminiResponse = R"({
    "candidates": [{
        "finishReason": "STOP",
        "content": {
            "parts": [
                {"text": "int add(int a, int b) {"},
                {"text": " return a + b; }"}
            ]
        }
    }]
})";

std::string responseWithFinishReason(const std::string& finishReason) {
    return R"({"candidates":[{"finishReason":")" + finishReason +
           R"(","content":{"parts":[{"text":"generated text"}]}}]})";
}

} // namespace

class ResponseParserTest : public ::testing::Test {
protected:
    ResponseParser parser;
};

TEST_F(ResponseParserTest, ParsesGeminiTextParts) {
    const ParsedResponse response =
        parser.parseResponse(kValidGeminiResponse);

    ASSERT_TRUE(response.succeeded());
    EXPECT_EQ(
        response.generatedText,
        "int add(int a, int b) { return a + b; }"
    );
    ASSERT_TRUE(response.finishReason.has_value());
    EXPECT_EQ(*response.finishReason, "STOP");
    EXPECT_EQ(
        response.completionStatus,
        ResponseCompletionStatus::Complete
    );
    EXPECT_TRUE(response.isComplete());
    EXPECT_FALSE(response.isPartial());
    EXPECT_FALSE(response.explanation.has_value());
}

TEST_F(ResponseParserTest, RejectsSafetyResponseEvenWhenItContainsText) {
    const ParsedResponse response =
        parser.parseResponse(responseWithFinishReason("SAFETY"));

    EXPECT_FALSE(response.succeeded());
    EXPECT_EQ(response.error, ResponseParseError::BlockedResponse);
    EXPECT_EQ(
        response.errorMessage,
        "Gemini flagged the response; generated text was not accepted."
    );
    ASSERT_TRUE(response.finishReason.has_value());
    EXPECT_EQ(*response.finishReason, "SAFETY");
    EXPECT_EQ(response.completionStatus, ResponseCompletionStatus::Blocked);
    EXPECT_TRUE(response.generatedText.empty());
    EXPECT_FALSE(response.isComplete());
    EXPECT_FALSE(response.isPartial());
}

TEST_F(ResponseParserTest, RejectsRecitationResponseEvenWhenItContainsText) {
    const ParsedResponse response =
        parser.parseResponse(responseWithFinishReason("RECITATION"));

    EXPECT_FALSE(response.succeeded());
    EXPECT_EQ(response.error, ResponseParseError::BlockedResponse);
    ASSERT_TRUE(response.finishReason.has_value());
    EXPECT_EQ(*response.finishReason, "RECITATION");
    EXPECT_EQ(response.completionStatus, ResponseCompletionStatus::Blocked);
    EXPECT_TRUE(response.generatedText.empty());
    EXPECT_FALSE(response.isComplete());
}

TEST_F(ResponseParserTest, MarksMaxTokensOutputAsPartial) {
    const ParsedResponse response =
        parser.parseResponse(responseWithFinishReason("MAX_TOKENS"));

    EXPECT_FALSE(response.succeeded());
    EXPECT_EQ(response.error, ResponseParseError::IncompleteResponse);
    EXPECT_EQ(
        response.errorMessage,
        "Gemini stopped at the token limit; output may be incomplete."
    );
    ASSERT_TRUE(response.finishReason.has_value());
    EXPECT_EQ(*response.finishReason, "MAX_TOKENS");
    EXPECT_EQ(response.completionStatus, ResponseCompletionStatus::Partial);
    EXPECT_EQ(response.generatedText, "generated text");
    EXPECT_TRUE(response.isPartial());
    EXPECT_FALSE(response.isComplete());
}

TEST_F(ResponseParserTest, MarksContinuationOutputAsPartial) {
    const ParsedResponse response =
        parser.parseResponse(responseWithFinishReason("CONTINUATION"));

    EXPECT_FALSE(response.succeeded());
    EXPECT_EQ(response.error, ResponseParseError::IncompleteResponse);
    ASSERT_TRUE(response.finishReason.has_value());
    EXPECT_EQ(*response.finishReason, "CONTINUATION");
    EXPECT_EQ(response.completionStatus, ResponseCompletionStatus::Partial);
    EXPECT_EQ(response.generatedText, "generated text");
    EXPECT_TRUE(response.isPartial());
    EXPECT_FALSE(response.isComplete());
}

TEST_F(ResponseParserTest, AcceptsMissingFinishReasonAsUnspecifiedLegacyStatus) {
    const ParsedResponse response = parser.parseResponse(
        R"({"candidates":[{"content":{"parts":[{"text":"legacy text"}]}}]})"
    );

    EXPECT_TRUE(response.succeeded());
    EXPECT_FALSE(response.finishReason.has_value());
    EXPECT_EQ(
        response.completionStatus,
        ResponseCompletionStatus::Unspecified
    );
    EXPECT_EQ(response.generatedText, "legacy text");
    EXPECT_FALSE(response.isComplete());
    EXPECT_FALSE(response.isPartial());
}

TEST_F(ResponseParserTest, RejectsUnknownFinishReasonWithoutReturningText) {
    const ParsedResponse response =
        parser.parseResponse(responseWithFinishReason("FUTURE_REASON"));

    EXPECT_FALSE(response.succeeded());
    EXPECT_EQ(response.error, ResponseParseError::UnsupportedFinishReason);
    EXPECT_EQ(
        response.errorMessage,
        "Gemini returned an unsupported finish reason."
    );
    ASSERT_TRUE(response.finishReason.has_value());
    EXPECT_EQ(*response.finishReason, "FUTURE_REASON");
    EXPECT_EQ(response.completionStatus, ResponseCompletionStatus::Unknown);
    EXPECT_TRUE(response.generatedText.empty());
    EXPECT_FALSE(response.isComplete());
    EXPECT_FALSE(response.isPartial());
}

TEST_F(ResponseParserTest, RejectsMalformedJson) {
    const ParsedResponse response = parser.parseResponse("not JSON");

    EXPECT_FALSE(response.succeeded());
    EXPECT_EQ(response.error, ResponseParseError::MalformedJson);
    EXPECT_TRUE(response.generatedText.empty());
}

TEST_F(ResponseParserTest, RejectsMissingCandidates) {
    const ParsedResponse response = parser.parseResponse("{}");

    EXPECT_FALSE(response.succeeded());
    EXPECT_EQ(response.error, ResponseParseError::MissingCandidates);
}

TEST_F(ResponseParserTest, RejectsEmptyCandidatesArray) {
    const ParsedResponse response = parser.parseResponse(
        R"({"candidates":[]})"
    );

    EXPECT_FALSE(response.succeeded());
    EXPECT_EQ(response.error, ResponseParseError::MissingCandidates);
    EXPECT_EQ(response.errorMessage,
              "Gemini response contains no candidates.");
}

TEST_F(ResponseParserTest, RejectsNonArrayCandidatesAsUnexpectedStructure) {
    const ParsedResponse response = parser.parseResponse(
        R"({"candidates":"unexpected"})"
    );

    EXPECT_FALSE(response.succeeded());
    EXPECT_EQ(response.error, ResponseParseError::UnexpectedStructure);
    EXPECT_EQ(response.errorMessage,
              "Gemini response candidates field is not an array.");
}

TEST_F(ResponseParserTest, RejectsMissingContent) {
    const ParsedResponse response = parser.parseResponse(
        R"({"candidates":[{}]})"
    );

    EXPECT_FALSE(response.succeeded());
    EXPECT_EQ(response.error, ResponseParseError::MissingContent);
}

TEST_F(ResponseParserTest, RejectsWrongTypedContent) {
    const ParsedResponse response = parser.parseResponse(
        R"({"candidates":[{"content":null}]})"
    );

    EXPECT_FALSE(response.succeeded());
    EXPECT_EQ(response.error, ResponseParseError::MissingContent);
    EXPECT_EQ(response.errorMessage, "Gemini response has no valid content.");
}

TEST_F(ResponseParserTest, RejectsMissingParts) {
    const ParsedResponse response = parser.parseResponse(
        R"({"candidates":[{"content":{}}]})"
    );

    EXPECT_FALSE(response.succeeded());
    EXPECT_EQ(response.error, ResponseParseError::MissingParts);
}

TEST_F(ResponseParserTest, RejectsWrongTypedParts) {
    const ParsedResponse response = parser.parseResponse(
        R"({"candidates":[{"content":{"parts":"unexpected"}}]})"
    );

    EXPECT_FALSE(response.succeeded());
    EXPECT_EQ(response.error, ResponseParseError::MissingParts);
    EXPECT_EQ(response.errorMessage,
              "Gemini response contains no content parts.");
}

TEST_F(ResponseParserTest, RejectsFinishReasonWithUnexpectedJsonType) {
    const ParsedResponse response = parser.parseResponse(
        R"({"candidates":[{"finishReason":1,"content":{"parts":[{"text":"text"}]}}]})"
    );

    EXPECT_FALSE(response.succeeded());
    EXPECT_EQ(response.error, ResponseParseError::UnexpectedStructure);
    EXPECT_EQ(response.errorMessage,
              "Gemini response has an invalid finishReason value.");
    EXPECT_EQ(response.completionStatus,
              ResponseCompletionStatus::Unspecified);
    EXPECT_TRUE(response.generatedText.empty());
}

TEST_F(ResponseParserTest, RejectsEmptyGeneratedText) {
    const ParsedResponse response = parser.parseResponse(
        R"({"candidates":[{"content":{"parts":[{"text":"  "}]}}]})"
    );

    EXPECT_FALSE(response.succeeded());
    EXPECT_EQ(response.error, ResponseParseError::EmptyGeneratedText);
}

TEST_F(ResponseParserTest, ReportsProviderErrorMessage) {
    const ParsedResponse response = parser.parseResponse(
        R"({"error":{"message":"Request was rejected"}})"
    );

    EXPECT_FALSE(response.succeeded());
    EXPECT_EQ(response.error, ResponseParseError::ProviderError);
    EXPECT_EQ(response.errorMessage, "Request was rejected");
}

TEST_F(ResponseParserTest, RejectsUnexpectedRootStructure) {
    const ParsedResponse response = parser.parseResponse("[]");

    EXPECT_FALSE(response.succeeded());
    EXPECT_EQ(response.error, ResponseParseError::UnexpectedStructure);
}

TEST_F(ResponseParserTest, RejectsUnexpectedCandidateStructure) {
    const ParsedResponse response = parser.parseResponse(
        R"({"candidates":[null]})"
    );

    EXPECT_FALSE(response.succeeded());
    EXPECT_EQ(response.error, ResponseParseError::UnexpectedStructure);
}

namespace {

// Builds a Gemini response whose text parts are given verbatim. An empty
// finishReason omits the field (legacy responses).
std::string responseWithParts(
    const std::vector<std::string>& parts,
    const std::string& finishReason = "STOP"
) {
    nlohmann::json textParts = nlohmann::json::array();
    for (const auto& part : parts) {
        textParts.push_back({{"text", part}});
    }
    nlohmann::json candidate = {{"content", {{"parts", textParts}}}};
    if (!finishReason.empty()) {
        candidate["finishReason"] = finishReason;
    }
    return nlohmann::json{{"candidates", nlohmann::json::array({candidate})}}.dump();
}

std::string responseWithText(
    const std::string& text,
    const std::string& finishReason = "STOP"
) {
    return responseWithParts({text}, finishReason);
}

} // namespace

TEST_F(ResponseParserTest, StripsAnEnclosingCodeFenceAndKeepsTheCodeExactly) {
    const std::string code =
        "#include <vector>\n"
        "\n"
        "int total(const std::vector<int>& values) {\n"
        "    int sum = 0;\n"
        "\n"
        "    for (int value : values) {\n"
        "        sum += value;\n"
        "    }\n"
        "    return sum;\n"
        "}\n";
    const ParsedResponse response =
        parser.parseResponse(responseWithText("```cpp\n" + code + "```"));

    ASSERT_TRUE(response.succeeded());
    EXPECT_EQ(response.generatedText, code);
    EXPECT_TRUE(response.isComplete());
}

TEST_F(ResponseParserTest, StripsFencesWithCommonOrMissingLanguageLabels) {
    for (const std::string opening : {"```cpp", "```c++", "```C++", "```c", "```CPP",
                                      "```", "``` cpp ", "````cpp"}) {
        const std::string closing = opening.rfind("````", 0) == 0 ? "````" : "```";
        const ParsedResponse response = parser.parseResponse(
            responseWithText(opening + "\nint x = 1;\n" + closing)
        );
        ASSERT_TRUE(response.succeeded()) << opening;
        EXPECT_EQ(response.generatedText, "int x = 1;\n") << opening;
    }
}

TEST_F(ResponseParserTest, StripsFenceSurroundedByWhitespaceOrSplitAcrossParts) {
    EXPECT_EQ(parser.parseResponse(responseWithText(
                  "\n\n  ```cpp\nint a;\n```  \n\n")).generatedText,
              "int a;\n");
    EXPECT_EQ(parser.parseResponse(responseWithText(
                  "```cpp\r\nint a;\r\nint b;\r\n```\r\n")).generatedText,
              "int a;\r\nint b;\r\n");
    EXPECT_EQ(parser.parseResponse(responseWithParts(
                  {"```cpp\nint ", "a;\n", "```"})).generatedText,
              "int a;\n");
}

TEST_F(ResponseParserTest, LeavesUnfencedCodeUnchanged) {
    const std::string code = "int add(int a, int b) {\n    return a + b;\n}\n";
    const ParsedResponse response = parser.parseResponse(responseWithText(code));
    ASSERT_TRUE(response.succeeded());
    EXPECT_EQ(response.generatedText, code);
}

TEST_F(ResponseParserTest, KeepsBackticksAndMarkdownLikeTextInsideTheCode) {
    const std::string code =
        "// Example: `add(1, 2)` returns 3. **Not** markdown.\n"
        "const char* fence = \"```\";\n"
        "const char* raw = R\"(```cpp)\";\n"
        "# define NOTE \"- item\"\n";
    EXPECT_EQ(parser.parseResponse(responseWithText("```cpp\n" + code + "```")).generatedText,
              code);

    // An inner ``` line is ordinary content inside a four-backtick fence.
    const std::string withInnerFence = "/*\n```cpp\nexample();\n```\n*/\nint x;\n";
    EXPECT_EQ(parser.parseResponse(responseWithText(
                  "````cpp\n" + withInnerFence + "````")).generatedText,
              withInnerFence);
}

TEST_F(ResponseParserTest, LeavesTextUnchangedWhenItIsNotExactlyOneFencedBlock) {
    const std::vector<std::string> notEnclosed = {
        "```cpp\nint x;\n",                         // no closing fence
        "int x;\n```",                              // no opening fence
        "Here is the code:\n```cpp\nint x;\n```",   // prose before
        "```cpp\nint x;\n```\nHope this helps!",    // prose after
        "```cpp\nint x;\n``` trailing",             // closing line has text
        "````cpp\nint x;\n```",                     // mismatched fence lengths
        "```cpp\nint x;\n````",                     // mismatched fence lengths
        "```cpp\nint a;\n```\n\n```cpp\nint b;\n```",  // two blocks
        "```cpp int x; ```",                        // single line
        "```cpp title=\"a.cpp\"\nint x;\n```",      // not a plain language label
        "``cpp\nint x;\n``",                        // two backticks is not a fence
    };
    for (const auto& text : notEnclosed) {
        const ParsedResponse response = parser.parseResponse(responseWithText(text));
        ASSERT_TRUE(response.succeeded()) << text;
        EXPECT_EQ(response.generatedText, text) << text;
    }
}

TEST_F(ResponseParserTest, AnEmptyFencedBlockIsReportedAsEmptyText) {
    for (const std::string text : {"```cpp\n```", "```\n\n   \n```"}) {
        const ParsedResponse response = parser.parseResponse(responseWithText(text));
        EXPECT_FALSE(response.succeeded()) << text;
        EXPECT_EQ(response.error, ResponseParseError::EmptyGeneratedText) << text;
        EXPECT_TRUE(response.generatedText.empty()) << text;
    }
}

TEST_F(ResponseParserTest, StripsFenceFromLegacyResponsesWithoutFinishReason) {
    const ParsedResponse response =
        parser.parseResponse(responseWithText("```cpp\nint x;\n```", ""));
    ASSERT_TRUE(response.succeeded());
    EXPECT_EQ(response.completionStatus, ResponseCompletionStatus::Unspecified);
    EXPECT_EQ(response.generatedText, "int x;\n");
}

TEST_F(ResponseParserTest, PartialFencedOutputIsReturnedUnchanged) {
    for (const std::string reason : {"MAX_TOKENS", "CONTINUATION"}) {
        const std::string text = "```cpp\nint x;\n```";
        const ParsedResponse response = parser.parseResponse(responseWithText(text, reason));
        EXPECT_EQ(response.error, ResponseParseError::IncompleteResponse) << reason;
        EXPECT_TRUE(response.isPartial()) << reason;
        EXPECT_EQ(response.generatedText, text) << reason;
    }
}

TEST_F(ResponseParserTest, BlockedRejectedAndUnknownFencedResponsesReturnNoText) {
    const std::vector<std::pair<std::string, ResponseParseError>> cases = {
        {"SAFETY", ResponseParseError::BlockedResponse},
        {"MALFORMED_RESPONSE", ResponseParseError::RejectedFinishReason},
        {"FUTURE_REASON", ResponseParseError::UnsupportedFinishReason},
    };
    for (const auto& [reason, error] : cases) {
        const ParsedResponse response =
            parser.parseResponse(responseWithText("```cpp\nint x;\n```", reason));
        EXPECT_FALSE(response.succeeded()) << reason;
        EXPECT_EQ(response.error, error) << reason;
        EXPECT_TRUE(response.generatedText.empty()) << reason;
    }
}

TEST_F(ResponseParserTest, SeparatesThoughtPartsFromGeneratedText) {
    const ParsedResponse response = parser.parseResponse(R"({
        "candidates": [{
            "finishReason": "STOP",
            "content": {
                "parts": [
                    {"text": "**Planning**\nAdd two ints.", "thought": true},
                    {"text": "```cpp\nint add(int a, int b);\n```"},
                    {"text": " Then return.", "thought": true}
                ]
            }
        }]
    })");

    ASSERT_TRUE(response.succeeded());
    EXPECT_EQ(response.generatedText, "int add(int a, int b);\n");
    EXPECT_EQ(response.thoughts, "**Planning**\nAdd two ints. Then return.");
}

TEST_F(ResponseParserTest, ResponseWithOnlyThoughtsHasNoGeneratedText) {
    const ParsedResponse response = parser.parseResponse(
        R"({"candidates":[{"finishReason":"STOP","content":{"parts":[{"text":"thinking","thought":true}]}}]})"
    );

    EXPECT_EQ(response.error, ResponseParseError::EmptyGeneratedText);
}
