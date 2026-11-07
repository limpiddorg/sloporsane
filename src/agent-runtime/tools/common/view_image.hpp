#ifndef SOS_VIEW_IMAGE_HPP
#define SOS_VIEW_IMAGE_HPP
#include "../tool.hpp"
class ViewImageTool : public Tool {
public:
    ToolDefinition getDefinition() const override;
    ToolResult execute(const nlohmann::json& arguments, const ToolContext& context) const override;
};
#endif // SOS_VIEW_IMAGE_HPP
