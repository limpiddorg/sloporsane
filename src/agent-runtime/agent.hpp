#ifndef SOS_AGENT_HPP
#define SOS_AGENT_HPP

#include "conversation/conversation.hpp"
#include "conversation/compaction_context.hpp"
#include "providers/provider.hpp"
#include "tools/registry.hpp"
#include "agent_event.hpp"
#include "conversation/messages/implementation_completion.hpp"
#include "skills/skill_catalog.hpp"
#include <atomic>
#include <chrono>

#include <future>
#include <memory>
#include <mutex>
#include <string>

class Agent {
public:
    using ContextPreparation = std::function<std::optional<Conversation>(const CompactionContext&, Provider&, core::CancellationCheck)>;
    Agent(std::unique_ptr<Provider> provider, std::string model,
        ToolRegistry tools = {}, ToolContext toolContext = {}, std::string reasoningEffort = "medium");
    ~Agent();

    void append(const Message& message);
    void enableProjectContext(const std::filesystem::path& workspace, const std::filesystem::path& configuration = {});
    using WorkspaceHandler = std::function<nlohmann::json(const nlohmann::json&, const std::function<void()>&)>;
    void setWorkspaceHandler(WorkspaceHandler handler);
    void switchWorkspace(const std::filesystem::path& workspace, const std::filesystem::path& root);
    std::vector<Message> getMessages() const;
    bool hasImages() const;
    void replaceConversation(Conversation replacement);
    std::shared_future<void> run(std::chrono::steady_clock::time_point startedAt = std::chrono::steady_clock::now());
    bool isRunning() const;
    void setEventHandler(AgentEventHandler handler);
    void setContextPreparation(ContextPreparation prepare);
    using ToolSource = std::function<std::vector<std::unique_ptr<Tool>>(core::CancellationCheck)>;
    // Installed while idle; invoked on the worker at each generation boundary.
    void setToolSource(ToolSource source);
    void setContextLimit(std::optional<std::uint64_t> limit);
    void cancel();
    void setReasoningEffort(std::string effort);
    void setSessionId(std::string id);
    void setGenerationSettings(std::string model, std::string effort, std::size_t portableBefore,
        std::unique_ptr<Provider> replacement = {});
    void queueGenerationSettings(std::string model, std::string effort, std::optional<std::uint64_t> limit,
        std::unique_ptr<Provider> replacement = {});
    void finishGenerationSettings();
    GenerationMetadata executionGeneration() const;
    void setPermissionHandler(ToolPermissionHandler handler, const std::filesystem::path& root);
    ContextUsage getContextUsage() const;
    void restoreContextUsage(ContextUsage usage);
    void setImageReader(std::function<Image(const std::string&)> reader);
    void setFileMutationHandler(FileMutationHandler handler);
    void setApprovedActionResolver(std::function<ApprovedToolAction(const std::string&)> resolver);
    void setQuestionHandler(std::function<std::string(const std::string&, const std::vector<std::string>&,
        const std::function<void()>&)> handler);
    void setSanePlanning(bool enabled, std::string plan = {}, std::string stack = {});
    void setImplementationPlanning(bool enabled);
    using ImplementationProgressHandler = std::function<std::int64_t(const nlohmann::json&)>;
    void setImplementationExecution(bool enabled, std::function<nlohmann::json()> inspect = {},
        ImplementationProgressHandler progress = {});

private:
    void adoptWorkspace(const std::filesystem::path& workspace, const std::filesystem::path& root);
    std::filesystem::path projectWorkspace;
    std::filesystem::path skillsWorkspace;
    bool workspaceMoved = false;
    std::shared_ptr<SkillCatalog> skillCatalog;
    struct PendingSettings {
        std::string model, reasoning;
        std::optional<std::uint64_t> limit;
        std::unique_ptr<Provider> provider;
    };
    bool applyPendingSettings(); // Requires mutex; only the worker may call it while running.
    std::optional<PendingSettings> pendingSettings;
    std::string activeReasoning;
    std::unique_ptr<Provider> provider;
    std::string model;
    std::string sessionId;
    std::string reasoningEffort;
    ContextUsage contextUsage;
    std::optional<std::uint64_t> contextLimit;
    std::size_t measuredMessages = 0;
    ContextPreparation prepareContext;
    ToolSource toolSource;
    Conversation conversation;
    ToolRegistry tools;
    ToolContext toolContext;
    ToolPermissionHandler permissionHandler;
    mutable std::mutex mutex;
    bool running = false;
    std::shared_future<void> execution;
    AgentEventHandler eventHandler;
    std::atomic<bool> cancellationRequested = false;
    bool sanePlanning = false;
    bool implementationPlanning = false;
    bool implementationExecution = false;
};

#endif // SOS_AGENT_HPP
