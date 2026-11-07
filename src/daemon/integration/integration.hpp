#ifndef SOS_INTEGRATION_HPP
#define SOS_INTEGRATION_HPP

#include "../session/session_registry.hpp"
#include "../session/session_title.hpp"
#include "../transport/connection_socket.hpp"
#include "../transport/async_queue.hpp"
#include "../config.hpp"
#include "../permissions/permission_manager.hpp"
#include "../providers/provider_connection.hpp"
#include "../mcp/management.hpp"
#include "../mcp/oauth.hpp"
#include "question_state.hpp"
#include "../session/implementation_run.hpp"
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <variant>
#include <functional>
#include <future>
#include <map>
struct IntegrationRequest {
    std::string type;
    std::string requestId;
    std::string sessionId;
    std::string direction = "latest";
    std::string anchor;
    std::string groupId;
    std::size_t activityIndex = 0;
    bool clean = false;
    std::string pingId;
    std::string text;
    bool saneRequest = false;
    bool implementationRequest = false;
    std::string implementationPace;
    std::string repositoryTree, repositoryPath;
    std::optional<std::size_t> reviewHunk;
    std::string reviewPath;
    std::size_t stepIndex = 0, substepIndex = 0;
    std::uint64_t revision = 0;
    bool revisionProvided = false;
    bool historyAll = false;
    std::string questionId;
    std::optional<std::size_t> questionChoice;
    std::vector<SkillInvocation> skills;
    std::string branchId;
    std::string objectiveId;
    std::string worktreeName, worktreePath;
    std::vector<Image> images;
    bool hasImages = false;
    std::string workspace;
    std::string terminalSessionId;
    std::string model = daemon_config::DEFAULT_MODEL;
    std::optional<std::string> reasoning;
    ExecutionMode mode = ExecutionMode::SANE;
    std::optional<ExecutionMode> messageMode;
    std::string permissionId;
    PermissionMode permissionMode = PermissionMode::ASK;
    std::string permissionKey;
    std::optional<int> permissionConfidence;
    std::string refusedMessageId;
    bool manuallyApproved = false;
    PermissionDecision decision = PermissionDecision::NO;
    bool cancelApplied = false; // Internal; never read from client JSON.
    bool blockedBySummary = false;
    std::optional<ProviderRequest> providerRequest;
    std::optional<mcp::ManagementRequest> mcpRequest;
    bool invalidMcp = false; // Internal parse failure, never read from the wire.
};
void from_json(const nlohmann::json& json, IntegrationRequest& request);

struct IntegrationHealthSettings {
    std::chrono::milliseconds interval = daemon_config::INTEGRATION_PING_INTERVAL;
    std::chrono::milliseconds timeout = daemon_config::INTEGRATION_PONG_TIMEOUT;
};

class Integration : public std::enable_shared_from_this<Integration> {
public:
    using BtwProviderFactory = std::function<std::unique_ptr<Provider>(const ProviderSelection&)>;
    using ReasoningEffortsLoader = std::function<std::vector<std::string>(const std::string& model)>;
    Integration(asio::io_context& io, ConnectionSocket connection, SessionRegistry& registry,
        SessionTitleGenerator titleGenerator, ReasoningEffortsLoader reasoningEffortsLoader = {},
        IntegrationHealthSettings health = {}, std::shared_ptr<ProviderConnections> providers = {},
        CompactionService::ProviderFactory summaryProvider = {}, BtwProviderFactory btwProvider = {},
        std::shared_ptr<mcp::Connections> mcps = {});
    asio::awaitable<void> run();
    ~Integration();
    void close();

private:
    struct Disconnected {};
    struct Ping {};
    struct McpRestored { std::vector<std::string> errors; bool hasServers = false; };
    struct BtwRun {
        std::shared_ptr<Session> owner;
        BtwConversation branch;
        std::unique_ptr<Agent> agent;
        std::size_t reviewStep = 0;
        std::uint64_t reviewRevision = 0;
        std::optional<ImplementationRevisionProposal> reworkProposal;
        std::shared_future<void> execution;
        std::filesystem::path reviewDirectory;
        ~BtwRun() { if (!reviewDirectory.empty()) { std::error_code error; std::filesystem::remove_all(reviewDirectory, error); } }
    };
    struct BtwEvent { std::shared_ptr<BtwRun> run; AgentEvent event; };
    struct ImplementationNext {};
    struct WorkspaceRequest {
        nlohmann::json arguments;
        std::shared_ptr<std::promise<nlohmann::json>> result;
    };
    using Event = std::variant<IntegrationRequest, AgentEvent, PermissionEvent, QuestionEvent, BtwEvent, Disconnected, Ping, McpRestored, ImplementationNext, WorkspaceRequest>;
    asio::awaitable<nlohmann::json> handleWorkspace(const WorkspaceRequest& request);
    asio::awaitable<nlohmann::json> autoPlan(const IntegrationRequest& request);
    asio::awaitable<void> finishAutoPlan(const AgentEvent& event);
    void acceptImplementationPlan(const std::string& pace);
    void annotateWorktrees(nlohmann::json& entries, const std::filesystem::path& current) const;
    asio::awaitable<nlohmann::json> deleteWorktree(const std::filesystem::path& source, const std::filesystem::path& target);
    std::filesystem::path pendingWorkspace;
    std::filesystem::path idleWorkspace;
    std::filesystem::path currentWorkspace() const;
    std::shared_ptr<QuestionState> questions = std::make_shared<QuestionState>();
    std::map<std::string, std::shared_ptr<BtwRun>> btwRuns;
    std::uint64_t btwTransfer = 0;
    BtwProviderFactory btwProvider;
    asio::awaitable<nlohmann::json> handleBtw(const IntegrationRequest& request);
    std::shared_ptr<BtwRun> startBtw(const IntegrationRequest& request);
    asio::awaitable<nlohmann::json> recordBtwEvent(const BtwEvent& event);
    asio::awaitable<void> acceptImplementationRevision(const IntegrationRequest& request);
    asio::awaitable<void> seedImplementationRevision();
    asio::awaitable<void> sendBtw(const BtwConversation& branch, const std::string& sessionId);
    asio::awaitable<void> readRequests();
    asio::awaitable<nlohmann::json> handle(const IntegrationRequest& request);
    void subscribe();
    void schedulePing();
    void startReader();
    void prepareRequest(IntegrationRequest& request);
    void acknowledgePong(const std::string& id);
    nlohmann::json makePing();
    nlohmann::json runtimeInfo() const;
    mutable nlohmann::json skillInfo = {{"skills", nlohmann::json::array()}, {"skill_errors", nlohmann::json::array()}};
    asio::awaitable<nlohmann::json> dispatch(Event& event);
    asio::awaitable<nlohmann::json> handleRequest(const IntegrationRequest& request);
    asio::awaitable<nlohmann::json> recordAgentEvent(const AgentEvent& event);
    asio::awaitable<void> sendReply(nlohmann::json reply);
    asio::awaitable<nlohmann::json> handleProvider(const IntegrationRequest& request);
    asio::awaitable<nlohmann::json> handleMcp(const IntegrationRequest& request);
    asio::awaitable<void> reportMcpRestoration(std::shared_future<void> restoration, std::filesystem::path workspace);
    asio::awaitable<nlohmann::json> startMcpOAuth(const IntegrationRequest& request);
    asio::awaitable<nlohmann::json> pollMcpOAuth(const IntegrationRequest& request);
    asio::awaitable<nlohmann::json> cancelMcpOAuth(const IntegrationRequest& request);
    std::shared_ptr<mcp::Connections> mcps;
    std::shared_ptr<std::atomic<bool>> mcpCancellation;
    bool mcpRestoring = false;
    std::unique_ptr<mcp::OAuthFlow> mcpOAuth;
    std::string mcpOAuthName, mcpOAuthIdentity;
    nlohmann::json handlePermissions(const IntegrationRequest& request);
    asio::awaitable<nlohmann::json> selectPermissions(const IntegrationRequest& request);
    nlohmann::json newSession(const IntegrationRequest& request);
    asio::awaitable<nlohmann::json> attachSession(const IntegrationRequest& request);
    asio::awaitable<nlohmann::json> switchImplementation(const IntegrationRequest& request);
    asio::awaitable<nlohmann::json> handleImplementation(const IntegrationRequest& request);
    asio::awaitable<void> prepareImplementationCheckout();
    asio::awaitable<void> importImplementationStep(std::size_t step, const TurnSnapshot& boundary);
    std::filesystem::path executionPermissionRoot() const;
    asio::awaitable<nlohmann::json> startImplementationStep();
    void configureImplementationEvidence(const ImplementationRun& run);
    asio::awaitable<void> finishImplementationStep(const AgentEvent& event);
    ImplementationRun& ensureImplementationRun();
    std::unique_ptr<ImplementationRun> implementationRun;
    std::string runSessionId;
    bool implementationExecuting = false, implementationStopping = false;
    std::shared_ptr<Session> discussion;
    std::shared_ptr<Session> implementation;
    bool viewingArchive = false;
    asio::awaitable<void> sendHistory(const IntegrationRequest& request, const std::optional<Message>& draft = {},
        std::shared_ptr<Session> source = {});
    asio::awaitable<nlohmann::json> navigateSession(const IntegrationRequest& request);
    asio::awaitable<void> sendHistoryFrame(nlohmann::json frame);
    asio::awaitable<nlohmann::json> nextReasoningEffort(const IntegrationRequest& request);
    asio::awaitable<nlohmann::json> sendMessage(const IntegrationRequest& request);
    std::vector<Session::LoadedSkill> loadSkills(const IntegrationRequest& request) const;
    asio::awaitable<void> checkImageInput(bool hasNewImages);
    asio::awaitable<ImageSupport> loadImageSupport();
    asio::awaitable<nlohmann::json> compactSession(const IntegrationRequest& request);
    asio::awaitable<nlohmann::json> recapSession(const IntegrationRequest& request);
    asio::awaitable<std::optional<CompactionResult>> generateCompaction(const SessionInfo& info, const std::string& requestId);
    SessionInfo makeSessionInfo(const IntegrationRequest& request);
    asio::awaitable<bool> createSession(SessionInfo info, const std::string& text);
    asio::awaitable<std::optional<std::string>> generateTitle(const SessionInfo& info, const std::string& text);
    nlohmann::json cancelledReply(const IntegrationRequest& request, const SessionInfo& info,
        std::chrono::steady_clock::time_point startedAt) const;
    bool consumePreparationCancellation();
    void requireIdleSession() const;
    asio::awaitable<void> stopExecution();
    void saveSession();
    nlohmann::json listSessions() const;
    void detachSession();
    void resetConversationDefaults();
    void applyCurrentSelection();

    asio::io_context& io;
    ConnectionSocket connection;
    SessionRegistry& registry;
    SessionTitleGenerator titleGenerator;
    ReasoningEffortsLoader reasoningEffortsLoader;
    std::shared_ptr<Session> session;
    std::string terminalSessionId;
    std::shared_ptr<AsyncQueue<Event>> events;
    std::shared_ptr<PermissionManager> permissions;
    std::optional<ExecutionMode> selectedMode;
    std::shared_ptr<std::atomic<bool>> preparingCancellation;
    std::shared_ptr<std::atomic<bool>> btwPreparationCancellation;
    std::shared_ptr<std::atomic<bool>> summaryCancellation;
    std::shared_ptr<std::atomic<bool>> historyCancellation;
    std::shared_ptr<std::atomic<bool>> permissionCancellation;
    CompactionService::ProviderFactory summaryProvider;
    IntegrationHealthSettings health;
    asio::steady_timer healthTimer;
    std::uint64_t pingNumber = 0;
    std::string expectedPong;
    bool pingDue = false;
    bool closing = false;
    std::shared_ptr<ProviderConnections> providers;
    ProviderConnection providerConnection;
};

#endif // SOS_INTEGRATION_HPP
