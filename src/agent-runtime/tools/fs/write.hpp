#ifndef SOS_WRITE_TOOL_HPP
#define SOS_WRITE_TOOL_HPP

#include "../tool.hpp"

struct WriteArguments { std::string filePath; std::string content; };
void from_json(const nlohmann::json& json, WriteArguments& arguments);

class WriteTool : public Tool {
public:
    explicit WriteTool(bool explained = false) : explained(explained) {}
    ToolDefinition getDefinition() const override;
    ToolResult execute(const nlohmann::json& arguments, const ToolContext& context) const override;
private:
    bool explained;
};

#endif // SOS_WRITE_TOOL_HPP
