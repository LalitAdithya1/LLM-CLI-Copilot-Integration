
#include "LLMClient.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <utility>

#include "EnvLoader.hpp"
#include "ResponseParser.hpp"

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

size_t writeResponse(
    char* data,
    size_t size,
    size_t count,
    void* userData
) {
    const size_t totalBytes = size * count;

    auto* response = static_cast<std::string*>(userData);
    response->append(data, totalBytes);

    return totalBytes;
}

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
        std::cerr << "Error: Failed to initialize libcurl.\n";
        return false;
    }

    CURL* curl = curl_easy_init();

    if (curl == nullptr) {
        std::cerr << "Error: Failed to create HTTP request.\n";
        return false;
    }

    struct curl_slist* headers = nullptr;

    headers = curl_slist_append(
        headers,
        "Content-Type: application/json"
    );

    if (headers == nullptr) {
        std::cerr << "Error: Failed to create HTTP headers.\n";
        curl_easy_cleanup(curl);
        return false;
    }

    const std::string apiKeyHeader = "x-goog-api-key: " + apiKey;

    struct curl_slist* updatedHeaders = curl_slist_append(
        headers,
        apiKeyHeader.c_str()
    );

    if (updatedHeaders == nullptr) {
        std::cerr << "Error: Failed to add API key header.\n";
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return false;
    }

    headers = updatedHeaders;

    responseBody.clear();
    httpStatus = 0;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(
        curl,
        CURLOPT_POSTFIELDS,
        requestBody.c_str()
    );
    curl_easy_setopt(
        curl,
        CURLOPT_POSTFIELDSIZE,
        static_cast<long>(requestBody.size())
    );
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeResponse);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);

    // Bound request duration and verify TLS certificates.
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    const CURLcode result = curl_easy_perform(curl);

    if (result == CURLE_OK) {
        curl_easy_getinfo(
            curl,
            CURLINFO_RESPONSE_CODE,
            &httpStatus
        );
    } else {
        std::cerr
            << "Network/HTTP transport error: "
            << curl_easy_strerror(result)
            << '\n';
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return result == CURLE_OK;
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

} // namespace

LLMClient::LLMClient()
    : LLMClient(
          performHttpRequest,
          "gemini-3.5-flash"
      ) {
    // Load .env for the real Gemini client.
    // Existing environment variables are preserved.
    if (!loadEnvFile(".env")) {
        std::cerr
            << "Warning: Could not load .env from the current "
            << "working directory. Checking the process environment "
            << "for GEMINI_API_KEY instead.\n";
    }
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
    return !isWhitespaceOnly(prompt);
}

bool LLMClient::generateContent(
    const std::string& prompt,
    std::string& response
) const {
    return generateContentImpl(prompt, response, false);
}

bool LLMClient::generateCompleteContent(
    const std::string& prompt,
    std::string& response
) const {
    return generateContentImpl(prompt, response, true);
}

bool LLMClient::generateContentImpl(
    const std::string& prompt,
    std::string& response,
    bool requireExplicitCompletion
) const {
    response.clear();

    if (!isValidPrompt(prompt) || !transport_ || model_.empty()) {
        std::cerr
            << "Error: Invalid prompt, transport, or model.\n";
        return false;
    }

    const char* apiKeyEnvironment =
        std::getenv("GEMINI_API_KEY");

    if (apiKeyEnvironment == nullptr ||
        isWhitespaceOnly(apiKeyEnvironment)) {
        std::cerr
            << "Error: GEMINI_API_KEY is missing or empty. "
            << "Check your .env file and working directory.\n";
        return false;
    }

    const std::string apiKey(apiKeyEnvironment);

    const json requestBody = {
        {"contents", json::array({
            {
                {"parts", json::array({
                    {{"text", prompt}}
                })}
            }
        })}
    };

    std::string responseBody;
    long httpStatus = 0;

    const bool requestSucceeded = transport_(
        buildGeminiUrl(model_),
        apiKey,
        requestBody.dump(),
        responseBody,
        httpStatus
    );

    if (!requestSucceeded) {
        std::cerr
            << "Gemini request failed at the network or "
            << "transport layer.\n";
        return false;
    }

    ResponseParser parser;
    const ParsedResponse parsedResponse =
        parser.parseResponse(responseBody);

    if (httpStatus < 200 || httpStatus >= 300) {
        std::cerr
            << "Gemini HTTP status: "
            << httpStatus << '\n';
        std::cerr << safeResponseDiagnostic(parsedResponse) << '\n';
        return false;
    }

    if (!parsedResponse.succeeded() ||
        (requireExplicitCompletion && !parsedResponse.isComplete())) {
        std::cerr << "Error: ";
        if (parsedResponse.succeeded() && requireExplicitCompletion) {
            std::cerr << "Gemini did not confirm that the response is complete.\n";
        } else {
            std::cerr << safeResponseDiagnostic(parsedResponse) << '\n';
        }
        return false;
    }

    response = parsedResponse.generatedText;
    return true;
}
