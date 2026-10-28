#ifndef GUI_HTTP_SERVER_HPP
#define GUI_HTTP_SERVER_HPP

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "NativeDialog.hpp"

class GuiHttpServer {
public:
    // thoughts may be left empty; when set it is shown in the chat as the
    // model's reasoning and is never treated as code.
    //
    // Both handlers may throw CodeAgentError (see CodeAgentError.hpp): the
    // server answers with its httpStatus(), code(), and what(), so the user
    // learns why the request failed. Any other exception is logged and
    // reported as a generic failure without its message.
    using GenerationHandler = std::function<bool(
        const std::string& instruction,
        const std::string& language,
        std::string& generatedCode,
        std::string& thoughts
    )>;
    // Return true only when refactoredCode contains a valid, complete result.
    using RefactoringHandler = std::function<bool(
        const std::string& instruction,
        const std::string& language,
        const std::string& sourceCode,
        std::string& refactoredCode,
        std::string& thoughts
    )>;

    // A port of 0 asks the operating system to select an available port.
    explicit GuiHttpServer(int port = 8080);
    GuiHttpServer(
        int port,
        std::filesystem::path launchWorkingDirectory
    );
    GuiHttpServer(
        int port,
        std::filesystem::path launchWorkingDirectory,
        GenerationHandler generationHandler
    );
    GuiHttpServer(
        int port,
        std::filesystem::path launchWorkingDirectory,
        GenerationHandler generationHandler,
        RefactoringHandler refactoringHandler
    );
    GuiHttpServer(
        int port,
        std::filesystem::path launchWorkingDirectory,
        GenerationHandler generationHandler,
        RefactoringHandler refactoringHandler,
        std::shared_ptr<NativeDialogProvider> dialogProvider
    );
    ~GuiHttpServer();

    GuiHttpServer(const GuiHttpServer&) = delete;
    GuiHttpServer& operator=(const GuiHttpServer&) = delete;

    // A server instance is single-use: start() can be attempted once. A failed
    // start or a stopped server cannot be restarted. Listening is on 127.0.0.1.
    // Returns false for an invalid/occupied port or a startup failure.
    bool start();

    // Serves the browser frontend from directory at "/" alongside the API.
    // Call before start(). Returns false when the directory does not exist or
    // the server has already been started.
    bool serveFrontend(const std::filesystem::path& directory);

    // Every /api request except POST /api/session must carry the session
    // token in this header. The token is random per server instance and is
    // only handed out by POST /api/session.
    static constexpr const char* kSessionTokenHeader = "X-CodeAgent-Token";

    // Returns a new single-use code that a page can exchange for the session
    // token at POST /api/session within 5 minutes. Pass it to the browser in
    // the URL fragment (#bootstrap=<code>) so it is never sent in a request
    // line. Thread-safe; at most 8 unused codes are kept.
    std::string issueBootstrapCode();

    // Also accepts requests from origin, which must be exactly
    // http://127.0.0.1:<port> or http://localhost:<port>, e.g. a development
    // server. Only the server's own origin is allowed otherwise. Call before
    // start(). Returns false for any other value.
    bool allowDevelopmentOrigin(const std::string& origin);

    // Replaces the clock used for bootstrap-code and proposal expiry, so
    // tests can advance time. Call before start().
    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    bool setClock(Clock clock);

    // Stops the listener and joins its worker thread. Safe to call repeatedly.
    void stop();

    bool isRunning() const;
    int port() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

#endif
