#include "UnifiedDiff.hpp"

#include <gtest/gtest.h>

#include <string>

TEST(UnifiedDiffTest, ReportsIdenticalAndEmptyContentWithoutChanges) {
    EXPECT_EQ(makeUnifiedDiff("", ""), "No differences.\n");
    EXPECT_EQ(makeUnifiedDiff("same\n", "same\n"), "No differences.\n");
}

TEST(UnifiedDiffTest, ShowsUnchangedRemovedAndAddedLines) {
    const std::string diff = makeUnifiedDiff(
        "first\nold\nlast\n",
        "first\nnew\nlast\n"
    );

    EXPECT_EQ(
        diff,
        "--- original\n"
        "+++ proposed\n"
        "@@ -1,3 +1,3 @@\n"
        " first\n"
        "-old\n"
        "+new\n"
        " last\n"
    );
}

TEST(UnifiedDiffTest, PreservesContextAcrossSeparatedEdits) {
    EXPECT_EQ(
        makeUnifiedDiff(
            "a\nb\nc\nd\ne\n",
            "a\nB\nc\nd\nE\n"
        ),
        "--- original\n"
        "+++ proposed\n"
        "@@ -1,5 +1,5 @@\n"
        " a\n"
        "-b\n"
        "+B\n"
        " c\n"
        " d\n"
        "-e\n"
        "+E\n"
    );
}

TEST(UnifiedDiffTest, HandlesAddingToEmptyContent) {
    EXPECT_EQ(
        makeUnifiedDiff("", "one\ntwo\n"),
        "--- original\n"
        "+++ proposed\n"
        "@@ -0,0 +1,2 @@\n"
        "+one\n"
        "+two\n"
    );
}

TEST(UnifiedDiffTest, MarksMissingTrailingNewline) {
    EXPECT_EQ(
        makeUnifiedDiff("line\n", "line"),
        "--- original\n"
        "+++ proposed\n"
        "@@ -1,1 +1,1 @@\n"
        "-line\n"
        "+line\n"
        "\\ No newline at end of file\n"
    );
}

TEST(UnifiedDiffTest, ShowsCrLfToLfOnlyChanges) {
    const std::string diff = makeUnifiedDiff(
        "first\r\nsecond\r\n",
        "first\nsecond\n"
    );

    EXPECT_NE(diff.find("-first\n"), std::string::npos);
    EXPECT_NE(diff.find("-second\n"), std::string::npos);
    EXPECT_NE(diff.find("+first\n"), std::string::npos);
    EXPECT_NE(diff.find("+second\n"), std::string::npos);
    EXPECT_EQ(diff.find(" first\n"), std::string::npos);
}

TEST(UnifiedDiffTest, HandlesLargeSubstantiallyDissimilarInputs) {
    constexpr std::size_t lineCount = 1500;
    std::string original;
    std::string proposed;
    for (std::size_t i = 0; i < lineCount; ++i) {
        original += "old_line_" + std::to_string(i) + '\n';
        proposed += "new_line_" + std::to_string(i) + '\n';
    }

    const std::string diff = makeUnifiedDiff(original, proposed);
    std::size_t removed = 0;
    std::size_t added = 0;
    const std::size_t hunkStart = diff.find("@@ -");
    ASSERT_NE(hunkStart, std::string::npos);
    const std::size_t bodyStart = diff.find('\n', hunkStart) + 1;
    for (std::size_t i = bodyStart; i < diff.size(); ++i) {
        if ((i == bodyStart || diff[i - 1] == '\n') && diff[i] == '-') {
            ++removed;
        } else if ((i == bodyStart || diff[i - 1] == '\n') && diff[i] == '+') {
            ++added;
        }
    }

    EXPECT_EQ(removed, lineCount);
    EXPECT_EQ(added, lineCount);
}
