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
