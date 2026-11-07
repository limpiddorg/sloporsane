#ifndef SOS_MCP_TOOL_RESULT_HPP
#define SOS_MCP_TOOL_RESULT_HPP

#include <agent-runtime/tools/tool.hpp>

namespace mcp {
// Untrusted protocol result -> bounded runtime output and validated images.
// No network, permission or connection state; links are never fetched.
ToolResult renderToolResult(const nlohmann::json& response, const std::filesystem::path& outputDirectory);
}

#endif // SOS_MCP_TOOL_RESULT_HPP
