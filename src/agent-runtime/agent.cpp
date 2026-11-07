#include "agent.hpp"
#include "tools/common/view_image.hpp"
#include "tools/common/rerun_approved_action.hpp"
#include "skills/project_instructions.hpp"

#include <stdexcept>
#include <algorithm>
#include <utility>

namespace {
using core::Cancelled;
class WorktreeTool : public Tool {
public:
    explicit WorktreeTool(Agent::WorkspaceHandler handler) : handler(std::move(handler)) {}
    ToolDefinition getDefinition() const override {
        return {"worktree", "List Git worktrees, or create/select a worktree and move this SOS conversation into it. "
            "Only create or switch when the user directly asks you to do so; never do it proactively. "
            "Creation starts from committed HEAD, not local changes. Configuration stays inherited from the source project. "
            "No extra confirmation is requested. Commit, merge and push remain separate Bash operations. "
            "Creation and switching are allowed in Sane on explicit request, but unavailable during an active Implement. Worktrees are not sandboxes.",
            {{"type", "object"}, {"properties", {
                {"action", {{"type", "string"}, {"enum", {"list", "create", "switch"}}}},
                {"name", {{"type", "string"}, {"description", "Required for create: unique worktree name."}}},
                {"path", {{"type", "string"}, {"description", "Required for switch: absolute path from list."}}}}},
                {"required", {"action"}}, {"additionalProperties", false}}};
    }
    ToolResult execute(const nlohmann::json& arguments, const ToolContext& context) const override {
        const auto action = arguments.at("action").get<std::string>();
        if (action != "list" && action != "create" && action != "switch") throw std::invalid_argument("Unknown worktree action");
        ToolPermissionRequest request{"worktree", {}, {}, nlohmann::json::object()};
        request.readOnly = action == "list";
        context.ask(request);
        const auto output = handler(arguments, context.checkCancelled);
        return {"Worktree", output.dump(2), output};
    }
private:
    Agent::WorkspaceHandler handler;
};
class WorkspaceTool : public Tool {
public:
    explicit WorkspaceTool(Agent::WorkspaceHandler handler) : handler(std::move(handler)) {}
    ToolDefinition getDefinition() const override {
        return {"workspace", "Move this SOS conversation to an existing directory, with or without Git, including another repository or worktree. "
            "Only switch on a direct user request or when a skill/workflow explicitly invoked by the user clearly prescribes it; never proactively. "
            "No extra confirmation is requested. Allowed in Sane, but unavailable during active Implement or review. "
            "Skills and MCP configuration remain inherited from the original workspace. Files are not copied or created. "
            "The worktree tool still creates/selects worktrees and moves the conversation within the current repository.",
            {{"type", "object"}, {"properties", {{"path", {{"type", "string"}, {"description", "Absolute path of an existing destination directory."}}}}},
                {"required", {"path"}}, {"additionalProperties", false}}};
    }
    ToolResult execute(const nlohmann::json& arguments, const ToolContext& context) const override {
        context.ask(ToolPermissionRequest{"workspace", {}, {}, nlohmann::json::object()});
        const auto output = handler({{"action", "switch_directory"}, {"path", arguments.at("path")}}, context.checkCancelled);
        return {"Workspace", output.dump(2), output};
    }
private:
    Agent::WorkspaceHandler handler;
};
class ObjectivePlanTool : public Tool {
public:
    explicit ObjectivePlanTool(std::shared_ptr<std::pair<std::string, std::string>> current, bool patch = false)
        : current(std::move(current)), patch(patch) {}
    ToolDefinition getDefinition() const override {
        if (patch) return {"patch_objective_plan", "Update a usable current Objective Plan with exact, unique replacements. Only publish a complete, ready-to-use plan. Each call creates one version immediately; never publish partial drafts.",
            {{"type", "object"}, {"properties", {{"updates", {{"type", "array"}, {"minItems", 1}, {"maxItems", 50},
                {"items", {{"type", "object"}, {"properties", {
                    {"field", {{"type", "string"}, {"enum", {"plan", "stack"}}}},
                    {"old_str", {{"type", "string"}, {"minLength", 1}}}, {"new_str", {{"type", "string"}}}}},
                    {"required", {"field", "old_str", "new_str"}}, {"additionalProperties", false}}}}}}},
                {"required", {"updates"}}, {"additionalProperties", false}}};
        return {"submit_objective_plan", "Create or fully replace the current Objective Plan and optional Stack when the discussion supplies a concrete, ready-to-use objective. Each call creates one version immediately. Never publish half-baked drafts. Do not include section headings in either field; the UI adds Objective Plan and Stack headings. Do not repeat the submitted plan in your final response.",
            {{"type", "object"}, {"properties", {
                {"plan", {{"type", "string"}, {"description", "Detailed high-level Objective Plan in Markdown: what to build, key behaviors, boundaries and verification, without a Stack heading or package list."}}},
                {"stack", {{"type", "string"}, {"description", "Optional short Markdown bullet list of key packages or stack choices and their roles. Omit or leave empty when an existing codebase needs no new stack choices. Do not include a Stack heading."}}}
            }},
                {"required", {"plan"}}, {"additionalProperties", false}}};
    }
    ToolResult execute(const nlohmann::json& arguments, const ToolContext&) const override {
        auto [plan, stack] = *current;
        if (patch) {
            if (plan.empty()) throw std::invalid_argument("No current Objective Plan to patch");
            const auto& updates = arguments.at("updates");
            if (!updates.is_array() || updates.empty() || updates.size() > 50)
                throw std::invalid_argument("Provide between 1 and 50 replacements");
            for (const auto& update : updates) {
                const auto field = update.at("field").get<std::string>();
                if (field != "plan" && field != "stack") throw std::invalid_argument("Unknown Objective Plan field");
                auto& target = field == "plan" ? plan : stack;
                const auto old = update.at("old_str").get<std::string>();
                const auto position = target.find(old);
                if (old.empty() || position == std::string::npos || target.find(old, position + 1) != std::string::npos)
                    throw std::invalid_argument("Objective Plan replacement must match exactly once");
                target.replace(position, old.size(), update.at("new_str").get<std::string>());
            }
        } else {
            plan = arguments.at("plan").get<std::string>();
            stack = arguments.value("stack", std::string{});
        }
        if (plan.find_first_not_of(" \t\r\n") == std::string::npos || plan.size() > 32000)
            throw std::invalid_argument("Objective plan must contain between 1 and 32000 characters");
        if (stack.size() > 8000) throw std::invalid_argument("Stack must contain at most 8000 characters");
        if (*current == std::pair{plan, stack}) throw std::invalid_argument("Objective Plan is unchanged");
        *current = {plan, stack};
        return {"Objective plan updated", "Current Objective Plan:\n" + plan + "\nStack:\n" + stack,
            {{"objective_plan", plan}, {"objective_stack", stack}}};
    }
private:
    std::shared_ptr<std::pair<std::string, std::string>> current;
    bool patch;
};
class ImplementationPlanTool : public Tool {
public:
    ToolDefinition getDefinition() const override {
        return {"submit_implementation_plan",
            "Submit the implementation plan for the accepted Objective Plan as ordered steps and substeps. "
            "Each step is a coherent construction outcome exercisable at its own level, with explicit validation. "
            "Keep the plan proportional to the request: use the fewest coherent steps/substeps needed, cover requested behavior and concrete regression risks, and do not add exhaustive hypothetical edge-case tests or features merely because this is Sane. "
            "Specify checks only where needed; if no additional check is needed, state why in validation. Do not invent verification-only substeps, repeated test suites or a mandatory final audit. Reuse still-valid earlier results. "
            "Prefer existing tests and short checks; do not prescribe a large one-off browser simulation solely because a browser runner is unavailable. "
            "Substeps describe meaningful construction intentions and planned code changes, not tool calls or a file checklist. "
            "Intermediate substeps may be incomplete. The UI renders the structured plan; do not repeat it afterward.",
            implementationPlanSchema()};
    }
    ToolResult execute(const nlohmann::json& arguments, const ToolContext&) const override {
        const auto plan = arguments.get<ImplementationPlan>();
        return {"Implementation plan submitted", "The implementation plan has been submitted. Do not repeat it.",
            {{"implementation_plan", plan}}};
    }
};
class AskChoiceTool : public Tool {
public:
    ToolDefinition getDefinition() const override {
        return {"ask_choice", "Ask only if it is impossible to choose without committing to one of two substantially different product directions. Never ask about packages, frameworks, libraries, or other technical implementation choices: select these autonomously. The answer is returned to this tool call, not added as a user message.",
            {{"type", "object"}, {"properties", {
                {"question", {{"type", "string"}}},
                {"options", {{"type", "array"}, {"items", {{"type", "string"}}}, {"minItems", 2}, {"maxItems", 6}}}
            }}, {"required", {"question", "options"}}, {"additionalProperties", false}}};
    }
    ToolResult execute(const nlohmann::json& arguments, const ToolContext& context) const override {
        const auto question = arguments.at("question").get<std::string>();
        const auto options = arguments.at("options").get<std::vector<std::string>>();
        if (question.empty() || question.size() > 1000 || options.size() < 2 || options.size() > 6 ||
            std::any_of(options.begin(), options.end(), [](const auto& option) { return option.empty() || option.size() > 300; }))
            throw std::invalid_argument("Invalid question or choices");
        if (!context.askQuestion) throw std::runtime_error("Interactive questions are not available");
        return {"Question answered", context.askQuestion(question, options, context.checkCancelled)};
    }
};
class InspectImplementationChangesTool : public Tool {
public:
    explicit InspectImplementationChangesTool(std::function<nlohmann::json()> inspect)
        : inspect(std::move(inspect)) {}
    ToolDefinition getDefinition() const override {
        return {"inspect_implementation_changes",
            "Optionally read the current step's cumulative changes when useful to resolve uncertainty. Construction ownership is already captured; do not redistribute changes between substeps. Inspection is not required for completion.",
            {{"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}}};
    }
    ToolResult execute(const nlohmann::json&, const ToolContext&) const override {
        if (!inspect) throw std::runtime_error("Implementation change inspection is unavailable");
        return {"Step changes", inspect().dump()};
    }
private:
    std::function<nlohmann::json()> inspect;
};
class ImplementationProgressTool : public Tool {
public:
    explicit ImplementationProgressTool(Agent::ImplementationProgressHandler capture)
        : capture(std::move(capture)) {}
    ToolDefinition getDefinition() const override {
        return {"report_implementation_progress",
            "The harness activates the first substep before normal construction; do not announce started again. Report completed with 2-4 short description bullets. The harness automatically activates the first remaining substep after completed/unnecessary, or enters validation if none remain: do not send separate started/validating announcements for these transitions. In revisions, follow the explicit correcting/validating instructions. Use unnecessary only for genuinely obsolete work. If checks are needed, run them in this phase. Reuse still-valid earlier results; do not reread all files or repeat checks without a concrete reason. If no additional check is needed, explain why and complete the step without inventing checks. Normally validation makes no edits. If a check fails, report correcting with the ORIGINAL substep index BEFORE its repair, then completed when that repair is done and rerun affected checks. All validation activity stays in Validating the step; only repair edits update the targeted substep. These boundaries preserve ownership and do not grant human validation.",
            {{"type", "object"}, {"properties", {{"substep", {{"type", {"integer", "null"}}, {"minimum", 0}, {"maximum", 29}}},
                 {"status", {{"type", "string"}, {"enum", {"started", "completed", "unnecessary", "validating", "correcting"}}}}, {"explanation", {{"type", "string"}}}}},
                {"required", {"substep", "status", "explanation"}}, {"additionalProperties", false}}};
    }
    ToolResult execute(const nlohmann::json& arguments, const ToolContext&) const override {
        const auto substep = arguments.at("substep").is_null() ? -1 : arguments.at("substep").get<int>();
        const auto status = arguments.at("status").get<std::string>();
        const auto explanation = arguments.at("explanation").get<std::string>();
        if ((status == "validating" ? substep != -1 : substep < 0 || substep >= 30) ||
            (status != "started" && status != "completed" && status != "unnecessary" && status != "validating" && status != "correcting") || explanation.size() > 2000 ||
            ((status == "completed" || status == "unnecessary") && explanation.find_first_not_of(" \r\n\t") == std::string::npos))
            throw std::invalid_argument("Invalid substep progress announcement");
        auto metadata = nlohmann::json{{"implementation_progress", arguments}};
        if (capture) metadata["implementation_boundary"] = capture(arguments);
        if (status == "validating") return {"Technical validation started",
            "Finish this assigned step: if checks are needed, run them here; complete missing in-scope work and repair failures in their original substeps, "
            "then rerun affected checks if needed and call complete_implementation_step before ending normally. "
            "Entering validation does not require new checks. If no additional check is needed, explain why and complete the step without inventing checks. "
            "Keep checks focused on this step's changes and explicit plan requirements. Reuse still-valid earlier results; do not reread all files or audit the whole implementation. "
            "Broaden verification only for a concrete uncertainty or regression risk; the last step is not a fresh global audit. "
            "Prefer existing tests and short checks, not a large ad-hoc test framework for unavailable browser interaction unless explicitly required. "
            "Do not stop with a prose audit or a promise to continue. Genuine blockers, denied actions or cancellation remain explicit interruptions; "
            "never fabricate completion. Human validation remains separate.", metadata};
        return {"Progress announced", "Continue the same step directly with useful work. In normal construction, completed/unnecessary automatically activates the first remaining substep or enters validation; do not repeat its started/validating announcement. If technical checks are needed, reserve them for validation. Human validation remains separate.", metadata};
    }
private:
    Agent::ImplementationProgressHandler capture;
};
class CompleteImplementationStepTool : public Tool {
public:
    explicit CompleteImplementationStepTool(Agent::ImplementationProgressHandler capture)
        : capture(std::move(capture)) {}
    ToolDefinition getDefinition() const override {
        using Json = nlohmann::json;
        Json substep{{"type", "object"}, {"properties", {{"explanation", {{"type", "string"}}}}},
            {"additionalProperties", false}};
        const auto description =
            "Complete the assigned step after Validating the step. Provide a concise global explanation and one object per planned substep, in order. State the result and actual check outcomes once, without a separate recap. Use {} to reuse its latest completed/unnecessary explanation; provide explanation only to revise it, as 2-4 short concrete bullets. Construction boundaries determine diffs; do NOT assign hunks. No systematic diff inspection is required. Never invent changes.";
        return {"complete_implementation_step", std::string(description) +
            " Technical validation must finish the step, not end with a prose audit or a promise to resume. Complete missing in-scope work and rerun affected checks if needed before submitting this tool. If no additional check is needed, explain why; validation does not require new checks. A message alone is not completion. Only a genuine blocker or cancellation may interrupt without submission; never fabricate completion. " +
            " Required checks must pass except failures demonstrated to predate this implementation and unrelated to its changes: document the baseline evidence and remaining failures. Never claim failing checks passed; new regressions or unexplained failures still block completion. Human validation remains separate. On the LAST step, conclusion must summarize the ENTIRE implementation, checks and remaining caveats in a few readable paragraphs or bullets (at most 6000 characters); otherwise use an empty conclusion. A successful submission ends execution immediately: put this call last and include the conclusion here, not in a later message.",
            {{"type", "object"}, {"properties", {{"explanation", {{"type", "string"}}},
                {"conclusion", {{"type", "string"}}},
                {"substeps", {{"type", "array"}, {"items", substep}, {"minItems", 1}, {"maxItems", 30}}}}},
                {"required", {"explanation", "substeps", "conclusion"}}, {"additionalProperties", false}}};
    }
    ToolResult execute(const nlohmann::json& arguments, const ToolContext&) const override {
        const auto completion = arguments.get<ImplementationCompletion>();
        if (completion.conclusion.size() > 6000) throw std::invalid_argument("Conclusion must be at most 6000 characters");
        const auto valid = [](const std::string& text) { return text.size() <= 2000 && text.find_first_not_of(" \r\n\t") != std::string::npos; };
        if (!valid(completion.explanation) || completion.substeps.empty() || completion.substeps.size() > 30)
            throw std::invalid_argument("Provide a concise step explanation and ordered substeps");
        for (const auto& part : completion.substeps)
            if (!part.explanation.empty() && !valid(part.explanation))
                throw std::invalid_argument("Each revised substep needs a concise explanation (at most 2000 characters)");
        auto metadata = nlohmann::json{{"implementation_completion", completion}};
        if (capture) metadata["implementation_boundary"] = capture({{"status", "finished"}, {"substep", nullptr},
            {"explanation", completion.explanation}, {"completion", completion}});
        return {"Step completion submitted", "Execution finished; human validation remains separate.", metadata};
    }
private:
    Agent::ImplementationProgressHandler capture;
};
ProviderFormat formatOf(const std::unique_ptr<Provider>& provider) {
    if (!provider) throw std::invalid_argument("Agent requires a provider");
    return provider->getFormat();
}
}

Agent::Agent(std::unique_ptr<Provider> provider, std::string model, ToolRegistry tools, ToolContext toolContext,
    std::string reasoningEffort)
    : provider(std::move(provider)), model(std::move(model)), reasoningEffort(std::move(reasoningEffort)), conversation(formatOf(this->provider)),
      tools(std::move(tools)), toolContext(std::move(toolContext)) {
    if (this->model.empty()) throw std::invalid_argument("Agent requires a model");
    if (this->reasoningEffort.empty()) throw std::invalid_argument("Agent requires a reasoning effort");
    this->provider->setCancellationCheck([this] { return cancellationRequested.load(); });
}

Agent::~Agent() {
    // Join before destroying the provider and conversation used by the worker.
    if (execution.valid()) execution.wait();
}

void Agent::append(const Message& message) {
    std::lock_guard lock(mutex);
    if (running) throw std::logic_error("Cannot edit conversation while the agent is running");
    conversation.append(message);
    if (eventHandler) eventHandler({AgentEventType::HARNESS_MESSAGE_ARRIVED, {message}, {}, contextUsage});
}

void Agent::enableProjectContext(const std::filesystem::path& workspace, const std::filesystem::path& configuration) {
    std::lock_guard lock(mutex);
    if (running) throw std::logic_error("Cannot enable project context while running");
    projectWorkspace = std::filesystem::canonical(workspace);
    skillsWorkspace = configuration.empty() ? projectWorkspace : std::filesystem::canonical(configuration);
    workspaceMoved = skillsWorkspace != projectWorkspace;
    if (!skillCatalog) {
        skillCatalog = std::make_shared<SkillCatalog>();
        tools.add(std::make_unique<SkillTool>(skillCatalog));
    }
}

void Agent::setWorkspaceHandler(WorkspaceHandler handler) {
    std::lock_guard lock(mutex);
    if (running) throw std::logic_error("Cannot replace workspace handler while running");
    tools.remove("worktree");
    tools.remove("workspace");
    if (!handler) return;
    const WorkspaceHandler navigate = [this, handler = std::move(handler)](const auto& arguments, const auto& check) {
        auto result = handler(arguments, check);
        if (result.value("switched", false)) {
            // This callback runs on the agent worker between tools. Never rebuild the running agent.
            std::lock_guard lock(mutex);
            adoptWorkspace(result.at("workspace").template get<std::string>(), result.at("permission_root").template get<std::string>());
        }
        return result;
    };
    tools.add(std::make_unique<WorktreeTool>(navigate));
    tools.add(std::make_unique<WorkspaceTool>(navigate));
}

void Agent::adoptWorkspace(const std::filesystem::path& workspace, const std::filesystem::path& root) {
    toolContext.directory = workspace;
    toolContext.worktree = workspace;
    toolContext.permissionRoot = root;
    projectWorkspace = workspace;
    workspaceMoved = true;
}

void Agent::switchWorkspace(const std::filesystem::path& workspace, const std::filesystem::path& root) {
    std::lock_guard lock(mutex);
    if (running) throw std::logic_error("Cannot switch an active agent from the UI");
    adoptWorkspace(workspace, root);
}

void Agent::setEventHandler(AgentEventHandler handler) {
    std::lock_guard lock(mutex);
    if (running) throw std::logic_error("Cannot change event handler while running");
    eventHandler = std::move(handler);
}

void Agent::cancel() { cancellationRequested = true; }

void Agent::setImageReader(std::function<Image(const std::string&)> reader) {
    std::lock_guard lock(mutex);
    if (running) throw std::logic_error("Cannot change image reader while running");
    if (!toolContext.readImage) tools.add(std::make_unique<ViewImageTool>());
    toolContext.readImage = std::move(reader);
}

void Agent::setContextLimit(std::optional<std::uint64_t> limit) {
    std::lock_guard lock(mutex);
    if (running) throw std::logic_error("Cannot change context limit while running");
    if (limit && *limit < 32768) throw std::invalid_argument("Context limit must be at least 32768 tokens");
    if (contextLimit != limit) contextUsage.totalContext = limit;
    contextLimit = limit;
}

void Agent::setApprovedActionResolver(std::function<ApprovedToolAction(const std::string&)> resolver) {
    std::lock_guard lock(mutex);
    if (running) throw std::logic_error("Cannot replace approval resolver while running");
    if (!resolver) throw std::invalid_argument("Approval resolver must not be empty");
    if (!toolContext.approvedAction) tools.add(std::make_unique<RerunApprovedActionTool>(tools));
    toolContext.approvedAction = std::move(resolver);
}

void Agent::setQuestionHandler(std::function<std::string(const std::string&, const std::vector<std::string>&,
    const std::function<void()>&)> handler) {
    std::lock_guard lock(mutex);
    if (running) throw std::logic_error("Cannot replace question handler while running");
    toolContext.askQuestion = std::move(handler);
}

void Agent::setSanePlanning(bool enabled, std::string plan, std::string stack) {
    std::lock_guard lock(mutex);
    if (running) throw std::logic_error("Cannot change planning tools while running");
    tools.remove("submit_objective_plan");
    tools.remove("patch_objective_plan");
    if (!implementationPlanning) tools.remove("ask_choice");
    if (enabled) {
        auto current = std::make_shared<std::pair<std::string, std::string>>(std::move(plan), std::move(stack));
        tools.add(std::make_unique<ObjectivePlanTool>(current));
        tools.add(std::make_unique<ObjectivePlanTool>(current, true));
        if (!implementationPlanning) tools.add(std::make_unique<AskChoiceTool>());
    } else {
        tools.remove("submit_objective_plan");
        if (!implementationPlanning) tools.remove("ask_choice");
    }
    sanePlanning = enabled;
}

void Agent::setImplementationExecution(bool enabled, std::function<nlohmann::json()> inspect, ImplementationProgressHandler progress) {
    std::lock_guard lock(mutex);
    if (running) throw std::logic_error("Cannot change execution tools while running");
    tools.remove("inspect_implementation_changes");
    tools.remove("complete_implementation_step");
    tools.remove("report_implementation_progress");
    if (enabled) {
        tools.add(std::make_unique<CompleteImplementationStepTool>(progress));
        tools.add(std::make_unique<ImplementationProgressTool>(std::move(progress)));
        tools.add(std::make_unique<InspectImplementationChangesTool>(std::move(inspect)));
    }
    implementationExecution = enabled;
}

void Agent::setImplementationPlanning(bool enabled) {
    std::lock_guard lock(mutex);
    if (running) throw std::logic_error("Cannot change planning tools while running");
    if (implementationPlanning == enabled) return;
    if (enabled) {
        tools.add(std::make_unique<ImplementationPlanTool>());
        if (!sanePlanning) tools.add(std::make_unique<AskChoiceTool>());
    } else {
        tools.remove("submit_implementation_plan");
        if (!sanePlanning) tools.remove("ask_choice");
    }
    implementationPlanning = enabled;
}

void Agent::setContextPreparation(ContextPreparation prepare) {
    std::lock_guard lock(mutex);
    if (running) throw std::logic_error("Cannot change context preparation while running");
    prepareContext = std::move(prepare);
}

void Agent::setToolSource(ToolSource source) {
    std::lock_guard lock(mutex);
    if (running) throw std::logic_error("Cannot replace tool source while running");
    tools.replaceDynamic({});
    toolSource = std::move(source);
}

void Agent::setFileMutationHandler(FileMutationHandler handler) {
    std::lock_guard lock(mutex);
    if (running) throw std::logic_error("Cannot replace mutation handler while running");
    toolContext.mutateFiles = std::move(handler);
}

void Agent::replaceConversation(Conversation replacement) {
    std::lock_guard lock(mutex);
    if (running) throw std::logic_error("Cannot replace conversation during execution");
    if (replacement.getProvider() != provider->getFormat()) throw std::invalid_argument("Incompatible conversation format");
    conversation = std::move(replacement);
    contextUsage = {};
    measuredMessages = 0;
}

void Agent::setPermissionHandler(ToolPermissionHandler handler, const std::filesystem::path& root) {
    std::lock_guard lock(mutex);
    if (running) throw std::logic_error("Cannot replace the permission handler while running");
    permissionHandler = std::move(handler);
    toolContext.permissionRoot = root;
}

void Agent::setSessionId(std::string id) {
    std::lock_guard lock(mutex);
    if (running) throw std::logic_error("Cannot change session identity during execution");
    provider->setSessionId(id);
    sessionId = std::move(id);
}

void Agent::setReasoningEffort(std::string effort) {
    if (effort.empty()) throw std::invalid_argument("Agent requires a reasoning effort");
    std::lock_guard lock(mutex);
    reasoningEffort = std::move(effort);
}

void Agent::setGenerationSettings(std::string model, std::string effort, std::size_t portableBefore,
    std::unique_ptr<Provider> replacement) {
    if (model.empty() || effort.empty()) throw std::invalid_argument("Agent requires a model and reasoning effort");
    std::lock_guard lock(mutex);
    if (running) throw std::logic_error("Cannot replace generation settings during execution");
    if (replacement && replacement->getFormat() != conversation.getProvider())
        throw std::invalid_argument("Cannot change conversation format");
    conversation.rebuildProviderContext(portableBefore);
    if (replacement) {
        replacement->setSessionId(sessionId);
        replacement->setCancellationCheck([this] { return cancellationRequested.load(); });
        provider = std::move(replacement);
    }
    this->model = std::move(model);
    reasoningEffort = std::move(effort);
    contextUsage = {};
    measuredMessages = 0;
}

ContextUsage Agent::getContextUsage() const {
    std::lock_guard lock(mutex);
    return contextUsage;
}

void Agent::queueGenerationSettings(std::string model, std::string effort, std::optional<std::uint64_t> limit,
    std::unique_ptr<Provider> replacement) {
    if (model.empty() || effort.empty()) throw std::invalid_argument("Agent requires a model and reasoning effort");
    if (limit && *limit < 32768) throw std::invalid_argument("Context limit must be at least 32768 tokens");
    std::lock_guard lock(mutex);
    if (replacement && replacement->getFormat() != conversation.getProvider())
        throw std::invalid_argument("Cannot change conversation format");
    pendingSettings = PendingSettings{std::move(model), std::move(effort), limit, std::move(replacement)};
}

bool Agent::applyPendingSettings() {
    if (!pendingSettings) return false;
    auto settings = std::move(*pendingSettings);
    pendingSettings.reset();
    conversation.rebuildProviderContext(conversation.getMessages().size());
    if (settings.provider) {
        settings.provider->setSessionId(sessionId);
        settings.provider->setCancellationCheck([this] { return cancellationRequested.load(); });
        provider = std::move(settings.provider);
    }
    model = std::move(settings.model);
    reasoningEffort = activeReasoning = std::move(settings.reasoning);
    contextLimit = settings.limit;
    contextUsage = {};
    measuredMessages = 0;
    return true;
}

void Agent::finishGenerationSettings() {
    std::lock_guard lock(mutex);
    if (running) throw std::logic_error("Wait for execution before finishing settings");
    applyPendingSettings();
}

GenerationMetadata Agent::executionGeneration() const {
    std::lock_guard lock(mutex);
    return {model, activeReasoning.empty() ? reasoningEffort : activeReasoning, 0};
}

void Agent::restoreContextUsage(ContextUsage usage) {
    std::lock_guard lock(mutex);
    if (running) throw std::logic_error("Cannot restore context while running");
    contextUsage = std::move(usage);
    // Persisted usage has no exact message boundary; estimate locally until the next response.
    measuredMessages = 0;
}

std::vector<Message> Agent::getMessages() const {
    std::lock_guard lock(mutex);
    return conversation.getMessages();
}

bool Agent::hasImages() const {
    std::lock_guard lock(mutex);
    for (const auto& message : conversation.getMessages()) {
        if (!message.images.empty()) return true;
        for (const auto& part : message.content)
            if (const auto* tool = std::get_if<ToolResultContent>(&part))
                for (const auto& attachment : tool->attachments)
                    if (attachment.mime.starts_with("image/")) return true;
    }
    return false;
}

bool Agent::isRunning() const {
    std::lock_guard lock(mutex);
    return running;
}

std::shared_future<void> Agent::run(std::chrono::steady_clock::time_point startedAt) {
    std::lock_guard lock(mutex);
    if (running) throw std::logic_error("Agent is already running");
    if (conversation.getMessages().empty()) throw std::logic_error("Conversation is empty");
    // While running, only the worker mutates the conversation and cached input.
    running = true;
    activeReasoning = reasoningEffort;
    cancellationRequested = false;
    try {
        execution = std::async(std::launch::async, [this, startedAt, reasoningEffort = reasoningEffort]() mutable {
            const auto generationMetadata = [this, startedAt, &reasoningEffort] {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - startedAt).count();
                return GenerationMetadata{model, reasoningEffort, static_cast<std::uint64_t>(elapsed > 0 ? elapsed : 0)};
            };
            std::optional<GenerationMetadata> finalMetadata;
            const auto notify = [this, &finalMetadata, &generationMetadata](AgentEvent event) {
                event.context = getContextUsage();
                if (event.isTerminal()) event.metadata = finalMetadata ? *finalMetadata : generationMetadata();
                if (eventHandler) eventHandler(std::move(event));
            };
            const auto checkCancelled = [this] {
                if (cancellationRequested) throw Cancelled();
            };
            const auto notifyRetry = [&](const GenerationRetryStatus& status) {
                AgentEvent event{AgentEventType::EXECUTION_RETRY, {}, {}};
                event.retry = status;
                notify(std::move(event));
            };
            try {
                checkCancelled();
                bool projectAllowed = false;
                const auto refreshProjectContext = [&] {
                    if (workspaceMoved && !toolContext.directory.empty()) {
                        const auto text = "Harness workspace: the current working directory is " + toolContext.directory.string() +
                            ". This supersedes earlier working-directory announcements. " +
                            (skillsWorkspace.empty() ? std::string{} : "Skills use the source configuration at " + skillsWorkspace.string() + ".");
                        if (auto message = changedProjectContext("project_instructions_workspace", text, conversation.getMessages())) {
                            { std::lock_guard lock(mutex); conversation.append(*message); }
                            notify({AgentEventType::HARNESS_MESSAGE_ARRIVED, {*message}, {}});
                        }
                    }
                    const auto projectRelative = toolContext.permissionRoot
                        ? projectWorkspace.lexically_relative(std::filesystem::weakly_canonical(*toolContext.permissionRoot))
                        : std::filesystem::path{"."};
                    projectAllowed = skillCatalog && !projectRelative.empty() && *projectRelative.begin() != "..";
                    if (skillCatalog && !projectAllowed) *skillCatalog = SkillCatalog{};
                    if (projectAllowed) {
                        *skillCatalog = SkillCatalog(skillsWorkspace);
                        auto contextMessages = projectInstructions(projectWorkspace, projectWorkspace, conversation.getMessages());
                        if (auto catalog = changedProjectContext("skill_catalog", skillCatalog->prompt(), conversation.getMessages()))
                            contextMessages.push_back(std::move(*catalog));
                        for (const auto& message : contextMessages) {
                            { std::lock_guard lock(mutex); conversation.append(message); }
                            notify({AgentEventType::HARNESS_MESSAGE_ARRIVED, {message}, {}});
                        }
                    }
                };
                ModelInfo modelInfo;
                bool loadModelInfo = true;
                for (;;) {
                    checkCancelled();
                    refreshProjectContext();
                    if (toolSource) {
                        auto replacement = toolSource([this] { return cancellationRequested.load(); });
                        checkCancelled();
                        tools.replaceDynamic(std::move(replacement));
                    }
                    // Freeze definitions through retries, compaction and this response's tools.
                    const auto definitions = tools.getDefinitions();
                    bool changed;
                    {
                        std::lock_guard settingsLock(mutex);
                        changed = applyPendingSettings();
                        if (changed) reasoningEffort = activeReasoning;
                    }
                    if (changed) {
                        // Publish current execution settings without inventing a conversation message.
                        notify({AgentEventType::HARNESS_MESSAGE_ARRIVED, {}, {}});
                        loadModelInfo = true;
                    }
                    if (loadModelInfo) {
                        runWithRetry([&] {
                            modelInfo = provider->getModelInfo(model);
                        }, [this] { return cancellationRequested.load(); }, notifyRetry);
                        if (contextLimit) {
                            const auto maximum = modelInfo.maxContextLimit ? modelInfo.maxContextLimit : modelInfo.contextLimit;
                            if (maximum && *contextLimit > *maximum)
                                throw std::runtime_error("Configured context limit exceeds the provider maximum; update /model");
                            modelInfo.contextLimit = contextLimit;
                            modelInfo.maxContextLimit = contextLimit;
                            // Keep the usual headroom within the explicitly selected input budget.
                            modelInfo.inputLimit = std::min(modelInfo.inputLimit.value_or(*contextLimit), *contextLimit);
                        }
                        if (const auto limit = modelInfo.maxContextLimit ? modelInfo.maxContextLimit : modelInfo.contextLimit) {
                            std::lock_guard contextLock(mutex);
                            contextUsage.totalContext = limit;
                        }
                        loadModelInfo = false;
                    }
                    checkCancelled();
                    for (const auto& message : conversation.getMessages()) {
                        if (!message.images.empty()) requireImageSupport(modelInfo.imageInput);
                        for (const auto& part : message.content)
                            if (const auto* tool = std::get_if<ToolResultContent>(&part))
                                for (const auto& attachment : tool->attachments)
                                    if (attachment.mime.starts_with("image/")) requireImageSupport(modelInfo.imageInput);
                    }
                    if (prepareContext) {
                        auto prepared = prepareContext({conversation, modelInfo, definitions,
                            contextUsage.lastTurnUsedContext, measuredMessages, reasoningEffort}, *provider,
                            [this] { return cancellationRequested.load(); });
                        if (prepared) {
                            std::lock_guard contextLock(mutex);
                            conversation = std::move(*prepared);
                            contextUsage.lastTurnUsedContext.reset();
                            measuredMessages = 0;
                        }
                    }
                    checkCancelled();
                    std::chrono::steady_clock::time_point modelStarted;
                    auto response = generateWithRetry([&] {
                        modelStarted = std::chrono::steady_clock::now();
                        return provider->generate(conversation.getProviderFormat(), model, definitions, reasoningEffort);
                    }, [this] { return cancellationRequested.load(); }, notifyRetry);
                    const auto modelDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - modelStarted).count();
                    std::vector<ToolCallContent> calls;
                    std::string responseId;
                    Message completedMessage;
                    {
                        std::lock_guard responseLock(mutex);
                        contextUsage.lastTurnUsedContext = provider->getUsedContext(response);
                        conversation.append(response);
                        conversation.setLastElapsedMs(generationMetadata().durationMs);
                        conversation.setLastModelCall({model, reasoningEffort, static_cast<std::uint64_t>(modelDuration),
                            provider->getOutputTokens(response)});
                        measuredMessages = conversation.getMessages().size();
                        const auto& message = conversation.getMessages().back();
                        responseId = message.id;
                        for (const auto& content : message.content)
                            if (const auto* call = std::get_if<ToolCallContent>(&content)) calls.push_back(*call);
                        if (calls.empty()) {
                            finalMetadata = generationMetadata();
                            conversation.setLastGenerationMetadata(*finalMetadata);
                        }
                        completedMessage = message;
                    }
                    notify({AgentEventType::PROVIDER_MESSAGE_ARRIVED, {std::move(completedMessage)}, {}});
                    if (calls.empty()) { checkCancelled(); break; }

                    bool stepSubmitted = false;
                    // Execute in order, outside the conversation lock: status queries remain available.
                    for (const auto& call : calls) {
                        if (call.toolName == "view_image") requireImageSupport(modelInfo.imageInput);
                        std::vector<Image> images;
                        ToolResultContent result;
                        result.callId = call.callId;
                        bool started = false;
                        bool awaitingPermission = false;
                        std::vector<PermissionRecord> decisions;
                        std::optional<nlohmann::json> replayedAction;
                        auto mcpIdentity = nlohmann::json::object();
                        try {
                            checkCancelled();
                            if (stepSubmitted) throw std::logic_error("Tool not executed: step completion already ended execution. Submit completion last.");
                            auto context = toolContext;
                            const auto previousCheck = context.checkCancelled;
                            context.checkCancelled = [checkCancelled, previousCheck] {
                                checkCancelled();
                                if (previousCheck) previousCheck();
                            };
                            if (permissionHandler) {
                                context.ask = [&](const ToolPermissionRequest& request) {
                                    if (request.permission == "mcp") {
                                        mcpIdentity = {{"mcp_server", request.metadata.at("server")},
                                            {"mcp_tool", request.metadata.at("mcp_tool")}};
                                    }
                                    awaitingPermission = true;
                                    auto identified = request;
                                    identified.callId = call.callId;
                                    if (call.toolName != "rerun_approved_action" || !request.manuallyApproved) {
                                        identified.toolName = call.toolName;
                                        identified.metadata["arguments"] = call.arguments;
                                        identified.metadata["working_directory"] = context.directory.string();
                                        identified.manuallyApproved = false;
                                        identified.manualApprovalConfidence.reset();
                                    }
                                    if (identified.manuallyApproved)
                                        replayedAction = {{"id", identified.metadata.at("approval_id")}, {"tool", identified.toolName},
                                            {"arguments", identified.metadata.at("arguments")}};
                                    const auto answer = permissionHandler(identified, context.checkCancelled);
                                    if (answer.decision != PermissionDecision::ALLOW)
                                        decisions.push_back({answer.id, request.permission, request.patterns, answer.decision, answer.confidence});
                                    if (answer.decision == PermissionDecision::NO) {
                                        cancellationRequested = true;
                                        throw Cancelled();
                                    }
                                    if (answer.decision == PermissionDecision::AI_REFUSED) {
                                        result.metadata["refused_action"] = {{"tool", call.toolName}, {"arguments", call.arguments},
                                            {"working_directory", context.directory.string()},
                                            {"workspace", context.permissionRoot.value_or(context.worktree).string()}};
                                        throw std::runtime_error(answer.reason);
                                    }
                                    awaitingPermission = false;
                                };
                                // Reject disabled tool families before they can inspect targets or compute diffs.
                                context.ask({"tool", {}, {}, nlohmann::json::object()});
                            }
                            const auto& tool = tools.get(call.toolName);
                            started = true;
                            const auto output = tool.execute(call.arguments, context);
                            result.output = output.output;
                            result.attachments = output.attachments;
                            result.metadata = output.metadata;
                            images = output.images;
                        } catch (const std::exception& error) {
                            if (awaitingPermission) started = false;
                            // A denied, unknown or failed tool is a result the model can react to.
                            result.output = error.what();
                            result.isError = true;
                            if (cancellationRequested) {
                                result.output = started
                                    ? "Tool interrupted. Its effects may be partial; inspect the current state before retrying."
                                    : "Tool was not executed because the run was cancelled.";
                                result.metadata = {{"cancelled", true}, {"started", started}};
                            }
                        }
                        result.metadata.update(mcpIdentity);
                        if (!decisions.empty()) result.metadata["permissions"] = decisions;
                        if (replayedAction) result.metadata["approved_action"] = *replayedAction;
                        Message message{"tool_" + responseId + "_" + call.callId, MessageRole::TOOL, {std::move(result)}};
                        message.images = std::move(images);
                        auto& storedResult = std::get<ToolResultContent>(message.content.front());
                        stepSubmitted |= implementationExecution && call.toolName == "complete_implementation_step" &&
                            !storedResult.isError && storedResult.metadata.contains("implementation_completion");
                        if (stepSubmitted && !finalMetadata) {
                            finalMetadata = generationMetadata();
                            message.metadata = *finalMetadata;
                        }
                        if (storedResult.metadata.contains("refused_action")) storedResult.metadata["refused_message_id"] = message.id;
                        message.elapsedMs = generationMetadata().durationMs;
                        {
                            std::lock_guard resultLock(mutex);
                            conversation.append(message);
                        }
                        const bool loadedFile = !storedResult.isError && call.toolName == "read";
                        notify({AgentEventType::HARNESS_MESSAGE_ARRIVED, {std::move(message)}, {}});
                        if (projectAllowed && loadedFile) {
                            const auto target = std::filesystem::path(call.arguments.at("filePath").get<std::string>());
                            for (const auto& instruction : projectInstructions(projectWorkspace,
                                target.is_absolute() ? target : toolContext.directory / target, conversation.getMessages())) {
                                { std::lock_guard lock(mutex); conversation.append(instruction); }
                                notify({AgentEventType::HARNESS_MESSAGE_ARRIVED, {instruction}, {}});
                            }
                        }
                    }
                    if (stepSubmitted) {
                        checkCancelled();
                        break;
                    }
                }
            } catch (const Cancelled&) {
                try {
                    Message interruption;
                    {
                        std::lock_guard historyLock(mutex);
                        interruption = Message{"interruption_" + std::to_string(conversation.getMessages().size()),
                            MessageRole::SYSTEM, {TextContent{
                                "The previous run was interrupted. Preserve completed results. Interrupted tools may have partial effects; "
                                "check their current state before retrying. Do not automatically repeat unexecuted tools. "
                                "Continue according to the next user instruction."
                            }}};
                        conversation.append(interruption);
                    }
                    notify({AgentEventType::HARNESS_MESSAGE_ARRIVED, {std::move(interruption)}, {}});
                    notify({AgentEventType::EXECUTION_CANCELLED, {}, "Execution cancelled"});
                }
                catch (...) {
                    std::lock_guard failureLock(mutex);
                    running = false;
                    throw;
                }
                std::lock_guard completionLock(mutex);
                running = false;
                return;
            } catch (...) {
                const auto failure = std::current_exception();
                std::string message = "Unknown execution failure";
                std::string code;
                try { std::rethrow_exception(failure); }
                catch (const ImageInputError& error) { message = error.what(); code = "image_input"; }
                catch (const GenerationRetriesExhausted& error) { message = error.what(); code = "retries_exhausted"; }
                catch (const std::exception& error) { message = error.what(); }
                catch (...) {}
                // Preserve the original failure even if the subscriber fails too.
                try {
                    AgentEvent event{AgentEventType::EXECUTION_FAILED, {}, message};
                    event.errorCode = code;
                    notify(std::move(event));
                } catch (...) {}
                std::lock_guard failureLock(mutex);
                running = false;
                throw;
            }
            try { notify({AgentEventType::EXECUTION_COMPLETED, {}, {}}); }
            catch (...) {
                std::lock_guard failureLock(mutex);
                running = false;
                throw;
            }
            std::lock_guard completionLock(mutex);
            running = false;
        }).share();
    } catch (...) {
        running = false;
        throw;
    }
    return execution;
}
