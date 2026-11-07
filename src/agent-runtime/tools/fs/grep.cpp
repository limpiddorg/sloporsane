#include "grep.hpp"
#include "common.hpp"
#include "../../core/process.hpp"

#include <stdexcept>

namespace {
struct Match {
    std::string path;
    std::size_t line;
    std::string text;
};
void from_json(const nlohmann::json& json, Match& match) {
    const auto& data = json.at("data");
    data.at("path").at("text").get_to(match.path);
    data.at("line_number").get_to(match.line);
    data.at("lines").at("text").get_to(match.text);
}
}

void from_json(const nlohmann::json& json, GrepArguments& value) {
    json.at("pattern").get_to(value.pattern);
    if (value.pattern.empty()) throw std::invalid_argument("pattern is required");
    value.path = json.contains("path") ? std::optional(json.at("path").get<std::string>()) : std::nullopt;
    value.include = json.contains("include") ? std::optional(json.at("include").get<std::string>()) : std::nullopt;
}

GrepTool::GrepTool(std::filesystem::path ripgrep) : ripgrep(std::move(ripgrep)) {}

ToolDefinition GrepTool::getDefinition() const {
    return {"grep", "Search file contents with a regular expression. Returns paths, line numbers and matching text, up to 100 matches. include filters filenames (e.g. *.cpp).",
        {{"type", "object"}, {"properties", {{"pattern", {{"type", "string"}}}, {"path", {{"type", "string"}}},
            {"include", {{"type", "string"}}}}}, {"required", {"pattern"}}}};
}

ToolResult GrepTool::execute(const nlohmann::json& arguments, const ToolContext& context) const {
    const auto params = arguments.get<GrepArguments>();
    const auto path = fs_tools::resolve(context, params.path.value_or(context.directory.string()));
    const bool directory = std::filesystem::is_directory(path);
    if (!directory && !std::filesystem::is_regular_file(path)) throw std::runtime_error("Search path not found: " + path.string());
    fs_tools::checkExternal(context, path, directory);
    context.ask({"grep", {params.pattern}, {"*"}, {{"pattern", params.pattern}, {"path", path.string()}}});
    const auto cwd = directory ? path : path.parent_path();
    std::vector<std::string> args{"--no-config", "--json", "--hidden", "--no-messages", "--glob=!**/.git/**"};
    if (params.include) args.push_back("--glob=" + *params.include);
    args.insert(args.end(), {"--", params.pattern, directory ? "." : path.filename().string()});
    std::vector<Match> matches;
    const auto result = core::runProcess(ripgrep, args, cwd, context.checkCancelled, [&](const std::string& line) {
        const auto record = nlohmann::json::parse(line);
        if (record.at("type") != "match") return true;
        auto match = record.get<Match>();
        match.path = (cwd / match.path).lexically_normal().string();
        while (!match.text.empty() && (match.text.back() == '\n' || match.text.back() == '\r')) match.text.pop_back();
        if (match.text.size() > 2000) {
            std::size_t end = 2000;
            while (end && (static_cast<unsigned char>(match.text[end]) & 0xc0) == 0x80) --end;
            match.text = match.text.substr(0, end) + "...";
        }
        matches.push_back(std::move(match));
        return matches.size() < 100;
    });
    if (!result.stopped && result.exitCode != 0 && result.exitCode != 1)
        throw std::runtime_error("grep failed: " + result.error);
    std::string output = matches.empty() ? "No files found" : "Found " + std::to_string(matches.size()) + " matches\n";
    std::string current;
    for (const auto& match : matches) {
        if (current != match.path) { current = match.path; output += "\n" + current + ":\n"; }
        output += "  Line " + std::to_string(match.line) + ": " + match.text + '\n';
    }
    if (result.stopped) output += "\n(Results truncated. Use a more specific path or pattern.)";
    return {params.pattern, output, {{"matches", matches.size()}, {"truncated", result.stopped}}, {}};
}
