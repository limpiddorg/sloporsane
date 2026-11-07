#ifndef SOS_WEB_FETCH_TOOL_HPP
#define SOS_WEB_FETCH_TOOL_HPP

#include "../tool.hpp"

enum class WebFetchFormat { TEXT, MARKDOWN, HTML };

struct WebFetchArguments {
    std::string url;
    WebFetchFormat format = WebFetchFormat::MARKDOWN;
    double timeout = 30;
};
void from_json(const nlohmann::json& json, WebFetchArguments& arguments);

class WebFetchTool : public Tool {
public:
    ToolDefinition getDefinition() const override;
    ToolResult execute(const nlohmann::json& arguments, const ToolContext& context) const override;
};

#endif // SOS_WEB_FETCH_TOOL_HPP
