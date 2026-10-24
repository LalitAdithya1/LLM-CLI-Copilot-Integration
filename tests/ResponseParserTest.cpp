#include "ResponseParser.hpp"

#include <gtest/gtest.h>

#include <string>

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
