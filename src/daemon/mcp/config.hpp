#ifndef SOS_MCP_CONFIG_HPP
#define SOS_MCP_CONFIG_HPP

#include <filesystem>
#include <map>
#include <string>
#include <variant>
#include <vector>
#include <nlohmann/json.hpp>

namespace mcp {
enum class Scope { GLOBAL, WORKSPACE };
enum class Authentication { NONE, TOKEN, OAUTH, AUTO };
bool supportsOAuth(Authentication authentication);

struct Local {
    std::vector<std::string> command;
    std::filesystem::path cwd;
    // Public environment only. Secret values belong to Credentials::environment.
    std::map<std::string, std::string> environment;
};

struct Remote {
    std::string url;
    Authentication authentication = Authentication::NONE;
    std::string clientId;
    std::string oauthScope;
    unsigned short callbackPort = 0; // Zero selects an ephemeral loopback port.
};

struct Server {
    std::string name;
    Scope scope = Scope::GLOBAL;
    std::variant<Local, Remote> connection;
    bool enabled = false;
    int startupTimeoutMs = 10000;
    int requestTimeoutMs = 30000;
};

struct ConfigIssue {
    Scope scope;
    std::string name;
    std::string message;
};

struct Configuration {
    std::map<std::string, Server> servers;
    std::vector<ConfigIssue> issues;
};

void validate(const Server& server);
void validate(const Local& local);
void validate(const Remote& remote);
void validateEnvironment(const std::map<std::string, std::string>& environment);
void to_json(nlohmann::json& json, const Server& server);
void from_json(const nlohmann::json& json, Server& server);
const char* scopeName(Scope scope);

class ConfigStore {
public:
    explicit ConfigStore(std::filesystem::path workspace);
    Configuration load() const;
    void save(const Server& server) const;
    void remove(Scope scope, const std::string& name) const;
    std::filesystem::path path(Scope scope) const;
    // Includes scope, canonical workspace (for local credentials) and exact
    // connection settings. Enablement/timeouts do not change credential identity.
    std::string identity(const Server& server) const;
private:
    std::filesystem::path workspace;
};
}

#endif // SOS_MCP_CONFIG_HPP
