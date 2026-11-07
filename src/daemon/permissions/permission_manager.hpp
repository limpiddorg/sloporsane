#ifndef SOS_PERMISSION_MANAGER_HPP
#define SOS_PERMISSION_MANAGER_HPP

#include <agent-runtime/tools/permission.hpp>
#include "execution_mode.hpp"
#include "permission_mode.hpp"
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <set>

enum class PermissionEventType { REQUESTED, RESOLVED, DISMISSED };
struct PermissionEvent {
    PermissionEventType type;
    std::string id;
    ToolPermissionRequest request;
    std::optional<PermissionDecision> decision;
    std::optional<double> confidence;
};
void to_json(nlohmann::json& json, const PermissionEvent& event);

// One instance per connection. Asio owns configuration/replies; the agent worker calls ask().
class PermissionManager {
public:
    explicit PermissionManager(std::function<void(PermissionEvent)> notify);
    void configure(const std::filesystem::path& workspace, ExecutionMode mode);
    bool isConfigured() const;
    std::filesystem::path root() const;
    void setExecutionRoot(const std::filesystem::path& root);
    ExecutionMode mode() const;
    void setMode(ExecutionMode mode);
    void setImplementationPhase(ImplementationPhase phase);
    using Verifier = std::function<std::optional<double>(const ToolPermissionRequest&, const std::function<void()>&)>;
    void setPermissionMode(PermissionMode mode, Verifier verifier = {}, int confidenceThreshold = 80);
    void setManualActions(std::vector<nlohmann::json> actions);
    PermissionAnswer ask(const ToolPermissionRequest& request, const std::function<void()>& checkCancelled);
    void reply(const std::string& id, PermissionDecision decision);
    void close();

private:
    struct Pending {
        std::string id;
        ToolPermissionRequest request;
        std::optional<PermissionDecision> answer;
    };
    bool forbidden(const ToolPermissionRequest& request) const;
    void requireAllowed(const ToolPermissionRequest& request) const;
    bool approved(const ToolPermissionRequest& request) const;
    void remember(const ToolPermissionRequest& request);
    PermissionAnswer waitForApproval(const ToolPermissionRequest& request,
        const std::function<void()>& checkCancelled, std::unique_lock<std::mutex>& lock,
        std::optional<double> confidence = {});

    mutable std::mutex mutex;
    std::condition_variable changed;
    std::filesystem::path workspace;
    std::filesystem::path executionRoot;
    ExecutionMode currentMode = ExecutionMode::SANE;
    ImplementationPhase implementationPhase = ImplementationPhase::PLANNING;
    PermissionMode permissionMode = PermissionMode::ASK;
    Verifier verifier;
    int confidenceThreshold = 80;
    std::vector<nlohmann::json> manualActions;
    std::uint64_t policyVersion = 0;
    std::vector<std::filesystem::path> directories;
    std::vector<std::filesystem::path> callDirectories;
    bool editsApproved = false;
    std::set<std::string> shellCommands;
    std::set<std::string> mcpTools;
    std::optional<Pending> pending;
    std::uint64_t nextId = 0;
    bool closed = false;
    std::function<void(PermissionEvent)> notify;
};

#endif // SOS_PERMISSION_MANAGER_HPP
