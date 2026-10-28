#include "LLMClient.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <cctype>
#include <cstddef>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

#include "CodeAgentError.hpp"
#include "EnvLoader.hpp"
#include "ResponseParser.hpp"

namespace {

using json = nlohmann::json;
constexpr std::size_t kMaximumProviderResponseBytes = 2 * 1024 * 1024;

struct ProviderResponseBuffer {
    std::string body;
    bool exceededLimit = false;
};

// RAII owners for libcurl handles: they are released on every path out of
// performHttpRequest, including the exceptions it throws.
struct CurlEasyDeleter {
    void operator()(CURL* handle) const noexcept { curl_easy_cleanup(handle); }
};
struct CurlHeaderListDeleter {
    void operator()(curl_slist* list) const noexcept { curl_slist_free_all(list); }
};
using CurlHandle = std::unique_ptr<CURL, CurlEasyDeleter>;
using CurlHeaderList = std::unique_ptr<curl_slist, CurlHeaderListDeleter>;

void appendHeader(CurlHeaderList& headers, const std::string& header) {
    curl_slist* updated = curl_slist_append(headers.get(), header.c_str());
    if (updated == nullptr) throw std::bad_alloc();
    // The list head only changes when the list was empty.
    if (!headers) headers.reset(updated);
}

size_t writeResponse(
    char* data,
    size_t size,
    size_t count,
    void* userData
) {
    if (size != 0 && count > static_cast<std::size_t>(-1) / size) return 0;
    const size_t totalBytes = size * count;
    auto* response = static_cast<ProviderResponseBuffer*>(userData);
    if (totalBytes > kMaximumProviderResponseBytes - response->body.size()) {
        response->exceededLimit = true;
        return 0;
    }
    response->body.append(data, totalBytes);

    return totalBytes;
}

[[noreturn]] void throwTransportError(CURLcode result) {
    const std::string diagnostic =
        std::string("libcurl error ") + std::to_string(static_cast<int>(result)) +
        ": " + curl_easy_strerror(result);
    if (result == CURLE_OPERATION_TIMEDOUT) throw TimeoutError(diagnostic);
    throw NetworkError(diagnostic);
}

// Throws NetworkError, TimeoutError, or ProviderResponseError when no usable
// HTTP response arrives. The API key is only placed in a request header and
// never appears in an exception.
bool performHttpRequest(
    const std::string& url,
    const std::string& apiKey,
    const std::string& requestBody,
    std::string& responseBody,
    long& httpStatus
) {
    static std::once_flag curlInitFlag;
    static CURLcode curlInitResult = CURLE_FAILED_INIT;

    std::call_once(curlInitFlag, [] {
        curlInitResult = curl_global_init(CURL_GLOBAL_DEFAULT);
    });

    if (curlInitResult != CURLE_OK) {
        throw NetworkError("libcurl global initialization failed");
    }

    const CurlHandle curl(curl_easy_init());
    if (!curl) {
        throw NetworkError("libcurl could not create a request handle");
    }

    CurlHeaderList headers;
    appendHeader(headers, "Content-Type: application/json");
    appendHeader(headers, "x-goog-api-key: " + apiKey);

    responseBody.clear();
    httpStatus = 0;
    ProviderResponseBuffer responseBuffer;

    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, requestBody.c_str());
    curl_easy_setopt(
        curl.get(),
        CURLOPT_POSTFIELDSIZE,
        static_cast<long>(requestBody.size())
    );
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, writeResponse);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &responseBuffer);

    // Bound request duration and verify TLS certificates.
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);

    const CURLcode result = curl_easy_perform(curl.get());
    if (responseBuffer.exceededLimit) {
        throw ProviderResponseError(
            ProviderResponseError::Kind::Invalid,
            "provider response exceeded the 2 MiB limit"
        );
    }
    if (result != CURLE_OK) {
        throwTransportError(result);
    }

    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &httpStatus);
    responseBody = std::move(responseBuffer.body);
    return true;
}

std::string buildGeminiUrl(const std::string& model) {
    return "https://generativelanguage.googleapis.com/"
           "v1beta/models/" + model + ":generateContent";
}

std::string safeResponseDiagnostic(const ParsedResponse& response) {
    switch (response.error) {
    case ResponseParseError::MalformedJson:
        return "Gemini returned malformed JSON.";
    case ResponseParseError::ProviderError:
        return "Gemini returned a provider error.";
    case ResponseParseError::MissingCandidates:
        return "Gemini response contained no candidates.";
    case ResponseParseError::UnexpectedStructure:
        return "Gemini response had an unexpected structure.";
    case ResponseParseError::MissingContent:
        return "Gemini response had no valid content.";
    case ResponseParseError::MissingParts:
        return "Gemini response had no content parts.";
    case ResponseParseError::EmptyGeneratedText:
        return "Gemini response contained no generated text.";
    case ResponseParseError::BlockedResponse:
        return "Gemini flagged the response; generated text was not accepted.";
    case ResponseParseError::IncompleteResponse:
        if (response.finishReason && *response.finishReason == "MAX_TOKENS") {
            return "Gemini stopped at the token limit; output may be incomplete.";
        }
        if (response.finishReason && *response.finishReason == "CONTINUATION") {
            return "Gemini returned a continuation; output is incomplete.";
        }
        return "Gemini response may be incomplete.";
    case ResponseParseError::RejectedFinishReason:
        return "Gemini did not produce a usable completed response.";
    case ResponseParseError::UnsupportedFinishReason:
        return "Gemini returned an unsupported finish reason.";
    case ResponseParseError::None:
        return "Gemini returned an unsuccessful response.";
    }
    return "Gemini returned an unsuccessful response.";
}

// Summarizes an error response for the log from its structured fields only.
std::string describeProviderError(long httpStatus,
                                  const std::optional<ProviderErrorDetails>& details) {
    std::string text = "Gemini HTTP " + std::to_string(httpStatus) + "; provider error";
    if (!details) return text + " body was not a structured error";
    if (!details->status.empty()) text += " status " + details->status;
    if (!details->reason.empty()) text += ", reason " + details->reason;
    for (const auto& quotaId : details->quotaIds) text += ", quota " + quotaId;
    if (details->retryDelay) text += ", retry after " + *details->retryDelay;
    return text;
}

bool hasDailyQuotaViolation(const ProviderErrorDetails& details) {
    for (const auto& quotaId : details.quotaIds) {
        if (quotaId.find("PerDay") != std::string::npos) return true;
    }
    return false;
}

// Turns a non-2xx response into the matching exception, using Gemini's
// structured status and details rather than the HTTP status alone.
[[noreturn]] void throwHttpError(long httpStatus, const std::string& responseBody,
                                 const std::string& model) {
    const auto details = ResponseParser().parseProviderError(responseBody);
    const std::string diagnostic = describeProviderError(httpStatus, details);
    const std::string status = details ? details->status : "";
    const std::string reason = details ? details->reason : "";

    // Gemini reports an invalid key as HTTP 400 INVALID_ARGUMENT with the
    // API_KEY_INVALID reason, so the reason is checked before the status.
    if (httpStatus == 401 || httpStatus == 403 || status == "UNAUTHENTICATED" ||
        status == "PERMISSION_DENIED" || reason == "API_KEY_INVALID") {
        throw AuthenticationError(diagnostic);
    }
    if (httpStatus == 429 || status == "RESOURCE_EXHAUSTED") {
        const bool quotaExhausted = details && hasDailyQuotaViolation(*details);
        throw RateLimitError(quotaExhausted, details ? details->retryDelay : std::nullopt,
                             diagnostic);
    }
    if (httpStatus == 408 || httpStatus == 504 || status == "DEADLINE_EXCEEDED") {
        throw TimeoutError(diagnostic);
    }
    if (httpStatus >= 500) {
        throw ProviderUnavailableError(diagnostic);
    }
    if (httpStatus == 404) {
        throw ProviderError(
            "Gemini did not find the model '" + model + "'. Check the configured model name.",
            diagnostic);
    }
    throw ProviderError(
        "Gemini rejected the request (HTTP " + std::to_string(httpStatus) +
            "). Shorten or rephrase the instruction and try again.",
        diagnostic);
}

ProviderResponseError::Kind responseErrorKind(ResponseParseError error) {
    switch (error) {
    case ResponseParseError::BlockedResponse:
        return ProviderResponseError::Kind::Blocked;
    case ResponseParseError::IncompleteResponse:
        return ProviderResponseError::Kind::Incomplete;
    default:
        return ProviderResponseError::Kind::Invalid;
    }
}

} // namespace

LLMClient::LLMClient()
    : LLMClient(
          performHttpRequest,
          "gemini-3.5-flash"
      ) {
    // Reads the .env files once per process; see EnvLoader.hpp for the
    // lookup order. Existing environment variables are preserved.
    loadConfigurationFilesOnce();
}

LLMClient::LLMClient(
    HttpTransport transport,
    std::string model
)
    : transport_(std::move(transport)),
      model_(std::move(model)) {
}

bool LLMClient::isValidPrompt(
    const std::string& prompt
) const {
    for (unsigned char ch : prompt) {
        if (!std::isspace(ch)) {
            return true;
        }
    }
    return false;
}

LLMClient::GenerationResult LLMClient::generate(
    const std::string& prompt,
    GenerationOptions options
) const {
    if (!isValidPrompt(prompt) || !transport_ || model_.empty()) {
        throw std::invalid_argument("Invalid prompt, transport, or model.");
    }

    const std::string apiKey = requireEnvironmentVariable("GEMINI_API_KEY");

    json requestBody = {
        {"contents", json::array({
            {
                {"parts", json::array({
                    {{"text", prompt}}
                })}
            }
        })}
    };
    if (options.includeThoughts) {
        requestBody["generationConfig"] = {
            {"thinkingConfig", {{"includeThoughts", true}}}
        };
    }

    std::string responseBody;
    long httpStatus = 0;

    if (!transport_(
            buildGeminiUrl(model_),
            apiKey,
            requestBody.dump(),
            responseBody,
            httpStatus)) {
        throw NetworkError("transport reported a failure without a response");
    }

    if (responseBody.size() > kMaximumProviderResponseBytes) {
        throw ProviderResponseError(
            ProviderResponseError::Kind::Invalid,
            "provider response exceeded the 2 MiB limit"
        );
    }

    if (httpStatus < 200 || httpStatus >= 300) {
        throwHttpError(httpStatus, responseBody, model_);
    }

    const ParsedResponse parsedResponse =
        ResponseParser().parseResponse(responseBody);

    if (!parsedResponse.succeeded()) {
        throw ProviderResponseError(
            responseErrorKind(parsedResponse.error),
            safeResponseDiagnostic(parsedResponse)
        );
    }
    if (options.requireExplicitCompletion && !parsedResponse.isComplete()) {
        throw ProviderResponseError(
            ProviderResponseError::Kind::Incomplete,
            "Gemini did not confirm that the response is complete."
        );
    }

    GenerationResult result;
    result.text = parsedResponse.generatedText;
    if (options.includeThoughts) {
        result.thoughts = parsedResponse.thoughts;
    }
    return result;
}

bool LLMClient::generateContent(
    const std::string& prompt,
    std::string& response
) const {
    return generateContentReportingErrors(prompt, response, false, nullptr);
}

bool LLMClient::generateCompleteContent(
    const std::string& prompt,
    std::string& response
) const {
    return generateContentReportingErrors(prompt, response, true, nullptr);
}

bool LLMClient::generateContent(
    const std::string& prompt,
    std::string& response,
    std::string& thoughts
) const {
    return generateContentReportingErrors(prompt, response, false, &thoughts);
}

bool LLMClient::generateCompleteContent(
    const std::string& prompt,
    std::string& response,
    std::string& thoughts
) const {
    return generateContentReportingErrors(prompt, response, true, &thoughts);
}

bool LLMClient::generateContentReportingErrors(
    const std::string& prompt,
    std::string& response,
    bool requireExplicitCompletion,
    std::string* thoughts
) const {
    response.clear();
    if (thoughts != nullptr) {
        thoughts->clear();
    }

    try {
        GenerationResult result = generate(
            prompt,
            GenerationOptions{requireExplicitCompletion, thoughts != nullptr}
        );
        response = std::move(result.text);
        if (thoughts != nullptr) {
            *thoughts = std::move(result.thoughts);
        }
        return true;
    } catch (const CodeAgentError& error) {
        logError(std::cerr, error);
    } catch (const std::invalid_argument& error) {
        std::cerr << "Error: " << error.what() << '\n';
    }
    return false;
}
