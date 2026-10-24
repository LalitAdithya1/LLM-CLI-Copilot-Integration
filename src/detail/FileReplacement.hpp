#ifndef CODEAGENT_DETAIL_FILE_REPLACEMENT_HPP
#define CODEAGENT_DETAIL_FILE_REPLACEMENT_HPP

#include <filesystem>
#include <functional>
#include <system_error>

namespace codeagent {
namespace detail {

using ReplacementOperation = std::function<void(
    const std::filesystem::path&,
    const std::filesystem::path&,
    std::error_code&
)>;

inline bool replaceFile(
    const std::filesystem::path& temporaryPath,
    const std::filesystem::path& targetPath,
    std::error_code& error,
    ReplacementOperation operation = {}
) {
    error.clear();

    if (operation) {
        operation(temporaryPath, targetPath, error);
    } else {
        std::filesystem::rename(
            temporaryPath,
            targetPath,
            error
        );
    }

    if (error) {
        std::error_code cleanupError;
        std::filesystem::remove(temporaryPath, cleanupError);
        return false;
    }

    return true;
}

} // namespace detail
} // namespace codeagent

#endif // CODEAGENT_DETAIL_FILE_REPLACEMENT_HPP
