#ifndef RESPONSE_PARSER_HPP
#define RESPONSE_PARSER_HPP

#include <optional>
#include <string>
#include <vector>

enum class ResponseParseError {
    None,
    MalformedJson,
    ProviderError,
    MissingCandidates,
    UnexpectedStructure,
    MissingContent,
    MissingParts,
    EmptyGeneratedText,
    BlockedResponse,
    IncompleteResponse,
    RejectedFinishReason,
    UnsupportedFinishReason
};

enum class ResponseCompletionStatus {
    // Compatibility status for older responses with no finishReason.
    Unspecified,
    Complete,
    Partial,
    Blocked,
    Rejected,
    Unknown
};

struct ParsedResponse {
    // Gemini's text parts are combined into this field.
    std::string generatedText;

    // Gemini's current response shape has no separate explanation field.
    std::optional<std::string> explanation;

    // Thought-summary parts ("thought": true), combined in order. They are
    // never part of generatedText. Empty unless thoughts were requested.
    std::string thoughts;

    // Kept exactly as provided. An absent field stays empty; no reason is
    // inferred from the response text.
    std::optional<std::string> finishReason;
    ResponseCompletionStatus completionStatus =
        ResponseCompletionStatus::Unspecified;

    ResponseParseError error = ResponseParseError::None;
    std::string errorMessage;

    bool succeeded() const noexcept {
        return error == ResponseParseError::None;
    }

    bool isComplete() const noexcept {
        return succeeded() &&
               completionStatus == ResponseCompletionStatus::Complete;
    }

    bool isPartial() const noexcept {
        return completionStatus == ResponseCompletionStatus::Partial;
    }
};

// The machine-readable parts of a Gemini error body
// ({"error": {"code", "status", "details": [...]}}). The free-text message is
// deliberately not kept: it is never shown or logged. Every field is checked
// against a strict character set, so it is safe to log.
struct ProviderErrorDetails {
    long code = 0;
    std::string status;                   // e.g. RESOURCE_EXHAUSTED
    std::string reason;                   // ErrorInfo reason, e.g. API_KEY_INVALID
    std::vector<std::string> quotaIds;    // QuotaFailure violations
    std::optional<std::string> retryDelay; // RetryInfo, e.g. "30s"
};

class ResponseParser {
public:
    ParsedResponse parseResponse(
        const std::string& responseBody
    ) const;

    // Returns nullopt when the body is not a JSON object with an "error"
    // object.
    std::optional<ProviderErrorDetails> parseProviderError(
        const std::string& responseBody
    ) const;
};

#endif // RESPONSE_PARSER_HPP
