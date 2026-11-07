#ifndef SOS_APPLY_PATCH_TOOL_HPP
#define SOS_APPLY_PATCH_TOOL_HPP

#include "../tool.hpp"

struct ApplyPatchArguments { std::string patchText; };
void from_json(const nlohmann::json& json, ApplyPatchArguments& arguments);

class ApplyPatchTool : public Tool {
public:
    ToolDefinition getDefinition() const override;
    ToolResult execute(const nlohmann::json& arguments, const ToolContext& context) const override;
};

#endif // SOS_APPLY_PATCH_TOOL_HPP
