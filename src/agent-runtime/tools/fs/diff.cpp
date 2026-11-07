#include "common.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace fs_tools {
namespace {
constexpr std::size_t CONTEXT_LINES = 3;
using Lines = std::span<const std::string_view>;

enum class Change : char { CONTEXT = ' ', REMOVE = '-', ADD = '+' };
struct DiffLine {
    Change change;
    std::string_view text;
};
struct Split {
    std::size_t before;
    std::size_t after;
};
struct Hunk {
    std::size_t begin;
    std::size_t end;
};
struct LineCounts {
    std::size_t before = 0;
    std::size_t after = 0;
};

std::vector<std::string_view> splitLines(std::string_view text) {
    std::vector<std::string_view> result;
    for (std::size_t start = 0; start < text.size();) {
        const auto newline = text.find('\n', start);
        const auto end = newline == std::string_view::npos ? text.size() : newline + 1;
        // Keep the terminator: adding/removing the final newline is a real change.
        result.push_back(text.substr(start, end - start));
        start = end;
    }
    return result;
}

bool haveCommonLine(Lines before, Lines after) {
    if (before.size() > after.size()) std::swap(before, after);
    const std::unordered_set<std::string_view> oldLines(before.begin(), before.end());
    return std::any_of(after.begin(), after.end(), [&](auto line) { return oldLines.contains(line); });
}

std::optional<Split> findMiddle(Lines before, Lines after) {
    // Myers' bidirectional search: retain only two frontiers, not a quadratic
    // LCS table or a copy of the frontier at every edit distance.
    using Index = std::ptrdiff_t;
    const auto n = static_cast<Index>(before.size());
    const auto m = static_cast<Index>(after.size());
    const Index limit = (n + m + 1) / 2;
    const Index offset = limit + 1;
    const Index delta = n - m;
    const bool odd = delta % 2 != 0;
    std::vector<Index> forward(2 * limit + 3, -1), reverse(2 * limit + 3, -1);
    forward[offset + 1] = reverse[offset + 1] = 0;

    for (Index distance = 0; distance < limit; ++distance) {
        for (Index diagonal = -distance; diagonal <= distance; diagonal += 2) {
            const Index index = offset + diagonal;
            Index x = diagonal == -distance || (diagonal != distance && forward[index - 1] < forward[index + 1])
                ? forward[index + 1] : forward[index - 1] + 1;
            Index y = x - diagonal;
            while (x < n && y < m && before[x] == after[y]) { ++x; ++y; }
            forward[index] = x;
            const Index opposite = delta - diagonal;
            if (odd && x <= n && y <= m && opposite >= -(distance - 1) && opposite <= distance - 1 &&
                reverse[offset + opposite] >= 0 && x + reverse[offset + opposite] >= n)
                return Split{static_cast<std::size_t>(x), static_cast<std::size_t>(y)};
        }
        for (Index diagonal = -distance; diagonal <= distance; diagonal += 2) {
            const Index index = offset + diagonal;
            Index x = diagonal == -distance || (diagonal != distance && reverse[index - 1] < reverse[index + 1])
                ? reverse[index + 1] : reverse[index - 1] + 1;
            Index y = x - diagonal;
            while (x < n && y < m && before[n - x - 1] == after[m - y - 1]) { ++x; ++y; }
            reverse[index] = x;
            const Index opposite = delta - diagonal;
            if (!odd && x <= n && y <= m && opposite >= -distance && opposite <= distance &&
                forward[offset + opposite] >= 0 && forward[offset + opposite] + x >= n) {
                const Index splitX = forward[offset + opposite];
                return Split{static_cast<std::size_t>(splitX), static_cast<std::size_t>(splitX - opposite)};
            }
        }
    }
    return std::nullopt;
}

class LineDiff {
public:
    std::vector<DiffLine> result;

    void compare(Lines before, Lines after) {
        std::size_t prefix = 0;
        while (prefix < before.size() && prefix < after.size() && before[prefix] == after[prefix]) ++prefix;
        append(before.first(prefix), Change::CONTEXT);
        before = before.subspan(prefix);
        after = after.subspan(prefix);
        std::size_t suffix = 0;
        while (suffix < before.size() && suffix < after.size() &&
               before[before.size() - suffix - 1] == after[after.size() - suffix - 1]) ++suffix;
        compareMiddle(before.first(before.size() - suffix), after.first(after.size() - suffix));
        append(before.last(suffix), Change::CONTEXT);
    }

private:
    void append(Lines lines, Change change) {
        for (const auto line : lines) result.push_back({change, line});
    }

    void replace(Lines before, Lines after) {
        append(before, Change::REMOVE);
        append(after, Change::ADD);
    }

    void compareMiddle(Lines before, Lines after) {
        if (before.empty() || after.empty()) return replace(before, after);
        if (before.size() == 1 || after.size() == 1) {
            const bool removal = after.size() == 1;
            const auto longer = removal ? before : after;
            const auto shorter = removal ? after : before;
            const auto match = std::find(longer.begin(), longer.end(), shorter.front());
            if (match == longer.end()) return replace(before, after);
            const auto index = static_cast<std::size_t>(match - longer.begin());
            const auto change = removal ? Change::REMOVE : Change::ADD;
            append(longer.first(index), change);
            append(shorter, Change::CONTEXT);
            append(longer.subspan(index + 1), change);
            return;
        }
        // A complete rewrite without any shared line needs no edit-path search.
        if (!haveCommonLine(before, after)) return replace(before, after);
        const auto split = findMiddle(before, after);
        if (!split) return replace(before, after);
        compare(before.first(split->before), after.first(split->after));
        compare(before.subspan(split->before), after.subspan(split->after));
    }
};

void orderReplacements(std::vector<DiffLine>& lines) {
    for (auto begin = lines.begin(); begin != lines.end();) {
        if (begin->change == Change::CONTEXT) { ++begin; continue; }
        const auto end = std::find_if(begin, lines.end(), [](const auto& line) { return line.change == Change::CONTEXT; });
        std::stable_partition(begin, end, [](const auto& line) { return line.change == Change::REMOVE; });
        begin = end;
    }
}

std::vector<Hunk> contextHunks(const std::vector<DiffLine>& lines) {
    std::vector<Hunk> hunks;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].change == Change::CONTEXT) continue;
        const auto begin = i > CONTEXT_LINES ? i - CONTEXT_LINES : 0;
        const auto end = std::min(lines.size(), i + CONTEXT_LINES + 1);
        if (!hunks.empty() && begin <= hunks.back().end) hunks.back().end = end;
        else hunks.push_back({begin, end});
    }
    return hunks;
}

LineCounts countLines(std::span<const DiffLine> lines) {
    LineCounts counts;
    for (const auto& line : lines) {
        counts.before += line.change != Change::ADD;
        counts.after += line.change != Change::REMOVE;
    }
    return counts;
}

std::string range(std::size_t start, std::size_t count) {
    return std::to_string(count ? start + 1 : start) + (count == 1 ? "" : "," + std::to_string(count));
}

std::string formatDiff(const std::filesystem::path& path, const std::vector<DiffLine>& lines) {
    std::string result = "--- " + path.string() + "\n+++ " + path.string() + "\n";
    const std::span<const DiffLine> all(lines);
    LineCounts position;
    std::size_t cursor = 0;
    for (const auto& hunk : contextHunks(lines)) {
        const auto skipped = countLines(all.subspan(cursor, hunk.begin - cursor));
        position.before += skipped.before;
        position.after += skipped.after;
        const auto body = all.subspan(hunk.begin, hunk.end - hunk.begin);
        const auto counts = countLines(body);
        result += "@@ -" + range(position.before, counts.before) + " +" + range(position.after, counts.after) + " @@\n";
        for (const auto& line : body) {
            result += static_cast<char>(line.change);
            result += line.text;
            if (!line.text.ends_with('\n')) result += "\n\\ No newline at end of file\n";
        }
        position.before += counts.before;
        position.after += counts.after;
        cursor = hunk.end;
    }
    return result;
}
}

std::string diff(const std::filesystem::path& path, const std::string& before, const std::string& after) {
    if (before == after) return {};
    const auto oldLines = splitLines(before);
    const auto newLines = splitLines(after);
    LineDiff changes;
    changes.compare(oldLines, newLines);
    orderReplacements(changes.result);
    return formatDiff(path, changes.result);
}
}
