#ifndef NATIVE_DIALOG_HPP
#define NATIVE_DIALOG_HPP

#include <filesystem>
#include <memory>
#include <string>

enum class NativeDialogKind {
    SourceFile,
    OutputDirectory
};

enum class NativeDialogStatus {
    Selected,
    Cancelled,
    Unavailable,
    TimedOut,
    Failed
};

struct NativeDialogResult {
    NativeDialogStatus status = NativeDialogStatus::Failed;
    std::filesystem::path selectedPath;
};

class NativeDialogProvider {
public:
    virtual ~NativeDialogProvider() = default;
    virtual NativeDialogResult choose(NativeDialogKind kind) = 0;
};

// Creates the platform provider. Ubuntu/Linux uses zenity when a graphical
// display is available; under WSL without zenity it uses a Windows dialog via
// powershell.exe. Unsupported or headless systems report unavailable.
std::shared_ptr<NativeDialogProvider> createNativeDialogProvider();

// Maps C:\dir, C:/dir and \\wsl.localhost\<distribution>\dir (or \\wsl$\...)
// to /mnt/c/dir and /dir. Any other path is returned unchanged.
std::string translateWindowsPathForWsl(const std::string& path);

// Applies translateWindowsPathForWsl under WSL; elsewhere returns path as is.
std::filesystem::path toHostPath(const std::string& path);

// Asks the desktop to open url in the default browser without waiting for it.
// WSL uses wslview or rundll32.exe url.dll,FileProtocolHandler, Linux uses
// xdg-open, macOS uses open.
// Returns false when no launcher could be started; the caller keeps running.
bool launchDefaultBrowser(const std::string& url);

#endif
