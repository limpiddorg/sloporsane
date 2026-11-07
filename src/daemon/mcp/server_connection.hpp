#ifndef SOS_MCP_SERVER_CONNECTION_HPP
#define SOS_MCP_SERVER_CONNECTION_HPP

#include "auth_store.hpp"
#include "client.hpp"
#include <atomic>

namespace mcp {
enum class ConnectionState { NEEDS_TRUST, DISCONNECTED, CONNECTING, CONNECTED, NEEDS_AUTH, FAILED };
const char* stateName(ConnectionState state);
using TransportFactory = std::function<std::unique_ptr<Transport>(const Server&, const Credentials&)>;

struct ServerStatus {
    Server server;
    std::string identity;
    ConnectionState state = ConnectionState::DISCONNECTED;
    std::string error;
    std::vector<ToolDefinition> tools;
    AuthorizationChallenge authorization;
};

// Owns configuration and client; enablement is independent. Serializes operations
// while keeping status/cancellation accessible. Client state is private; retirement is final.
class ServerConnection {
public:
    ServerConnection(Server server, std::string identity, std::shared_ptr<std::atomic<bool>> attempted,
        TransportFactory factory, AuthStore auth);
    ~ServerConnection();
    ServerStatus snapshot() const;
    void setEnabled(bool value); // Tool availability is independent of transport lifetime.
    void setPreferredEnabled(bool value);
    void wantConnection(bool value, bool retry = true); // Publish without waiting on network I/O.
    void restore(core::CancellationCheck cancelled);
    void retire(); // Nonblocking: cancels work and prevents queued/new connects.
    void disconnect();
    void trust(const std::string& identity, bool accepted);
    void authenticate(const std::string& identity, const Credentials& credentials);
    void connect(bool automatic, core::CancellationCheck cancelled);
    void refreshTools(core::CancellationCheck cancelled);
    nlohmann::json browse(const std::string& identity, const std::optional<ContentSelection>& selection,
        core::CancellationCheck cancelled);
    nlohmann::json call(const std::string& identity, const ToolDefinition& tool,
        const nlohmann::json& arguments, core::CancellationCheck cancelled);
private:
    std::unique_lock<std::mutex> acquire(core::CancellationCheck cancelled);
    std::unique_lock<std::mutex> suspend(const std::optional<std::string>& identity = {}, bool graceful = false);
    void requireIdentity(const std::string& identity) const; // state lock held
    void closeClient(); // operation lock held
    void failed(ConnectionState state, const std::string& error); // operation lock held
    void needsAuthentication(const AuthenticationRequired& error);
    void start(core::CancellationCheck cancelled); // operation lock held
    mutable std::mutex mutex;
    std::mutex operation;
    std::mutex administration; // Serializes stop/trust/authenticate transactions.
    ServerStatus status;
    AuthStore auth;
    TransportFactory factory;
    std::shared_ptr<std::atomic<bool>> attempted;
    std::shared_ptr<std::atomic<bool>> stopped = std::make_shared<std::atomic<bool>>(false);
    std::unique_ptr<Client> client;
    bool retired = false;
    bool suspending = false;
    std::uint64_t revision = 0;
    std::atomic<bool> enabled;
    bool preferredEnabled = false;
    std::optional<bool> wanted;
};
}

#endif // SOS_MCP_SERVER_CONNECTION_HPP
