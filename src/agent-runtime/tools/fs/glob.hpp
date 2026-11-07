#ifndef SOS_GLOB_TOOL_HPP
#define SOS_GLOB_TOOL_HPP

#include "../tool.hpp"
#include <optional>

struct GlobArguments {
    std::string pattern;
    std::optional<std::string> path;
};
void from_json(const nlohmann::json& json, GlobArguments& arguments);

class GlobTool : public Tool {
public:
    explicit GlobTool(std::filesystem::path ripgrep);
    ToolDefinition getDefinition() const override;
    ToolResult execute(const nlohmann::json& arguments, const ToolContext& context) const override;
private:
    std::filesystem::path ripgrep;
};

#endif // SOS_GLOB_TOOL_HPP
