#include "btw_conversation.hpp"
#include "implementation_run.hpp"
#include "../prompts.hpp"
#include <agent-runtime/conversation/messages/message_json.hpp>
#include <agent-runtime/tools/fs/read.hpp>
#include <agent-runtime/tools/fs/glob.hpp>
#include <agent-runtime/tools/fs/grep.hpp>
#include <agent-runtime/tools/common/webfetch.hpp>
#include <agent-runtime/core/ripgrep.hpp>
#include <map>

namespace {
class HistoricalReadTool : public Tool {
public:
    HistoricalReadTool(std::unique_ptr<Tool> tool, std::vector<std::filesystem::path> roots,
        std::filesystem::path destination, std::function<void(const std::function<void()>&)> prepare)
        : tool(std::move(tool)), roots(std::move(roots)), destination(std::move(destination)), prepare(std::move(prepare)) {}
    ToolDefinition getDefinition() const override { return tool->getDefinition(); }
    ToolResult execute(const nlohmann::json& arguments, const ToolContext& context) const override {
        if (prepare) prepare(context.checkCancelled);
        auto mapped = arguments;
        for (const auto key : {"filePath", "path"}) if (mapped.contains(key)) {
            const std::filesystem::path path(mapped.at(key).get<std::string>());
            if (!path.is_absolute()) continue;
            for (const auto& root : roots) {
                const auto relative = path.lexically_normal().lexically_relative(root);
                if (relative.empty() || *relative.begin() == "..") continue;
                mapped[key] = (destination / relative).lexically_normal().string();
                break;
            }
        }
        return tool->execute(mapped, context);
    }
private:
    std::unique_ptr<Tool> tool;
    std::vector<std::filesystem::path> roots;
    std::filesystem::path destination;
    std::function<void(const std::function<void()>&)> prepare;
};
class ProposeStepReworkTool : public Tool {
public:
    explicit ProposeStepReworkTool(bool revisePlan = false) : revisePlan(revisePlan) {}
    ToolDefinition getDefinition() const override {
        if (revisePlan) return {"propose_implementation_revision",
            "Propose a revised Implementation Plan only when an explicit change makes the accepted plan inaccurate, or update the candidate during revised-plan discussion. Keep request to one or two short sentences; do not repeat the plan or procedure in chat. In review this only proposes opening a new planning page, never starts execution. The user separately accepts the candidate there. Preserve the validated prefix exactly. retained_steps maps each NEW step to its OLD zero-based step index (null for new); keep the reviewed step identity at the review cursor. substep_owners maps each OLD substep of its retained source to the NEW substep owning its contribution; [] for new steps. Future steps may be reordered/removed, never validated ones.",
            {{"type", "object"}, {"properties", {{"request", {{"type", "string"}, {"minLength", 1}, {"maxLength", 8000}}},
                {"plan", implementationPlanSchema()}, {"retained_steps", {{"type", "array"}, {"items", {{"type", {"integer", "null"}}, {"minimum", 0}}}}},
                {"substep_owners", {{"type", "array"}, {"items", {{"type", "array"}, {"items", {{"type", "integer"}, {"minimum", 0}}}}}}}}},
                {"required", {"request", "plan", "retained_steps", "substep_owners"}}, {"additionalProperties", false}}};
        return {"propose_step_rework", "Propose reworking the reviewed step only for an explicit change with an accurate accepted plan. Write request as a SIMPLE, SUCCINCT Markdown summary: 2-3 short bullet points starting with '- ', in plain language explaining what will be changed. No title, nested bullets, long explanation, jargon or procedural recap. Use inline code for file names or commands only when useful. Do not repeat the summary in chat. The inline card obtains authorization; this tool never starts execution. Not for questions or inaccurate plans.",
            {{"type", "object"}, {"properties", {{"request", {{"type", "string"}, {"minLength", 1}, {"maxLength", 8000}}}}}, {"required", {"request"}}, {"additionalProperties", false}}};
    }
    ToolResult execute(const nlohmann::json& arguments, const ToolContext&) const override {
        const auto request = arguments.at("request").get<std::string>();
        if (request.find_first_not_of(" \r\n\t") == std::string::npos || request.size() > 8000)
            throw std::invalid_argument("Invalid step rework request");
        const auto proposal = arguments.get<ImplementationRevisionProposal>();
        if (revisePlan != proposal.plan.has_value()) throw std::invalid_argument("Use the matching proposal tool for a revised plan or a local rework");
        return {"Revision proposed", revisePlan ? "This proposes a revised Implementation Plan only. The UI opens its planning page before separate final acceptance. Do not print the candidate in chat or edit code." : "The inline card requires acceptance before corrections start. Do not repeat the proposal or edit code.", {{"implementation_revision", proposal}}};
    }
private:
    bool revisePlan;
};
}

void to_json(nlohmann::json& json, const BtwConversation& branch) {
    json = {{"id", branch.id}, {"anchor", branch.anchor}, {"pageAnchor", branch.pageAnchor}, {"standalone", branch.standalone}, {"snapshot", branch.snapshot},
        {"messages", branch.messages}, {"active_context", branch.activeContext}, {"context_message_count", branch.contextMessageCount}, {"review_segment_start", branch.reviewSegmentStart}, {"context_construction_key", branch.contextConstructionKey}, {"status", branch.status}, {"error", branch.error},
        {"model", branch.model}, {"reasoning", branch.reasoning}, {"provider", branch.provider}, {"review", branch.review}, {"revision_planning", branch.revisionPlanning}, {"review_summary", branch.reviewSummary}};
    if (branch.reviewStep) json["review_step"] = *branch.reviewStep;
}
void from_json(const nlohmann::json& json, BtwConversation& branch) {
    json.at("id").get_to(branch.id);
    json.at("anchor").get_to(branch.anchor);
    json.at("pageAnchor").get_to(branch.pageAnchor);
    json.at("standalone").get_to(branch.standalone);
    json.at("review").get_to(branch.review);
    json.at("revision_planning").get_to(branch.revisionPlanning);
    json.at("review_summary").get_to(branch.reviewSummary);
    if (json.contains("review_step")) branch.reviewStep = json.at("review_step").get<std::size_t>();
    // Journal bodies omit the frozen snapshot, stored in its own SQLite column.
    branch.snapshot = json.value("snapshot", std::vector<Message>{});
    json.at("messages").get_to(branch.messages);
    json.at("review_segment_start").get_to(branch.reviewSegmentStart);
    if (branch.reviewSegmentStart > branch.messages.size()) throw std::invalid_argument("Invalid review segment boundary");
    json.at("active_context").get_to(branch.activeContext);
    json.at("context_construction_key").get_to(branch.contextConstructionKey);
    json.at("context_message_count").get_to(branch.contextMessageCount);
    if (branch.contextMessageCount > branch.messages.size()) throw std::invalid_argument("Invalid review context cursor");
    json.at("status").get_to(branch.status);
    json.at("error").get_to(branch.error);
    json.at("model").get_to(branch.model);
    json.at("reasoning").get_to(branch.reasoning);
    json.at("provider").get_to(branch.provider);
}

std::vector<Message> currentReviewMessages(const BtwConversation& branch) {
    std::vector<Message> messages;
    for (auto it = branch.messages.begin() + branch.reviewSegmentStart; it != branch.messages.end(); ++it)
        if (!it->compaction && !it->id.starts_with("compaction-status-")) messages.push_back(*it);
    return messages;
}

std::vector<Message> freezeBtwContext(std::vector<Message> messages) {
    // A principal tool can still be running when the snapshot is taken. Balance
    // its call without executing it here or inventing its outcome.
    std::map<std::string, bool> pending;
    for (const auto& message : messages)
        for (const auto& part : message.content) {
            if (const auto* call = std::get_if<ToolCallContent>(&part)) pending[call->callId] = true;
            if (const auto* result = std::get_if<ToolResultContent>(&part)) pending.erase(result->callId);
        }
    for (const auto& [id, unused] : pending)
        messages.push_back(Message{"btw-pending-" + id, MessageRole::TOOL,
            {ToolResultContent{id, "This tool was still pending in the principal conversation when this snapshot was taken. Its outcome is unknown. Do not execute it on behalf of the principal agent.", true}}});
    return messages;
}
std::unique_ptr<Agent> makeBtwAgent(const BtwConversation& branch, std::unique_ptr<Provider> provider,
    const ProviderSelection& selection, const std::filesystem::path& workspace,
    std::function<Image(const std::string&)> readImage, const std::filesystem::path& authorizedRoot, bool allowRework,
    std::vector<std::filesystem::path> historicalRoots, std::function<void(const std::function<void()>&)> prepareFiles) {
    ToolRegistry tools;
    const auto addRead = [&](std::unique_ptr<Tool> tool) {
        if (!historicalRoots.empty()) tool = std::make_unique<HistoricalReadTool>(std::move(tool), historicalRoots, authorizedRoot, prepareFiles);
        tools.add(std::move(tool));
    };
    addRead(std::make_unique<ReadTool>());
    const auto ripgrep = core::ensureRipgrep();
    addRead(std::make_unique<GlobTool>(ripgrep));
    addRead(std::make_unique<GrepTool>(ripgrep));
    tools.add(std::make_unique<WebFetchTool>());
    if (branch.review && allowRework) {
        if (!branch.revisionPlanning) tools.add(std::make_unique<ProposeStepReworkTool>());
        tools.add(std::make_unique<ProposeStepReworkTool>(true));
    }
    ToolContext context;
    context.directory = context.worktree = workspace;
    auto agent = std::make_unique<Agent>(std::move(provider), selection.model, std::move(tools), context, selection.reasoning);
    agent->setImageReader(std::move(readImage));
    agent->setPermissionHandler([](const ToolPermissionRequest& request, const auto& check) {
        check();
        if (request.toolName != "read" && request.toolName != "glob" && request.toolName != "grep" &&
            request.toolName != "view_image" && request.toolName != "webfetch" && request.toolName != "propose_step_rework" && request.toolName != "propose_implementation_revision")
            throw std::runtime_error("BTW conversations are read-only. This tool is not allowed.");
        if (request.permission != "tool" && request.permission != "webfetch" && request.permission != "read" &&
            request.permission != "glob" && request.permission != "grep")
            throw std::runtime_error("BTW reads are limited to the authorized workspace; external access is not approved.");
        return PermissionAnswer{};
    }, authorizedRoot);
    Conversation conversation(ProviderFormat::OPENAI_RESPONSES);
    for (const auto& message : branch.snapshot) conversation.append(message);
    const std::string executionContext = branch.standalone ? "" :
        "Its latest user request was being processed at snapshot time; any unfinished work belongs to the principal agent, not you. ";
    conversation.append(Message{"btw-instructions", MessageRole::SYSTEM, {TextContent{branch.review ?
        (branch.revisionPlanning ? "Current phase: revised Implementation Plan discussion. Read-only tools and plan proposals only. Never execute changes or propose a local rework. Publish complete candidates through propose_implementation_revision; the user separately accepts the final plan." : prompts::implementationPhaseAnnouncement(ImplementationPhase::REVIEW)) :
        "You are an independent, informative BTW side conversation. The preceding messages are a frozen snapshot of the principal conversation. "
        + executionContext +
        "Answer only the BTW user's questions. Do not continue the principal task. You may inspect files and fetch information, but never modify files, run commands, or perform destructive actions. "
        "Only read, glob, grep, view_image and webfetch are available. File reads are restricted to the authorized workspace. "
        "Your replies are not injected into the principal conversation. The snapshot will not be refreshed on subsequent BTW turns."}}});
    if (!branch.activeContext.empty()) {
        conversation = Conversation(ProviderFormat::OPENAI_RESPONSES);
        for (const auto& message : branch.activeContext) conversation.append(message);
    }
    for (std::size_t i = branch.activeContext.empty() ? branch.reviewSegmentStart : branch.contextMessageCount; i < branch.messages.size(); ++i)
        if (!branch.messages[i].id.starts_with("compaction-status-") && !branch.messages[i].compaction)
            conversation.append(branch.messages[i]);
    // Branches may use a different provider/model from the principal or last turn.
    conversation.rebuildProviderContext(conversation.getMessages().size());
    agent->replaceConversation(std::move(conversation));
    agent->setContextLimit(selection.contextLimit);
    return agent;
}
