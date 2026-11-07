#ifndef SOS_MCP_CONNECTIONS_HPP
#define SOS_MCP_CONNECTIONS_HPP

#include "manager.hpp"
#include <agent-runtime/agent.hpp>
#include <future>

namespace mcp {
// Daemon lifetime, one manager per canonical workspace. Keeps disconnect/failure
// state across /new and /resume. Owners drain agent workers before destruction.
class Connections {
public:
    explicit Connections(Manager::TransportFactory factory = {});
    std::shared_ptr<Manager> forWorkspace(const std::filesystem::path& workspace);
    void bind(Agent& agent, const std::filesystem::path& workspace);
    void cancelAll();
    void restore(core::CancellationCheck cancelled = {});
    std::shared_future<void> restoreInBackground(const std::filesystem::path& workspace,
        core::CancellationCheck cancelled = {});
    void waitForRestore(const std::filesystem::path& workspace, core::CancellationCheck cancelled = {});
private:
    Manager::TransportFactory factory;
    std::shared_ptr<Preferences> preferences;
    std::mutex mutex;
    std::map<std::filesystem::path, std::shared_ptr<Manager>> workspaces;
    std::map<std::filesystem::path, std::shared_future<void>> restorations;
    bool stopped = false;
};
}

#endif // SOS_MCP_CONNECTIONS_HPP
