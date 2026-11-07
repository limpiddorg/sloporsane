#include "session_registry.hpp"
#include "agent_worktree.hpp"
#include "../prompts.hpp"
#include "../transport/await_future.hpp"

#include <agent-runtime/conversation/messages/message_json.hpp>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <random>

namespace {
namespace fs = std::filesystem;
bool validId(const std::string& id) {
    return id.size() == 32 && id.find_first_not_of("0123456789abcdef") == std::string::npos;
}
std::string newId() {
    constexpr char HEX[] = "0123456789abcdef";
    std::random_device random;
    std::string id;
    for (int i = 0; i < 32; ++i) id += HEX[random() & 15];
    return id;
}
SessionInfo readInfo(const fs::path& path) {
    std::ifstream stream(path);
    if (!stream) throw std::runtime_error("Session metadata not found");
    return nlohmann::json::parse(stream).get<SessionInfo>();
}
struct ImplementationSnapshot {
    std::vector<Message> messages;
    std::string objectiveId;
    std::size_t contextBoundary = 0;
};
ImplementationSnapshot implementationSnapshot(Session& discussion, const std::string& objectiveId) {
    auto objective = discussion.latestObjectivePlan();
    if (!objectiveId.empty()) {
        objective.reset();
        for (const auto& plan : discussion.objectivePlans()) if (plan.id == objectiveId) objective = plan;
    }
    if (!objective) throw std::logic_error("An Objective Plan is required before implementation planning");
    auto snapshot = discussion.getMessages();
    const auto anchor = std::find_if(snapshot.begin(), snapshot.end(), [&](const auto& message) { return message.id == objective->id; });
    if (anchor == snapshot.end()) throw std::logic_error("Objective Plan is missing from the journal");
    // Preserve checkpoints and system updates after the selected objective.
    const auto boundary = std::min(discussion.getInfo().providerContextStart, snapshot.size());
    const auto portable = static_cast<std::size_t>(std::count_if(snapshot.begin(), snapshot.begin() + boundary,
        [](const auto& message) { return !message.manualApproval; }));
    // Keep referenced IDs intact for compaction and images, but not permission grants.
    std::erase_if(snapshot, [](const auto& message) { return message.manualApproval.has_value(); });
    std::string accepted;
    for (const auto& part : objective->content)
        if (const auto* text = std::get_if<TextContent>(&part)) accepted += text->text;
    snapshot.push_back(Message{"accepted_objective_" + objective->id, MessageRole::SYSTEM, {TextContent{
        "The user selected this Objective Plan version for implementation. It supersedes other Objective Plan versions "
        "in the inherited discussion. Plan and implement this selected objective (JSON data):\n" +
        nlohmann::json{{"id", objective->id}, {"plan", accepted}, {"stack", objective->objectiveStack.value_or("")}}.dump()}}});
    return {std::move(snapshot), objective->id, portable};
}
SessionInfo implementationInfo(const SessionInfo& discussion, const ImplementationSnapshot& snapshot) {
    auto info = discussion;
    info.discussionId = info.id;
    info.objectiveId = snapshot.objectiveId;
    info.inheritedMessages = snapshot.messages.size();
    info.implementationIds.clear();
    info.activeImplementationId.clear();
    info.terminalOpenings.clear();
    info.mode = ExecutionMode::SANE;
    // A planning branch retains usage and opaque history for the same model.
    info.providerContextStart = snapshot.contextBoundary;
    return info;
}
}

SessionRegistry::SessionRegistry(fs::path directory, SessionAgentFactory factory)
    : directory(fs::absolute(directory)), factory(std::move(factory)) {
    fs::create_directories(this->directory);
}

std::shared_ptr<Session> SessionRegistry::create(SessionInfo info) {
    requireWorkspaceAvailable(info.workspace);
    if (!info.workspace.is_absolute() || !fs::is_directory(info.workspace))
        throw std::invalid_argument("Session workspace must be an existing absolute directory");
    info.workspace = fs::canonical(info.workspace);
    info.createdAt = info.updatedAt = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    fs::path path;
    do { info.id = newId(); path = directory / info.id; } while (!fs::create_directory(path));
    try {
        auto agent = factory(info);
        if (!agent) throw std::invalid_argument("Session factory returned no agent");
        agent->append(Message{"system_" + info.id, MessageRole::SYSTEM, {TextContent{
            std::string(prompts::DISCUSSION_SYSTEM_PROMPT) + "\nYour working directory is " + info.workspace.string() + ".\n" +
                prompts::executionModeAnnouncement(info.mode)
        }}});
        const auto initial = agent->getMessages();
        auto session = std::make_shared<Session>(SessionState{info, std::move(agent)}, path);
        session->record({AgentEventType::HARNESS_MESSAGE_ARRIVED, initial, {}, info.context});
        active[info.id] = session;
        return session;
    } catch (...) {
        std::error_code error;
        fs::remove_all(path, error);
        throw;
    }
}

std::shared_ptr<Session> SessionRegistry::load(const std::string& id, std::optional<ProviderSelection> selection) {
    if (!validId(id)) throw std::invalid_argument("Invalid session id");
    if (loading.contains(id)) throw std::logic_error("Session is loading; retry shortly");
    if (const auto it = active.find(id); it != active.end() && !it->second.expired())
        throw std::logic_error("Session is already attached to an integration");
    const auto info = inspect(id);
    requireWorkspaceAvailable(info.workspace);
    if (!info.executionWorkspace.empty()) requireWorkspaceAvailable(info.executionWorkspace);
    auto session = readSession(id, selection);
    active[id] = session;
    return session;
}

std::shared_ptr<Session> SessionRegistry::createImplementation(Session& discussion, const std::string& objectiveId) {
    if (discussion.isExecuting() || !discussion.getInfo().discussionId.empty() ||
        !discussion.getInfo().activeImplementationId.empty())
        throw std::logic_error("An idle Discussion without an active implementation is required");
    if (discussion.storageDirectory() != directory / discussion.getInfo().id)
        throw std::logic_error("Discussion belongs to another registry");
    auto snapshot = implementationSnapshot(discussion, objectiveId);
    const auto info = implementationInfo(discussion.getInfo(), snapshot);
    auto branch = createImplementationJournal(info, std::move(snapshot.messages));
    try { discussion.linkImplementation(branch->getInfo().id); }
    catch (...) {
        const auto path = branch->storageDirectory();
        branch.reset();
        std::error_code error;
        fs::remove_all(path, error);
        throw;
    }
    return branch;
}

std::shared_ptr<Session> SessionRegistry::createImplementationJournal(SessionInfo info, std::vector<Message> snapshot) {
    info.createdAt = info.updatedAt = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    fs::path path;
    do { info.id = newId(); path = directory / info.id; } while (!fs::create_directory(path));
    try {
        auto journal = std::make_shared<SessionJournal>(path);
        snapshot.push_back(Message{"implementation_context_" + info.id, MessageRole::SYSTEM, {TextContent{
            "Continue the preceding conversation in its implementation-planning phase. The Objective Plan above is accepted. "
            "Discuss how to build it; do not modify files. Sane mode is fixed. This is a continuation, not a new task. "
            "Only new exchanges appear in the Implement tab; the preceding context remains available to you."}}});
        journal->save(snapshot, info);
        auto implementation = std::make_shared<Session>(info, path, factory, std::move(journal));
        active[info.id] = implementation;
        return implementation;
    } catch (...) {
        std::error_code error;
        fs::remove_all(path, error);
        throw;
    }
}

void SessionRegistry::freezeImplementation(Session& discussion, Session& implementation) {
    if (implementation.getInfo().discussionId != discussion.getInfo().id ||
        discussion.getInfo().activeImplementationId != implementation.getInfo().id || discussion.isExecuting())
        throw std::logic_error("Implementation is not the active branch of this Discussion");
    // Freeze first: an interruption between commits cannot leave a writable archive.
    implementation.freezeImplementation();
    discussion.returnToDiscussion();
}

std::string SessionRegistry::resumeTarget(const Session& discussion) const {
    const auto& info = discussion.getInfo();
    if (info.activeImplementationId.empty()) return info.id;
    if (!validId(info.activeImplementationId)) throw std::runtime_error("Invalid active implementation id");
    if (!fs::is_regular_file(directory / info.activeImplementationId / "session.sqlite"))
        throw std::runtime_error("Active implementation is missing");
    SessionJournal journal(directory / info.activeImplementationId);
    const auto branch = journal.info();
    if (!branch || branch->discussionId != info.id) throw std::runtime_error("Active implementation is missing or unrelated");
    return branch->frozen ? info.id : branch->id;
}

std::shared_ptr<Session> SessionRegistry::readSession(const std::string& id, std::optional<ProviderSelection> selection,
    core::CancellationCheck cancelled) const {
    const auto path = directory / id;
    if (!fs::is_directory(path)) throw std::runtime_error("Session not found");
    auto journal = std::make_shared<SessionJournal>(path, cancelled);
    const auto storedInfo = journal->info();
    if (!storedInfo) throw std::runtime_error("Session metadata not found");
    auto info = *storedInfo;
    if (!info.frozen && !fs::is_directory(info.workspace))
        throw std::runtime_error("Session workspace is unavailable: " + info.workspace.string() + ". Restore the directory before resuming this session.");
    if (info.id != id) throw std::runtime_error("Session id does not match its directory");
    if (info.frozen) selection.reset();
    const bool changedModel = selection && (selection->model != info.model || selection->provider != info.provider);
    if (selection) {
        info.provider = selection->provider;
        info.model = selection->model;
        info.reasoning = selection->reasoning;
    }
    if (journal->updatedAt()) info.updatedAt = journal->updatedAt();
    // Only pending calls are read. Never replay side effects automatically.
    std::vector<Message> interrupted;
    for (const auto& callId : journal->recovery().interruptedCalls(cancelled)) {
        core::checkCancellation(cancelled);
        interrupted.push_back(Message{"interrupted_" + id + "_" + callId, MessageRole::TOOL,
            {ToolResultContent{callId, "Previous execution was interrupted; the tool outcome is unknown", true}}});
    }
    if (changedModel) {
        info.providerContextStart = journal->count();
        info.context = {};
    }
    auto session = std::make_shared<Session>(info, path, factory, std::move(journal));
    core::checkCancellation(cancelled);
    if (!interrupted.empty()) session->record({AgentEventType::HARNESS_MESSAGE_ARRIVED, std::move(interrupted), {}, info.context});
    session->save(); // Persist a selection change and refresh the readable cache.
    return session;
}

SessionInfo SessionRegistry::inspect(const std::string& id) const {
    if (!validId(id)) throw std::invalid_argument("Invalid session id");
    if (const auto it = active.find(id); it != active.end())
        if (const auto session = it->second.lock()) return session->getInfo();
    if (!fs::is_regular_file(directory / id / "session.sqlite")) throw std::runtime_error("Session not found");
    SessionJournal journal(directory / id);
    const auto info = journal.info();
    if (!info || info->id != id) throw std::runtime_error("Session metadata not found");
    return *info;
}

void SessionRegistry::requireWorkspaceAvailable(const fs::path& workspace) const {
    for (const auto& path : deletingWorktrees) {
        if (AgentWorktree::containsWorkspace(path, workspace))
            throw std::logic_error("Worktree deletion is in progress");
    }
}

void SessionRegistry::beginWorktreeDeletion(const fs::path& path) {
    const auto target = fs::canonical(path);
    requireWorkspaceAvailable(target);
    for (const auto& workspace : openedWorkspaces()) {
        if (AgentWorktree::containsWorkspace(target, workspace))
            throw std::logic_error("Worktree is opened in a session and cannot be deleted");
    }
    deletingWorktrees.insert(target);
}

void SessionRegistry::endWorktreeDeletion(const fs::path& path) { deletingWorktrees.erase(path); }

void SessionRegistry::watchWorkspace(const void* owner, std::function<fs::path()> workspace) {
    workspaceObservers[owner] = std::move(workspace);
}

void SessionRegistry::unwatchWorkspace(const void* owner) { workspaceObservers.erase(owner); }

std::vector<fs::path> SessionRegistry::openedWorkspaces() const {
    std::vector<fs::path> result;
    for (const auto& [owner, workspace] : workspaceObservers) {
        const auto path = workspace();
        if (!path.empty()) result.push_back(path);
    }
    for (const auto& [id, weak] : active) {
        if (const auto session = weak.lock()) {
            result.push_back(session->getInfo().workspace);
            if (!session->getInfo().executionWorkspace.empty()) result.push_back(session->getInfo().executionWorkspace);
        }
    }
    for (const auto& id : loading) {
        const auto info = inspect(id);
        result.push_back(info.workspace);
        if (!info.executionWorkspace.empty()) result.push_back(info.executionWorkspace);
    }
    return result;
}

std::vector<SessionInfo> SessionRegistry::list() const {
    std::vector<SessionInfo> result;
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (!entry.is_directory() || !validId(entry.path().filename().string())) continue;
        try { result.push_back(readInfo(entry.path() / "info.json")); }
        catch (const std::exception&) {
            if (!fs::exists(entry.path() / "session.sqlite")) throw;
            SessionJournal journal(entry.path());
            const auto info = journal.info();
            if (!info) throw std::runtime_error("Session metadata not found");
            result.push_back(*info);
            try { journal.publishInfo(); } catch (const std::exception&) { /* Cache remains optional. */ }
        }
    }
    std::erase_if(result, [](const auto& info) { return !info.discussionId.empty(); });
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.updatedAt > b.updatedAt; });
    return result;
}

std::string SessionRegistry::lastUserMessage(const std::string& id, core::CancellationCheck cancelled) const {
    if (!validId(id)) throw std::invalid_argument("Invalid session id");
    const auto path = directory / id;
    if (!fs::is_directory(path)) throw std::runtime_error("Session not found");
    SessionJournal journal(path, std::move(cancelled));
    if (!journal.info() || journal.info()->id != id) throw std::runtime_error("Session id does not match its directory");
    return journal.lastUserMessage();
}

asio::awaitable<std::shared_ptr<Session>> SessionRegistry::loadAsync(const std::string& id,
    std::optional<ProviderSelection> selection, core::CancellationCheck cancelled) {
    if (!validId(id)) throw std::invalid_argument("Invalid session id");
    if (loading.contains(id) || (active.contains(id) && !active.at(id).expired()))
        throw std::logic_error("Session is already attached or loading");
    const auto info = inspect(id);
    requireWorkspaceAvailable(info.workspace);
    if (!info.executionWorkspace.empty()) requireWorkspaceAvailable(info.executionWorkspace);
    loading.insert(id);
    try {
        auto job = std::async(std::launch::async, [this, id, selection, cancelled] { return readSession(id, selection, cancelled); });
        co_await awaitFuture(job);
        auto session = job.get();
        core::checkCancellation(cancelled);
        active[id] = session;
        loading.erase(id);
        co_return session;
    } catch (...) { loading.erase(id); throw; }
}

asio::awaitable<std::string> SessionRegistry::previewAsync(const std::string& id, core::CancellationCheck cancelled) {
    if (!validId(id)) throw std::invalid_argument("Invalid session id");
    if (loading.contains(id)) throw std::logic_error("Session is loading; retry shortly");
    if (const auto found = active.find(id); found != active.end())
        if (const auto session = found->second.lock()) co_return session->lastUserMessage();
    loading.insert(id);
    try {
        auto job = std::async(std::launch::async, [this, id, cancelled] { return lastUserMessage(id, cancelled); });
        co_await awaitFuture(job);
        auto text = job.get();
        loading.erase(id);
        co_return text;
    } catch (...) { loading.erase(id); throw; }
}
