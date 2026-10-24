#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <system_error>
#include "FileManager.hpp"
#include "../src/detail/FileReplacement.hpp"

namespace {

class ScopedCinInput {
public:
    explicit ScopedCinInput(const std::string& input)
        : input_(input),
          originalBuffer_(std::cin.rdbuf(input_.rdbuf())) {
    }

    ~ScopedCinInput() {
        std::cin.rdbuf(originalBuffer_);
    }

private:
    std::istringstream input_;
    std::streambuf* originalBuffer_;
};

std::string readContents(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>()
    );
}

} // namespace

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

TEST_F(FileManagerTestFixture, FailedReadClearsOutput) {
    FileManager manager;
    std::string content = "old contents";
    std::filesystem::path missing = testDir / "missing_file.cpp";

    EXPECT_FALSE(manager.readFile(missing, content));
    EXPECT_TRUE(content.empty());
}

// 3. Write failure on empty path
TEST_F(FileManagerTestFixture, EmptyPathWriteRejection) {
    FileManager manager;
    EXPECT_FALSE(manager.writeFile("", "code payload"));
}

TEST_F(FileManagerTestFixture, WritesNewFileSuccessfully) {
    FileManager manager;
    const std::filesystem::path output = testDir / "new_file.cpp";
    const std::string expected = "int main() { return 0; }\n";
    ScopedCinInput input("y\n");

    ASSERT_TRUE(manager.writeFile(output, expected));
    EXPECT_EQ(readContents(output), expected);
}

TEST_F(FileManagerTestFixture, OverwritesExistingFileSuccessfully) {
    FileManager manager;
    const std::filesystem::path output = testDir / "existing_file.cpp";
    const std::string expected = "int main() { return 1; }\n";
    std::ofstream(output) << "old contents\n";
    ScopedCinInput input("y\ny\n");

    ASSERT_TRUE(manager.writeFile(output, expected));
    EXPECT_EQ(readContents(output), expected);
}

TEST_F(FileManagerTestFixture, ReplacementFailurePreservesOriginalContents) {
    const std::filesystem::path target = testDir / "preserve.cpp";
    const std::filesystem::path temporary =
        target.string() + ".codeagent.tmp";
    const std::string original = "original contents\n";
    std::ofstream(target) << original;
    std::ofstream(temporary) << "replacement contents\n";

    bool originalWasPresentAtReplacement = false;
    std::error_code replacementError;

    const bool replaced = codeagent::detail::replaceFile(
        temporary,
        target,
        replacementError,
        [&originalWasPresentAtReplacement, &original](
            const std::filesystem::path&,
            const std::filesystem::path& targetPath,
            std::error_code& error
        ) {
            originalWasPresentAtReplacement =
                readContents(targetPath) == original;
            error = std::make_error_code(
                std::errc::permission_denied
            );
        }
    );

    EXPECT_FALSE(replaced);
    EXPECT_TRUE(originalWasPresentAtReplacement);
    EXPECT_EQ(readContents(target), original);
    EXPECT_FALSE(std::filesystem::exists(temporary));
}
