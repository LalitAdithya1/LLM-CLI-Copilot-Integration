#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include "FileManager.hpp"

// Test fixture to manage temporary test directories cleanly
class FileManagerTestFixture : public ::testing::Test {
protected:
    std::filesystem::path testDir = "test_sandbox";

    void SetUp() override {
        std::filesystem::create_directories(testDir);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(testDir, ec);
    }
};

// 1. Verify extension validation for supported source files
TEST_F(FileManagerTestFixture, ValidSourceFileExtensions) {
    FileManager manager;
    
    std::filesystem::path cppFile = testDir / "sample.cpp";
    std::ofstream(cppFile) << "int main() { return 0; }";

    std::filesystem::path txtFile = testDir / "notes.txt";
    std::ofstream(txtFile) << "Plain text content";

    EXPECT_TRUE(manager.isValidSourceFile(cppFile));
    EXPECT_FALSE(manager.isValidSourceFile(txtFile));
}

// 2. Non-existent file should fail validation
TEST_F(FileManagerTestFixture, NonExistentFileValidation) {
    FileManager manager;
    std::filesystem::path missing = testDir / "ghost_file.cpp";

    EXPECT_FALSE(manager.isValidSourceFile(missing));
}

// 3. Write failure on empty path
TEST_F(FileManagerTestFixture, EmptyPathWriteRejection) {
    FileManager manager;
    EXPECT_FALSE(manager.writeFile("", "code payload"));
}
