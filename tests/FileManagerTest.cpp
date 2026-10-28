#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <system_error>
#include <unistd.h>
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

// --------------------------------------------------------------------------
// saveFile: validation, confirmation, and failure reporting
// --------------------------------------------------------------------------

class SaveFileTest : public FileManagerTestFixture {
protected:
    FileManager::WriteResult save(
        const std::filesystem::path& path,
        const std::string& answers,
        FileManager::PermissionPrompt prompt =
            FileManager::PermissionPrompt::AlreadyGranted
    ) {
        std::istringstream input(answers);
        output.str("");
        error.str("");
        FileManager manager(input, output, error);
        return manager.saveFile(path, kCode, prompt);
    }

    const std::string kCode = "int main() { return 0; }\n";
    std::ostringstream output;
    std::ostringstream error;
};

TEST_F(SaveFileTest, InvalidPathsAreRejectedBeforeAnythingIsWritten) {
    EXPECT_EQ(save("", ""), FileManager::WriteResult::InvalidPath);
    EXPECT_NE(error.str().find("cannot be empty"), std::string::npos);

    EXPECT_EQ(save(testDir / "notes.txt", ""), FileManager::WriteResult::InvalidPath);
    EXPECT_NE(error.str().find("not a supported source file name"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(testDir / "notes.txt"));

    EXPECT_EQ(save(testDir / "missing" / "main.cpp", ""), FileManager::WriteResult::InvalidPath);
    EXPECT_NE(error.str().find("does not exist. Create it first or choose another path."),
              std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(testDir / "missing"));

    std::filesystem::create_directory(testDir / "folder.cpp");
    EXPECT_EQ(save(testDir / "folder.cpp", ""), FileManager::WriteResult::InvalidPath);
    EXPECT_NE(error.str().find("is a folder. Include a file name"), std::string::npos);
}

TEST_F(SaveFileTest, AUserChosenPathIsWrittenWithoutAPermissionPrompt) {
    const auto target = testDir / "main.cpp";
    EXPECT_EQ(save(target, ""), FileManager::WriteResult::Written);
    EXPECT_EQ(readContents(target), kCode);
    EXPECT_EQ(output.str().find("wants permission"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(target.string() + ".codeagent.tmp"));
}

TEST_F(SaveFileTest, AnExistingFileIsOnlyReplacedAfterConfirmation) {
    const auto target = testDir / "existing.cpp";
    std::ofstream(target) << "old contents\n";

    EXPECT_EQ(save(target, "n\n"), FileManager::WriteResult::Declined);
    EXPECT_NE(output.str().find("Overwrite this file? [y/N]"), std::string::npos);
    EXPECT_EQ(readContents(target), "old contents\n");

    // End of input is not consent.
    EXPECT_EQ(save(target, ""), FileManager::WriteResult::Declined);
    EXPECT_EQ(readContents(target), "old contents\n");

    EXPECT_EQ(save(target, "y\n"), FileManager::WriteResult::Written);
    EXPECT_EQ(readContents(target), kCode);
}

TEST_F(SaveFileTest, PermissionDeniedIsReportedAsSuch) {
    if (::geteuid() == 0) GTEST_SKIP() << "root ignores file permissions";
    // A folder on the local file system: drives mounted from Windows under
    // WSL ignore permission bits.
    const auto folder = std::filesystem::temp_directory_path() /
        ("codeagent-save-" + std::to_string(::getpid()));
    std::filesystem::create_directories(folder);
    std::filesystem::permissions(folder, std::filesystem::perms::owner_read |
                                         std::filesystem::perms::owner_exec);
    const bool enforced = !std::ofstream(folder / "probe.cpp").is_open();
    const auto target = folder / "main.cpp";
    const auto result = enforced ? save(target, "") : FileManager::WriteResult::Failed;
    std::filesystem::permissions(folder, std::filesystem::perms::owner_all);
    const bool written = std::filesystem::exists(target);
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
    if (!enforced) GTEST_SKIP() << "this file system does not enforce permissions";

    EXPECT_EQ(result, FileManager::WriteResult::Failed);
    EXPECT_NE(error.str().find("Permission denied: CodeAgent cannot write to"), std::string::npos);
    EXPECT_FALSE(written);
}

TEST_F(SaveFileTest, OtherWriteFailuresNameTheCauseAndKeepUnrelatedFiles) {
    const auto target = testDir / "main.cpp";
    // Something that is not ours already occupies the temporary path.
    const auto blocker = std::filesystem::path(target.string() + ".codeagent.tmp");
    std::filesystem::create_directory(blocker);

    EXPECT_EQ(save(target, ""), FileManager::WriteResult::Failed);
    EXPECT_NE(error.str().find("Error: Could not write"), std::string::npos);
    EXPECT_NE(error.str().find("Is a directory"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(target));
    EXPECT_TRUE(std::filesystem::is_directory(blocker));
}
