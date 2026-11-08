#ifndef SOS_EDIT_TOOL_HPP
#define SOS_EDIT_TOOL_HPP

#include "../tool.hpp"

struct EditArguments {
    std::string filePath;
    std::string oldString;
    std::string newString;
    bool replaceAll = false;
};
void from_json(const nlohmann::json& json, EditArguments& arguments);

class EditTool : public Tool {
public:
    explicit EditTool(bool explained = false) : explained(explained) {}
    ToolDefinition getDefinition() const override;
    ToolResult execute(const nlohmann::json& arguments, const ToolContext& context) const override;
private:
    bool explained;
};

#endif // SOS_EDIT_TOOL_HPP
