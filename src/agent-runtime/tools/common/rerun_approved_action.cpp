#include "rerun_approved_action.hpp"
#include <stdexcept>

ToolDefinition RerunApprovedActionTool::getDefinition() const {
    return {"rerun_approved_action",
        "Run an exact action manually approved by the user in this session. Use only an approval ID supplied by the harness. "
        "The stored tool, arguments and working directory cannot be overridden. Revoked grants and Sane restrictions still apply.",
        {{"type", "object"}, {"properties", {{"id", {{"type", "string"}}}}},
         {"required", {"id"}}, {"additionalProperties", false}}};
}

ToolResult RerunApprovedActionTool::execute(const nlohmann::json& arguments, const ToolContext& context) const {
    if (context.checkCancelled) context.checkCancelled();
    if (!arguments.is_object() || arguments.size() != 1 || !arguments.contains("id") || !arguments.at("id").is_string())
        throw std::invalid_argument("Supply only the approved action id; arguments cannot be overridden");
    if (!context.approvedAction || !context.ask) throw std::runtime_error("No session approval resolver is available");
    const auto id = arguments.at("id").get<std::string>();
    const auto action = context.approvedAction(id);
    if (action.id != id || action.tool == "rerun_approved_action") throw std::invalid_argument("Invalid approved action");
    if (action.directory != context.directory || action.workspace != context.permissionRoot.value_or(context.worktree))
        throw std::runtime_error("Approval belongs to a different working directory or permission workspace");
    auto replay = context;
    replay.ask = [&](const ToolPermissionRequest& request) {
        // Recheck revocation before every permission boundary, including after file inspection.
        if (context.checkCancelled) context.checkCancelled();
        const auto current = context.approvedAction(id);
        if (current.tool != action.tool || current.arguments != action.arguments || current.directory != action.directory ||
            current.workspace != action.workspace) throw std::runtime_error("Approved action changed");
        auto identified = request;
        identified.toolName = action.tool;
        identified.metadata["arguments"] = action.arguments;
        identified.metadata["working_directory"] = action.directory.string();
        identified.metadata["approval_id"] = id;
        identified.manuallyApproved = true;
        identified.manualApprovalConfidence = current.confidence;
        context.ask(identified);
    };
    replay.ask({"tool", {}, {}, nlohmann::json::object()});
    auto result = tools.get(action.tool).execute(action.arguments, replay);
    result.metadata["approved_action"] = {{"id", id}, {"tool", action.tool}, {"arguments", action.arguments}};
    return result;
}
