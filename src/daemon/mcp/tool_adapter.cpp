#include "tool_adapter.hpp"
#include <agent-runtime/tools/fs/common.hpp>
#include "tool_result.hpp"
#include "fingerprint.hpp"

namespace mcp {
namespace {
using Json = nlohmann::json;
std::string prefix(const std::string& name) {
    std::string result;
    for (const unsigned char ch : name.substr(0, 12))
        result += (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ? ch : '_';
    return result;
}
std::string bindingId(const std::string& server, const std::string& identity, const ToolDefinition& tool) {
    const auto bytes = Json{{"identity", identity}, {"tool", tool.name}, {"description", tool.description},
        {"schema", tool.inputSchema}, {"read_only", tool.readOnly}}.dump();
    return "mcp_" + prefix(server) + "_" + prefix(tool.name) + "_" + fingerprint(bytes).substr(0, 24);
}
}

ToolAdapter::ToolAdapter(std::shared_ptr<Manager> manager, const ServerStatus& server, ToolDefinition definition)
    : manager(std::move(manager)), serverName(server.server.name), identity(server.identity),
      local(std::holds_alternative<Local>(server.server.connection)), definition(std::move(definition)) {
    if (!this->manager) throw std::invalid_argument("Missing MCP manager");
    id = bindingId(serverName, identity, this->definition);
}

::ToolDefinition ToolAdapter::getDefinition() const {
    return {id, "MCP server " + serverName + ", tool " + definition.name + ". " + definition.description,
        definition.inputSchema};
}

ToolResult ToolAdapter::execute(const Json& arguments, const ToolContext& context) const {
    if (!arguments.is_object()) throw std::invalid_argument("MCP tool arguments must be an object");
    if (!context.ask) throw std::runtime_error("MCP tool permissions are not configured");
    const auto cancelled = [&] {
        if (context.checkCancelled) context.checkCancelled();
        return false;
    };
    core::checkCancellation(cancelled);
    ToolPermissionRequest request{"mcp", {serverName + "/" + definition.name}, {id},
        {{"server", serverName}, {"identity", identity}, {"mcp_tool", definition.name},
         {"read_only_declared", definition.readOnly}, {"arguments", arguments},
         {"working_directory", context.directory.string()}}, {}, id, definition.readOnly};
    context.ask(request);
    core::checkCancellation(cancelled);
    ToolResult result;
    const auto invoke = [&] {
        // Manager rechecks the binding after permission approval; a changed
        // workspace config or revoked trust must not redirect an approved call.
        result = renderToolResult(manager->call(serverName, identity, definition, arguments, cancelled),
            context.worktree / ".sos/tool-output");
    };
    if (local) fs_tools::mutate(context, {}, invoke);
    else invoke();
    result.title = serverName + "/" + definition.name;
    result.metadata.update({{"mcp_server", serverName}, {"mcp_tool", definition.name},
        {"read_only_declared", definition.readOnly}, {"external_effects_undoable", false}});
    return result;
}


std::vector<std::unique_ptr<::Tool>> makeTools(const std::shared_ptr<Manager>& manager) {
    std::vector<std::unique_ptr<::Tool>> result;
    for (const auto& server : manager->list().servers) {
        if (!server.server.enabled || server.state != ConnectionState::CONNECTED) continue;
        for (const auto& tool : server.tools) result.push_back(std::make_unique<ToolAdapter>(manager, server, tool));
    }
    return result;
}
}
