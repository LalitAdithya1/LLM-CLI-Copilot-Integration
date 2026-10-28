
#ifndef LLM_CLIENT_HPP
#define LLM_CLIENT_HPP

#include <functional>
#include <string>

class LLMClient {
public:
    // Represents an HTTP transport that can be replaced in unit tests.
    // Returns true when a response arrived, with any HTTP status. It may
    // throw NetworkError or TimeoutError to say why no response arrived;
    // returning false is treated as a NetworkError.
    using HttpTransport = std::function<bool(
        const std::string& url,
        const std::string& apiKey,
        const std::string& requestBody,
        std::string& responseBody,
        long& httpStatus
    )>;

    struct GenerationOptions {
        // Refactoring requires an explicitly complete provider response.
        bool requireExplicitCompletion = false;
        // Also asks Gemini for a summary of its thinking.
        bool includeThoughts = false;
    };

    struct GenerationResult {
        std::string text;
        // Empty unless thoughts were requested and the model returned some.
        std::string thoughts;
    };

    // Uses the default libcurl-based HTTP transport. Loads .env on first use.
    LLMClient();

    // Loads .env from the working directory into the environment once per
    // process; later calls do nothing. Call before starting threads that read
    // the environment, because setting variables is not thread-safe.
    static void loadDotEnvOnce();

    // Allows unit tests to inject a simulated HTTP transport.
    explicit LLMClient(
        HttpTransport transport,
        std::string model = "gemini-3.5-flash"
    );

    // Sends a prompt to Gemini and returns the generated text.
    // Throws:
    //   std::invalid_argument  for an empty prompt, transport, or model;
    //   ConfigurationError     when GEMINI_API_KEY is not configured;
    //   ProviderError          or a subclass (AuthenticationError,
    //                          RateLimitError, ProviderUnavailableError,
    //                          NetworkError, TimeoutError,
    //                          ProviderResponseError) when Gemini fails.
    // Every message is safe to show to the user.
    GenerationResult generate(
        const std::string& prompt,
        GenerationOptions options
    ) const;

    // The functions below catch the errors above, log them to standard
    // error, and report them as false. They are kept for callers that only
    // need success or failure.

    // Sends a prompt to Gemini and stores generated text in response.
    // Returns true on success and false on failure.
    bool generateContent(
        const std::string& prompt,
        std::string& response
    ) const;

    // Refactoring requires an explicitly complete provider response.
    bool generateCompleteContent(
        const std::string& prompt,
        std::string& response
    ) const;

    // Same as above, but also asks Gemini for a summary of its thinking and
    // stores it in thoughts (empty when the model returns none).
    bool generateContent(
        const std::string& prompt,
        std::string& response,
        std::string& thoughts
    ) const;
    bool generateCompleteContent(
        const std::string& prompt,
        std::string& response,
        std::string& thoughts
    ) const;

private:
    bool isValidPrompt(const std::string& prompt) const;
    // thoughts is null when thought summaries are not requested.
    bool generateContentReportingErrors(
        const std::string& prompt,
        std::string& response,
        bool requireExplicitCompletion,
        std::string* thoughts
    ) const;

    HttpTransport transport_;
    std::string model_;
};

#endif // LLM_CLIENT_HPP
