#include "FileManager.hpp"

#include "detail/FileReplacement.hpp"

#include <fstream>
#include <iostream>
#include <set>
#include <string>

namespace {

constexpr const char* kPermissionDirectory = ".codeagent";
constexpr const char* kPermissionFile = "permissions.txt";

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
    if (filePath.empty()) {
        error_
            << "Error: Output file path cannot be empty.\n";

        return false;
    }

    const std::filesystem::path normalizedPath =
        normalizePath(filePath);

    const std::filesystem::path parentDirectory =
        normalizedPath.parent_path();

    if (!parentDirectory.empty() &&
        !std::filesystem::exists(parentDirectory)) {

        error_
            << "Error: Parent directory does not exist: "
            << parentDirectory << '\n';

        return false;
    }

    if (!parentDirectory.empty() &&
        !std::filesystem::is_directory(parentDirectory)) {

        error_
            << "Error: Parent path is not a directory: "
            << parentDirectory << '\n';

        return false;
    }

    if (!requestPermission(normalizedPath, PermissionType::Write)) {
        error_
            << "Error: Write access denied for: "
            << normalizedPath << '\n';

        return false;
    }

    const bool fileAlreadyExists =
        std::filesystem::exists(normalizedPath);

    if (fileAlreadyExists) {
        if (!std::filesystem::is_regular_file(normalizedPath)) {
            error_
                << "Error: Target path is not a regular file: "
                << normalizedPath << '\n';

            return false;
        }

        /*
         * Permanent write permission does NOT authorize overwriting.
         *
         * Every modification of an existing file requires
         * explicit approval from the user.
         */
        if (!requestOverwritePermission(normalizedPath)) {
            error_
                << "File was not modified.\n";

            return false;
        }
    }

    /*
     * Write to a temporary file first.
     *
     * This prevents an interrupted write from immediately
     * truncating the original file.
     */
    const std::filesystem::path temporaryPath =
        normalizedPath.string() + ".codeagent.tmp";

    std::ofstream outputFile(
        temporaryPath,
        std::ios::binary | std::ios::trunc
    );

    if (!outputFile.is_open()) {
        error_
            << "Error: Unable to create temporary output file: "
            << temporaryPath << '\n';

        return false;
    }

    outputFile << content;

    if (!outputFile.good()) {
        outputFile.close();

        std::error_code cleanupError;
        std::filesystem::remove(
            temporaryPath,
            cleanupError
        );

        error_
            << "Error: Failed while writing temporary file.\n";

        return false;
    }

    outputFile.close();

    if (!outputFile.good()) {
        std::error_code cleanupError;
        std::filesystem::remove(
            temporaryPath,
            cleanupError
        );

        error_
            << "Error: Failed while writing temporary file.\n";

        return false;
    }

    std::error_code renameError;

    if (!codeagent::detail::replaceFile(
        temporaryPath,
        normalizedPath,
        renameError
    )) {

        error_
            << "Error: Unable to finalize file write: "
            << renameError.message() << '\n';

        return false;
    }

    output_
        << "File written successfully: "
        << normalizedPath << '\n';

    return true;
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
