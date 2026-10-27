#ifndef FILE_MANAGER_HPP
#define FILE_MANAGER_HPP

#include <filesystem>
#include <iosfwd>
#include <string>

class FileManager {
public:
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

    bool writeFile(
        const std::filesystem::path& filePath,
        const std::string& content
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

    bool isSupportedExtension(
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
