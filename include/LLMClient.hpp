
#ifndef LLM_CLIENT_HPP
#define LLM_CLIENT_HPP

#include <functional>
#include <string>

class LLMClient {
public:
    // Represents an HTTP transport that can be replaced in unit tests.
    using HttpTransport = std::function<bool(
        const std::string& url,
        const std::string& apiKey,
        const std::string& requestBody,
        std::string& responseBody,
        long& httpStatus
    )>;

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
    bool generateContentImpl(
        const std::string& prompt,
        std::string& response,
        bool requireExplicitCompletion,
        std::string* thoughts
    ) const;

    HttpTransport transport_;
    std::string model_;
};

#endif // LLM_CLIENT_HPP
