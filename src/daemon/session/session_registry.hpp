#ifndef SOS_SESSION_REGISTRY_HPP
#define SOS_SESSION_REGISTRY_HPP

#include "session.hpp"
#include <map>
#include <set>

// Owned by the daemon and used on its io_context thread. Active sessions are weak references.
class SessionRegistry {
public:
    SessionRegistry(std::filesystem::path directory, SessionAgentFactory factory);
    std::shared_ptr<Session> create(SessionInfo info);
    std::shared_ptr<Session> createImplementation(Session& discussion, const std::string& objectiveId = {});
    void freezeImplementation(Session& discussion, Session& implementation);
    std::string resumeTarget(const Session& discussion) const;
    std::shared_ptr<Session> load(const std::string& id, std::optional<ProviderSelection> selection = {});
    asio::awaitable<std::shared_ptr<Session>> loadAsync(const std::string& id,
        std::optional<ProviderSelection> selection, core::CancellationCheck cancelled = {});
    asio::awaitable<std::string> previewAsync(const std::string& id, core::CancellationCheck cancelled = {});
    SessionInfo inspect(const std::string& id) const;
    std::vector<SessionInfo> list() const;
    void watchWorkspace(const void* owner, std::function<std::filesystem::path()> workspace);
    void unwatchWorkspace(const void* owner);
    std::vector<std::filesystem::path> openedWorkspaces() const;
    void requireWorkspaceAvailable(const std::filesystem::path& workspace) const;
    void beginWorktreeDeletion(const std::filesystem::path& path);
    void endWorktreeDeletion(const std::filesystem::path& path);
    std::string lastUserMessage(const std::string& id, core::CancellationCheck cancelled = {}) const;

private:
    std::shared_ptr<Session> createImplementationJournal(SessionInfo info, std::vector<Message> snapshot);
    std::shared_ptr<Session> readSession(const std::string& id, std::optional<ProviderSelection> selection,
        core::CancellationCheck cancelled = {}) const;
    std::filesystem::path directory;
    SessionAgentFactory factory;
    std::map<std::string, std::weak_ptr<Session>> active;
    std::set<std::string> loading;
    std::map<const void*, std::function<std::filesystem::path()>> workspaceObservers;
    std::set<std::filesystem::path> deletingWorktrees;
};

#endif // SOS_SESSION_REGISTRY_HPP
