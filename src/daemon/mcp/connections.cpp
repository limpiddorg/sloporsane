#include "connections.hpp"
#include "tool_adapter.hpp"

namespace mcp {
Connections::Connections(Manager::TransportFactory factory)
    : factory(std::move(factory)), preferences(std::make_shared<Preferences>()) {}

std::shared_ptr<Manager> Connections::forWorkspace(const std::filesystem::path& workspace) {
    const auto canonical = std::filesystem::canonical(workspace);
    std::lock_guard lock(mutex);
    if (stopped) throw core::Cancelled();
    auto& manager = workspaces[canonical];
    if (!manager) manager = std::make_shared<Manager>(canonical, factory, preferences);
    return manager;
}

void Connections::bind(Agent& agent, const std::filesystem::path& workspace) {
    agent.setToolSource([this, workspace, manager = forWorkspace(workspace)](core::CancellationCheck cancelled) {
        waitForRestore(workspace, cancelled);
        manager->prepare(cancelled);
        core::checkCancellation(cancelled);
        return makeTools(manager);
    });
}

std::shared_future<void> Connections::restoreInBackground(const std::filesystem::path& workspace,
    core::CancellationCheck cancelled) {
    const auto canonical = std::filesystem::canonical(workspace);
    auto manager = forWorkspace(canonical);
    manager->list(); // Populate /mcps before any connection attempt begins.
    std::lock_guard lock(mutex);
    if (stopped) throw core::Cancelled();
    auto& current = restorations[canonical];
    if (current.valid() && current.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return current;
    current = std::async(std::launch::async, [manager, cancelled] { manager->restore(cancelled); }).share();
    return current;
}

void Connections::waitForRestore(const std::filesystem::path& workspace, core::CancellationCheck cancelled) {
    std::shared_future<void> current;
    {
        std::lock_guard lock(mutex);
        const auto found = restorations.find(std::filesystem::canonical(workspace));
        if (found != restorations.end()) current = found->second;
    }
    if (!current.valid()) return;
    while (current.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready) core::checkCancellation(cancelled);
    core::checkCancellation(cancelled);
    try { current.get(); }
    catch (const std::exception&) { /* A failed restore must not prevent non-MCP tools from running. */ }
}

void Connections::cancelAll() {
    std::lock_guard lock(mutex);
    stopped = true;
    for (const auto& [workspace, manager] : workspaces) manager->cancelAll();
}

void Connections::restore(core::CancellationCheck cancelled) {
    std::vector<std::shared_ptr<Manager>> managers;
    {
        std::lock_guard lock(mutex);
        if (stopped) throw core::Cancelled();
        for (const auto& [workspace, manager] : workspaces) managers.push_back(manager);
    }
    for (const auto& manager : managers) manager->restore(cancelled);
}
}
