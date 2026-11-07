#ifndef SOS_SESSION_HPP
#define SOS_SESSION_HPP

#include "session_state.hpp"
#include "compaction_service.hpp"
#include "session_history_service.hpp"
#include "session_journal.hpp"
#include "../snapshots/turn_files.hpp"
#include <agent-runtime/skills/skill_catalog.hpp>
#include <asio/awaitable.hpp>

class Session {
public:
    static std::optional<Conversation> compactBeforeGeneration(const CompactionContext& context, Provider& provider,
        const SessionInfo& info, const std::string& id, const AgentEventHandler& emit, core::CancellationCheck cancelled);

    struct LoadedSkill {
        SkillInvocation invocation;
        std::string content;
        nlohmann::json metadata = nlohmann::json::object();
    };
    Session(SessionState state, std::filesystem::path directory);
    Session(SessionInfo info, std::filesystem::path directory, SessionAgentFactory factory,
        std::shared_ptr<SessionJournal> journal);

    const SessionInfo& getInfo() const;
    void setExecutionWorkspace(const std::filesystem::path& workspace);
    void setWorkspaceHandler(Agent::WorkspaceHandler handler);
    asio::awaitable<void> switchWorkspace(const std::filesystem::path& workspace, const std::filesystem::path& root);
    void recordTerminalOpen(const std::string& terminalSessionId, std::uint64_t openedAt = 0);
    std::vector<Message> getMessages() const;
    std::vector<Message> getHistory() const;
    std::size_t messageCount() const;
    std::string lastUserMessage() const;
    bool objectivePlanReady() const;
    bool implementationPlanReady() const;
    std::optional<Message> latestImplementationPlan(bool requireReady = true) const;
    std::optional<Message> latestObjectivePlan() const;
    std::vector<Message> objectivePlans() const;
    struct ObjectivePlanTurn {
        std::string messageId, objectiveId;
        bool updated = false;
    };
    std::vector<ObjectivePlanTurn> objectivePlanTurns() const;
    void linkImplementation(const std::string& id);
    void freezeImplementation();
    void returnToDiscussion();
    bool agentReady() const;
    asio::awaitable<bool> prepareAgent(core::CancellationCheck cancelled);
    SessionHistoryService& history();
    bool lastHistoryIsRecap() const;
    bool hasStoredImages() const;
    bool hasActiveImages() const;
    std::vector<Message> btwSnapshot() const;
    std::shared_ptr<SessionJournal> btwStore() const { return journal; }
    const std::filesystem::path& storageDirectory() const { return directory; }
    Conversation activeConversation();
    std::vector<Message> reviewSnapshot(std::optional<std::size_t> end = {}) const;
    void incorporateAcceptedReview(const std::string& branchId, const std::string& request);
    TurnSnapshot completedSnapshot() const;
    std::optional<TurnSnapshot> latestSnapshot() const { return journal->navigationTurn(false); }
    const std::optional<ImplementationCompletion>& implementationCompletion() const { return completedImplementation; }
    void inspectImplementationChanges(std::function<nlohmann::json()> inspect) { implementationInspector = std::move(inspect); }
    void captureImplementationProgress(std::function<std::int64_t(const nlohmann::json&, const TurnSnapshot&)> capture,
        std::function<void(bool)> mutationGuard = {}) {
        implementationProgress = std::move(capture);
        implementationMutationGuard = std::move(mutationGuard);
    }
    void applyCompaction(CompactionResult result);
    void saveRecap(const Message& recap);
    void setEventHandler(AgentEventHandler handler);
    void sendMessage(const std::string& text, ExecutionMode mode,
        std::chrono::steady_clock::time_point startedAt = std::chrono::steady_clock::now(), std::vector<Image> images = {},
        std::vector<LoadedSkill> loadedSkills = {}, bool saneRequest = false, bool implementationRequest = false,
        ImplementationPhase phase = ImplementationPhase::PLANNING);
    bool isExecuting() const;
    const std::string& getExecutionReasoning() const;
    GenerationMetadata executionGeneration() const;
    void cancel();
    void setReasoningEffort(const std::string& effort);
    void applySelection(const ProviderSelection& selection, std::unique_ptr<Provider> replacement = {});
    void setPermissionHandler(ToolPermissionHandler handler, const std::filesystem::path& root);
    void setQuestionHandler(std::function<std::string(const std::string&, const std::vector<std::string>&,
        const std::function<void()>&)> handler);
    // Called by the Asio event consumer, never by the agent's worker.
    void record(const AgentEvent& event);
    void save(bool publish = true);
    void setAutoPlan(std::string pace, std::string error = {});
    void setManualApproval(const std::string& refusedMessageId, bool approved);
    nlohmann::json manualApprovalStates() const;
    std::vector<nlohmann::json> manualActions() const;
    asio::awaitable<void> finishExecution();
    asio::awaitable<void> prepareTurn();
    asio::awaitable<Message> navigate(bool redo);

private:
    void captureSubmittedPlans(const std::vector<Message>& messages);
    void appendFinalizedPlans();
    void appendSkillCalls(const std::string& userId, const std::vector<LoadedSkill>& skills);
    void announceMode(ExecutionMode mode);
    void announceManualApprovals();
    void prepareApprovedReplay();
    std::vector<nlohmann::json> replayApprovals() const;
    std::unique_ptr<Agent> buildAgent(core::CancellationCheck cancelled) const;
    Conversation loadConversation(core::CancellationCheck cancelled, std::optional<std::size_t> end = {}) const;
    void installAgent(std::unique_ptr<Agent> agent);
    void ensureAgent();
    void prepareAutomaticCompaction();
    void saveCompaction(Message summary);
    void publishInfo() const;
    SessionState state;
    std::filesystem::path directory;
    std::shared_ptr<SessionJournal> journal;
    SessionAgentFactory factory;
    std::optional<std::uint64_t> contextLimit;
    ToolPermissionHandler permissionHandler;
    Agent::WorkspaceHandler workspaceHandler;
    std::function<std::string(const std::string&, const std::vector<std::string>&,
        const std::function<void()>&)> questionHandler;
    std::filesystem::path permissionRoot;
    SessionHistoryService displayHistory;
    std::vector<Message> pending;
    AgentEventHandler eventHandler;
    std::shared_future<void> execution;
    bool executing = false;
    std::string executionReasoning;
    std::optional<ProviderSelection> pendingSelection;
    std::unique_ptr<Provider> pendingProvider;
    std::shared_ptr<TurnFiles> turnFiles;
    std::optional<TurnSnapshot> preparedTurn;
    bool saneTurn = false;
    bool implementationTurn = false;
    bool constructionTurn = false;
    std::optional<ImplementationCompletion> completedImplementation;
    std::function<nlohmann::json()> implementationInspector;
    std::function<std::int64_t(const nlohmann::json&, const TurnSnapshot&)> implementationProgress;
    std::function<void(bool)> implementationMutationGuard;
    std::optional<ImplementationPlan> submittedImplementationPlan;
    std::optional<Message> finalizedImplementationPlan;
};

#endif // SOS_SESSION_HPP
