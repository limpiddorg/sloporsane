#ifndef SOS_BASH_TOOL_HPP
#define SOS_BASH_TOOL_HPP

#include "../tool.hpp"
#include <optional>

struct BashArguments {
    std::string command;
    std::string mainCommand;
    std::optional<std::string> workdir;
    std::int64_t timeout = 120000;
};
void from_json(const nlohmann::json& json, BashArguments& arguments);

class BashTool : public Tool {
public:
    ToolDefinition getDefinition() const override;
    ToolResult execute(const nlohmann::json& arguments, const ToolContext& context) const override;
};

class WaitTool : public BashTool {
public:
    ToolDefinition getDefinition() const override;
    ToolResult execute(const nlohmann::json& arguments, const ToolContext& context) const override;
};

#endif // SOS_BASH_TOOL_HPP
