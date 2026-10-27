#ifndef NATIVE_DIALOG_HPP
#define NATIVE_DIALOG_HPP

#include <filesystem>
#include <memory>

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
// display is available; unsupported or headless systems report unavailable.
std::shared_ptr<NativeDialogProvider> createNativeDialogProvider();

#endif
