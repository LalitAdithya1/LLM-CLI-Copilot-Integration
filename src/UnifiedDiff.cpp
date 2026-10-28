#include "UnifiedDiff.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace {

enum class LineEnding {
    None,
    LF,
    CRLF
};

struct Line {
    std::string text;
    LineEnding ending = LineEnding::None;
};

enum class EditKind {
    Equal,
    Delete,
    Insert
};

struct Edit {
    EditKind kind;
    std::size_t originalIndex;
    std::size_t proposedIndex;
};

using Match = std::pair<std::size_t, std::size_t>;

std::vector<Line> splitLines(const std::string& text) {
    std::vector<Line> lines;

    for (std::size_t start = 0; start < text.size();) {
        const std::size_t newline = text.find('\n', start);
        const bool terminated = newline != std::string::npos;
        const std::size_t end = terminated ? newline : text.size();

        std::string line = text.substr(start, end - start);
        LineEnding ending = LineEnding::None;
        if (terminated) {
            ending = LineEnding::LF;
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
                ending = LineEnding::CRLF;
            }
        }

        lines.push_back({std::move(line), ending});
        start = terminated ? end + 1 : text.size();
    }

    return lines;
}

bool sameLine(const Line& left, const Line& right) {
    return left.text == right.text && left.ending == right.ending;
}

std::vector<std::size_t> lcsPrefixLengths(
    const std::vector<Line>& original,
    std::size_t originalBegin,
    std::size_t originalEnd,
    const std::vector<Line>& proposed,
    std::size_t proposedBegin,
    std::size_t proposedEnd
) {
    const std::size_t proposedLength = proposedEnd - proposedBegin;
    std::vector<std::size_t> previous(proposedLength + 1, 0);
    std::vector<std::size_t> current(proposedLength + 1, 0);

    for (std::size_t i = originalBegin; i < originalEnd; ++i) {
        current[0] = 0;
        for (std::size_t j = 1; j <= proposedLength; ++j) {
            if (sameLine(original[i], proposed[proposedBegin + j - 1])) {
                current[j] = previous[j - 1] + 1;
            } else {
                current[j] = std::max(previous[j], current[j - 1]);
            }
        }
        previous.swap(current);
    }

    return previous;
}

std::vector<std::size_t> lcsSuffixLengths(
    const std::vector<Line>& original,
    std::size_t originalBegin,
    std::size_t originalEnd,
    const std::vector<Line>& proposed,
    std::size_t proposedBegin,
    std::size_t proposedEnd
) {
    const std::size_t proposedLength = proposedEnd - proposedBegin;
    std::vector<std::size_t> previous(proposedLength + 1, 0);
    std::vector<std::size_t> current(proposedLength + 1, 0);

    for (std::size_t i = originalEnd; i > originalBegin; --i) {
        current[proposedLength] = 0;
        for (std::size_t j = proposedLength; j > 0; --j) {
            if (sameLine(original[i - 1], proposed[proposedBegin + j - 1])) {
                current[j - 1] = previous[j] + 1;
            } else {
                current[j - 1] = std::max(previous[j - 1], current[j]);
            }
        }
        previous.swap(current);
    }

    return previous;
}

// Hirschberg's LCS reconstruction uses O(proposed lines) auxiliary memory.
// Its O(original lines * proposed lines) time avoids retaining a full edit
// trace, so large diffs cannot grow the trace quadratically in memory.
void collectMatches(
    const std::vector<Line>& original,
    std::size_t originalBegin,
    std::size_t originalEnd,
    const std::vector<Line>& proposed,
    std::size_t proposedBegin,
    std::size_t proposedEnd,
    std::vector<Match>& matches
) {
    const std::size_t originalLength = originalEnd - originalBegin;
    const std::size_t proposedLength = proposedEnd - proposedBegin;

    if (originalLength == 0 || proposedLength == 0) {
        return;
    }

    if (originalLength == 1) {
        for (std::size_t j = proposedBegin; j < proposedEnd; ++j) {
            if (sameLine(original[originalBegin], proposed[j])) {
                matches.emplace_back(originalBegin, j);
                break;
            }
        }
        return;
    }

    const std::size_t middle = originalBegin + originalLength / 2;
    std::size_t proposedMiddle = proposedBegin;
    {
        const std::vector<std::size_t> prefix = lcsPrefixLengths(
            original, originalBegin, middle,
            proposed, proposedBegin, proposedEnd
        );
        const std::vector<std::size_t> suffix = lcsSuffixLengths(
            original, middle, originalEnd,
            proposed, proposedBegin, proposedEnd
        );

        std::size_t bestLength = 0;
        for (std::size_t split = 0; split <= proposedLength; ++split) {
            const std::size_t length = prefix[split] + suffix[split];
            if (length > bestLength) {
                bestLength = length;
                proposedMiddle = proposedBegin + split;
            }
        }
    }

    collectMatches(
        original, originalBegin, middle,
        proposed, proposedBegin, proposedMiddle, matches
    );
    collectMatches(
        original, middle, originalEnd,
        proposed, proposedMiddle, proposedEnd, matches
    );
}

std::vector<Edit> diffLines(
    const std::vector<Line>& original,
    const std::vector<Line>& proposed
) {
    std::vector<Match> matches;
    collectMatches(
        original, 0, original.size(),
        proposed, 0, proposed.size(), matches
    );

    std::vector<Edit> edits;
    std::size_t originalIndex = 0;
    std::size_t proposedIndex = 0;

    for (const Match& match : matches) {
        while (originalIndex < match.first) {
            edits.push_back({EditKind::Delete, originalIndex++, 0});
        }
        while (proposedIndex < match.second) {
            edits.push_back({EditKind::Insert, 0, proposedIndex++});
        }
        edits.push_back({EditKind::Equal, originalIndex, proposedIndex});
        ++originalIndex;
        ++proposedIndex;
    }

    while (originalIndex < original.size()) {
        edits.push_back({EditKind::Delete, originalIndex++, 0});
    }
    while (proposedIndex < proposed.size()) {
        edits.push_back({EditKind::Insert, 0, proposedIndex++});
    }

    return edits;
}

void appendLine(
    std::string& diff,
    char prefix,
    const Line& line
) {
    diff += prefix;
    diff += line.text;
    diff += '\n';
    if (line.ending == LineEnding::None) {
        diff += "\\ No newline at end of file\n";
    }
}

} // namespace

std::string makeUnifiedDiff(
    const std::string& originalText,
    const std::string& proposedText
) {
    if (originalText == proposedText) {
        return "No differences.\n";
    }

    const std::vector<Line> original = splitLines(originalText);
    const std::vector<Line> proposed = splitLines(proposedText);
    const std::vector<Edit> edits = diffLines(original, proposed);

    const std::size_t originalStart = original.empty() ? 0 : 1;
    const std::size_t proposedStart = proposed.empty() ? 0 : 1;

    std::string diff = "--- original\n+++ proposed\n@@ -";
    diff += std::to_string(originalStart);
    diff += ',';
    diff += std::to_string(original.size());
    diff += " +";
    diff += std::to_string(proposedStart);
    diff += ',';
    diff += std::to_string(proposed.size());
    diff += " @@\n";

    for (const Edit& edit : edits) {
        switch (edit.kind) {
        case EditKind::Equal:
            appendLine(diff, ' ', original[edit.originalIndex]);
            break;
        case EditKind::Delete:
            appendLine(diff, '-', original[edit.originalIndex]);
            break;
        case EditKind::Insert:
            appendLine(diff, '+', proposed[edit.proposedIndex]);
            break;
        }
    }

    return diff;
}
