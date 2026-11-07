#include "read.hpp"
#include "common.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
using nlohmann::json;

constexpr std::size_t DEFAULT_READ_LIMIT = 2000;
constexpr std::size_t MAX_LINE_LENGTH = 2000;
constexpr std::size_t MAX_BYTES = 50 * 1024;
constexpr std::size_t SAMPLE_BYTES = 4096;

void checkpoint(const ToolContext& context) {
    if (context.checkCancelled) context.checkCancelled();
}

std::optional<std::size_t> number(const json& json, const char* field) {
    if (!json.contains(field)) return std::nullopt;
    const auto& value = json.at(field);
    if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<std::int64_t>() < 0))
        throw std::invalid_argument(std::string(field) + " must be a nonnegative integer");
    const auto result = value.get<std::uint64_t>();
    if (result > std::numeric_limits<std::size_t>::max()) throw std::invalid_argument("Read range is too large");
    return static_cast<std::size_t>(result);
}

std::string lower(std::string value) {
    for (auto& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

std::string mime(const std::string& sample) {
    if (sample.starts_with(std::string("\x89PNG\r\n\x1a\n", 8))) return "image/png";
    if (sample.starts_with(std::string("\xff\xd8\xff", 3))) return "image/jpeg";
    if (sample.starts_with("GIF87a") || sample.starts_with("GIF89a")) return "image/gif";
    if (sample.size() >= 12 && sample.starts_with("RIFF") && sample.substr(8, 4) == "WEBP") return "image/webp";
    if (sample.starts_with("%PDF-")) return "application/pdf";
    return {};
}

bool binary(const fs::path& path, const std::string& sample) {
    static const std::set<std::string> EXTENSIONS{
        ".zip", ".tar", ".gz", ".exe", ".dll", ".so", ".class", ".jar", ".war", ".7z",
        ".doc", ".docx", ".xls", ".xlsx", ".ppt", ".pptx", ".odt", ".ods", ".odp",
        ".bin", ".dat", ".obj", ".o", ".a", ".lib", ".wasm", ".pyc", ".pyo"
    };
    if (EXTENSIONS.contains(lower(path.extension().string()))) return true;
    std::size_t nonPrintable = 0;
    for (unsigned char c : sample) {
        if (c == 0) return true;
        if (c < 9 || (c > 13 && c < 32)) ++nonPrintable;
    }
    return !sample.empty() && static_cast<double>(nonPrintable) / sample.size() > 0.3;
}

std::string base64(const std::string& bytes) {
    constexpr char ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const auto a = static_cast<unsigned char>(bytes[i]);
        const auto b = i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i + 1]) : 0;
        const auto c = i + 2 < bytes.size() ? static_cast<unsigned char>(bytes[i + 2]) : 0;
        result += ALPHABET[a >> 2];
        result += ALPHABET[((a & 3) << 4) | (b >> 4)];
        result += i + 1 < bytes.size() ? ALPHABET[((b & 15) << 2) | (c >> 6)] : '=';
        result += i + 2 < bytes.size() ? ALPHABET[c & 63] : '=';
    }
    return result;
}

std::string join(const std::vector<std::string>& lines, std::size_t limit = std::numeric_limits<std::size_t>::max()) {
    std::string result;
    for (std::size_t i = 0; i < std::min(lines.size(), limit); ++i) {
        if (i) result += '\n';
        result += lines[i];
    }
    return result;
}

// Bound display length without cutting a UTF-8 code point in half.
std::string shortLine(const std::string& line) {
    std::size_t characters = 0;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if ((static_cast<unsigned char>(line[i]) & 0xc0) == 0x80) continue;
        if (characters++ == MAX_LINE_LENGTH)
            return line.substr(0, i) + "... (line truncated to 2000 chars)";
    }
    return line;
}

[[noreturn]] void missing(const fs::path& path) {
    std::string message = "File not found: " + path.string();
    std::error_code error;
    std::vector<std::string> suggestions;
    const auto base = lower(path.filename().string());
    for (fs::directory_iterator it(path.parent_path(), error), end; !error && it != end; it.increment(error)) {
        const auto name = lower(it->path().filename().string());
        if (name.find(base) != std::string::npos || base.find(name) != std::string::npos)
            suggestions.push_back(it->path().string());
        if (suggestions.size() == 3) break;
    }
    if (!suggestions.empty()) message += "\n\nDid you mean one of these?\n" + join(suggestions);
    throw std::runtime_error(message);
}

ToolResult directory(const fs::path& path, const std::string& title, std::size_t offset,
                     std::size_t limit, const ToolContext& context) {
    std::vector<std::string> entries;
    for (const auto& item : fs::directory_iterator(path)) {
        checkpoint(context);
        std::error_code error;
        entries.push_back(item.path().filename().string() + (item.is_directory(error) ? "/" : ""));
    }
    std::sort(entries.begin(), entries.end());
    const auto start = std::min(offset - 1, entries.size());
    const auto count = std::min(limit, entries.size() - start);
    std::vector<std::string> slice(entries.begin() + start, entries.begin() + start + count);
    const bool truncated = start + count < entries.size();
    std::string output = "<path>" + path.string() + "</path>\n<type>directory</type>\n<entries>\n" + join(slice);
    if (truncated)
        output += "\n\n(Showing " + std::to_string(count) + " of " + std::to_string(entries.size()) +
            " entries. Use 'offset' parameter to read beyond entry " + std::to_string(offset + count) + ")";
    else output += "\n\n(" + std::to_string(entries.size()) + " entries)";
    output += "\n</entries>";
    return {title, output, {{"preview", join(slice, 20)}, {"truncated", truncated}, {"loaded", json::array()},
        {"display", {{"type", "directory"}, {"path", path.string()}, {"entries", slice}, {"offset", offset},
                     {"totalEntries", entries.size()}, {"truncated", truncated}}}}, {}};
}
}

void from_json(const nlohmann::json& json, ReadArguments& arguments) {
    if (!json.is_object()) throw std::invalid_argument("read arguments must be an object");
    json.at("filePath").get_to(arguments.filePath);
    if (arguments.filePath.empty() || arguments.filePath.find('\0') != std::string::npos)
        throw std::invalid_argument("filePath must be a nonempty path without null bytes");
    arguments.offset = number(json, "offset");
    arguments.limit = number(json, "limit");
}

ToolDefinition ReadTool::getDefinition() const {
    return {"read",
        "Read a file or directory from the local filesystem. filePath should be absolute. "
        "Text is returned with line numbers. offset is 1-indexed; limit defaults to 2000. "
        "Use offset and limit to continue reading large files. Lines longer than 2000 characters are truncated. "
        "Directories return sorted entries with / after subdirectories. "
        "Images (JPEG, PNG, GIF, WebP) and PDFs are returned as attachments. Other binary files are rejected.",
        {{"type", "object"}, {"properties", {
            {"filePath", {{"type", "string"}, {"description", "The absolute path to the file or directory to read"}}},
            {"offset", {{"type", "integer"}, {"minimum", 0}, {"description", "Starting line number (1-indexed)"}}},
            {"limit", {{"type", "integer"}, {"minimum", 0}, {"description", "Maximum lines or entries (default 2000)"}}}
        }}, {"required", {"filePath"}}}};
}

ToolResult ReadTool::execute(const nlohmann::json& arguments, const ToolContext& context) const {
    const auto params = arguments.get<ReadArguments>();
    checkpoint(context);
    if (!context.directory.is_absolute() || !context.worktree.is_absolute())
        throw std::invalid_argument("Tool context requires absolute directory and worktree paths");
    if (!context.ask) throw std::invalid_argument("Tool context requires a permission handler");
    const auto path = (fs::path(params.filePath).is_absolute() ? fs::path(params.filePath) :
        context.directory / params.filePath).lexically_normal();
    fs_tools::checkExternal(context, path, fs::is_directory(path));
    const auto title = path.lexically_relative(context.worktree).generic_string();
    context.ask({"read", {title}, {"*"}, json::object()});
    checkpoint(context);
    if (!fs::exists(path)) missing(path);
    const auto offset = std::max<std::size_t>(1, params.offset.value_or(1));
    const auto limit = params.limit.value_or(DEFAULT_READ_LIMIT);
    if (fs::is_directory(path)) return directory(path, title, offset, limit, context);
    if (!fs::is_regular_file(path)) throw std::runtime_error("Not a regular file: " + path.string());
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot read file: " + path.string());
    std::array<char, SAMPLE_BYTES> buffer{};
    file.read(buffer.data(), buffer.size());
    const std::string sample(buffer.data(), static_cast<std::size_t>(file.gcount()));
    if (file.bad()) throw std::runtime_error("Failed to read file: " + path.string());
    file.clear();
    file.seekg(0);
    const auto attachmentMime = mime(sample);
    if (!attachmentMime.empty()) {
        std::string bytes;
        while (file) {
            checkpoint(context);
            file.read(buffer.data(), buffer.size());
            bytes.append(buffer.data(), static_cast<std::size_t>(file.gcount()));
        }
        if (file.bad()) throw std::runtime_error("Failed to read attachment: " + path.string());
        const std::string output = attachmentMime == "application/pdf" ? "PDF read successfully" : "Image read successfully";
        return {title, output, {{"preview", output}, {"truncated", false}, {"loaded", json::array()}},
            {{attachmentMime, "data:" + attachmentMime + ";base64," + base64(bytes)}}};
    }
    if (binary(path, sample)) throw std::runtime_error("Cannot read binary file: " + path.string());
    std::vector<std::string> lines;
    std::size_t count = 0;
    std::size_t bytes = 0;
    bool more = false;
    bool cut = false;
    std::string line;
    while (std::getline(file, line)) {
        checkpoint(context);
        ++count;
        if (count < offset) continue;
        if (lines.size() >= limit) { more = true; continue; }
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto text = shortLine(line);
        const auto size = text.size() + (lines.empty() ? 0 : 1);
        if (size > MAX_BYTES - bytes) { cut = true; more = true; break; }
        lines.push_back(std::move(text));
        bytes += size;
    }
    if (file.bad()) throw std::runtime_error("Failed to read file: " + path.string());
    if (count < offset && !(count == 0 && offset == 1))
        throw std::out_of_range("Offset " + std::to_string(offset) + " is out of range for this file (" + std::to_string(count) + " lines)");
    const auto last = offset - 1 + lines.size();
    std::string output = "<path>" + path.string() + "</path>\n<type>file</type>\n<content>\n";
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i) output += '\n';
        output += std::to_string(offset + i) + ": " + lines[i];
    }
    if (cut)
        output += "\n\n(Output capped at 50 KB. Showing lines " + std::to_string(offset) + "-" +
            std::to_string(last) + ". Use offset=" + std::to_string(last + 1) + " to continue.)";
    else if (more)
        output += "\n\n(Showing lines " + std::to_string(offset) + "-" + std::to_string(last) + " of " +
            std::to_string(count) + ". Use offset=" + std::to_string(last + 1) + " to continue.)";
    else output += "\n\n(End of file - total " + std::to_string(count) + " lines)";
    output += "\n</content>";
    return {title, output, {{"preview", join(lines, 20)}, {"truncated", more || cut}, {"loaded", json::array()},
        {"display", {{"type", "file"}, {"path", path.string()}, {"text", join(lines)}, {"lineStart", offset},
                     {"lineEnd", last}, {"totalLines", count}, {"truncated", more || cut}}}}, {}};
}
