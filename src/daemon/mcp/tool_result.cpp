#include "tool_result.hpp"
#include <agent-runtime/tools/common/output_capture.hpp>

namespace mcp {
ToolResult renderToolResult(const nlohmann::json& response, const std::filesystem::path& outputDirectory) {
    ToolResult result;
    OutputCapture output(outputDirectory);
    const auto append = [&](const std::string& text) { output.append(text); output.append("\n"); };
    if (response.contains("content")) {
        if (!response.at("content").is_array()) throw std::runtime_error("Invalid MCP tool content");
        for (const auto& item : response.at("content")) {
            const auto type = item.at("type").get<std::string>();
            if (type == "text") append(item.at("text").get<std::string>());
            else if (type == "image") {
                Image image;
                image.mime = item.at("mimeType");
                image.data = item.at("data");
                image.origin = "mcp";
                result.images.push_back(std::move(image));
            } else if (type == "resource" && item.at("resource").contains("text")) {
                append(item.at("resource").at("text").get<std::string>());
            } else if (type == "resource_link") {
                append("MCP resource link (not fetched): " + item.at("uri").get<std::string>());
            } else append("[Unsupported MCP content omitted]");
        }
    }
    if ((!response.contains("content") || response.at("content").empty()) && response.contains("structuredContent"))
        append(response.at("structuredContent").dump());
    validateImages(result.images);
    result.output = output.finish();
    result.metadata["truncated"] = output.isTruncated();
    if (output.isTruncated()) {
        result.metadata["outputPath"] = output.getPath().string();
        result.output = "Full MCP output saved to: " + output.getPath().string() + "\n\n" + result.output;
    }
    if (response.contains("isError") && !response.at("isError").is_boolean())
        throw std::runtime_error("Invalid MCP tool error flag");
    if (response.value("isError", false))
        throw std::runtime_error(result.output.empty() ? "MCP tool reported an error" : result.output);
    return result;
}
}
