#include "FileManager.hpp"
#include <fstream>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

void printTestHeader(const std::string& testName) {
    std::cout
        << "\n========================================\n"
        << testName
        << "\n========================================\n";
}

} // namespace

int main() {
    FileManager fileManager;

    const std::filesystem::path testDirectory =
        "file_manager_test_data";

    const std::filesystem::path sourceFile =
        testDirectory / "test.cpp";

    const std::filesystem::path outputFile =
        testDirectory / "output.cpp";

    const std::filesystem::path unsupportedFile =
        testDirectory / "test.txt";

    /*
     * Clean up previous test data.
     */
    std::error_code error;

    std::filesystem::remove_all(
        testDirectory,
        error
    );

    /*
     * Create test directory.
     */
    if (!std::filesystem::create_directories(
            testDirectory,
            error)) {

        if (error) {
            std::cerr
                << "Failed to create test directory: "
                << error.message()
                << '\n';

            return 1;
        }
    }

    /*
     * Create a source file directly.
     *
     * This is test setup, not FileManager testing yet.
     */
    {
        std::ofstream testFile(sourceFile);

        if (!testFile.is_open()) {
            std::cerr
                << "Failed to create test source file.\n";

            return 1;
        }

        testFile
            << "#include <iostream>\n\n"
            << "int main() {\n"
            << "    std::cout << \"Hello\\n\";\n"
            << "    return 0;\n"
            << "}\n";
    }

    // --------------------------------------------------
    // TEST 1: Read a valid source file
    // --------------------------------------------------

    printTestHeader(
        "TEST 1: Read valid source file"
    );

    std::string content;

    if (fileManager.readFile(
            sourceFile,
            content)) {

        std::cout
            << "PASS: File was read successfully.\n"
            << "Content size: "
            << content.size()
            << " bytes.\n";
    } else {
        std::cerr
            << "FAIL: File could not be read.\n";

        return 1;
    }

    // --------------------------------------------------
    // TEST 2: Unsupported file
    // --------------------------------------------------

    printTestHeader(
        "TEST 2: Reject unsupported file"
    );

    {
        std::ofstream testFile(unsupportedFile);

        if (!testFile.is_open()) {
            std::cerr
                << "Failed to create unsupported test file.\n";

            return 1;
        }

        testFile << "This is not C++.";
    }

    std::string unsupportedContent;

    if (!fileManager.readFile(
            unsupportedFile,
            unsupportedContent)) {

        std::cout
            << "PASS: Unsupported file was rejected.\n";
    } else {
        std::cerr
            << "FAIL: Unsupported file was accepted.\n";

        return 1;
    }

    // --------------------------------------------------
    // TEST 3: Nonexistent file
    // --------------------------------------------------

    printTestHeader(
        "TEST 3: Reject nonexistent file"
    );

    const std::filesystem::path missingFile =
        testDirectory / "does_not_exist.cpp";

    std::string missingContent;

    if (!fileManager.readFile(
            missingFile,
            missingContent)) {

        std::cout
            << "PASS: Nonexistent file was rejected.\n";
    } else {
        std::cerr
            << "FAIL: Nonexistent file was accepted.\n";

        return 1;
    }

    // --------------------------------------------------
    // TEST 4: Write a new file
    // --------------------------------------------------

    printTestHeader(
        "TEST 4: Write new file"
    );

    const std::string generatedContent =
        "#include <iostream>\n\n"
        "int main() {\n"
        "    std::cout << \"Generated file\\n\";\n"
        "    return 0;\n"
        "}\n";

    if (fileManager.writeFile(
            outputFile,
            generatedContent)) {

        std::cout
            << "PASS: New file was written.\n";
    } else {
        std::cerr
            << "FAIL: New file could not be written.\n";

        return 1;
    }

    // --------------------------------------------------
    // TEST 5: Verify written file
    // --------------------------------------------------

    printTestHeader(
        "TEST 5: Verify written file"
    );

    std::string outputContent;

    if (fileManager.readFile(
            outputFile,
            outputContent)) {

        if (outputContent == generatedContent) {
            std::cout
                << "PASS: Written content matches expected content.\n";
        } else {
            std::cerr
                << "FAIL: Written content does not match.\n";

            return 1;
        }

    } else {
        std::cerr
            << "FAIL: Could not read generated file.\n";

        return 1;
    }

    // --------------------------------------------------
    // TEST 6: Existing file overwrite
    // --------------------------------------------------

    printTestHeader(
        "TEST 6: Existing file overwrite"
    );

    std::cout
        << "The FileManager should ask for write permission\n"
        << "and then ask for explicit overwrite permission.\n\n";

    const std::string replacementContent =
        "// This content replaces the old content.\n";

    if (fileManager.writeFile(
            outputFile,
            replacementContent)) {

        std::cout
            << "PASS: Existing file was overwritten.\n";
    } else {
        std::cout
            << "Overwrite was rejected or failed.\n";
    }

    // --------------------------------------------------
    // TEST 7: Verify final content
    // --------------------------------------------------

    printTestHeader(
        "TEST 7: Verify final file content"
    );

    std::string finalContent;

    if (fileManager.readFile(
            outputFile,
            finalContent)) {

        std::cout
            << "Final content:\n\n"
            << finalContent
            << '\n';

    } else {
        std::cerr
            << "FAIL: Could not read final file.\n";

        return 1;
    }

    std::cout
        << "\n========================================\n"
        << "FileManager tests completed.\n"
        << "========================================\n";

    return 0;
}