#include "edit.hpp"
#include "common.hpp"

#include <stdexcept>

namespace {
std::string replacement(std::string content, const std::string& old, const std::string& next, bool all) {
    std::string needle = old;
    auto position = content.find(needle);
    if (position == std::string::npos) {
        // Line-wise whitespace fallback, only when it identifies an unambiguous block.
        const auto source = fs_tools::lines(content);
        const auto wanted = fs_tools::lines(old);
        std::vector<std::pair<std::size_t, std::size_t>> candidates;
        std::size_t offset = 0;
        for (std::size_t i = 0; i < source.size(); offset += source[i++].size() + 1) {
            if (wanted.empty() || wanted.size() > source.size() - i) continue;
            bool match = true;
            std::size_t size = 0;
            for (std::size_t j = 0; j < wanted.size(); ++j) {
                if (fs_tools::trim(source[i + j]) != fs_tools::trim(wanted[j])) { match = false; break; }
                size += source[i + j].size() + (j ? 1 : 0);
            }
            if (match) {
                if (old.ends_with('\n') && offset + size < content.size()) ++size;
                candidates.emplace_back(offset, size);
            }
        }
        if (candidates.size() != 1) throw std::runtime_error(candidates.empty() ?
            "Could not find oldString in the file. Re-read it and provide matching text." :
            "Found multiple matches for oldString. Provide more surrounding context.");
        position = candidates[0].first;
        needle = content.substr(position, candidates[0].second);
    }
    if (!all && content.find(needle, position + needle.size()) != std::string::npos)
        throw std::runtime_error("Found multiple matches for oldString. Provide more context or use replaceAll.");
    do {
        content.replace(position, needle.size(), next);
        position = all ? content.find(needle, position + next.size()) : std::string::npos;
    } while (position != std::string::npos);
    return content;
}
}

void from_json(const nlohmann::json& json, EditArguments& value) {
    json.at("filePath").get_to(value.filePath);
    json.at("oldString").get_to(value.oldString);
    json.at("newString").get_to(value.newString);
    if (json.contains("replaceAll") && !json.at("replaceAll").is_boolean()) throw std::invalid_argument("replaceAll must be a boolean");
    value.replaceAll = json.value("replaceAll", false);
}

ToolDefinition EditTool::getDefinition() const {
    return {"edit", "Replace oldString with newString in a file. oldString must identify a unique occurrence unless replaceAll is true. Read the file first. Empty oldString creates a new file only; use write to overwrite a whole file.",
        {{"type", "object"}, {"properties", {{"filePath", {{"type", "string"}}}, {"oldString", {{"type", "string"}}},
            {"newString", {{"type", "string"}}}, {"replaceAll", {{"type", "boolean"}}}}},
            {"required", {"filePath", "oldString", "newString"}}}};
}

ToolResult EditTool::execute(const nlohmann::json& arguments, const ToolContext& context) const {
    const auto params = arguments.get<EditArguments>();
    if (params.oldString == params.newString) throw std::invalid_argument("No changes: oldString and newString are identical");
    const auto path = fs_tools::resolve(context, params.filePath);
    fs_tools::checkExternal(context, path);
    std::unique_lock lock(fs_tools::mutationMutex());
    const bool exists = std::filesystem::exists(path);
    if (exists && params.oldString.empty()) throw std::invalid_argument("oldString cannot be empty for an existing file");
    if (!exists && !params.oldString.empty()) throw std::runtime_error("File not found: " + path.string());
    const auto before = exists ? fs_tools::readText(path) : "";
    const auto changed = !exists ? params.newString : replacement(fs_tools::normalizeLines(fs_tools::withoutBom(before)),
        fs_tools::normalizeLines(params.oldString), fs_tools::normalizeLines(params.newString), params.replaceAll);
    const auto after = fs_tools::withOriginalFormat(changed, before);
    const auto diff = fs_tools::diff(path, before, after);
    lock.unlock();
    context.ask({"edit", {fs_tools::relative(context, path)}, {"*"}, {{"filepath", path.string()}, {"exists", exists}, {"diff", diff}}});
    lock.lock();
    fs_tools::checkpoint(context);
    if (std::filesystem::exists(path) != exists || (exists && fs_tools::readText(path) != before))
        throw std::runtime_error("File changed while awaiting permission: " + path.string());
    fs_tools::mutate(context, {path}, [&] { fs_tools::writeText(path, after); });
    return {fs_tools::relative(context, path), "Edit applied successfully.", {{"diff", diff}, {"filepath", path.string()}}, {}};
}
