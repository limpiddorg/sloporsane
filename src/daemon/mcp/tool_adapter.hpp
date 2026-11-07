#ifndef SOS_MCP_TOOL_ADAPTER_HPP
#define SOS_MCP_TOOL_ADAPTER_HPP

#include "manager.hpp"
#include <agent-runtime/tools/tool.hpp>

namespace mcp {
// Binds a definition to its approved server identity. Its runtime ID also
// includes the definition fingerprint, so old manual grants cannot target a
// changed tool after reconnecting.
class ToolAdapter final : public ::Tool {
public:
    ToolAdapter(std::shared_ptr<Manager> manager, const ServerStatus& server, ToolDefinition definition);
    ::ToolDefinition getDefinition() const override;
    ToolResult execute(const nlohmann::json& arguments, const ToolContext& context) const override;
private:
    std::shared_ptr<Manager> manager;
    std::string serverName, identity, id;
    bool local;
    ToolDefinition definition;
};

std::vector<std::unique_ptr<::Tool>> makeTools(const std::shared_ptr<Manager>& manager);
}

#endif // SOS_MCP_TOOL_ADAPTER_HPP
