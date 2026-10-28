#include "NativeDialog.hpp"

#include <algorithm>
#include <cctype>
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

// Long enough to browse to a file; the request waits while the dialog is open.
constexpr auto kDialogTimeout = std::chrono::seconds(120);
constexpr std::size_t kMaximumPathBytes = 64 * 1024;

bool graphicalDisplayAvailable() {
    const char* display = std::getenv("DISPLAY");
    const char* waylandDisplay = std::getenv("WAYLAND_DISPLAY");
    return (display != nullptr && *display != '\0') ||
           (waylandDisplay != nullptr && *waylandDisplay != '\0');
}

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

// Runs a dialog program and reads the selected path from its stdout. Exit
// status 0 means selected, 1 cancelled, 127 (or a missing program) unavailable.
NativeDialogResult runDialogProcess(const std::vector<std::string>& argumentList) {
    {
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
        // Keep Windows interop programs from reading the server's terminal.
        if (actionResult == 0) {
            actionResult = ::posix_spawn_file_actions_addopen(
                &actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0
            );
        }

        std::vector<std::string> argumentStorage(argumentList);
        std::vector<char*> arguments;
        for (auto& argument : argumentStorage) {
            arguments.push_back(argument.data());
        }
        arguments.push_back(nullptr);
        pid_t child = -1;
        int spawnResult = actionResult;
        if (spawnResult == 0) {
            spawnResult = ::posix_spawnp(
                &child, arguments[0], &actions, nullptr, arguments.data(), environ
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
}

// A Windows Forms dialog started through WSL interop. The script uses only
// single quotes so interop's argument quoting cannot alter it; failures exit 2.
std::vector<std::string> windowsDialogArguments(NativeDialogKind kind) {
    const std::string dialog = kind == NativeDialogKind::SourceFile
        ? "$d = New-Object Windows.Forms.OpenFileDialog; "
          "$d.Title = 'CodeAgent - choose a source file'; "
          "$d.Filter = 'C/C++ source|*.c;*.cc;*.cpp;*.cxx;*.h;*.hh;*.hpp;*.hxx'; "
          "$ok = $d.ShowDialog($o) -eq 'OK'; $p = $d.FileName; "
        : "$d = New-Object Windows.Forms.FolderBrowserDialog; "
          "$d.Description = 'CodeAgent - choose the output directory'; "
          "$ok = $d.ShowDialog($o) -eq 'OK'; $p = $d.SelectedPath; ";
    return {
        "powershell.exe", "-NoProfile", "-NonInteractive", "-STA", "-Command",
        "try { Add-Type -AssemblyName System.Windows.Forms; "
        "[Console]::OutputEncoding = [Text.Encoding]::UTF8; "
        "$o = New-Object Windows.Forms.Form -Property @{TopMost = $true; "
        "ShowInTaskbar = $false; Opacity = 0; StartPosition = 'CenterScreen'}; "
        "$o.Show(); $o.Activate(); " + dialog +
        "$o.Close(); if ($ok) { [Console]::Out.Write($p); exit 0 } else { exit 1 } "
        "} catch { exit 2 }"
    };
}

class SystemNativeDialogProvider final : public NativeDialogProvider {
public:
    NativeDialogResult choose(NativeDialogKind kind) override {
        if (graphicalDisplayAvailable()) {
            const NativeDialogResult result = runDialogProcess(
                kind == NativeDialogKind::SourceFile
                    ? std::vector<std::string>{"zenity", "--file-selection",
                          "--file-filter=Source files | *.c *.cc *.cpp *.cxx *.h *.hh *.hpp *.hxx"}
                    : std::vector<std::string>{"zenity", "--file-selection", "--directory"}
            );
            if (result.status != NativeDialogStatus::Unavailable) {
                return result;
            }
        }
        // WSL without zenity: use the Windows dialog and map its path back.
        if (runningUnderWsl()) {
            NativeDialogResult result = runDialogProcess(windowsDialogArguments(kind));
            if (result.status == NativeDialogStatus::Selected) {
                result.selectedPath = toHostPath(result.selectedPath.string());
            }
            return result;
        }
        return {NativeDialogStatus::Unavailable, {}};
    }
};

} // namespace

#endif

std::shared_ptr<NativeDialogProvider> createNativeDialogProvider() {
    return std::make_shared<SystemNativeDialogProvider>();
}

std::string translateWindowsPathForWsl(const std::string& path) {
    const auto isSeparator = [](char c) { return c == '\\' || c == '/'; };
    std::string rest;
    std::string prefix;
    if (path.size() >= 3 && std::isalpha(static_cast<unsigned char>(path[0])) &&
        path[1] == ':' && isSeparator(path[2])) {
        prefix = std::string("/mnt/") +
                 static_cast<char>(std::tolower(static_cast<unsigned char>(path[0])));
        rest = path.substr(2);
    } else {
        // \\wsl.localhost\<distribution>\... and \\wsl$\<distribution>\...
        // name this distribution's own root filesystem.
        for (const std::string host : {"wsl.localhost", "wsl$"}) {
            if (path.size() > host.size() + 2 && isSeparator(path[0]) &&
                isSeparator(path[1])) {
                std::string candidate = path.substr(2, host.size());
                std::transform(candidate.begin(), candidate.end(), candidate.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (candidate == host && isSeparator(path[host.size() + 2])) {
                    const auto distributionEnd =
                        path.find_first_of("\\/", host.size() + 3);
                    rest = distributionEnd == std::string::npos
                        ? "/" : path.substr(distributionEnd);
                    break;
                }
            }
        }
        if (rest.empty()) {
            return path;
        }
    }
    std::replace(rest.begin(), rest.end(), '\\', '/');
    return prefix + rest;
}

#ifdef _WIN32

std::filesystem::path toHostPath(const std::string& path) {
    return path;
}

bool launchDefaultBrowser(const std::string&) {
    return false;
}

#else

std::filesystem::path toHostPath(const std::string& path) {
    return runningUnderWsl() ? translateWindowsPathForWsl(path) : path;
}

namespace {

// Starts the launcher command (program first) with url appended as the last
// argument, detached from the terminal's process group so Ctrl+C on the
// server does not reach the browser. Arguments are passed directly to exec;
// no shell is involved.
bool spawnBrowserLauncher(const std::vector<std::string>& command, const std::string& url) {
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

    std::vector<std::string> argumentStorage(command);
    argumentStorage.push_back(url);
    std::vector<char*> arguments;
    for (auto& argument : argumentStorage) arguments.push_back(argument.data());
    arguments.push_back(nullptr);
    pid_t child = -1;
    if (result == 0) {
        result = ::posix_spawnp(&child, arguments[0], &actions, &attributes, arguments.data(), environ);
    }
    ::posix_spawnattr_destroy(&attributes);
    ::posix_spawn_file_actions_destroy(&actions);
    if (result != 0) {
        return false;
    }

    // Reap the launcher without blocking the server; launchers such as
    // rundll32.exe report unreliable exit codes, so the status is ignored.
    std::thread([child] {
        int status = 0;
        while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    }).detach();
    return true;
}

} // namespace

bool launchDefaultBrowser(const std::string& url) {
    using Command = std::vector<std::string>;
#ifdef __APPLE__
    const std::vector<Command> launchers = {{"open"}};
#else
    // Under WSL the Windows browser is the default; wslview is optional.
    // explorer.exe is not used: it does not recognise a URL with a fragment
    // (the #bootstrap= startup code) and opens File Explorer instead.
    // url.dll's FileProtocolHandler passes the whole URL to the default
    // browser.
    const bool wsl = runningUnderWsl();
    if (!wsl && !graphicalDisplayAvailable()) {
        return false;
    }
    const std::vector<Command> launchers = wsl
        ? std::vector<Command>{{"wslview"}, {"rundll32.exe", "url.dll,FileProtocolHandler"}}
        : std::vector<Command>{{"xdg-open"}};
#endif
    for (const auto& launcher : launchers) {
        if (spawnBrowserLauncher(launcher, url)) {
            return true;
        }
    }
    return false;
}

#endif
