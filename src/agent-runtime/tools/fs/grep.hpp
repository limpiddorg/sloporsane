#ifndef SOS_GREP_TOOL_HPP
#define SOS_GREP_TOOL_HPP

#include "../tool.hpp"
#include <optional>

struct GrepArguments {
    std::string pattern;
    std::optional<std::string> path;
    std::optional<std::string> include;
};
void from_json(const nlohmann::json& json, GrepArguments& arguments);

class GrepTool : public Tool {
public:
    explicit GrepTool(std::filesystem::path ripgrep);
    ToolDefinition getDefinition() const override;
    ToolResult execute(const nlohmann::json& arguments, const ToolContext& context) const override;
private:
    std::filesystem::path ripgrep;
};

#endif // SOS_GREP_TOOL_HPP
