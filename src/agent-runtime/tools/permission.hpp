#ifndef SOS_PERMISSION_HPP
#define SOS_PERMISSION_HPP

#include <nlohmann/json.hpp>
#include <functional>
#include <optional>
#include <string>
#include <vector>

struct ToolPermissionRequest {
    std::string permission;
    std::vector<std::string> patterns;
    std::vector<std::string> always;
    nlohmann::json metadata = nlohmann::json::object();
    std::string callId;
    std::string toolName;
    bool readOnly = false;
    // Set only by the trusted replay tool after resolving a live session grant.
    bool manuallyApproved = false;
    std::optional<double> manualApprovalConfidence;
};

enum class PermissionDecision { ALLOW, YES, NO, ALWAYS, AI_ACCEPTED, AI_REFUSED, MANUALLY_APPROVED };
const char* permissionDecisionName(PermissionDecision decision);
PermissionDecision parsePermissionDecision(const std::string& value);

struct PermissionAnswer {
    PermissionDecision decision = PermissionDecision::ALLOW;
    std::string id;
    std::string reason;
    std::optional<double> confidence; // Confidence to execute, in [0,1], never confidence to deny.
};

struct PermissionRecord {
    std::string id;
    std::string permission;
    std::vector<std::string> patterns;
    PermissionDecision decision;
    std::optional<double> confidence;
};
void to_json(nlohmann::json& json, const PermissionRecord& record);

// Runs on the agent worker; the handler must periodically invoke the cancellation checkpoint.
// A "tool" preflight precedes execute(), allowing disabled tool families to be rejected before file inspection.
using ToolPermissionHandler = std::function<PermissionAnswer(const ToolPermissionRequest&, const std::function<void()>&)>;

#endif // SOS_PERMISSION_HPP
