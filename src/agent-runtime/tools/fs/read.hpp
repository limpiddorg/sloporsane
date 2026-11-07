#ifndef SOS_READ_TOOL_HPP
#define SOS_READ_TOOL_HPP

#include "../tool.hpp"

#include <optional>

struct ReadArguments {
    std::string filePath;
    std::optional<std::size_t> offset;
    std::optional<std::size_t> limit;
};

void from_json(const nlohmann::json& json, ReadArguments& arguments);

class ReadTool : public Tool {
public:
    ToolDefinition getDefinition() const override;
    ToolResult execute(const nlohmann::json& arguments, const ToolContext& context) const override;
};

#endif // SOS_READ_TOOL_HPP
