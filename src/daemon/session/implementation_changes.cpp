#include "implementation_changes.hpp"
#include <agent-runtime/tools/fs/common.hpp>
#include <regex>
#include <sstream>
#include <set>
#include <map>
#include <algorithm>
#include <tuple>

namespace {
const std::regex HEADER(R"(^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@)");
struct Hunk { std::size_t start, count; std::string replacement, patch; };
std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> result;
    for (std::size_t start = 0; start < text.size();) {
        const auto end = text.find('\n', start);
        const auto next = end == std::string::npos ? text.size() : end + 1;
        result.push_back(text.substr(start, next - start)); start = next;
    }
    return result;
}
std::vector<Hunk> hunks(const std::string& patch) {
    std::vector<Hunk> result;
    char previous = 0;
    for (const auto& line : lines(patch)) {
        std::smatch match;
        if (std::regex_search(line, match, HEADER)) {
            const auto count = match[2].matched ? std::stoull(match[2]) : 1;
            const auto start = std::stoull(match[1]);
            result.push_back({count ? start - 1 : start, count, "", line}); previous = 0;
        } else if (!result.empty()) {
            auto& hunk = result.back();
            if (line.starts_with("\\ No newline")) {
                if ((previous == '+' || previous == ' ') && hunk.replacement.ends_with('\n')) hunk.replacement.pop_back();
                hunk.patch += line;
            } else if (line[0] == '+' || line[0] == '-' || line[0] == ' ') {
                if (line[0] != '-') hunk.replacement += line.substr(1);
                hunk.patch += line; previous = line[0];
            }
        }
    }
    return result;
}
}

nlohmann::json implementationChangeEvidence(const std::vector<ImplementationDiff>& diffs) {
    auto evidence = nlohmann::json::array();
    for (const auto& diff : diffs) {
        auto zones = nlohmann::json::array();
        const auto changes = hunks(diff.patch);
        if (changes.empty()) zones.push_back({{"index", 0}, {"patch", diff.patch}});
        else for (std::size_t i = 0; i < changes.size(); ++i) zones.push_back({{"index", i}, {"patch", changes[i].patch}});
        evidence.push_back({{"path", diff.path}, {"hunks", zones}});
    }
    return evidence;
}

namespace {
struct OwnedLine {
    std::string text;
    std::optional<std::size_t> original, owner;
    std::uint64_t identity = 0;
};
struct OwnedFile {
    std::string before;
    std::vector<OwnedLine> current;
    std::map<std::size_t, std::size_t> deleted;
    std::optional<std::size_t> metadataOwner;
    bool opaque = false;
    std::uint64_t nextIdentity = 1;
};
std::string contents(const std::vector<OwnedLine>& source) {
    std::string result;
    for (const auto& line : source) result += line.text;
    return result;
}
std::string metadata(const std::string& patch) {
    std::string result;
    for (const auto& line : lines(patch))
        if (line.starts_with("old mode ") || line.starts_with("new mode ") || line.starts_with("new file mode ") ||
            line.starts_with("deleted file mode ") || line.starts_with("File kind/mode:")) result += line;
    return result;
}
OwnedFile initialFile(const std::string& before) {
    OwnedFile file;
    file.before = before;
    const auto source = lines(before);
    for (std::size_t i = 0; i < source.size(); ++i) file.current.push_back({source[i], i, {}, file.nextIdentity++});
    return file;
}
void rememberChanges(OwnedFile& file, const ImplementationDiff& diff, std::size_t owner) {
    if (diff.patch.find("GIT binary patch") != std::string::npos || diff.patch.find("Binary contents changed") != std::string::npos ||
        diff.patch.find("Non-UTF-8 contents changed") != std::string::npos) {
        file.opaque = true;
        file.metadataOwner = owner;
        return; // The exact binary state stays in snapshot blobs, not line ownership.
    }
    if (file.opaque) { file.metadataOwner = owner; return; }
    if (contents(file.current) != diff.before) throw std::runtime_error("Construction boundaries are not contiguous: " + diff.path);
    const auto changes = hunks(diff.patch);
    if (!metadata(diff.patch).empty() || changes.empty()) file.metadataOwner = owner;
    if (changes.empty()) {
        if (diff.before != diff.after) throw std::runtime_error("Missing textual construction evidence: " + diff.path);
        return;
    }
    const auto after = lines(diff.after);
    std::vector<OwnedLine> updated;
    std::size_t cursor = 0;
    for (const auto& hunk : changes) {
        if (hunk.start < cursor || hunk.start + hunk.count > file.current.size())
            throw std::runtime_error("Invalid construction boundary: " + diff.path);
        while (cursor < hunk.start) updated.push_back(file.current[cursor++]);
        bool header = true;
        for (const auto& line : lines(hunk.patch)) {
            if (header) { header = false; continue; }
            if (line.starts_with("-")) {
                const auto& removed = file.current.at(cursor++);
                if (removed.original) file.deleted[*removed.original] = owner;
            } else if (line.starts_with("+")) {
                updated.push_back({after.at(updated.size()), {}, owner, file.nextIdentity++});
            } else if (line.starts_with(" ")) {
                auto unchanged = file.current.at(cursor++);
                unchanged.text = after.at(updated.size());
                updated.push_back(std::move(unchanged));
            }
        }
    }
    while (cursor < file.current.size()) updated.push_back(file.current[cursor++]);
    if (contents(updated) != diff.after) throw std::runtime_error("Construction evidence does not match its captured file: " + diff.path);
    file.current = std::move(updated);
}
struct FinalEdit {
    std::size_t start, count, owner;
    std::string added;
    std::uint64_t identity = 0;
};
std::vector<FinalEdit> finalEdits(const OwnedFile& file, const ImplementationDiff& diff) {
    std::vector<FinalEdit> result;
    if (diff.before == diff.after) return result;
    const auto original = lines(file.before);
    std::size_t cursor = 0, position = 0;
    while (cursor < original.size() || position < file.current.size()) {
        const auto begin = position;
        while (position < file.current.size() && !file.current[position].original) ++position;
        const auto end = position < file.current.size() ? *file.current[position].original : original.size();
        if (end < cursor) throw std::runtime_error("Construction line identities are out of order: " + diff.path);
        for (auto removed = cursor; removed < end; ++removed) {
            if (!file.deleted.contains(removed)) throw std::runtime_error("Final deletion has no construction owner: " + diff.path);
            result.push_back({removed, 1, file.deleted.at(removed), ""});
        }
        for (auto added = begin; added < position; ++added) {
            const auto& line = file.current[added];
            if (!line.owner) throw std::runtime_error("Final addition has no construction owner: " + diff.path);
            // Pair replacements with their original positions so applying the
            // first intention cannot move it behind a later, unselected one.
            result.push_back({cursor + std::min(added - begin, end - cursor), 0, *line.owner, line.text, line.identity});
        }
        cursor = end;
        if (position < file.current.size()) { ++position; ++cursor; }
    }
    std::stable_sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return std::tie(left.start, left.count) < std::tie(right.start, right.count);
    });
    return result;
}
std::string intermediate(const std::string& before, const std::vector<FinalEdit>& edits, std::size_t part) {
    const auto original = lines(before);
    std::string result;
    std::size_t cursor = 0;
    for (const auto& edit : edits) {
        while (cursor < edit.start) result += original.at(cursor++);
        if (edit.count) {
            if (edit.owner > part) result += original.at(cursor);
            cursor += edit.count;
        } else if (edit.owner <= part) result += edit.added;
    }
    while (cursor < original.size()) result += original[cursor++];
    return result;
}

std::vector<std::uint64_t> intermediateOrigins(std::size_t size, const std::vector<FinalEdit>& edits,
    std::optional<std::size_t> part) {
    std::vector<std::uint64_t> result;
    std::size_t cursor = 0;
    for (const auto& edit : edits) {
        while (cursor < edit.start) result.push_back(++cursor);
        if (edit.count) {
            if (!part || edit.owner > *part) result.push_back(cursor + 1);
            cursor += edit.count;
        } else if (part && edit.owner <= *part) {
            result.push_back(edit.identity);
        }
    }
    while (cursor < size) result.push_back(++cursor);
    return result;
}

struct OrderedHunk {
    std::set<std::uint64_t> origins;
    std::uint64_t order;
    bool overlaps(const OrderedHunk& other) const {
        return std::any_of(origins.begin(), origins.end(), [&](const auto identity) { return other.origins.contains(identity); });
    }
};

// Replay durable mutations using stable line identities, including lines inserted
// by earlier substeps. Shifting line numbers must never reorder contributions.
class ChangeChronology {
public:
    explicit ChangeChronology(std::size_t count) : histories(count), metadataOrders(count) {}

    void observe(const OwnedFile& file, const ImplementationDiff& mutation) {
        if (file.metadataOwner) {
            auto& order = metadataOrders.at(*file.metadataOwner)[mutation.path];
            if (!order) order = next++;
        }
        if (file.opaque) return;
        const ImplementationDiff net{mutation.path, "", file.before, contents(file.current)};
        const auto edits = finalEdits(file, net);
        const auto originalSize = lines(file.before).size();
        auto previous = file.before;
        auto origins = intermediateOrigins(originalSize, edits, {});
        for (std::size_t part = 0; part < histories.size(); ++part) {
            const auto current = intermediate(file.before, edits, part);
            update(histories[part][mutation.path], fs_tools::diff(mutation.path, previous, current), origins);
            origins = intermediateOrigins(originalSize, edits, part);
            previous = current;
        }
    }

    std::vector<std::uint64_t> orders(std::size_t part, const std::string& path) const {
        std::vector<std::uint64_t> result;
        if (const auto found = histories.at(part).find(path); found != histories.at(part).end())
            for (const auto& hunk : found->second) result.push_back(hunk.order);
        if (result.empty()) {
            const auto& metadata = metadataOrders.at(part);
            if (const auto found = metadata.find(path); found != metadata.end()) result.push_back(found->second);
        }
        return result;
    }

private:
    void update(std::vector<OrderedHunk>& previous, const std::string& patch,
        const std::vector<std::uint64_t>& origins) {
        std::vector<OrderedHunk> current;
        for (const auto& hunk : hunks(patch)) {
            OrderedHunk zone{{}, 0};
            for (auto line = hunk.start; line < hunk.start + hunk.count; ++line) zone.origins.insert(origins.at(line));
            if (zone.origins.empty()) zone.origins.insert(0); // One all-added hunk in an empty source.
            for (const auto& old : previous) if (zone.overlaps(old) && (!zone.order || old.order < zone.order)) zone.order = old.order;
            if (!zone.order) zone.order = next++;
            current.push_back(zone);
        }
        previous = std::move(current); // Reverted zones cease to own a position.
    }

    std::uint64_t next = 1;
    std::vector<std::map<std::string, std::vector<OrderedHunk>>> histories;
    std::vector<std::map<std::string, std::uint64_t>> metadataOrders;
};
}

std::vector<std::vector<ImplementationDiff>> preserveImplementationOwnership(
    const std::vector<ImplementationBoundary>& boundaries, const std::vector<ImplementationDiff>& finalDiffs,
    std::size_t count) {
    std::map<std::string, OwnedFile> files;
    ChangeChronology chronology(count);
    for (const auto& boundary : boundaries) for (const auto& diff : boundary.diffs) {
        if (!boundary.owner || *boundary.owner >= count) throw std::runtime_error("Captured change has no valid substep owner");
        auto [entry, inserted] = files.try_emplace(diff.path, initialFile(diff.before));
        rememberChanges(entry->second, diff, *boundary.owner);
        chronology.observe(entry->second, diff);
    }
    std::vector<std::vector<ImplementationDiff>> result(count);
    for (const auto& diff : finalDiffs) {
        if (!files.contains(diff.path)) throw std::runtime_error("Final change was not captured in a substep: " + diff.path);
        const auto& file = files.at(diff.path);
        if (file.opaque) {
            result.at(*file.metadataOwner).push_back(diff);
            result.at(*file.metadataOwner).back().hunkOrder = chronology.orders(*file.metadataOwner, diff.path);
            continue;
        }
        if (file.before != diff.before || contents(file.current) != diff.after)
            throw std::runtime_error("Files changed after the final implementation boundary: " + diff.path);
        const auto edits = finalEdits(file, diff);
        const auto extra = metadata(diff.patch);
        if (edits.empty()) {
            if (!file.metadataOwner) throw std::runtime_error("File metadata change has no construction owner");
            result.at(*file.metadataOwner).push_back(diff);
            result.at(*file.metadataOwner).back().hunkOrder = chronology.orders(*file.metadataOwner, diff.path);
            continue;
        }
        std::string previous = diff.before;
        for (std::size_t part = 0; part < count; ++part) {
            const auto current = intermediate(diff.before, edits, part);
            const bool ownsMetadata = !extra.empty() && file.metadataOwner == part;
            if (current == previous && !ownsMetadata) continue;
            result[part].push_back({diff.path, fs_tools::diff(diff.path, previous, current) + (ownsMetadata ? extra : ""), previous, current});
            result[part].back().hunkOrder = chronology.orders(part, diff.path);
            previous = current;
        }
        if (previous != diff.after) throw std::runtime_error("Corrected substeps do not reproduce the final file");
    }
    return result;
}
