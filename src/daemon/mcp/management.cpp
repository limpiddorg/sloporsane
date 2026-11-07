#include "management.hpp"
#include "oauth_credentials.hpp"

namespace mcp {
void from_json(const nlohmann::json& json, ManagementRequest& request) {
    const auto type = json.at("type").get<std::string>();
    if (type == "mcp.list") { request.action = ManagementAction::LIST; return; }
    if (type == "mcp.oauth_poll") { request.action = ManagementAction::OAUTH_POLL; return; }
    request.name = json.at("name").get<std::string>();
    request.identity = json.value("identity", "");
    if (type == "mcp.oauth_revoke") request.action = ManagementAction::OAUTH_REVOKE;
    else if (type == "mcp.oauth_client") {
        request.action = ManagementAction::OAUTH_CLIENT;
        request.clientSecret = json.at("client_secret");
        request.clientAuthMethod = json.at("method");
        if (request.clientSecret.empty() || request.clientSecret.size() > 16384 ||
            (request.clientAuthMethod != "client_secret_basic" && request.clientAuthMethod != "client_secret_post"))
            throw std::invalid_argument("Invalid OAuth client credentials");
    } else if (type == "mcp.catalogue") request.action = ManagementAction::CATALOGUE;
    else if (type == "mcp.preview") {
        request.action = ManagementAction::PREVIEW;
        request.selection.kind = contentKind(json.at("kind"));
        request.selection.target = json.at("target");
        request.selection.uri = json.value("uri", "");
        request.selection.arguments = json.value("arguments", std::map<std::string, std::string>{});
        if (request.selection.target.size() > 16384 || request.selection.uri.size() > 16384 || request.selection.arguments.size() > 128)
            throw std::invalid_argument("MCP content selection exceeds limits");
    } else if (type == "mcp.oauth_start") request.action = ManagementAction::OAUTH_START;
    else if (type == "mcp.secrets") {
        request.action = ManagementAction::SECRETS;
        request.environment = json.at("environment").get<std::map<std::string, std::string>>();
        validateEnvironment(request.environment);
    } else if (type == "mcp.save") {
        request.action = ManagementAction::SAVE;
        request.server = json.at("config").get<Server>();
        request.server.name = request.name;
        const auto scope = json.at("scope").get<std::string>();
        if (scope != "global" && scope != "workspace") throw std::invalid_argument("Invalid MCP scope");
        request.server.scope = scope == "global" ? Scope::GLOBAL : Scope::WORKSPACE;
        validate(request.server);
    } else if (type == "mcp.trust") {
        request.action = ManagementAction::TRUST;
        request.accepted = json.at("accepted").get<bool>();
    } else if (type == "mcp.connect") request.action = ManagementAction::CONNECT;
    else if (type == "mcp.disconnect") request.action = ManagementAction::DISCONNECT;
    else if (type == "mcp.remove") request.action = ManagementAction::REMOVE;
    else if (type == "mcp.enable") {
        request.action = ManagementAction::ENABLE;
        request.enabled = json.at("enabled").get<bool>();
    } else if (type == "mcp.token") {
        request.action = ManagementAction::TOKEN;
        request.token = json.at("token").get<std::string>();
        if (request.token.empty() || request.token.size() > 16384) throw std::invalid_argument("Invalid MCP token");
    } else throw std::invalid_argument("Unknown MCP management request");
}

nlohmann::json manage(Connections& connections, const std::filesystem::path& workspace,
    const ManagementRequest& request, core::CancellationCheck cancelled) {
    using Json = nlohmann::json;
    core::checkCancellation(cancelled);
    const auto manager = connections.forWorkspace(workspace);
    const auto before = manager->list();
    const ServerStatus* selected = nullptr;
    for (const auto& item : before.servers) if (item.server.name == request.name) selected = &item;
    if (request.action != ManagementAction::LIST) {
        if (request.action == ManagementAction::SAVE && request.identity.empty()) {
            if (selected) throw std::runtime_error("MCP name already exists; edit it after refreshing");
        } else if (!selected || request.identity.empty() || selected->identity != request.identity)
            throw std::runtime_error("MCP configuration changed; refresh and review it");
    }
    switch (request.action) {
        case ManagementAction::OAUTH_REVOKE:
        case ManagementAction::OAUTH_CLIENT: {
            const auto* remote = std::get_if<Remote>(&selected->server.connection);
            if (!remote || !supportsOAuth(remote->authentication) || !AuthStore().trusted(request.identity))
                throw std::runtime_error("Approve an HTTP OAuth server first");
            if (request.action == ManagementAction::OAUTH_REVOKE) {
                manager->disconnect(request.name);
                OAuthCredentials(AuthStore(), request.identity).revoke(cancelled);
            } else {
                if (remote->clientId.empty()) throw std::runtime_error("Configure the registered OAuth client ID first");
                Credentials credentials;
                credentials.clientId = remote->clientId;
                credentials.clientSecret = request.clientSecret;
                credentials.tokenAuthMethod = request.clientAuthMethod;
                manager->authenticate(request.name, request.identity, credentials);
            }
            break;
        }
        case ManagementAction::CATALOGUE:
        case ManagementAction::PREVIEW: {
            auto result = manager->browse(request.name, request.identity,
                request.action == ManagementAction::PREVIEW ? std::optional(request.selection) : std::nullopt, cancelled);
            result["type"] = request.action == ManagementAction::PREVIEW ? "mcp.previewed" : "mcp.catalogued";
            result["name"] = request.name;
            result["identity"] = request.identity;
            return result;
        }
        case ManagementAction::OAUTH_START:
        case ManagementAction::OAUTH_POLL: throw std::logic_error("OAuth lifecycle belongs to the integration");
        case ManagementAction::SECRETS: {
            if (!std::holds_alternative<Local>(selected->server.connection)) throw std::runtime_error("Secret environment is for local servers only");
            Credentials credentials;
            credentials.environment = request.environment;
            manager->authenticate(request.name, request.identity, credentials);
            break;
        }
        case ManagementAction::LIST: break;
        case ManagementAction::SAVE:
            if (selected && selected->server.scope != request.server.scope)
                throw std::runtime_error("Changing MCP scope requires removing and adding the server");
            manager->save(request.server); break;
        case ManagementAction::TRUST: manager->trust(request.name, request.identity, request.accepted); break;
        case ManagementAction::CONNECT:
            try { manager->connect(request.name, cancelled); }
            catch (const AuthenticationRequired&) {
                // Authorization is an explicit next step, not an automatic browser flow.
                // Return the updated needs_auth state and guidance to the UI.
            }
            catch (...) {
                const auto error = std::current_exception();
                connections.restore(cancelled);
                std::rethrow_exception(error);
            }
            break;
        case ManagementAction::DISCONNECT: manager->disconnect(request.name); break;
        case ManagementAction::ENABLE: manager->enable(request.name, request.enabled); break;
        case ManagementAction::REMOVE: manager->remove(request.name); break;
        case ManagementAction::TOKEN: {
            const auto* remote = std::get_if<Remote>(&selected->server.connection);
            if (!remote || remote->authentication != Authentication::TOKEN)
                throw std::runtime_error("Select HTTP token authentication before saving a token");
            Credentials credentials;
            credentials.accessToken = request.token;
            manager->authenticate(request.name, request.identity, credentials);
            break;
        }
    }
    if (request.action != ManagementAction::LIST) connections.restore(cancelled);
    const auto listed = manager->list();
    Json servers = Json::array(), issues = Json::array();
    AuthStore auth;
    for (const auto& item : listed.servers)
        servers.push_back({{"name", item.server.name}, {"scope", scopeName(item.server.scope)},
            {"config", item.server}, {"identity", item.identity}, {"state", stateName(item.state)},
            {"trusted", auth.trusted(item.identity)}, {"error", item.error}, {"tool_count", item.tools.size()}});
    for (const auto& issue : listed.issues)
        issues.push_back({{"scope", scopeName(issue.scope)}, {"name", issue.name}, {"message", issue.message}});
    return {{"type", "mcp.listed"}, {"servers", servers}, {"issues", issues}, {"workspace", workspace.string()}};
}
}
