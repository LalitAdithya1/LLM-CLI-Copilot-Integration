#include "ResponseParser.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <optional>
#include <string>
#include <utility>

namespace {

using json = nlohmann::json;

bool isWhitespaceOnly(const std::string& text) {
    for (unsigned char ch : text) {
        if (!std::isspace(ch)) {
            return false;
        }
    }

    return true;
}

ParsedResponse failure(
    ResponseParseError error,
    const std::string& message,
    ResponseCompletionStatus completionStatus =
        ResponseCompletionStatus::Unspecified,
    std::optional<std::string> finishReason = std::nullopt
) {
    ParsedResponse response;
    response.error = error;
    response.errorMessage = message;
    response.completionStatus = completionStatus;
    response.finishReason = std::move(finishReason);
    return response;
}

struct FinishReasonResult {
    ResponseCompletionStatus status;
    ResponseParseError error;
    std::string errorMessage;
};

FinishReasonResult classifyFinishReason(const std::string& reason) {
    if (reason == "STOP") {
        return {
            ResponseCompletionStatus::Complete,
            ResponseParseError::None,
            ""
        };
    }

    if (reason == "MAX_TOKENS") {
        return {
            ResponseCompletionStatus::Partial,
            ResponseParseError::IncompleteResponse,
            "Gemini stopped at the token limit; output may be incomplete."
        };
    }

    if (reason == "CONTINUATION") {
        return {
            ResponseCompletionStatus::Partial,
            ResponseParseError::IncompleteResponse,
            "Gemini returned a continuation response; output is incomplete."
        };
    }

    if (reason == "SAFETY" ||
        reason == "RECITATION" ||
        reason == "LANGUAGE" ||
        reason == "BLOCKLIST" ||
        reason == "PROHIBITED_CONTENT" ||
        reason == "SPII" ||
        reason == "IMAGE_SAFETY" ||
        reason == "IMAGE_PROHIBITED_CONTENT" ||
        reason == "IMAGE_RECITATION" ||
        reason == "ESCALATION" ||
        reason == "PUP_LIMITED_DISABLED") {
        return {
            ResponseCompletionStatus::Blocked,
            ResponseParseError::BlockedResponse,
            "Gemini flagged the response; generated text was not accepted."
        };
    }

    if (reason == "MALFORMED_FUNCTION_CALL" ||
        reason == "IMAGE_OTHER" ||
        reason == "NO_IMAGE" ||
        reason == "UNEXPECTED_TOOL_CALL" ||
        reason == "TOO_MANY_TOOL_CALLS" ||
        reason == "MISSING_THOUGHT_SIGNATURE" ||
        reason == "MALFORMED_RESPONSE") {
        return {
            ResponseCompletionStatus::Rejected,
            ResponseParseError::RejectedFinishReason,
            "Gemini did not produce a usable completed response."
        };
    }

    // OTHER, FINISH_REASON_UNSPECIFIED, and future enum values do not
    // establish that the response completed normally. Fail closed.
    return {
        ResponseCompletionStatus::Unknown,
        ResponseParseError::UnsupportedFinishReason,
        "Gemini returned an unsupported finish reason."
    };
}

std::string trimSpacesAndTabs(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = text.find_last_not_of(" \t\r");
    return text.substr(first, last - first + 1);
}

// Returns the code inside one Markdown fence that encloses the whole text,
// such as "```cpp\n...\n```". Returns nothing unless the text is exactly one
// fenced block: an opening line of three or more backticks with an optional
// language label, and a last line holding the same backticks alone.
std::optional<std::string> stripEnclosingCodeFence(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    const auto last = text.find_last_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return std::nullopt;
    }

    std::size_t fenceLength = 0;
    while (first + fenceLength <= last && text[first + fenceLength] == '`') {
        ++fenceLength;
    }
    if (fenceLength < 3) {
        return std::nullopt;
    }
    const std::string fence(fenceLength, '`');

    const auto openingEnd = text.find('\n', first);
    if (openingEnd == std::string::npos || openingEnd > last) {
        return std::nullopt;
    }
    // The label is a language name such as cpp, c++, C++ or c, or nothing.
    const std::string label = trimSpacesAndTabs(
        text.substr(first + fenceLength, openingEnd - first - fenceLength)
    );
    for (unsigned char ch : label) {
        if (!std::isalnum(ch) && ch != '+' && ch != '#' && ch != '-' &&
            ch != '_' && ch != '.') {
            return std::nullopt;
        }
    }

    const auto closingLineBreak = text.rfind('\n', last);
    if (closingLineBreak == std::string::npos || closingLineBreak < openingEnd ||
        trimSpacesAndTabs(text.substr(closingLineBreak + 1, last - closingLineBreak)) != fence) {
        return std::nullopt;
    }

    // Keep the code exactly, including the line break that ends its last line.
    std::string code = text.substr(openingEnd + 1, closingLineBreak - openingEnd);

    // Another fence line inside means several blocks or prose around code.
    std::size_t lineStart = 0;
    while (lineStart < code.size()) {
        const auto lineEnd = code.find('\n', lineStart);
        const auto contentStart = code.find_first_not_of(" \t", lineStart);
        if (contentStart != std::string::npos &&
            (lineEnd == std::string::npos || contentStart < lineEnd) &&
            code.compare(contentStart, fenceLength, fence) == 0) {
            return std::nullopt;
        }
        if (lineEnd == std::string::npos) {
            break;
        }
        lineStart = lineEnd + 1;
    }
    return code;
}

} // namespace

ParsedResponse ResponseParser::parseResponse(
    const std::string& responseBody
) const {
    json result;

    try {
        result = json::parse(responseBody);
    } catch (const json::exception&) {
        return failure(
            ResponseParseError::MalformedJson,
            "Failed to parse Gemini's JSON response."
        );
    }

    if (!result.is_object()) {
        return failure(
            ResponseParseError::UnexpectedStructure,
            "Gemini response has an unexpected JSON structure."
        );
    }

    if (result.contains("error")) {
        std::string message = "Gemini returned an error response.";
        const json& providerError = result["error"];

        if (providerError.is_object() &&
            providerError.contains("message") &&
            providerError["message"].is_string()) {
            message = providerError["message"].get<std::string>();
        }

        return failure(
            ResponseParseError::ProviderError,
            message
        );
    }

    if (!result.contains("candidates")) {
        return failure(
            ResponseParseError::MissingCandidates,
            "Gemini response contains no candidates."
        );
    }

    if (!result["candidates"].is_array()) {
        return failure(
            ResponseParseError::UnexpectedStructure,
            "Gemini response candidates field is not an array."
        );
    }

    if (result["candidates"].empty()) {
        return failure(
            ResponseParseError::MissingCandidates,
            "Gemini response contains no candidates."
        );
    }

    const json& candidate = result["candidates"][0];

    if (!candidate.is_object()) {
        return failure(
            ResponseParseError::UnexpectedStructure,
            "Gemini response has an unexpected candidate structure."
        );
    }

    std::optional<std::string> finishReason;
    FinishReasonResult finishResult{
        ResponseCompletionStatus::Unspecified,
        ResponseParseError::None,
        ""
    };

    if (candidate.contains("finishReason")) {
        if (!candidate["finishReason"].is_string()) {
            return failure(
                ResponseParseError::UnexpectedStructure,
                "Gemini response has an invalid finishReason value."
            );
        }

        finishReason = candidate["finishReason"].get<std::string>();
        finishResult = classifyFinishReason(*finishReason);

        if (finishResult.status == ResponseCompletionStatus::Blocked ||
            finishResult.status == ResponseCompletionStatus::Rejected ||
            finishResult.status == ResponseCompletionStatus::Unknown) {
            return failure(
                finishResult.error,
                finishResult.errorMessage,
                finishResult.status,
                finishReason
            );
        }
    }

    const auto failWithFinishReason =
        [&finishResult, &finishReason](
            ResponseParseError error,
            const std::string& message
        ) {
            return failure(
                error,
                message,
                finishResult.status,
                finishReason
            );
        };

    if (!candidate.contains("content") ||
        !candidate["content"].is_object()) {
        return failWithFinishReason(
            ResponseParseError::MissingContent,
            "Gemini response has no valid content."
        );
    }

    const json& content = candidate["content"];

    if (!content.contains("parts") || !content["parts"].is_array()) {
        return failWithFinishReason(
            ResponseParseError::MissingParts,
            "Gemini response contains no content parts."
        );
    }

    std::string generatedText;

    for (const auto& part : content["parts"]) {
        if (!part.is_object()) {
            return failWithFinishReason(
                ResponseParseError::UnexpectedStructure,
                "Gemini response has an unexpected content part."
            );
        }

        if (part.contains("text")) {
            if (!part["text"].is_string()) {
                return failWithFinishReason(
                    ResponseParseError::UnexpectedStructure,
                    "Gemini response contains a non-text text part."
                );
            }

            generatedText += part["text"].get<std::string>();
        }
    }

    if (isWhitespaceOnly(generatedText)) {
        return failWithFinishReason(
            ResponseParseError::EmptyGeneratedText,
            "Gemini returned no generated text."
        );
    }

    // Models often wrap code in a Markdown fence despite the prompt. Only a
    // complete (or legacy unspecified) response is unwrapped; partial output
    // is returned exactly as received.
    if (finishResult.error == ResponseParseError::None) {
        if (auto code = stripEnclosingCodeFence(generatedText)) {
            if (isWhitespaceOnly(*code)) {
                return failWithFinishReason(
                    ResponseParseError::EmptyGeneratedText,
                    "Gemini returned an empty code block."
                );
            }
            generatedText = std::move(*code);
        }
    }

    ParsedResponse response;
    response.generatedText = std::move(generatedText);
    response.finishReason = std::move(finishReason);
    response.completionStatus = finishResult.status;

    if (finishResult.error != ResponseParseError::None) {
        response.error = finishResult.error;
        response.errorMessage = finishResult.errorMessage;
    }

    return response;
}
