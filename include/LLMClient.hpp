
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

    // Uses the default libcurl-based HTTP transport.
    LLMClient();

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

private:
    bool isValidPrompt(const std::string& prompt) const;

    HttpTransport transport_;
    std::string model_;
};

#endif // LLM_CLIENT_HPP
