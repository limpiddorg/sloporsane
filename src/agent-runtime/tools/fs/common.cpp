#include "common.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace fs_tools {
namespace fs = std::filesystem;
namespace {
bool contains(const fs::path& root, const fs::path& path) {
    auto p = path.begin();
    for (auto r = root.begin(); r != root.end(); ++r, ++p)
        if (p == path.end() || *p != *r) return false;
    return true;
}
}

void checkpoint(const ToolContext& context) { if (context.checkCancelled) context.checkCancelled(); }

fs::path resolve(const ToolContext& context, const std::string& path) {
    if (!context.directory.is_absolute() || !context.worktree.is_absolute() || !context.ask)
        throw std::invalid_argument("Tools require absolute context paths and a permission handler");
    if (path.empty() || path.find('\0') != std::string::npos) throw std::invalid_argument("Invalid empty or null-containing path");
    checkpoint(context);
    return (fs::path(path).is_absolute() ? fs::path(path) : context.directory / path).lexically_normal();
}

void checkExternal(const ToolContext& context, const fs::path& path, bool directory) {
    const auto actual = fs::weakly_canonical(path);
    if (context.permissionRoot) {
        if (contains(fs::weakly_canonical(*context.permissionRoot), actual)) return;
    } else if (contains(fs::weakly_canonical(context.directory), actual) || contains(fs::weakly_canonical(context.worktree), actual)) return;
    const auto parent = directory ? actual : actual.parent_path();
    const auto pattern = (parent / "*").generic_string();
    context.ask({"external_directory", {pattern}, {pattern}, {{"filepath", actual.string()}, {"parentDir", parent.string()}}});
}

std::string relative(const ToolContext& context, const fs::path& path) { return path.lexically_relative(context.worktree).generic_string(); }

std::string readText(const fs::path& path) {
    if (!fs::is_regular_file(path)) throw std::runtime_error("Not a regular file: " + path.string());
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot read file: " + path.string());
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    if (stream.bad()) throw std::runtime_error("Failed to read file: " + path.string());
    auto result = buffer.str();
    if (result.find('\0') != std::string::npos) throw std::runtime_error("Cannot edit binary file: " + path.string());
    return result;
}

void writeText(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    stream.close();
    if (!stream) throw std::runtime_error("Failed to write file: " + path.string());
}

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> result;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) result.push_back(line);
    return result;
}

std::string join(const std::vector<std::string>& values) {
    std::string result;
    for (const auto& line : values) result += line + '\n';
    return result;
}

std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

std::string normalizeLines(const std::string& text) {
    std::string result;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') continue;
        result += text[i];
    }
    return result;
}

std::string withoutBom(const std::string& text) { return text.starts_with("\xef\xbb\xbf") ? text.substr(3) : text; }

std::string withOriginalFormat(const std::string& text, const std::string& original) {
    const auto normalized = normalizeLines(withoutBom(text));
    std::string result = original.starts_with("\xef\xbb\xbf") || text.starts_with("\xef\xbb\xbf") ? "\xef\xbb\xbf" : "";
    const bool crlf = original.find("\r\n") != std::string::npos;
    for (const char c : normalized) {
        if (c == '\n' && crlf) result += '\r';
        result += c;
    }
    return result;
}

std::mutex& mutationMutex() { static std::mutex mutex; return mutex; }
}
