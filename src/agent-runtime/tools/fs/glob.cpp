#include "glob.hpp"
#include "common.hpp"
#include "../../core/process.hpp"

#include <stdexcept>

void from_json(const nlohmann::json& json, GlobArguments& value) {
    json.at("pattern").get_to(value.pattern);
    value.path = json.contains("path") ? std::optional(json.at("path").get<std::string>()) : std::nullopt;
}

GlobTool::GlobTool(std::filesystem::path ripgrep) : ripgrep(std::move(ripgrep)) {}

ToolDefinition GlobTool::getDefinition() const {
    return {"glob", "Find files by glob pattern, e.g. **/*.cpp. Returns up to 100 paths. Use path to restrict the search directory.",
        {{"type", "object"}, {"properties", {{"pattern", {{"type", "string"}}}, {"path", {{"type", "string"}}}}}, {"required", {"pattern"}}}};
}

ToolResult GlobTool::execute(const nlohmann::json& arguments, const ToolContext& context) const {
    const auto params = arguments.get<GlobArguments>();
    const auto path = fs_tools::resolve(context, params.path.value_or(context.directory.string()));
    if (!std::filesystem::is_directory(path)) throw std::invalid_argument("glob path must be a directory: " + path.string());
    fs_tools::checkExternal(context, path, true);
    context.ask({"glob", {params.pattern}, {"*"}, {{"pattern", params.pattern}, {"path", path.string()}}});
    std::vector<std::string> files;
    const auto result = core::runProcess(ripgrep, {"--no-config", "--files", "--glob=" + params.pattern,
        "--glob=!**/.git/**", "."}, path, context.checkCancelled, [&](const std::string& line) {
            files.push_back((path / line).lexically_normal().string());
            return files.size() < 100;
        });
    if (!result.stopped && result.exitCode != 0 && result.exitCode != 1)
        throw std::runtime_error("glob failed: " + result.error);
    std::string output = files.empty() ? "No files found" : fs_tools::join(files);
    if (result.stopped) output += "\n(Results truncated: showing first 100 results. Use a more specific path or pattern.)";
    return {fs_tools::relative(context, path), output, {{"count", files.size()}, {"truncated", result.stopped}}, {}};
}
