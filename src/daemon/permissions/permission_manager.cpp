#include "permission_manager.hpp"
#include <agent-runtime/core/cancellation.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>

namespace {
bool contains(const std::filesystem::path& root, const std::filesystem::path& path) {
    auto p = path.begin();
    for (auto r = root.begin(); r != root.end(); ++r, ++p)
        if (p == path.end() || *p != *r) return false;
    return true;
}
bool editTool(const std::string& name) { return name == "write" || name == "edit" || name == "apply_patch"; }
}

void to_json(nlohmann::json& json, const PermissionEvent& event) {
    const auto type = event.type == PermissionEventType::REQUESTED ? "permission.requested" :
        event.type == PermissionEventType::RESOLVED ? "permission.resolved" : "permission.dismissed";
    json = {{"type", type}, {"permissionId", event.id}, {"callId", event.request.callId},
        {"tool", event.request.toolName}, {"permission", event.request.permission},
        {"patterns", event.request.patterns}, {"always", event.request.always}, {"details", event.request.metadata}};
    if (event.decision) json["decision"] = permissionDecisionName(*event.decision);
    if (event.confidence) json["confidence"] = *event.confidence;
}

PermissionManager::PermissionManager(std::function<void(PermissionEvent)> notify) : notify(std::move(notify)) {}

void PermissionManager::configure(const std::filesystem::path& root, ExecutionMode mode) {
    if (!root.is_absolute() || !std::filesystem::is_directory(root))
        throw std::invalid_argument("Authorized workspace must be an existing absolute directory");
    const auto canonical = std::filesystem::canonical(root);
    std::lock_guard lock(mutex);
    if (!workspace.empty()) throw std::logic_error("Integration workspace is already configured");
    workspace = canonical;
    currentMode = mode;
}
bool PermissionManager::isConfigured() const { std::lock_guard lock(mutex); return !workspace.empty(); }
std::filesystem::path PermissionManager::root() const { std::lock_guard lock(mutex); return workspace; }
void PermissionManager::setExecutionRoot(const std::filesystem::path& root) {
    std::lock_guard lock(mutex);
    if (executionRoot == root) return;
    executionRoot = root;
    ++policyVersion;
    changed.notify_all();
}
ExecutionMode PermissionManager::mode() const { std::lock_guard lock(mutex); return currentMode; }
void PermissionManager::setMode(ExecutionMode mode) {
    std::lock_guard lock(mutex);
    currentMode = mode;
    ++policyVersion;
    changed.notify_all();
}

void PermissionManager::setImplementationPhase(ImplementationPhase phase) {
    std::lock_guard lock(mutex);
    if (implementationPhase == phase) return;
    implementationPhase = phase;
    // Grants made for a writable phase must not leak into a later phase.
    directories.clear();
    callDirectories.clear();
    shellCommands.clear();
    mcpTools.clear();
    editsApproved = false;
    ++policyVersion;
    changed.notify_all();
}

void PermissionManager::setPermissionMode(PermissionMode mode, Verifier check, int threshold) {
    if (threshold < 0 || threshold > 100) throw std::invalid_argument("Confidence must be between 0 and 100");
    std::lock_guard lock(mutex);
    if (permissionMode != mode) {
        directories.clear();
        callDirectories.clear();
        shellCommands.clear();
        mcpTools.clear();
        editsApproved = false;
    }
    permissionMode = mode;
    verifier = std::move(check);
    confidenceThreshold = threshold;
    ++policyVersion;
    changed.notify_all();
}

bool PermissionManager::forbidden(const ToolPermissionRequest& request) const {
    const bool readOnly = implementationPhase == ImplementationPhase::REVIEW ||
        (currentMode == ExecutionMode::SANE && implementationPhase != ImplementationPhase::EXECUTION);
    return readOnly && (editTool(request.toolName) || request.permission == "edit" ||
        ((request.permission == "worktree" || request.permission == "workspace") && !request.readOnly && implementationPhase == ImplementationPhase::REVIEW) ||
        (request.permission == "mcp" && !request.readOnly) ||
        ((request.permission == "bash" || (request.permission == "external_directory" &&
            (request.toolName == "bash" || request.toolName == "wait"))) && !request.readOnly));
}

void PermissionManager::requireAllowed(const ToolPermissionRequest& request) const {
    if (forbidden(request)) throw std::runtime_error(std::string(
        implementationPhase == ImplementationPhase::REVIEW ? "Forbidden in read-only review: " : "Forbidden in Sane mode: ") + request.toolName);
}

void PermissionManager::setManualActions(std::vector<nlohmann::json> actions) {
    std::lock_guard lock(mutex);
    manualActions = std::move(actions);
}

bool PermissionManager::approved(const ToolPermissionRequest& request) const {
    if (request.permission == "external_directory") {
        const auto path = std::filesystem::weakly_canonical(request.metadata.at("parentDir").get<std::string>());
        return std::any_of(directories.begin(), directories.end(), [&](const auto& root) { return contains(root, path); });
    }
    if (request.permission == "edit") return editsApproved;
    if (request.permission == "mcp")
        return !request.always.empty() && std::all_of(request.always.begin(), request.always.end(),
            [&](const auto& binding) { return mcpTools.contains(binding); });
    if (request.permission == "bash")
        return !request.always.empty() && std::all_of(request.always.begin(), request.always.end(),
            [&](const auto& command) { return shellCommands.contains(command); });
    return false;
}

void PermissionManager::remember(const ToolPermissionRequest& request) {
    if (request.permission == "external_directory")
        directories.push_back(std::filesystem::weakly_canonical(request.metadata.at("parentDir").get<std::string>()));
    else if (request.permission == "edit") editsApproved = true;
    else if (request.permission == "bash") shellCommands.insert(request.always.begin(), request.always.end());
    else if (request.permission == "mcp") mcpTools.insert(request.always.begin(), request.always.end());
}

PermissionAnswer PermissionManager::ask(const ToolPermissionRequest& request, const std::function<void()>& checkCancelled) {
    checkCancelled();
    std::unique_lock lock(mutex);
    if (closed) throw core::Cancelled();
    if (workspace.empty()) throw std::logic_error("Integration workspace is not configured");
    if (request.permission == "tool") callDirectories.clear();
    requireAllowed(request);
    if (request.manuallyApproved) {
        std::string id;
        if (request.permission == "tool") {
            id = std::to_string(++nextId);
            notify({PermissionEventType::RESOLVED, id, request, PermissionDecision::MANUALLY_APPROVED,
                request.manualApprovalConfidence});
        }
        return {PermissionDecision::MANUALLY_APPROVED, id, {}, request.manualApprovalConfidence};
    }
    if (request.permission == "worktree" || request.permission == "workspace" || request.permission == "tool" || request.permission == "read" || request.permission == "glob" ||
        request.permission == "grep" || request.permission == "webfetch" ||
        (request.permission == "bash" && request.readOnly)) return {};
    if (request.permission != "edit" && request.permission != "bash" && request.permission != "external_directory" && request.permission != "mcp")
        throw std::runtime_error("Unknown permission: " + request.permission);
    if (request.permission == "external_directory") {
        const auto path = std::filesystem::weakly_canonical(request.metadata.at("parentDir").get<std::string>());
        if (contains(workspace, path) || (!executionRoot.empty() && contains(executionRoot, path)) || std::any_of(callDirectories.begin(), callDirectories.end(),
            [&](const auto& root) { return contains(root, path); })) return {};
    }
    if (!manualActions.empty() && request.metadata.contains("arguments") && request.metadata.contains("working_directory")) {
        const nlohmann::json action{{"tool", request.toolName}, {"arguments", request.metadata.at("arguments")},
            {"working_directory", request.metadata.at("working_directory")},
            {"workspace", (executionRoot.empty() ? workspace : executionRoot).string()}};
        if (std::find(manualActions.begin(), manualActions.end(), action) != manualActions.end())
            return {PermissionDecision::MANUALLY_APPROVED, {}};
    }
    if (approved(request)) return {PermissionDecision::ALWAYS, {}};
    if (permissionMode == PermissionMode::DANGER || (permissionMode == PermissionMode::AUTO && confidenceThreshold == 0)) return {};
    if (permissionMode == PermissionMode::AUTO) {
        const auto check = verifier;
        const auto version = policyVersion;
        const auto threshold = confidenceThreshold;
        lock.unlock();
        std::optional<double> confidence;
        try { if (check) confidence = check(request, checkCancelled); }
        catch (const core::Cancelled&) { throw; }
        catch (const std::exception&) {}
        if (confidence && (!std::isfinite(*confidence) || *confidence < 0 || *confidence > 1)) confidence.reset();
        checkCancelled();
        lock.lock();
        if (closed) throw core::Cancelled();
        requireAllowed(request);
        const auto decision = confidence && *confidence >= threshold / 100.0 && version == policyVersion
            ? PermissionDecision::AI_ACCEPTED : PermissionDecision::AI_REFUSED;
        const auto id = std::to_string(++nextId);
        notify({PermissionEventType::RESOLVED, id, request, decision, confidence});
        if (decision == PermissionDecision::AI_ACCEPTED) return {decision, id, {}, confidence};
        if (implementationPhase == ImplementationPhase::EXECUTION) {
            // Keep the exact call suspended: no refusal reaches the model while
            // the user is deciding whether to authorize this action.
            if (version != policyVersion) {
                lock.unlock();
                return ask(request, checkCancelled);
            }
            return waitForApproval(request, checkCancelled, lock, confidence);
        }
        const std::string reason = !confidence
            ? "Tool action blocked: the command-verification AI is unavailable or its check failed. "
            : "Tool action refused by the command-verification AI. ";
        return {decision, id, reason + "The action was not executed. Do not retry or bypass this refusal without manual approval. "
            "You may ask the user to open Activity, expand the action under Refused tools and click Approve, then send a message. "
            "The next user turn includes a report of manual approval changes. Approval permits this exact action repeatedly "
            "in this session until revoked, but never overrides Sane restrictions. Use the supplied approval ID with "
            "rerun_approved_action after approval, without reconstructing the arguments. Otherwise continue without this action.", confidence};
    }
    return waitForApproval(request, checkCancelled, lock);
}

PermissionAnswer PermissionManager::waitForApproval(const ToolPermissionRequest& request,
    const std::function<void()>& checkCancelled, std::unique_lock<std::mutex>& lock,
    std::optional<double> confidence) {
    if (pending) throw std::logic_error("An integration can wait for only one permission at a time");
    const auto version = policyVersion;
    const auto id = std::to_string(++nextId);
    pending = Pending{id, request, {}};
    notify({PermissionEventType::REQUESTED, id, request, {}, confidence});
    try {
        for (;;) {
            lock.unlock();
            checkCancelled();
            lock.lock();
            if (closed) throw core::Cancelled();
            requireAllowed(request);
            if (pending->answer) break;
            if (version != policyVersion) {
                pending.reset();
                notify({PermissionEventType::DISMISSED, id, request, {}});
                lock.unlock();
                return ask(request, checkCancelled);
            }
            changed.wait_for(lock, std::chrono::milliseconds(20));
        }
        const auto decision = *pending->answer;
        if (decision == PermissionDecision::YES && request.permission == "external_directory")
            callDirectories.push_back(std::filesystem::weakly_canonical(request.metadata.at("parentDir").get<std::string>()));
        if (decision == PermissionDecision::ALWAYS) remember(request);
        pending.reset();
        notify({PermissionEventType::RESOLVED, id, request, decision});
        return {decision, id};
    } catch (...) {
        if (!lock.owns_lock()) lock.lock();
        if (pending && pending->id == id) {
            pending.reset();
            notify({PermissionEventType::DISMISSED, id, request, {}});
        }
        throw;
    }
}

void PermissionManager::reply(const std::string& id, PermissionDecision decision) {
    std::lock_guard lock(mutex);
    if (closed || !pending || pending->id != id || pending->answer)
        throw std::invalid_argument("Permission request is no longer pending");
    if (decision != PermissionDecision::YES && decision != PermissionDecision::NO && decision != PermissionDecision::ALWAYS)
        throw std::invalid_argument("Invalid permission reply");
    pending->answer = decision;
    changed.notify_all();
}

void PermissionManager::close() {
    std::lock_guard lock(mutex);
    closed = true;
    changed.notify_all();
}
