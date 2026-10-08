#ifndef FILE_MANAGER_HPP
#define FILE_MANAGER_HPP

#include <filesystem>
#include <string>

class FileManager {
public:
    FileManager();

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
};

#endif