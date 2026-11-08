#include "apply_patch.hpp"
#include "common.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
struct Change {
    fs::path path;
    fs::path target;
    std::string type;
    std::string before;
    std::string after;
};

std::size_t locate(const std::vector<std::string>& source, const std::vector<std::string>& needle,
    std::size_t start, bool eof) {
    if (needle.empty()) return source.size();
    for (const bool trim : {false, true}) {
        for (std::size_t i = start; i <= source.size(); ++i) {
            if (needle.size() > source.size() - i) break;
            if (eof && i + needle.size() != source.size()) continue;
            bool found = true;
            for (std::size_t j = 0; j < needle.size(); ++j) {
                if ((trim ? fs_tools::trim(source[i + j]) : source[i + j]) !=
                    (trim ? fs_tools::trim(needle[j]) : needle[j])) { found = false; break; }
            }
            if (found) return i;
        }
    }
    throw std::runtime_error("Patch context does not match file contents");
}

std::string update(const std::vector<std::string>& patch, std::size_t& index, const std::string& before) {
    auto source = fs_tools::lines(fs_tools::normalizeLines(fs_tools::withoutBom(before)));
    std::size_t cursor = 0;
    bool changed = false;
    while (index < patch.size() && !patch[index].starts_with("*** ")) {
        if (patch[index] != "@@" && !patch[index].starts_with("@@ "))
            throw std::invalid_argument("Expected @@ before patch changes");
        const auto anchor = patch[index].size() > 3 ? patch[index].substr(3) : "";
        ++index;
        if (!anchor.empty()) cursor = locate(source, {anchor}, cursor, false) + 1;
        std::vector<std::string> oldLines;
        std::vector<std::string> newLines;
        while (index < patch.size() && !patch[index].starts_with("*** ") && !patch[index].starts_with("@@")) {
            const auto& line = patch[index++];
            if (line.empty() || line[0] == ' ') {
                const auto content = line.empty() ? "" : line.substr(1);
                oldLines.push_back(content); newLines.push_back(content);
            } else if (line[0] == '-') oldLines.push_back(line.substr(1));
            else if (line[0] == '+') newLines.push_back(line.substr(1));
            else throw std::invalid_argument("Patch lines must start with space, + or -");
        }
        const bool eof = index < patch.size() && patch[index] == "*** End of File";
        if (eof) ++index;
        if (oldLines.empty() && newLines.empty()) throw std::invalid_argument("Empty patch hunk");
        const auto position = locate(source, oldLines, cursor, eof);
        source.erase(source.begin() + position, source.begin() + position + oldLines.size());
        source.insert(source.begin() + position, newLines.begin(), newLines.end());
        cursor = position + newLines.size();
        changed = true;
    }
    if (!changed) throw std::invalid_argument("Update patch must contain a hunk");
    return fs_tools::withOriginalFormat(fs_tools::join(source), before);
}
}

void from_json(const nlohmann::json& json, ApplyPatchArguments& value) { json.at("patchText").get_to(value.patchText); }

ToolDefinition ApplyPatchTool::getDefinition() const {
    return fs_tools::withExplanation({"apply_patch", "Apply a file-oriented patch enclosed by *** Begin Patch and *** End Patch. "
        "Use *** Add File: path (all content lines prefixed +), *** Delete File: path, or *** Update File: path. "
        "Updates contain @@ hunks with space-prefixed context, - removals and + additions; optional @@ anchor locates a preceding line. "
        "An update may include *** Move to: path before its hunks. *** End of File anchors a hunk at EOF. "
        "Existing add/move destinations and repeated paths are rejected. Read files before patching.",
        {{"type", "object"}, {"properties", {{"patchText", {{"type", "string"}}}}}, {"required", {"patchText"}}}}, explained);
}

ToolResult ApplyPatchTool::execute(const nlohmann::json& arguments, const ToolContext& context) const {
    const auto explanation = fs_tools::explanation(arguments, explained);
    const auto params = arguments.get<ApplyPatchArguments>();
    const auto patch = fs_tools::lines(fs_tools::normalizeLines(params.patchText));
    if (patch.size() < 3 || patch.front() != "*** Begin Patch" || patch.back() != "*** End Patch")
        throw std::invalid_argument("Expected a nonempty patch enclosed by Begin Patch and End Patch");
    std::unique_lock lock(fs_tools::mutationMutex());
    std::vector<Change> changes;
    std::set<fs::path> paths;
    const auto claim = [&](const fs::path& path) {
        if (!paths.insert(fs::weakly_canonical(path)).second) throw std::invalid_argument("Repeated patch path: " + path.string());
    };
    std::size_t index = 1;
    while (index < patch.size() - 1) {
        const auto header = patch[index++];
        Change change;
        if (header.starts_with("*** Add File: ")) { change.type = "add"; change.path = fs_tools::resolve(context, header.substr(14)); }
        else if (header.starts_with("*** Update File: ")) { change.type = "update"; change.path = fs_tools::resolve(context, header.substr(17)); }
        else if (header.starts_with("*** Delete File: ")) { change.type = "delete"; change.path = fs_tools::resolve(context, header.substr(17)); }
        else throw std::invalid_argument("Invalid patch file header: " + header);
        claim(change.path);
        lock.unlock();
        fs_tools::checkExternal(context, change.path);
        lock.lock();
        change.target = change.path;
        if (change.type == "add") {
            if (fs::exists(change.path)) throw std::runtime_error("Add destination already exists: " + change.path.string());
            while (index < patch.size() - 1 && !patch[index].starts_with("*** ")) {
                if (!patch[index].starts_with('+')) throw std::invalid_argument("Added file lines must start with +");
                change.after += patch[index++].substr(1) + '\n';
            }
        } else {
            change.before = fs_tools::readText(change.path);
            if (change.type == "update") {
                if (index < patch.size() && patch[index].starts_with("*** Move to: ")) {
                    change.target = fs_tools::resolve(context, patch[index++].substr(13));
                    claim(change.target);
                    lock.unlock();
                    fs_tools::checkExternal(context, change.target);
                    lock.lock();
                    if (fs::exists(change.target)) throw std::runtime_error("Move destination already exists: " + change.target.string());
                    change.type = "move";
                }
                change.after = update(patch, index, change.before);
            }
        }
        changes.push_back(std::move(change));
    }
    std::vector<std::string> patterns;
    nlohmann::json files = nlohmann::json::array();
    std::string totalDiff;
    for (const auto& change : changes) {
        patterns.push_back(fs_tools::relative(context, change.path));
        if (change.path != change.target) patterns.push_back(fs_tools::relative(context, change.target));
        const auto diff = fs_tools::diff(change.path, change.before, change.after);
        files.push_back({{"filePath", change.path.string()}, {"relativePath", fs_tools::relative(context, change.target)},
            {"type", change.type}, {"patch", diff}, {"movePath", change.target.string()}});
        totalDiff += diff;
    }
    lock.unlock();
    context.ask({"edit", patterns, {"*"}, {{"diff", totalDiff}, {"files", files}}});
    lock.lock();
    fs_tools::checkpoint(context);
    // Check every source again before the first write.
    for (const auto& change : changes) {
        if (change.type == "add" ? fs::exists(change.path) : fs_tools::readText(change.path) != change.before)
            throw std::runtime_error("File changed while awaiting permission: " + change.path.string());
        if (change.type == "move" && fs::exists(change.target)) throw std::runtime_error("Move destination appeared during permission request");
    }
    std::string output = "Success. Updated the following files:\n";
    std::vector<fs::path> affected;
    for (const auto& change : changes) {
        affected.push_back(change.path);
        if (change.target != change.path) affected.push_back(change.target);
    }
    fs_tools::mutate(context, affected, [&] {
        for (const auto& change : changes) {
            if (change.type != "delete") fs_tools::writeText(change.target, change.after);
            if (change.type == "delete" || change.type == "move") fs::remove(change.path);
        }
    }, explanation);
    for (const auto& change : changes) {
        output += (change.type == "add" ? "A " : change.type == "delete" ? "D " : "M ") +
            fs_tools::relative(context, change.target) + '\n';
    }
    return {output, output, {{"diff", totalDiff}, {"files", files}}, {}};
}
