#include "integration.hpp"
#include "../transport/await_future.hpp"

asio::awaitable<void> Integration::reportMcpRestoration(std::shared_future<void> restoration, std::filesystem::path workspace) {
    co_await awaitFuture(restoration);
    if (closing) co_return;
    McpRestored result;
    try {
        restoration.get();
        const auto listed = mcps->forWorkspace(workspace)->list();
        result.hasServers = !listed.servers.empty();
        for (const auto& issue : listed.issues) result.errors.push_back(issue.message);
        for (const auto& server : listed.servers)
            if (server.state == mcp::ConnectionState::FAILED)
                result.errors.push_back(server.server.name + ": " + server.error);
    } catch (const std::exception&) {
        result.errors.push_back("Could not restore MCP connections; check /mcps");
    }
    events->push(std::move(result));
}

asio::awaitable<nlohmann::json> Integration::handleMcp(const IntegrationRequest& request) {
    if (!permissions->isConfigured()) throw std::runtime_error("Configure a workspace before managing MCP servers");
    if (request.type == "mcp.cancel") co_return co_await cancelMcpOAuth(request);
    if (request.mcpRequest->action == mcp::ManagementAction::OAUTH_START) co_return co_await startMcpOAuth(request);
    if (request.mcpRequest->action == mcp::ManagementAction::OAUTH_POLL) co_return co_await pollMcpOAuth(request);
    if (mcpOAuth) throw std::runtime_error("Finish or cancel MCP OAuth before changing server settings");
    mcpCancellation = std::make_shared<std::atomic<bool>>(false);
    auto job = std::async(std::launch::async, [connections = mcps, workspace = session ? session->getInfo().configurationDirectory() : permissions->root(),
        command = *request.mcpRequest, cancel = mcpCancellation] {
        return mcp::manage(*connections, workspace, command, [cancel] { return cancel->load(); });
    });
    co_await awaitFuture(job);
    mcpCancellation.reset();
    auto response = job.get();
    response["requestId"] = request.requestId;
    if (request.mcpRequest->action == mcp::ManagementAction::LIST) response["restoring"] = mcpRestoring;
    co_return response;
}

asio::awaitable<nlohmann::json> Integration::startMcpOAuth(const IntegrationRequest& request) {
    if (mcpOAuth) throw std::runtime_error("MCP OAuth is already in progress");
    mcpCancellation = std::make_shared<std::atomic<bool>>(false);
    auto job = std::async(std::launch::async, [connections = mcps, workspace = session ? session->getInfo().configurationDirectory() : permissions->root(),
        command = *request.mcpRequest, cancel = mcpCancellation] {
        const auto manager = connections->forWorkspace(workspace);
        for (const auto& item : manager->list().servers) {
            if (item.server.name != command.name || item.identity != command.identity) continue;
            if (!mcp::AuthStore().trusted(item.identity)) throw std::runtime_error("Approve this MCP configuration before authorizing");
            const auto* remote = std::get_if<mcp::Remote>(&item.server.connection);
            if (!remote || !mcp::supportsOAuth(remote->authentication))
                throw std::runtime_error("Select HTTP OAuth authentication first");
            return mcp::startOAuth(*remote, [cancel] { return cancel->load(); }, cancel,
                mcp::AuthStore().get(item.identity).value_or(mcp::Credentials{}), item.authorization);
        }
        throw std::runtime_error("MCP configuration changed; refresh and review it");
    });
    co_await awaitFuture(job);
    mcpOAuth = job.get();
    if (mcpCancellation->load()) {
        mcpOAuth.reset();
        throw core::Cancelled();
    }
    mcpOAuthName = request.mcpRequest->name;
    mcpOAuthIdentity = request.mcpRequest->identity;
    co_return nlohmann::json{{"type", "mcp.oauth_started"}, {"requestId", request.requestId},
        {"url", mcpOAuth->url}, {"instructions", "Authorize in your browser, then return here. SOS listens on IPv4 loopback on this machine."}};
}

asio::awaitable<nlohmann::json> Integration::pollMcpOAuth(const IntegrationRequest& request) {
    if (!mcpOAuth) throw std::runtime_error("No MCP OAuth flow is active");
    if (mcpOAuth->completion.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        co_return nlohmann::json{{"type", "mcp.oauth_pending"}, {"requestId", request.requestId}};
    auto flow = std::move(mcpOAuth);
    core::checkCancellation([cancel = mcpCancellation] { return cancel->load(); });
    const auto credentials = flow->completion.get();
    auto job = std::async(std::launch::async, [connections = mcps, workspace = session ? session->getInfo().configurationDirectory() : permissions->root(),
        name = mcpOAuthName, identity = mcpOAuthIdentity, credentials, cancel = mcpCancellation] {
        core::checkCancellation([cancel] { return cancel->load(); });
        auto manager = connections->forWorkspace(workspace);
        if (!mcp::AuthStore().trusted(identity)) throw std::runtime_error("MCP approval was revoked during authorization");
        manager->authenticate(name, identity, credentials);
        return mcp::manage(*connections, workspace, mcp::ManagementRequest{}, [cancel] { return cancel->load(); });
    });
    co_await awaitFuture(job);
    mcpCancellation.reset();
    auto response = job.get();
    response["requestId"] = request.requestId;
    response["notice"] = "OAuth credentials saved. Enable and Connect the server.";
    co_return response;
}

asio::awaitable<nlohmann::json> Integration::cancelMcpOAuth(const IntegrationRequest& request) {
    if (mcpCancellation) mcpCancellation->store(true);
    if (mcpOAuth) {
        co_await awaitFuture(mcpOAuth->completion);
        mcpOAuth.reset(); // discard even a completed flow; cancellation never saves it
    }
    mcpCancellation.reset();
    co_return nlohmann::json{{"type", "mcp.cancelled"}, {"requestId", request.requestId}};
}
