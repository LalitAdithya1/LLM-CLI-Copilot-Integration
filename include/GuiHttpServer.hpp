#ifndef GUI_HTTP_SERVER_HPP
#define GUI_HTTP_SERVER_HPP

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "NativeDialog.hpp"

class GuiHttpServer {
public:
    // thoughts may be left empty; when set it is shown in the chat as the
    // model's reasoning and is never treated as code.
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

    // Stops the listener and joins its worker thread. Safe to call repeatedly.
    void stop();

    bool isRunning() const;
    int port() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

#endif
