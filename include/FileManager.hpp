#ifndef FILE_MANAGER_HPP
#define FILE_MANAGER_HPP

#include <filesystem>
#include <iosfwd>
#include <string>

class FileManager {
public:
    enum class WriteResult {
        Written,      // the file now holds the content
        InvalidPath,  // the path cannot be used; nothing was written
        Declined,     // the user denied writing or overwriting
        Failed        // the write failed; an existing file is unchanged
    };

    enum class PermissionPrompt {
        Ask,            // ask for write permission unless always allowed
        AlreadyGranted  // the user just chose this path themselves
    };

    FileManager();
    FileManager(
        std::istream& input,
        std::ostream& output,
        std::ostream& error
    );

    bool readFile(
        const std::filesystem::path& filePath,
        std::string& content
    ) const;

    // Same as saveFile with PermissionPrompt::Ask; true only when Written.
    bool writeFile(
        const std::filesystem::path& filePath,
        const std::string& content
    ) const;

    // Writes content to a text file of any extension atomically. An existing
    // file is only replaced after the user confirms overwriting it, whatever
    // the prompt setting. Every outcome except Written is explained on the
    // error stream; success is left to the caller to report.
    WriteResult saveFile(
        const std::filesystem::path& filePath,
        const std::string& content,
        PermissionPrompt prompt = PermissionPrompt::Ask
    ) const;

    bool isValidSourceFile(
        const std::filesystem::path& filePath
    ) const;

private:
    enum class PermissionType {
        Read,
        Write
    };

    bool requestPermission(
        const std::filesystem::path& filePath,
        PermissionType permission
    ) const;

    bool hasPermanentPermission(
        const std::filesystem::path& filePath,
        PermissionType permission
    ) const;

    bool savePermanentPermission(
        const std::filesystem::path& filePath,
        PermissionType permission
    ) const;

    bool requestOverwritePermission(
        const std::filesystem::path& filePath
    ) const;

    // Writes through a temporary file and a rename. Throws FileError (or a
    // subclass) carrying the operating-system cause.
    void writeAtomically(
        const std::filesystem::path& filePath,
        const std::string& content
    ) const;

    bool isSupportedFileName(
        const std::filesystem::path& filePath
    ) const;

    std::filesystem::path normalizePath(
        const std::filesystem::path& filePath
    ) const;

    std::filesystem::path permissionFilePath_;
    std::istream& input_;
    std::ostream& output_;
    std::ostream& error_;
};

#endif
