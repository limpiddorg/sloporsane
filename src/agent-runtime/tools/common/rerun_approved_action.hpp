#ifndef SOS_RERUN_APPROVED_ACTION_HPP
#define SOS_RERUN_APPROVED_ACTION_HPP

#include "../registry.hpp"

class RerunApprovedActionTool : public Tool {
public:
    explicit RerunApprovedActionTool(const ToolRegistry& tools) : tools(tools) {}
    ToolDefinition getDefinition() const override;
    ToolResult execute(const nlohmann::json& arguments, const ToolContext& context) const override;
private:
    const ToolRegistry& tools;
};

#endif // SOS_RERUN_APPROVED_ACTION_HPP
