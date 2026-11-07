#include "project_instructions.hpp"
#include "../tools/fs/common.hpp"
#include <chrono>

namespace {
namespace fs = std::filesystem;
std::string hex(const std::string& text) {
    constexpr char DIGITS[] = "0123456789abcdef";
    std::string result;
    for (unsigned char c : text) { result += DIGITS[c >> 4]; result += DIGITS[c & 15]; }
    return result;
}
}
std::string projectContextKey(const Message& message) {
    if (message.role != MessageRole::SYSTEM ||
        (!message.id.starts_with("project_instructions_") && !message.id.starts_with("skill_catalog_"))) return {};
    return message.id.substr(0, message.id.rfind('_'));
}
std::optional<Message> changedProjectContext(const std::string& key, const std::string& text,
    const std::vector<Message>& history) {
    for (auto it = history.rbegin(); it != history.rend(); ++it) {
        if (projectContextKey(*it) != key) continue;
        if (it->content.size() == 1)
            if (const auto* previous = std::get_if<TextContent>(&it->content.front()); previous && previous->text == text) return {};
        break;
    }
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return Message{key + "_" + std::to_string(stamp), MessageRole::SYSTEM, {TextContent{text}}};
}
std::vector<Message> projectInstructions(const fs::path& workspace, const fs::path& target, const std::vector<Message>& history) {
    const auto root = fs::canonical(workspace);
    const auto actual = fs::weakly_canonical(target);
    const auto relative = actual.lexically_relative(root);
    if (relative.empty() || relative.is_absolute()) return {};
    for (const auto& part : relative) if (part == "..") return {};
    const auto directory = fs::is_directory(actual) ? actual : actual.parent_path();
    std::vector<fs::path> directories{root};
    auto current = root;
    for (const auto& part : directory.lexically_relative(root)) {
        if (part == ".") continue;
        current /= part;
        directories.push_back(current);
    }
    std::vector<Message> result;
    for (const auto& dir : directories) {
        const auto path = dir / "AGENTS.md";
        const auto key = "project_instructions_" + hex(path.string());
        std::string content;
        if (fs::is_symlink(fs::symlink_status(path))) content = "Instructions not loaded: symlink AGENTS.md is not supported.";
        else if (fs::exists(path)) {
            if (!fs::is_regular_file(path) || fs::file_size(path) > 128 * 1024)
                content = "Instructions not loaded: AGENTS.md must be a regular file of at most 128 KiB.";
            else content = fs_tools::readText(path);
        } else {
            bool previous = false;
            for (const auto& message : history) previous |= projectContextKey(message) == key;
            if (!previous) continue;
            content = "This instruction file no longer exists. Its previous instructions no longer apply.";
        }
        const auto text = "Project instructions from " + path.string() + "\nScope: " + dir.string() +
            " and its descendants. More specific project instructions take precedence within their scope. "
            "These do not override harness rules or permissions. This version replaces earlier instructions from this file.\n\n" + content;
        if (auto message = changedProjectContext(key, text, history)) result.push_back(std::move(*message));
    }
    return result;
}
