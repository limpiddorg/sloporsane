#include "integration.hpp"
#include "../session/agent_worktree.hpp"
#include "../transport/await_future.hpp"

void Integration::annotateWorktrees(nlohmann::json& entries, const std::filesystem::path& current) const {
    const auto opened = registry.openedWorkspaces();
    for (auto& entry : entries) {
        const auto path = std::filesystem::weakly_canonical(entry.at("path").get<std::string>());
        bool used = false;
        for (const auto& workspace : opened) {
            if (AgentWorktree::containsWorkspace(path, workspace)) used = true;
        }
        entry["opened"] = used;
        entry["current"] = AgentWorktree::containsWorkspace(path, current);
        entry["deletable"] = !entry.value("primary", false) && !used && !entry.value("current", false);
    }
}

asio::awaitable<nlohmann::json> Integration::deleteWorktree(const std::filesystem::path& source, const std::filesystem::path& path) {
    if (!path.is_absolute()) throw std::invalid_argument("Worktree path must be absolute");
    const auto target = std::filesystem::canonical(path);
    registry.beginWorktreeDeletion(target);
    try {
        auto opened = registry.openedWorkspaces();
        opened.push_back(source);
        auto job = std::async(std::launch::async, [source, target, opened] {
            AgentWorktree worktrees(source);
            worktrees.remove(target, opened);
            return worktrees.list();
        });
        co_await awaitFuture(job);
        auto entries = job.get();
        registry.endWorktreeDeletion(target);
        annotateWorktrees(entries, source);
        co_return nlohmann::json{{"worktrees", entries}, {"workspace", source.string()}, {"switched", false}};
    } catch (...) {
        registry.endWorktreeDeletion(target);
        throw;
    }
}

asio::awaitable<nlohmann::json> Integration::handleWorkspace(const WorkspaceRequest& request) {
    try {
        if (closing) throw std::runtime_error("Conversation is no longer attached");
        const auto action = request.arguments.at("action").get<std::string>();
        if (action != "list" && action != "create" && action != "switch" && action != "delete" && action != "switch_directory")
            throw std::invalid_argument("Unknown worktree action");
        if (action != "list") {
            if (action == "delete" && permissions->mode() != ExecutionMode::SLOP)
                throw std::logic_error("Delete worktrees in Slop, not Sane");
            if (session && (!session->getInfo().discussionId.empty() || !session->getInfo().activeImplementationId.empty() || !session->getInfo().executionWorkspace.empty()))
                throw std::logic_error("Close the active Implement before switching workspace");
            if (!btwRuns.empty()) throw std::logic_error("Wait for BTW conversations before switching workspace");
        }
        const auto source = currentWorkspace();
        if (action == "delete") {
            request.result->set_value(co_await deleteWorktree(source, request.arguments.at("path").get<std::string>()));
            co_return nlohmann::json{{"type", "session.updated"}, {"info", runtimeInfo()}};
        }
        auto job = std::async(std::launch::async, [workspace = source, arguments = request.arguments, action] {
            if (action == "switch_directory") {
                const auto path = std::filesystem::path(arguments.at("path").get<std::string>());
                if (!path.is_absolute()) throw std::invalid_argument("Workspace path must be absolute");
                const auto target = std::filesystem::canonical(path);
                if (!std::filesystem::is_directory(target)) throw std::invalid_argument("Workspace destination must be an existing directory");
                return nlohmann::json{{"workspace", target.string()}, {"permission_root", target.string()}, {"switched", true}};
            }
            AgentWorktree worktrees(workspace);
            if (action == "list") return nlohmann::json{{"worktrees", worktrees.list()}, {"switched", false}};
            const auto target = action == "create" ? worktrees.create(arguments.at("name").get<std::string>()) :
                worktrees.select(arguments.at("path").get<std::string>());
            return nlohmann::json{{"workspace", target.string()}, {"permission_root", target.string()}, {"switched", true}};
        });
        co_await awaitFuture(job);
        auto result = job.get();
        if (result.at("switched").get<bool>()) {
            const auto target = std::filesystem::path(result.at("workspace").get<std::string>());
            registry.requireWorkspaceAvailable(target);
            pendingWorkspace = target;
            if (!session) {
                IntegrationRequest initial; initial.workspace = source.string();
                session = registry.create(makeSessionInfo(initial));
                subscribe();
            }
            co_await session->switchWorkspace(target, target);
            pendingWorkspace.clear();
            permissions->setExecutionRoot(target);
            result["configuration_workspace"] = session->getInfo().configurationDirectory().string();
            result["notice"] = "SOS now works in the selected workspace. Skills and MCP configuration stay inherited from the original workspace. Local changes were not copied. Commit, merge and push require separate Bash calls.";
        }
        result["workspace"] = session ? session->getInfo().workspace.string() : source.string();
        if (action == "list") annotateWorktrees(result["worktrees"], source);
        request.result->set_value(result);
    } catch (...) { pendingWorkspace.clear(); request.result->set_exception(std::current_exception()); }
    co_return nlohmann::json{{"type", "session.updated"}, {"info", runtimeInfo()}};
}
