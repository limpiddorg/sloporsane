#ifndef SOS_MCP_MANAGER_HPP
#define SOS_MCP_MANAGER_HPP

#include "server_connection.hpp"
#include "preferences.hpp"

namespace mcp {
struct ServerList {
    std::vector<ServerStatus> servers;
    std::vector<ConfigIssue> issues;
};

// Workspace catalogue/configuration owner. Delegates every live-client operation
// to ServerConnection. Never holds its catalogue lock while waiting on a server.
class Manager {
public:
    using TransportFactory = mcp::TransportFactory;
    explicit Manager(std::filesystem::path workspace, TransportFactory factory = {}, std::shared_ptr<Preferences> preferences = {});
    ~Manager();
    ServerList list();
    void save(const Server& server, const std::optional<Credentials>& credentials = {});
    void remove(const std::string& name);
    void enable(const std::string& name, bool enabled);
    void trust(const std::string& name, const std::string& identity, bool accepted);
    void authenticate(const std::string& name, const std::string& identity, const Credentials& credentials);
    void connect(const std::string& name, core::CancellationCheck cancelled = {});
    void prepare(core::CancellationCheck cancelled = {});
    void restore(core::CancellationCheck cancelled = {});
    void disconnect(const std::string& name);
    nlohmann::json browse(const std::string& name, const std::string& identity,
        const std::optional<ContentSelection>& selection, core::CancellationCheck cancelled = {});
    void cancelAll() noexcept; // Terminal shutdown, not a reusable per-turn cancellation.
    nlohmann::json call(const std::string& name, const std::string& identity, const ToolDefinition& tool,
        const nlohmann::json& arguments, core::CancellationCheck cancelled = {});
private:
    std::shared_ptr<ServerConnection> find(const std::string& name);
    std::vector<ConfigIssue> refresh();
    ConfigStore config;
    AuthStore auth;
    TransportFactory factory;
    std::shared_ptr<Preferences> preferences;
    std::mutex mutex;
    std::map<std::string, std::shared_ptr<ServerConnection>> entries;
    // Survives config reloads (including disable/enable and timeout changes).
    std::map<std::string, std::shared_ptr<std::atomic<bool>>> attempts;
    bool shuttingDown = false;
};
}

#endif // SOS_MCP_MANAGER_HPP
