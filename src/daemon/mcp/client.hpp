#ifndef SOS_MCP_CLIENT_HPP
#define SOS_MCP_CLIENT_HPP

#include "transport.hpp"
#include "content.hpp"
#include <agent-runtime/core/cancellation.hpp>
#include <chrono>
#include <memory>
#include <mutex>

namespace mcp {
// A well-formed JSON-RPC rejection does not invalidate the connection.
class RpcError : public std::runtime_error {
public:
    explicit RpcError(int code = -32000) : std::runtime_error("MCP server rejected the request"), code(code) {}
    const int code;
};

struct ToolDefinition {
    std::string name;
    std::string description;
    nlohmann::json inputSchema;
    bool readOnly = false;
    bool operator==(const ToolDefinition&) const = default;
};

// One in-flight request per connection. No automatic retry of tools/call.
// A timeout, cancellation or malformed envelope closes this client; the caller
// must explicitly create a new connection. Tool-level errors do not close it.
class Client {
public:
    Client(std::unique_ptr<Transport> transport, int requestTimeoutMs);
    ~Client();
    void initialize(int startupTimeoutMs, core::CancellationCheck cancelled = {});
    std::vector<ToolDefinition> tools(core::CancellationCheck cancelled = {});
    nlohmann::json call(const std::string& name, const nlohmann::json& arguments, core::CancellationCheck cancelled = {});
    std::vector<ContentEntry> catalogue(core::CancellationCheck cancelled = {});
    nlohmann::json preview(const ContentSelection& selection, core::CancellationCheck cancelled = {});
    void close();
    void disconnect();
    bool takeToolsChanged();
private:
    nlohmann::json request(const std::string& method, const nlohmann::json& params, Deadline deadline,
        core::CancellationCheck cancelled);
    void notify(const std::string& method, const nlohmann::json& params, Deadline deadline, core::CancellationCheck cancelled);
    void requireReady() const;
    std::vector<ToolDefinition> loadTools(core::CancellationCheck cancelled);
    std::vector<ContentEntry> loadContent(ContentKind kind, Deadline deadline, core::CancellationCheck cancelled);
    std::unique_ptr<Transport> transport;
    std::mutex mutex;
    std::uint64_t nextId = 0;
    std::chrono::milliseconds timeout;
    enum class State { NEW, READY, CLOSED };
    State state = State::NEW;
    bool hasTools = false;
    bool toolsChanged = false;
    bool hasResources = false, hasPrompts = false;
};
}

#endif // SOS_MCP_CLIENT_HPP
