#ifndef SOS_MCP_MANAGEMENT_HPP
#define SOS_MCP_MANAGEMENT_HPP

#include "connections.hpp"

namespace mcp {
enum class ManagementAction { LIST, SAVE, TRUST, CONNECT, DISCONNECT, ENABLE, REMOVE, TOKEN, SECRETS, OAUTH_START, OAUTH_POLL, CATALOGUE, PREVIEW, OAUTH_CLIENT, OAUTH_REVOKE };
struct ManagementRequest {
    ManagementAction action = ManagementAction::LIST;
    std::string name, identity, token;
    bool accepted = false, enabled = false;
    Server server;
    std::map<std::string, std::string> environment;
    ContentSelection selection;
    std::string clientSecret, clientAuthMethod;
};
void from_json(const nlohmann::json& json, ManagementRequest& request);
// Worker-only management boundary. Responses never include credentials or tools' results.
nlohmann::json manage(Connections& connections, const std::filesystem::path& workspace,
    const ManagementRequest& request, core::CancellationCheck cancelled);
}

#endif // SOS_MCP_MANAGEMENT_HPP
