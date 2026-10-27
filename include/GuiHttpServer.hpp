#ifndef GUI_HTTP_SERVER_HPP
#define GUI_HTTP_SERVER_HPP

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

class GuiHttpServer {
public:
    using GenerationHandler = std::function<bool(
        const std::string& instruction,
        const std::string& language,
        std::string& generatedCode
    )>;
    // Return true only when refactoredCode contains a valid, complete result.
    using RefactoringHandler = std::function<bool(
        const std::string& instruction,
        const std::string& language,
        const std::string& sourceCode,
        std::string& refactoredCode
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
    ~GuiHttpServer();

    GuiHttpServer(const GuiHttpServer&) = delete;
    GuiHttpServer& operator=(const GuiHttpServer&) = delete;

    // A server instance is single-use: start() can be attempted once. A failed
    // start or a stopped server cannot be restarted. Listening is on 127.0.0.1.
    // Returns false for an invalid/occupied port or a startup failure.
    bool start();

    // Stops the listener and joins its worker thread. Safe to call repeatedly.
    void stop();

    bool isRunning() const;
    int port() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

#endif
