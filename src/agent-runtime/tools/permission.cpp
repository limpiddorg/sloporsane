#include "permission.hpp"
#include <stdexcept>

const char* permissionDecisionName(PermissionDecision decision) {
    switch (decision) {
        case PermissionDecision::ALLOW: return "allow";
        case PermissionDecision::YES: return "yes";
        case PermissionDecision::NO: return "no";
        case PermissionDecision::ALWAYS: return "always";
        case PermissionDecision::AI_ACCEPTED: return "ai_accepted";
        case PermissionDecision::AI_REFUSED: return "ai_refused";
        case PermissionDecision::MANUALLY_APPROVED: return "manually_approved";
    }
    throw std::invalid_argument("Unknown permission decision");
}

PermissionDecision parsePermissionDecision(const std::string& value) {
    if (value == "yes") return PermissionDecision::YES;
    if (value == "no") return PermissionDecision::NO;
    if (value == "always") return PermissionDecision::ALWAYS;
    throw std::invalid_argument("Expected yes, no or always");
}

void to_json(nlohmann::json& json, const PermissionRecord& record) {
    json = {{"id", record.id}, {"permission", record.permission}, {"patterns", record.patterns},
        {"decision", permissionDecisionName(record.decision)}};
    if (record.confidence) json["confidence"] = *record.confidence;
}
