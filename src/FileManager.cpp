#include "FileManager.hpp"

#include "CodeAgentError.hpp"
#include "detail/FileReplacement.hpp"

#include <cerrno>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <system_error>
#include <utility>

namespace {

constexpr const char* kPermissionDirectory = ".codeagent";
constexpr const char* kPermissionFile = "permissions.txt";

// The cause of the last failed stream operation, or a generic I/O error
// when the library did not set errno.
std::error_code lastSystemError() {
    return errno != 0
        ? std::error_code(errno, std::generic_category())
        : std::make_error_code(std::errc::io_error);
}

// Removes the temporary file when it goes out of scope, unless it was
// renamed into place, so no failure path leaves it behind.
class TemporaryFileGuard {
public:
    explicit TemporaryFileGuard(std::filesystem::path path)
        : path_(std::move(path)) {
    }

    ~TemporaryFileGuard() {
        if (!released_) {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }
    }

    TemporaryFileGuard(const TemporaryFileGuard&) = delete;
    TemporaryFileGuard& operator=(const TemporaryFileGuard&) = delete;

    void release() noexcept { released_ = true; }

private:
    std::filesystem::path path_;
    bool released_ = false;
};

} // namespace

FileManager::FileManager()
    : FileManager(std::cin, std::cout, std::cerr) {
}

FileManager::FileManager(
    std::istream& input,
    std::ostream& output,
    std::ostream& error
)
    : permissionFilePath_(
          std::filesystem::path(kPermissionDirectory) /
          kPermissionFile
      ),
      input_(input),
      output_(output),
      error_(error) {
}

bool FileManager::readFile(
    const std::filesystem::path& filePath,
    std::string& content
) const {
    content.clear();

    if (!isValidSourceFile(filePath)) {
        error_
            << "Error: Invalid or unsupported source file: "
            << filePath << '\n';

        return false;
    }

    if (!requestPermission(filePath, PermissionType::Read)) {
        error_
            << "Error: Read access denied for: "
            << filePath << '\n';

        return false;
    }

    std::ifstream inputFile(filePath);

    if (!inputFile.is_open()) {
        error_
            << "Error: Unable to open file for reading: "
            << filePath << '\n';

        return false;
    }

    content.assign(
        std::istreambuf_iterator<char>(inputFile),
        std::istreambuf_iterator<char>()
    );

    if (inputFile.bad()) {
        error_
            << "Error: Failed while reading file: "
            << filePath << '\n';

        content.clear();
        return false;
    }

    return true;
}

bool FileManager::writeFile(
    const std::filesystem::path& filePath,
    const std::string& content
) const {
    return saveFile(filePath, content, PermissionPrompt::Ask) ==
           WriteResult::Written;
}

FileManager::WriteResult FileManager::saveFile(
    const std::filesystem::path& filePath,
    const std::string& content,
    PermissionPrompt prompt
) const {
    if (filePath.empty()) {
        error_
            << "Error: Output file path cannot be empty.\n";

        return WriteResult::InvalidPath;
    }

    if (!isSupportedExtension(filePath)) {
        error_
            << "Error: " << filePath << " is not a supported source file "
            << "name. Use a name ending in .c, .cc, .cpp, .cxx, .h, .hh, "
            << ".hpp, or .hxx.\n";

        return WriteResult::InvalidPath;
    }

    const std::filesystem::path normalizedPath =
        normalizePath(filePath);

    const std::filesystem::path parentDirectory =
        normalizedPath.parent_path();

    std::error_code statusError;
    const auto parentStatus =
        std::filesystem::status(parentDirectory, statusError);

    if (!std::filesystem::exists(parentStatus)) {
        error_
            << "Error: The folder " << parentDirectory
            << " does not exist. Create it first or choose another path.\n";

        return WriteResult::InvalidPath;
    }

    if (!std::filesystem::is_directory(parentStatus)) {
        error_
            << "Error: " << parentDirectory
            << " is not a folder. Choose a path inside an existing folder.\n";

        return WriteResult::InvalidPath;
    }

    const auto targetStatus =
        std::filesystem::symlink_status(normalizedPath, statusError);
    const bool fileAlreadyExists = std::filesystem::exists(targetStatus);

    if (std::filesystem::is_directory(targetStatus)) {
        error_
            << "Error: " << normalizedPath << " is a folder. Include a file "
            << "name, for example " << (normalizedPath / "main.cpp") << ".\n";

        return WriteResult::InvalidPath;
    }

    if (fileAlreadyExists &&
        !std::filesystem::is_regular_file(targetStatus)) {
        error_
            << "Error: Target path is not a regular file: "
            << normalizedPath << '\n';

        return WriteResult::InvalidPath;
    }

    if (prompt == PermissionPrompt::Ask &&
        !requestPermission(normalizedPath, PermissionType::Write)) {
        error_
            << "Error: Write access denied for: "
            << normalizedPath << '\n';

        return WriteResult::Declined;
    }

    /*
     * Permanent write permission does NOT authorize overwriting.
     *
     * Every modification of an existing file requires
     * explicit approval from the user.
     */
    if (fileAlreadyExists &&
        !requestOverwritePermission(normalizedPath)) {
        error_
            << "File was not modified.\n";

        return WriteResult::Declined;
    }

    try {
        writeAtomically(normalizedPath, content);
        return WriteResult::Written;
    } catch (const FilePermissionError&) {
        error_
            << "Error: Permission denied: CodeAgent cannot write to "
            << normalizedPath << ". Choose a folder you can write to, "
            << "or change its permissions.\n";
    } catch (const FileNotFoundError&) {
        error_
            << "Error: The folder " << parentDirectory
            << " is no longer available. Check the path and try again.\n";
    } catch (const FileError& failure) {
        error_
            << "Error: Could not write " << normalizedPath << ": "
            << failure.errorCode().message() << ".\n";
    }

    return WriteResult::Failed;
}

void FileManager::writeAtomically(
    const std::filesystem::path& filePath,
    const std::string& content
) const {
    /*
     * Write to a temporary file first.
     *
     * This prevents an interrupted write from immediately
     * truncating the original file.
     */
    const std::filesystem::path temporaryPath =
        filePath.string() + ".codeagent.tmp";

    errno = 0;
    std::ofstream outputFile(
        temporaryPath,
        std::ios::binary | std::ios::trunc
    );

    if (!outputFile.is_open()) {
        throwFileSystemError(FileOperation::Write, filePath, lastSystemError());
    }

    // Guard only a file this call created, never whatever was already at
    // the temporary path when opening it failed.
    TemporaryFileGuard temporaryFile(temporaryPath);

    errno = 0;
    outputFile << content;
    outputFile.close();

    if (!outputFile.good()) {
        throwFileSystemError(FileOperation::Write, filePath, lastSystemError());
    }

    std::error_code renameError;

    if (!codeagent::detail::replaceFile(
        temporaryPath,
        filePath,
        renameError
    )) {
        throwFileSystemError(FileOperation::Write, filePath, renameError);
    }

    temporaryFile.release();
}

bool FileManager::isValidSourceFile(
    const std::filesystem::path& filePath
) const {
    if (filePath.empty()) {
        return false;
    }

    std::error_code error;

    if (!std::filesystem::exists(filePath, error) || error) {
        return false;
    }

    if (!std::filesystem::is_regular_file(filePath, error) ||
        error) {
        return false;
    }

    return isSupportedExtension(filePath);
}

bool FileManager::requestPermission(
    const std::filesystem::path& filePath,
    PermissionType permission
) const {
    if (hasPermanentPermission(filePath, permission)) {
        return true;
    }

    const std::string permissionName =
        permission == PermissionType::Read
            ? "read"
            : "write";

    output_
        << "\nCodeAgent wants permission to "
        << permissionName << ":\n\n"
        << "    " << normalizePath(filePath) << "\n\n"
        << "[Y] Allow once\n"
        << "[A] Always allow\n"
        << "[N] Deny\n"
        << "\nChoice: ";

    std::string choice;

    if (!std::getline(input_, choice)) {
        return false;
    }

    if (choice.empty()) {
        return false;
    }

    const char selected =
        static_cast<char>(
            std::tolower(
                static_cast<unsigned char>(choice[0])
            )
        );

    if (selected == 'y') {
        return true;
    }

    if (selected == 'a') {
        return savePermanentPermission(
            filePath,
            permission
        );
    }

    return false;
}

bool FileManager::hasPermanentPermission(
    const std::filesystem::path& filePath,
    PermissionType permission
) const {
    std::ifstream permissionFile(
        permissionFilePath_
    );

    if (!permissionFile.is_open()) {
        return false;
    }

    const std::string normalizedPath =
        normalizePath(filePath).string();

    const std::string permissionName =
        permission == PermissionType::Read
            ? "read"
            : "write";

    std::string line;

    while (std::getline(permissionFile, line)) {
        const std::string expectedEntry =
            permissionName + "|" + normalizedPath;

        if (line == expectedEntry) {
            return true;
        }
    }

    return false;
}

bool FileManager::savePermanentPermission(
    const std::filesystem::path& filePath,
    PermissionType permission
) const {
    try {
        std::filesystem::create_directories(
            permissionFilePath_.parent_path()
        );
    } catch (const std::filesystem::filesystem_error& error) {
        error_
            << "Error: Unable to create permission directory: "
            << error.what() << '\n';

        return false;
    }

    if (hasPermanentPermission(filePath, permission)) {
        return true;
    }

    std::ofstream permissionFile(
        permissionFilePath_,
        std::ios::app
    );

    if (!permissionFile.is_open()) {
        error_
            << "Error: Unable to save file permission.\n";

        return false;
    }

    const std::string permissionName =
        permission == PermissionType::Read
            ? "read"
            : "write";

    permissionFile
        << permissionName
        << '|'
        << normalizePath(filePath).string()
        << '\n';

    if (!permissionFile.good()) {
        error_
            << "Error: Failed to save file permission.\n";

        return false;
    }

    return true;
}

bool FileManager::requestOverwritePermission(
    const std::filesystem::path& filePath
) const {
    output_
        << "\nWARNING: CodeAgent is requesting permission "
        << "to overwrite an existing file:\n\n"
        << "    " << normalizePath(filePath) << "\n\n"
        << "This will replace the current contents of the file.\n\n"
        << "Overwrite this file? [y/N]: ";

    std::string choice;

    if (!std::getline(input_, choice)) {
        return false;
    }

    if (choice.empty()) {
        return false;
    }

    const char selected =
        static_cast<char>(
            std::tolower(
                static_cast<unsigned char>(choice[0])
            )
        );

    return selected == 'y';
}

bool FileManager::isSupportedExtension(
    const std::filesystem::path& filePath
) const {
    const std::string extension =
        filePath.extension().string();

    static const std::set<std::string> supportedExtensions = {
        ".c",
        ".cc",
        ".cpp",
        ".cxx",
        ".h",
        ".hh",
        ".hpp",
        ".hxx"
    };

    return supportedExtensions.find(extension) !=
           supportedExtensions.end();
}

std::filesystem::path FileManager::normalizePath(
    const std::filesystem::path& filePath
) const {
    try {
        return std::filesystem::absolute(filePath)
            .lexically_normal();
    } catch (const std::filesystem::filesystem_error&) {
        return filePath.lexically_normal();
    }
}
