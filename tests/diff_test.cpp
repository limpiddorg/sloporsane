#include <agent-runtime/tools/fs/common.hpp>

#include <gtest/gtest.h>
#include <algorithm>
#include <random>
#include <regex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
const std::string HEADERS = "--- file\n+++ file\n";

std::vector<std::string> terminatedLines(const std::string& text) {
    auto result = fs_tools::lines(text);
    for (auto& line : result) line += '\n';
    if (!text.empty() && text.back() != '\n') result.back().pop_back();
    return result;
}

std::string applyDiff(const std::string& before, const std::string& diff) {
    if (diff.empty()) return before;
    const auto source = terminatedLines(before);
    const auto patch = fs_tools::lines(diff);
    if (patch.size() < 3 || !patch[0].starts_with("--- ") || !patch[1].starts_with("+++ "))
        throw std::runtime_error("Invalid diff headers");
    const std::regex header(R"(@@ -([0-9]+)(?:,([0-9]+))? \+([0-9]+)(?:,([0-9]+))? @@)");
    std::size_t oldPosition = 0, newPosition = 0;
    std::string result;
    for (std::size_t i = 2; i < patch.size();) {
        std::smatch match;
        if (!std::regex_match(patch[i++], match, header)) throw std::runtime_error("Invalid hunk header");
        const auto oldCount = match[2].matched ? std::stoull(match[2]) : 1;
        const auto newCount = match[4].matched ? std::stoull(match[4]) : 1;
        const auto start = std::stoull(match[1]) - (oldCount != 0);
        if (start < oldPosition || start > source.size()) throw std::runtime_error("Invalid hunk offset");
        while (oldPosition < start) { result += source[oldPosition++]; ++newPosition; }
        EXPECT_EQ(std::stoull(match[3]), newPosition + (newCount != 0));
        std::size_t removed = 0, added = 0;
        while (i < patch.size() && !patch[i].starts_with("@@ ")) {
            const auto line = patch[i++];
            if (line.empty() || (line[0] != ' ' && line[0] != '-' && line[0] != '+'))
                throw std::runtime_error("Invalid hunk line");
            std::string text = line.substr(1);
            if (i < patch.size() && patch[i] == "\\ No newline at end of file") ++i;
            else text += '\n';
            if (line[0] != '+') {
                if (oldPosition >= source.size()) throw std::runtime_error("Hunk exceeds source");
                EXPECT_EQ(source[oldPosition++], text);
                ++removed;
            }
            if (line[0] != '-') { result += text; ++newPosition; ++added; }
        }
        EXPECT_EQ(removed, oldCount);
        EXPECT_EQ(added, newCount);
    }
    while (oldPosition < source.size()) result += source[oldPosition++];
    return result;
}

std::size_t changedLines(const std::string& diff) {
    std::size_t count = 0;
    const auto lines = fs_tools::lines(diff);
    for (std::size_t i = 2; i < lines.size(); ++i)
        count += lines[i].starts_with('+') || lines[i].starts_with('-');
    return count;
}

std::size_t minimalChanges(const std::string& before, const std::string& after) {
    // A small, independent LCS oracle for the randomized tests only.
    const auto oldLines = terminatedLines(before), newLines = terminatedLines(after);
    std::vector<std::size_t> previous(newLines.size() + 1), current(previous.size());
    for (const auto& old : oldLines) {
        for (std::size_t j = 0; j < newLines.size(); ++j)
            current[j + 1] = old == newLines[j] ? previous[j] + 1 : std::max(previous[j + 1], current[j]);
        previous.swap(current);
    }
    return oldLines.size() + newLines.size() - 2 * previous.back();
}

std::string numberedLines(std::size_t count) {
    std::string text;
    for (std::size_t i = 1; i <= count; ++i) text += "line " + std::to_string(i) + '\n';
    return text;
}

std::size_t hunkCount(const std::string& diff) {
    const auto lines = fs_tools::lines(diff);
    return std::count_if(lines.begin(), lines.end(), [](const auto& line) { return line.starts_with("@@ "); });
}

TEST(UnifiedDiffTest, IdenticalFilesHaveNoDiff) {
    for (const std::string text : {"", "same\n", "same", "same\r\n"})
        EXPECT_TRUE(fs_tools::diff("file", text, text).empty());
}

TEST(UnifiedDiffTest, SingleReplacementHasOnlyThreeLinesOfContext) {
    const auto before = numberedLines(15);
    auto after = before;
    after.replace(after.find("line 8\n"), 7, "updated\n");
    const auto diff = fs_tools::diff("file", before, after);
    EXPECT_EQ(diff, HEADERS + "@@ -5,7 +5,7 @@\n line 5\n line 6\n line 7\n-line 8\n+updated\n line 9\n line 10\n line 11\n");
    EXPECT_EQ(applyDiff(before, diff), after);
}

TEST(UnifiedDiffTest, DistantChangesHaveSeparateHunksAndCorrectOffsets) {
    const auto before = numberedLines(30);
    auto after = before;
    after.insert(after.find("line 5\n"), "inserted one\ninserted two\n");
    after.replace(after.find("line 25\n"), 8, "updated\n");
    const auto diff = fs_tools::diff("file", before, after);
    EXPECT_EQ(hunkCount(diff), 2);
    EXPECT_NE(diff.find("@@ -2,6 +2,8 @@"), std::string::npos);
    EXPECT_NE(diff.find("@@ -22,7 +24,7 @@"), std::string::npos);
    EXPECT_EQ(changedLines(diff), 4);
    EXPECT_EQ(applyDiff(before, diff), after);
}

TEST(UnifiedDiffTest, NearbyContextIsMergedWithoutDuplicatingLines) {
    for (const auto gap : {0, 1, 6, 7}) {
        const auto before = numberedLines(24);
        auto after = before;
        const auto second = "line " + std::to_string(5 + gap) + '\n';
        after.replace(after.find(second), second.size(), "second\n");
        after.replace(after.find("line 4\n"), 7, "first\n");
        const auto diff = fs_tools::diff("file", before, after);
        EXPECT_EQ(hunkCount(diff), gap <= 6 ? 1 : 2) << gap;
        EXPECT_EQ(changedLines(diff), 4);
        EXPECT_EQ(applyDiff(before, diff), after);
    }
}

TEST(UnifiedDiffTest, EmptyFilesAndSingleLineRangesUseUnifiedCoordinates) {
    EXPECT_EQ(fs_tools::diff("file", "", "new\n"), HEADERS + "@@ -0,0 +1 @@\n+new\n");
    EXPECT_EQ(fs_tools::diff("file", "old\n", ""), HEADERS + "@@ -1 +0,0 @@\n-old\n");
    EXPECT_EQ(fs_tools::diff("file", "old\n", "new\n"), HEADERS + "@@ -1 +1 @@\n-old\n+new\n");
    EXPECT_EQ(fs_tools::diff("file", "", "\n"), HEADERS + "@@ -0,0 +1 @@\n+\n");
}

TEST(UnifiedDiffTest, BoundaryInsertionsAndDeletionsAreReplayable) {
    const auto text = numberedLines(12);
    const std::vector<std::pair<std::string, std::string>> cases{
        {text, "start\n" + text}, {text, text + "end\n"},
        {"start\n" + text, text}, {text + "end\n", text},
        {"a\nb\na\nb\nc\n", "b\na\nb\na\nc\n"},
    };
    for (const auto& [before, after] : cases) {
        const auto diff = fs_tools::diff("file", before, after);
        EXPECT_EQ(applyDiff(before, diff), after);
        EXPECT_EQ(changedLines(diff), minimalChanges(before, after));
    }
}

TEST(UnifiedDiffTest, MissingFinalNewlinesAreComparedAndAnnotated) {
    EXPECT_EQ(fs_tools::diff("file", "same", "same\n"),
        HEADERS + "@@ -1 +1 @@\n-same\n\\ No newline at end of file\n+same\n");
    const std::vector<std::pair<std::string, std::string>> cases{
        {"same\n", "same"}, {"old", "new"}, {"", "new"}, {"old", ""},
        {"old\nlast", "new\nlast"}, {"last", "first\nlast"},
    };
    for (const auto& [before, after] : cases)
        EXPECT_EQ(applyDiff(before, fs_tools::diff("file", before, after)), after);
}

TEST(UnifiedDiffTest, PreservesCrLfBomAndUtf8InContext) {
    const std::string before = "\xef\xbb\xbf" "Titre\r\nété\r\nancien\r\nfin\r\n";
    const std::string after = "\xef\xbb\xbf" "Titre\r\nété\r\nnouveau\r\nfin\r\n";
    const auto diff = fs_tools::diff("file", before, after);
    EXPECT_EQ(changedLines(diff), 2);
    EXPECT_NE(diff.find(" été\r\n"), std::string::npos);
    EXPECT_EQ(applyDiff(before, diff), after);
    EXPECT_EQ(applyDiff(before, fs_tools::diff("file", before, "Titre\nété\nnouveau\nfin\n")), "Titre\nété\nnouveau\nfin\n");
}

TEST(UnifiedDiffTest, RandomRepeatedLinesProduceMinimalReplayableDiffs) {
    std::mt19937 random(1847);
    const auto text = [&] {
        std::string result;
        const auto size = random() % 25;
        for (std::size_t i = 0; i < size; ++i) result += std::to_string(random() % 5) + '\n';
        if (!result.empty() && random() % 2) result.pop_back();
        return result;
    };
    for (int iteration = 0; iteration < 1500; ++iteration) {
        const auto before = text(), after = text();
        const auto diff = fs_tools::diff("file", before, after);
        SCOPED_TRACE(iteration);
        EXPECT_EQ(applyDiff(before, diff), after);
        EXPECT_EQ(changedLines(diff), minimalChanges(before, after));
    }
}

TEST(UnifiedDiffTest, LargeFileWithSparseEditsStaysCompact) {
    const auto before = numberedLines(20000);
    auto after = before;
    after.replace(after.find("line 19000\n"), 11, "last change\n");
    after.replace(after.find("line 1000\n"), 10, "first change\n");
    const auto diff = fs_tools::diff("file", before, after);
    EXPECT_EQ(hunkCount(diff), 2);
    EXPECT_EQ(changedLines(diff), 4);
    EXPECT_LT(diff.size(), 500);
    EXPECT_EQ(applyDiff(before, diff), after);
}

TEST(UnifiedDiffTest, EntirelyDifferentFilesAreStillReportedInFull) {
    const auto before = numberedLines(10000);
    std::string after;
    for (int i = 0; i < 10000; ++i) after += "replacement " + std::to_string(i) + '\n';
    const auto diff = fs_tools::diff("file", before, after);
    EXPECT_EQ(hunkCount(diff), 1);
    EXPECT_EQ(changedLines(diff), 20000);
    EXPECT_EQ(applyDiff(before, diff), after);
}
}
