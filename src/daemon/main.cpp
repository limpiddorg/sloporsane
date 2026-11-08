#include "main.hpp"
#include "config.hpp"
#include "transport/server_socket.hpp"
#include "integration/integration.hpp"
#include "mcp/connections.hpp"
#include <agent-runtime/providers/openrouter_provider.hpp>
#include <agent-runtime/common/path_resolution.hpp>
#include <agent-runtime/core/ripgrep.hpp>
#include <agent-runtime/tools/fs/read.hpp>
#include <agent-runtime/tools/fs/glob.hpp>
#include <agent-runtime/tools/fs/grep.hpp>
#include <agent-runtime/tools/fs/write.hpp>
#include <agent-runtime/tools/fs/edit.hpp>
#include <agent-runtime/tools/fs/apply_patch.hpp>
#include <agent-runtime/tools/common/bash.hpp>
#include <agent-runtime/tools/common/webfetch.hpp>
#include <cstdlib>

#include <asio/co_spawn.hpp>
#include <asio/signal_set.hpp>
#include <asio/steady_timer.hpp>
#include <csignal>
#include <exception>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>

namespace {
using Connections = std::map<std::size_t, std::shared_ptr<Integration>>;

void closeConnections(Connections& connections) {
    for (const auto& [id, connection] : connections) connection->close();
    connections.clear();
}

asio::awaitable<void> acceptConnections(asio::io_context& io, ServerSocket& server,
    SessionRegistry& registry, Connections& connections, asio::steady_timer& initialIdle,
    const bool& stopping, std::function<void()> stop, std::shared_ptr<ProviderConnections> providers,
    std::shared_ptr<mcp::Connections> mcps) {
    std::size_t nextId = 0;
    while (!stopping) {
        auto socket = co_await server.accept();
        if (stopping) {
            socket.close();
            co_return;
        }
        initialIdle.cancel();
        auto connection = std::make_shared<Integration>(io, std::move(socket), registry, SessionTitleGenerator{},
            Integration::ReasoningEffortsLoader{}, IntegrationHealthSettings{}, providers,
            CompactionService::ProviderFactory{}, Integration::BtwProviderFactory{}, mcps);
        const auto id = ++nextId;
        connections.emplace(id, connection);
        std::cout << "[sosd] New connection #" << id << std::endl;
        // Keep the socket alive until its pending read and completion callback finish.
        asio::co_spawn(io, connection->run(), [connection, id, &connections, stop](std::exception_ptr error) {
            if (error) {
                try {
                    std::rethrow_exception(error);
                } catch (const std::exception& exception) {
                    std::cerr << "[sosd] Connection #" << id << ": " << exception.what() << '\n';
                }
            }
            connection->close();
            connections.erase(id);
            std::cout << "[sosd] Connection #" << id << " closed" << std::endl;
            if (connections.empty()) {
                std::cout << "[sosd] No integrations remain; shutting down" << std::endl;
                stop();
            }
        });
    }
}

std::unique_ptr<Agent> createAgent(const SessionInfo& info, const ProviderConnections& providers, mcp::Connections& mcps) {
    auto provider = providers.create(info.provider);
    const auto ripgrep = core::ensureRipgrep();
    ToolRegistry tools;
    tools.add(std::make_unique<ReadTool>());
    tools.add(std::make_unique<GlobTool>(ripgrep));
    tools.add(std::make_unique<GrepTool>(ripgrep));
    tools.add(std::make_unique<WriteTool>());
    tools.add(std::make_unique<EditTool>());
    tools.add(std::make_unique<ApplyPatchTool>());
    tools.add(std::make_unique<BashTool>());
    tools.add(std::make_unique<WaitTool>());
    tools.add(std::make_unique<WebFetchTool>());
    ToolContext context{info.executionDirectory(), info.executionDirectory()};
    context.ask = [](const ToolPermissionRequest&) {
        throw std::runtime_error("Tool permissions have not been configured");
    };
    auto agent = std::make_unique<Agent>(std::move(provider), info.model,
        std::move(tools), std::move(context), info.reasoning);
    agent->enableProjectContext(info.executionDirectory(), info.configurationDirectory());
    mcps.bind(*agent, info.configurationDirectory());
    return agent;
}
}

int main(int argc, char* argv[]) {
    if (argc > 3) {
        std::cerr << "Usage: sosd [socket-path] [sessions-directory]\n";
        return 1;
    }
    try {
        const std::filesystem::path socketPath = argc >= 2 ? argv[1] : daemon_config::DEFAULT_SOCKET_PATH;
        asio::io_context io;
        ServerSocket server(io, socketPath);
        auto providers = std::make_shared<ProviderConnections>();
        auto mcps = std::make_shared<mcp::Connections>();
        SessionRegistry registry(argc == 3 ? std::filesystem::path(argv[2]) : PathResolution::sessionsDirectory(),
            [providers, mcps](const SessionInfo& info) { return createAgent(info, *providers, *mcps); });
        Connections connections;
        server.listen();
        std::cout << "[sosd] Listening on " << socketPath.string() << std::endl;

        asio::signal_set signals(io, SIGINT, SIGTERM);
        asio::steady_timer initialIdle(io, daemon_config::INITIAL_CONNECTION_TIMEOUT);
        bool stopping = false;
        const auto stop = [&] {
            if (stopping) return;
            stopping = true;
            mcps->cancelAll();
            initialIdle.cancel();
            server.close();
            closeConnections(connections);
            signals.cancel();
        };
        initialIdle.async_wait([&](const asio::error_code& error) {
            if (!error && connections.empty()) {
                std::cout << "[sosd] No integration connected; shutting down" << std::endl;
                stop();
            }
        });
        signals.async_wait([&](const asio::error_code& error, int) {
            if (error) return;
            std::cout << "[sosd] Shutting down server" << std::endl;
            stop();
        });
        int exitCode = 0;
        asio::co_spawn(io, acceptConnections(io, server, registry, connections, initialIdle, stopping, stop, providers, mcps), [&](std::exception_ptr error) {
            if (!error || stopping) return;
            try {
                std::rethrow_exception(error);
            } catch (const asio::system_error& exception) {
                if (exception.code() != asio::error::operation_aborted) {
                    std::cerr << "[sosd] " << exception.what() << '\n';
                    exitCode = 1;
                }
            } catch (const std::exception& exception) {
                std::cerr << "[sosd] " << exception.what() << '\n';
                exitCode = 1;
            }
            stop();
        });
        io.run();
        return exitCode;
    } catch (const std::exception& error) {
        std::cerr << "[sosd] " << error.what() << '\n';
        return 1;
    }
}
