#include "NativeDialog.hpp"

#include <cstdlib>
#include <string>

#ifdef _WIN32

namespace {
class SystemNativeDialogProvider final : public NativeDialogProvider {
public:
    NativeDialogResult choose(NativeDialogKind) override {
        return {NativeDialogStatus::Unavailable, {}};
    }
};
} // namespace

#else

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <fstream>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {

constexpr auto kDialogTimeout = std::chrono::seconds(30);
constexpr std::size_t kMaximumPathBytes = 64 * 1024;

bool graphicalDisplayAvailable() {
    const char* display = std::getenv("DISPLAY");
    const char* waylandDisplay = std::getenv("WAYLAND_DISPLAY");
    return (display != nullptr && *display != '\0') ||
           (waylandDisplay != nullptr && *waylandDisplay != '\0');
}

class SystemNativeDialogProvider final : public NativeDialogProvider {
public:
    NativeDialogResult choose(NativeDialogKind kind) override {
        if (!graphicalDisplayAvailable()) {
            return {NativeDialogStatus::Unavailable, {}};
        }

        int outputPipe[2] = {-1, -1};
        if (::pipe(outputPipe) != 0) {
            return {NativeDialogStatus::Failed, {}};
        }
        for (const int descriptor : outputPipe) {
            const int descriptorFlags = ::fcntl(descriptor, F_GETFD, 0);
            if (descriptorFlags < 0 ||
                ::fcntl(descriptor, F_SETFD, descriptorFlags | FD_CLOEXEC) < 0) {
                ::close(outputPipe[0]);
                ::close(outputPipe[1]);
                return {NativeDialogStatus::Failed, {}};
            }
        }

        posix_spawn_file_actions_t actions;
        if (::posix_spawn_file_actions_init(&actions) != 0) {
            ::close(outputPipe[0]);
            ::close(outputPipe[1]);
            return {NativeDialogStatus::Failed, {}};
        }
        int actionResult = ::posix_spawn_file_actions_adddup2(
            &actions, outputPipe[1], STDOUT_FILENO
        );
        if (actionResult == 0) {
            actionResult = ::posix_spawn_file_actions_addclose(&actions, outputPipe[0]);
        }
        if (actionResult == 0) {
            actionResult = ::posix_spawn_file_actions_addclose(&actions, outputPipe[1]);
        }
        if (actionResult == 0) {
            actionResult = ::posix_spawn_file_actions_addopen(
                &actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0
            );
        }

        char fileArguments[] = "--file-selection";
        char directoryArgument[] = "--directory";
        char filterArgument[] = "--file-filter=Source files | *.c *.cc *.cpp *.cxx *.h *.hh *.hpp *.hxx";
        char program[] = "zenity";
        char* fileArgumentsVector[] = {
            program, fileArguments, filterArgument, nullptr
        };
        char* directoryArgumentsVector[] = {
            program, fileArguments, directoryArgument, nullptr
        };
        pid_t child = -1;
        int spawnResult = actionResult;
        if (spawnResult == 0) {
            char** arguments = kind == NativeDialogKind::SourceFile
                ? fileArgumentsVector
                : directoryArgumentsVector;
            spawnResult = ::posix_spawnp(
                &child, "zenity", &actions, nullptr, arguments, environ
            );
        }
        ::posix_spawn_file_actions_destroy(&actions);
        if (spawnResult != 0) {
            ::close(outputPipe[0]);
            ::close(outputPipe[1]);
            return {
                spawnResult == ENOENT
                    ? NativeDialogStatus::Unavailable
                    : NativeDialogStatus::Failed,
                {}
            };
        }

        ::close(outputPipe[1]);
        const int flags = ::fcntl(outputPipe[0], F_GETFL, 0);
        if (flags >= 0) {
            ::fcntl(outputPipe[0], F_SETFL, flags | O_NONBLOCK);
        }

        std::string output;
        bool pipeClosed = false;
        bool childExited = false;
        int childStatus = 0;
        const auto deadline = std::chrono::steady_clock::now() + kDialogTimeout;
        NativeDialogStatus resultStatus = NativeDialogStatus::Failed;

        while (!pipeClosed || !childExited) {
            if (std::chrono::steady_clock::now() >= deadline) {
                ::kill(child, SIGKILL);
                while (::waitpid(child, &childStatus, 0) < 0 && errno == EINTR) {}
                ::close(outputPipe[0]);
                return {NativeDialogStatus::TimedOut, {}};
            }

            pollfd descriptor{outputPipe[0], POLLIN | POLLHUP, 0};
            (void)::poll(&descriptor, 1, 100);
            if (!pipeClosed && (descriptor.revents & (POLLIN | POLLHUP))) {
                char buffer[4096];
                for (;;) {
                    const ssize_t count = ::read(outputPipe[0], buffer, sizeof(buffer));
                    if (count > 0) {
                        if (output.size() + static_cast<std::size_t>(count) >
                            kMaximumPathBytes) {
                            ::kill(child, SIGKILL);
                            while (::waitpid(child, &childStatus, 0) < 0 && errno == EINTR) {}
                            ::close(outputPipe[0]);
                            return {NativeDialogStatus::Failed, {}};
                        }
                        output.append(buffer, static_cast<std::size_t>(count));
                    } else if (count == 0) {
                        pipeClosed = true;
                        break;
                    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    } else if (errno != EINTR) {
                        pipeClosed = true;
                        break;
                    }
                }
            }

            if (!childExited) {
                const pid_t waitResult = ::waitpid(child, &childStatus, WNOHANG);
                if (waitResult == child) {
                    childExited = true;
                } else if (waitResult < 0 && errno != EINTR) {
                    childExited = true;
                }
            }
        }
        ::close(outputPipe[0]);

        if (WIFEXITED(childStatus)) {
            const int exitCode = WEXITSTATUS(childStatus);
            if (exitCode == 127) {
                resultStatus = NativeDialogStatus::Unavailable;
            } else if (exitCode == 1) {
                resultStatus = NativeDialogStatus::Cancelled;
            } else if (exitCode == 0) {
                resultStatus = NativeDialogStatus::Selected;
            }
        }

        while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
            output.pop_back();
        }
        if (resultStatus != NativeDialogStatus::Selected || output.empty()) {
            if (resultStatus == NativeDialogStatus::Selected) {
                resultStatus = NativeDialogStatus::Failed;
            }
            return {resultStatus, {}};
        }
        return {NativeDialogStatus::Selected, std::filesystem::path(output)};
    }
};

} // namespace

#endif

std::shared_ptr<NativeDialogProvider> createNativeDialogProvider() {
    return std::make_shared<SystemNativeDialogProvider>();
}

#ifdef _WIN32

bool launchDefaultBrowser(const std::string&) {
    return false;
}

#else

namespace {

bool runningUnderWsl() {
    const char* distribution = std::getenv("WSL_DISTRO_NAME");
    if (distribution != nullptr && *distribution != '\0') {
        return true;
    }
    std::ifstream release("/proc/sys/kernel/osrelease");
    std::string text;
    std::getline(release, text);
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text.find("microsoft") != std::string::npos;
}

// Starts program with url as its only argument, detached from the terminal's
// process group so Ctrl+C on the server does not reach the browser. Arguments
// are passed directly to exec; no shell is involved.
bool spawnBrowserLauncher(const char* program, const std::string& url) {
    posix_spawn_file_actions_t actions;
    if (::posix_spawn_file_actions_init(&actions) != 0) {
        return false;
    }
    posix_spawnattr_t attributes;
    if (::posix_spawnattr_init(&attributes) != 0) {
        ::posix_spawn_file_actions_destroy(&actions);
        return false;
    }
    int result = ::posix_spawn_file_actions_addopen(
        &actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0
    );
    if (result == 0) {
        result = ::posix_spawn_file_actions_addopen(
            &actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0
        );
    }
    if (result == 0) {
        result = ::posix_spawn_file_actions_addopen(
            &actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0
        );
    }
    if (result == 0) {
        result = ::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    }
    if (result == 0) {
        result = ::posix_spawnattr_setpgroup(&attributes, 0);
    }

    std::string programArgument(program);
    std::string urlArgument(url);
    char* arguments[] = {programArgument.data(), urlArgument.data(), nullptr};
    pid_t child = -1;
    if (result == 0) {
        result = ::posix_spawnp(&child, program, &actions, &attributes, arguments, environ);
    }
    ::posix_spawnattr_destroy(&attributes);
    ::posix_spawn_file_actions_destroy(&actions);
    if (result != 0) {
        return false;
    }

    // Reap the launcher without blocking the server; launchers such as
    // explorer.exe report unreliable exit codes, so the status is ignored.
    std::thread([child] {
        int status = 0;
        while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    }).detach();
    return true;
}

} // namespace

bool launchDefaultBrowser(const std::string& url) {
#ifdef __APPLE__
    const std::vector<const char*> launchers = {"open"};
#else
    // Under WSL the Windows browser is the default; wslview is optional.
    const bool wsl = runningUnderWsl();
    if (!wsl && !graphicalDisplayAvailable()) {
        return false;
    }
    const std::vector<const char*> launchers = wsl
        ? std::vector<const char*>{"wslview", "explorer.exe"}
        : std::vector<const char*>{"xdg-open"};
#endif
    for (const char* launcher : launchers) {
        if (spawnBrowserLauncher(launcher, url)) {
            return true;
        }
    }
    return false;
}

#endif
