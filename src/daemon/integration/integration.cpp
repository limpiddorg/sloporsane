#include "integration.hpp"
#include "../transport/await_future.hpp"
#include "../session/history_chunks.hpp"
#include "../session/implementation_worktree.hpp"
#include <agent-runtime/conversation/messages/message_json.hpp>
#include <asio/co_spawn.hpp>
#include <asio/post.hpp>
#include <algorithm>
#include <iostream>

namespace {
const char* eventName(AgentEventType type) {
    switch (type) {
        case AgentEventType::PROVIDER_MESSAGE_ARRIVED: return "provider.message_arrived";
        case AgentEventType::HARNESS_MESSAGE_ARRIVED: return "harness.message_arrived";
        case AgentEventType::COMPACTION_STARTED: return "compaction.started";
        case AgentEventType::COMPACTION_PROGRESS: return "compaction.progress";
        case AgentEventType::COMPACTION_COMPLETED: return "compaction.completed";
        case AgentEventType::COMPACTION_FAILED: return "compaction.failed";
        case AgentEventType::COMPACTION_CANCELLED: return "compaction.cancelled";
        case AgentEventType::EXECUTION_COMPLETED: return "execution.completed";
        case AgentEventType::EXECUTION_FAILED: return "execution.failed";
        case AgentEventType::EXECUTION_RETRY: return "execution.retry";
        case AgentEventType::EXECUTION_CANCELLED: return "execution.cancelled";
    }
    throw std::logic_error("Unknown agent event");
}
}

Integration::Integration(asio::io_context& io, ConnectionSocket connection, SessionRegistry& registry,
    SessionTitleGenerator titleGenerator, ReasoningEffortsLoader reasoningEffortsLoader, IntegrationHealthSettings health,
    std::shared_ptr<ProviderConnections> providers, CompactionService::ProviderFactory summaryProvider, BtwProviderFactory btwProvider,
    std::shared_ptr<mcp::Connections> mcps)
    : io(io), connection(std::move(connection)), registry(registry), titleGenerator(std::move(titleGenerator)),
      reasoningEffortsLoader(std::move(reasoningEffortsLoader)),
      events(std::make_shared<AsyncQueue<Event>>(io.get_executor())), health(health), healthTimer(io),
      providers(std::move(providers)), providerConnection(this->providers) {
    this->summaryProvider = std::move(summaryProvider);
    this->btwProvider = std::move(btwProvider);
    this->mcps = mcps ? std::move(mcps) : std::make_shared<mcp::Connections>();
    if (health.interval.count() <= 0 || health.timeout.count() <= 0)
        throw std::invalid_argument("Integration health durations must be positive");
    permissions = std::make_shared<PermissionManager>([executor = io.get_executor(), queue = events](PermissionEvent event) {
        asio::post(executor, [queue, event = std::move(event)]() mutable { queue->push(std::move(event)); });
    });
    registry.watchWorkspace(this, [this] {
        return !pendingWorkspace.empty() ? pendingWorkspace : currentWorkspace();
    });
}

Integration::~Integration() { registry.unwatchWorkspace(this); }

std::filesystem::path Integration::currentWorkspace() const {
    return session ? session->getInfo().workspace : idleWorkspace.empty() ? permissions->root() : idleWorkspace;
}

nlohmann::json Integration::runtimeInfo() const {
    const auto info = session ? session->getInfo() : SessionInfo{};
    const auto selected = providerConnection.selection();
    const bool sameModel = session && info.provider == selected.provider && info.model == selected.model;
    nlohmann::json result = sameModel ? info.context : ContextUsage{};
    if (session) {
        result["workspace"] = info.workspace.string();
        result["configuration_workspace"] = info.configurationDirectory().string();
    } else if (!permissions->root().empty()) {
        result["workspace"] = currentWorkspace().string();
    }
    result["model"] = selected.model;
    result["provider"] = selected.provider;
    result["permission_mode"] = permissionModeName(providers ? providers->permissionMode() : PermissionMode::ASK);
    result["has_permission_key"] = providers && providers->hasPermissionKey();
    result["permission_confidence"] = providers ? providers->permissionConfidence() : 80;
    result["reasoning"] = selected.reasoning;
    result["context_limit"] = selected.contextLimit ? nlohmann::json(*selected.contextLimit) : nlohmann::json(nullptr);
    if (selected.contextLimit) result["total_context"] = *selected.contextLimit;
    const auto generation = session && session->isExecuting() ? std::optional{session->executionGeneration()} : std::nullopt;
    result["execution_model"] = generation ? nlohmann::json(generation->model) : nlohmann::json(nullptr);
    result["last_history_is_recap"] = session && session->lastHistoryIsRecap();
    result["objective_plan_ready"] = session && session->objectivePlanReady();
    const auto objectiveSource = discussion ? discussion : session;
    result["objective_plans"] = objectiveSource ? nlohmann::json(objectiveSource->objectivePlans()) : nlohmann::json::array();
    result["objective_plan_turns"] = nlohmann::json::array();
    if (objectiveSource)
        for (const auto& turn : objectiveSource->objectivePlanTurns())
            result["objective_plan_turns"].push_back({{"message_id", turn.messageId},
                {"objective_id", turn.objectiveId}, {"updated", turn.updated}});
    result["implementation_plan_ready"] = session && session->implementationPlanReady();
    const auto plan = session && !info.discussionId.empty() ? session->latestImplementationPlan(false) : std::nullopt;
    result["implementation_plan"] = plan ? nlohmann::json(*plan) : nlohmann::json(nullptr);
    result["discussion_id"] = discussion ? discussion->getInfo().id : info.id;
    result["implementation_view"] = !info.discussionId.empty();
    result["implementation_archive"] = viewingArchive;
    result["implementation_execution"] = !info.discussionId.empty() && runSessionId == info.id && implementationRun && implementationRun->exists();
    result["auto_plan_pending"] = !info.discussionId.empty() && !info.autoPlanPace.empty() && !result["implementation_execution"].get<bool>();
    result["auto_plan_running"] = result["auto_plan_pending"].get<bool>() && (session->isExecuting() || bool(preparingCancellation));
    result["auto_plan_pace"] = info.autoPlanPace;
    result["auto_plan_error"] = info.autoPlanError;
    const auto& parent = discussion ? discussion->getInfo() : info;
    result["active_implementation_id"] = parent.activeImplementationId;
    result["implementation_branches"] = nlohmann::json::array();
    for (const auto& id : parent.implementationIds) {
        const auto branch = registry.inspect(id);
        result["implementation_branches"].push_back({{"id", id}, {"objective_id", branch.objectiveId},
            {"frozen", branch.frozen}});
    }
    result["has_stored_images"] = session && session->hasStoredImages();
    result["execution_reasoning"] = generation ? nlohmann::json(generation->reasoning) : nlohmann::json(nullptr);
    if (!permissions->root().empty() && (!session || !session->isExecuting())) {
        const auto workspace = session ? info.configurationDirectory() : permissions->root();
        const auto relative = std::filesystem::weakly_canonical(workspace).lexically_relative(permissions->root());
        if ((session && !info.configurationWorkspace.empty()) || (!relative.empty() && *relative.begin() != "..")) {
            const SkillCatalog catalog(workspace);
            skillInfo = {{"skills", catalog.metadata(true)}, {"skill_errors", catalog.errors()}};
        } else skillInfo = {{"skills", nlohmann::json::array()}, {"skill_errors", {"Skills outside the integration workspace are not loaded automatically."}}};
    }
    result.update(skillInfo);
    return result;
}

void Integration::schedulePing() {
    if (closing) return;
    healthTimer.expires_after(health.interval);
    healthTimer.async_wait([self = shared_from_this()](const asio::error_code& error) {
        if (!error && !self->closing) {
            self->pingDue = true;
            self->events->push(Ping{});
        }
    });
}

void Integration::acknowledgePong(const std::string& id) {
    if (expectedPong.empty() || id != expectedPong) return;
    expectedPong.clear();
    schedulePing();
}

nlohmann::json Integration::makePing() {
    pingDue = false;
    expectedPong = std::to_string(++pingNumber);
    // Start the deadline at the writer, not while queued behind other work.
    healthTimer.expires_after(health.timeout);
    healthTimer.async_wait([self = shared_from_this()](const asio::error_code& error) {
        if (!error && !self->closing && !self->expectedPong.empty()) {
            std::cerr << "[sosd] Integration heartbeat timed out\n";
            self->close();
        }
    });
    return {{"type", "integration.ping"}, {"pingId", expectedPong}};
}

void Integration::close() {
    if (closing) return;
    closing = true;
    healthTimer.cancel();
    connection.close();
    if (preparingCancellation) preparingCancellation->store(true);
    if (btwPreparationCancellation) btwPreparationCancellation->store(true);
    if (summaryCancellation) summaryCancellation->store(true);
    if (historyCancellation) historyCancellation->store(true);
    if (permissionCancellation) permissionCancellation->store(true);
    if (mcpCancellation) mcpCancellation->store(true);
    providerConnection.cancel();
    if (session) session->cancel();
    for (const auto& [key, run] : btwRuns) run->agent->cancel();
    permissions->close();
    questions->close();
    events->push(Disconnected{});
}

void Integration::prepareRequest(IntegrationRequest& request) {
    if (request.type == "mcp.cancel" && mcpCancellation) mcpCancellation->store(true);
    const bool switching = request.type == "session.new" || request.type == "session.attach" ||
        (request.providerRequest && request.providerRequest->command == ProviderCommand::SELECT);
    request.blockedBySummary = summaryCancellation && request.type != "execution.cancel" && request.type != "mode.set" &&
        request.type != "session.list" && request.type != "session.preview" && !switching &&
        !request.providerRequest && request.type != "permissions.select" && request.type != "permissions.get";
    if (request.blockedBySummary) return;
    if (request.type == "session.compact" || request.type == "session.recap")
        summaryCancellation = std::make_shared<std::atomic<bool>>(false);
    if (request.providerRequest)
        providerConnection.prepare(*request.providerRequest);
    if (request.type == "message.send" && (!session || !session->isExecuting()) && !preparingCancellation)
        preparingCancellation = std::make_shared<std::atomic<bool>>(false);
    if (request.type != "execution.cancel" && !switching) return;
    if (session && !session->getInfo().autoPlanPace.empty()) implementationStopping = true;
    if (implementationRun && implementationRun->exists()) {
        implementationStopping = true;
        if (request.type == "execution.cancel" && implementationRun->next()) request.cancelApplied = true;
    }
    if (summaryCancellation) {
        summaryCancellation->store(true);
        request.cancelApplied = true;
    }
    if (preparingCancellation) {
        preparingCancellation->store(true);
        request.cancelApplied = true;
    }
    if (session && session->isExecuting()) {
        session->cancel();
        request.cancelApplied = true;
    }
}

asio::awaitable<void> Integration::readRequests() {
    while (auto json = co_await connection.receive()) {
        IntegrationRequest request;
        try { request = json->get<IntegrationRequest>(); }
        catch (const std::exception&) {
            if (!json->value("type", "").starts_with("mcp.")) throw;
            // Never echo parse diagnostics that could contain a credential.
            request = IntegrationRequest{};
            request.type = "mcp.invalid";
            request.requestId = json->value("requestId", "");
            request.invalidMcp = true;
        }
        if (request.type == "integration.close") { close(); co_return; }
        if (request.type == "integration.pong") { acknowledgePong(request.pingId); continue; }
        // Signal cancellation immediately, even while the consumer awaits a worker.
        prepareRequest(request);
        events->push(std::move(request));
    }
}

void Integration::subscribe() {
    permissions->setImplementationPhase(ImplementationPhase::PLANNING);
    if (!session->getInfo().discussionId.empty()) ensureImplementationRun();
    const auto root = executionPermissionRoot();
    permissions->setExecutionRoot(session->getInfo().executionWorkspace.empty() && session->getInfo().configurationWorkspace.empty()
        ? std::filesystem::path{} : root);
    session->setWorkspaceHandler([executor = io.get_executor(), queue = events](const auto& arguments, const auto& check) {
        if (check) check();
        auto result = std::make_shared<std::promise<nlohmann::json>>();
        auto future = result->get_future();
        asio::post(executor, [queue, arguments, result] { queue->push(WorkspaceRequest{arguments, result}); });
        // Once submitted, resolve the transition even if cancellation arrives. The
        // worker must adopt the committed workspace before observing cancellation.
        return future.get();
    });
    session->setQuestionHandler([questions = questions, executor = io.get_executor(), queue = events]
        (const std::string& question, const std::vector<std::string>& options, const std::function<void()>& check) {
        return questions->ask(question, options, check, [executor, queue](QuestionEvent event) {
            asio::post(executor, [queue, event = std::move(event)]() mutable { queue->push(std::move(event)); });
        });
    });
    session->setPermissionHandler([policy = permissions](const ToolPermissionRequest& request, const std::function<void()>& check) {
        return policy->ask(request, check);
    }, root);
    // Capture the mailbox, not this: the worker never accesses Integration/Session state.
    session->setEventHandler([executor = io.get_executor(), queue = events](AgentEvent event) {
        asio::post(executor, [queue, event = std::move(event)]() mutable { queue->push(std::move(event)); });
    });
}

asio::awaitable<nlohmann::json> Integration::handle(const IntegrationRequest& request) {
    using nlohmann::json;
    if (request.invalidMcp) throw std::invalid_argument("Invalid MCP request; check the configuration fields");
    if (request.type == "mcp.cancel") co_return co_await handleMcp(request);
    if (request.mcpRequest) co_return co_await handleMcp(request);
    if (request.type == "btw.send" && session && session->getInfo().frozen)
        throw std::logic_error("Archived Implement is read-only");
    if (request.type.starts_with("btw.")) co_return co_await handleBtw(request);
    if (request.blockedBySummary) throw std::logic_error("Wait for summary generation to finish");
    if (request.type.starts_with("worktree.")) {
        if ((session && session->isExecuting()) || preparingCancellation)
            throw std::logic_error("Wait for the current execution to finish before using worktrees");
        auto result = std::make_shared<std::promise<json>>();
        auto future = result->get_future();
        const auto action = request.type.substr(9);
        if (action != "list" && permissions->mode() != ExecutionMode::SLOP)
            throw std::logic_error("Use the native worktree tool to create or switch in Sane; the picker requires Slop");
        co_await handleWorkspace(WorkspaceRequest{{{"action", action}, {"name", request.worktreeName}, {"path", request.worktreePath}}, result});
        auto output = future.get();
        output["type"] = action == "list" ? "worktree.listed" : action == "delete" ? "worktree.deleted" : "worktree.switched";
        output["requestId"] = request.requestId;
        output["info"] = runtimeInfo();
        if (session) output["sessionId"] = session->getInfo().id;
        co_return output;
    }
    if (request.providerRequest) co_return co_await handleProvider(request);
    if (request.type == "permissions.select") co_return co_await selectPermissions(request);
    if (request.type == "permission.manual") {
        if (!session || session->getInfo().id != request.sessionId) throw std::invalid_argument("Session is not attached");
        session->setManualApproval(request.refusedMessageId, request.manuallyApproved);
        co_return json{{"type", "permission.manual_updated"}, {"requestId", request.requestId},
            {"sessionId", request.sessionId}, {"states", session->manualApprovalStates()}};
    }
    if (request.type == "permissions.get") {
        co_return nlohmann::json{{"type", "permissions.current"}, {"requestId", request.requestId},
            {"mode", permissionModeName(providers ? providers->permissionMode() : PermissionMode::ASK)},
            {"has_key", providers && providers->hasPermissionKey()},
            {"confidence", providers ? providers->permissionConfidence() : 80}};
    }
    if (request.type == "integration.configure") {
        auto response = handlePermissions(request);
        auto restoration = mcps->restoreInBackground(permissions->root());
        mcpRestoring = true;
        asio::co_spawn(io, reportMcpRestoration(std::move(restoration), permissions->root()),
            [self = shared_from_this()](std::exception_ptr error) {
            if (error && !self->closing) {
                try { std::rethrow_exception(error); }
                catch (const std::exception& exception) { std::cerr << "[sosd] MCP restore notification failed: " << exception.what() << '\n'; }
            }
        });
        co_return response;
    }
    if (request.type == "mode.set" || request.type == "permission.reply")
        co_return handlePermissions(request);
    if (request.type == "question.reply" || request.type == "question.reject") {
        questions->reply(request.questionId, request.questionChoice);
        co_return nlohmann::json{{"type", "question.answered"}, {"requestId", request.requestId}};
    }
    if (request.type == "session.new") {
        co_await stopExecution();
        co_return newSession(request);
    }
    if (request.type == "implementation.open" || request.type == "implementation.view" || request.type == "implementation.close")
        co_return co_await switchImplementation(request);
    if (request.type.starts_with("implementation.")) co_return co_await handleImplementation(request);
    if (request.type == "session.attach") co_return co_await attachSession(request);
    if (request.type == "session.history" || request.type == "session.activities" || request.type == "session.activity_detail") {
        auto source = session && session->getInfo().id == request.sessionId ? session :
            discussion && discussion->getInfo().id == request.sessionId ? discussion : nullptr;
        if (!source) throw std::invalid_argument("Session is not attached");
        co_await sendHistory(request, {}, source);
        co_return nullptr;
    }
    if (request.type == "session.compact") co_return co_await compactSession(request);
    if (request.type == "session.recap") co_return co_await recapSession(request);
    if (request.type == "session.undo" || request.type == "session.redo") {
        if (session && !session->getInfo().discussionId.empty() && ensureImplementationRun().exists())
            throw std::logic_error("Turn undo/redo is unavailable during structured implementation review");
        co_return co_await navigateSession(request);
    }
    if (request.type == "reasoning.next") co_return co_await nextReasoningEffort(request);
    if (request.type == "session.list")
        co_return json{{"type", "session.listed"}, {"requestId", request.requestId}, {"sessions", listSessions()}};
    if (request.type == "session.preview") {
        historyCancellation = std::make_shared<std::atomic<bool>>(false);
        const auto text = co_await registry.previewAsync(request.sessionId, [cancel = historyCancellation] { return cancel->load(); });
        historyCancellation.reset();
        co_return json{{"type", "session.previewed"}, {"requestId", request.requestId},
            {"sessionId", request.sessionId}, {"text", text}};
    }
    if (request.type == "execution.cancel") {
        if (implementationRun && implementationRun->exists() && !session->isExecuting() && implementationRun->next()) {
            implementationRun->stop("Implementation cancelled");
            co_await sendReply({{"type", "implementation.updated"}, {"sessionId", session->getInfo().id},
                {"implementation", implementationRun->view()}});
        }
        co_return json{{"type", request.cancelApplied ? "execution.cancel_requested" : "execution.cancel_ignored"},
            {"requestId", request.requestId}};
    }
    if (request.type == "message.check") {
        co_await checkImageInput(request.hasImages);
        co_return json{{"type", "message.checked"}, {"requestId", request.requestId}};
    }
    co_return co_await sendMessage(request);
}

asio::awaitable<nlohmann::json> Integration::handleProvider(const IntegrationRequest& request) {
    const bool selecting = request.providerRequest->command == ProviderCommand::SELECT;
    auto reply = co_await providerConnection.handle(*request.providerRequest);
    if (selecting) {
        co_await stopExecution();
        saveSession();
        detachSession();
        resetConversationDefaults();
    }
    if (request.providerRequest->command == ProviderCommand::SELECT_MODEL) applyCurrentSelection();
    reply["requestId"] = request.requestId;
    co_return reply;
}

asio::awaitable<nlohmann::json> Integration::selectPermissions(const IntegrationRequest& request) {
    if (!providers) throw std::runtime_error("Permission settings are unavailable");
    if (request.permissionMode == PermissionMode::AUTO && !request.permissionKey.empty()) {
        permissionCancellation = std::make_shared<std::atomic<bool>>(false);
        auto job = std::async(std::launch::async, [connections = providers, key = request.permissionKey,
            cancel = permissionCancellation] {
            connections->validateApiKey("openrouter", key, [cancel] { return cancel->load(); });
        });
        co_await awaitFuture(job);
        const bool cancelled = permissionCancellation->load();
        permissionCancellation.reset();
        job.get();
        if (closing || cancelled) co_return nullptr;
        providers->savePermissionKey(request.permissionKey);
    }
    providers->setPermissionMode(request.permissionMode, request.permissionConfidence);
    permissions->setPermissionMode(request.permissionMode,
        providers->permissionVerifier(permissions->root().string(), session ? session->lastUserMessage() : ""),
        providers->permissionConfidence());
    co_return nlohmann::json{{"type", "permissions.selected"}, {"requestId", request.requestId},
        {"mode", permissionModeName(request.permissionMode)}, {"confidence", providers->permissionConfidence()}};
}

nlohmann::json Integration::handlePermissions(const IntegrationRequest& request) {
    if (request.type == "integration.configure") {
        registry.requireWorkspaceAvailable(request.workspace);
        permissions->configure(request.workspace, request.mode);
        selectedMode = request.mode;
        terminalSessionId = request.terminalSessionId;
        return {{"type", "integration.configured"}, {"requestId", request.requestId},
            {"mode", executionModeName(permissions->mode())}, {"workspace", permissions->root().string()}};
    }
    if (request.type == "mode.set") {
        if (session && !session->getInfo().discussionId.empty() && request.mode != ExecutionMode::SANE)
            throw std::logic_error("Implement is fixed to Sane mode");
        selectedMode = request.mode;
        if (!session || !session->isExecuting()) permissions->setMode(request.mode);
        return {{"type", "mode.updated"}, {"requestId", request.requestId}, {"mode", executionModeName(request.mode)}};
    }
    permissions->reply(request.permissionId, request.decision);
    return {{"type", "permission.reply_accepted"}, {"requestId", request.requestId}, {"permissionId", request.permissionId}};
}

void Integration::requireIdleSession() const {
    if (session && session->isExecuting()) throw std::logic_error("Wait for execution to finish before switching sessions or providers");
    if (session && implementationRun && runSessionId == session->getInfo().id && implementationRun->status == "running")
        throw std::logic_error("Cancel implementation construction before switching sessions or providers");
}
asio::awaitable<void> Integration::stopExecution() {
    if (!session || !session->isExecuting()) {
        if (implementationStopping && implementationRun && implementationRun->status == "running")
            implementationRun->stop("Implementation interrupted before the next substep");
        co_return;
    }
    session->cancel();
    std::deque<Event> deferred;
    while (session->isExecuting()) {
        auto event = co_await events->receive();
        if (std::holds_alternative<IntegrationRequest>(event)) deferred.push_back(std::move(event));
        else co_await sendReply(co_await dispatch(event));
    }
    // Keep requests in their original order, ahead of events received while draining.
    while (!deferred.empty()) {
        events->pushFront(std::move(deferred.back()));
        deferred.pop_back();
    }
}
void Integration::saveSession() { if (session) session->save(); }
nlohmann::json Integration::listSessions() const {
    auto result = nlohmann::json::array();
    for (const auto& info : registry.list()) {
        nlohmann::json item = info;
        item.erase("terminalOpenings");
        for (const auto& opening : info.terminalOpenings)
            if (opening.terminalSessionId == terminalSessionId && !terminalSessionId.empty())
                item["terminalLastOpenedAt"] = opening.lastOpenedAt;
        result.push_back(std::move(item));
    }
    return result;
}
void Integration::detachSession() {
    if (session) session->setEventHandler({});
    session.reset();
    providerConnection.restoreSelection(std::nullopt);
    discussion.reset();
    implementation.reset();
    viewingArchive = false;
    implementationRun.reset();
    runSessionId.clear();
    implementationExecuting = implementationStopping = false;
}
void Integration::resetConversationDefaults() {
    permissions->setMode(ExecutionMode::SANE);
    selectedMode = ExecutionMode::SANE;
}

void Integration::applyCurrentSelection() {
    if (!session) return;
    const auto selected = providerConnection.selection();
    const auto& previous = session->getInfo();
    std::unique_ptr<Provider> replacement;
    if (session->agentReady() && providers && (session->isExecuting() || previous.model != selected.model || previous.provider != selected.provider))
        replacement = providers->create(selected.provider);
    session->applySelection(selected, std::move(replacement));
}

nlohmann::json Integration::newSession(const IntegrationRequest& request) {
    requireIdleSession();
    saveSession();
    const auto workspace = currentWorkspace();
    detachSession();
    idleWorkspace = workspace;
    permissions->setExecutionRoot(workspace);
    resetConversationDefaults();
    return {{"type", "session.reset"}, {"requestId", request.requestId}, {"workspace", workspace.string()}};
}

asio::awaitable<nlohmann::json> Integration::attachSession(const IntegrationRequest& request) {
    if (!permissions->isConfigured()) throw std::logic_error("Configure the integration workspace before attaching a session");
    co_await stopExecution();
    if (discussion && discussion->getInfo().id == request.sessionId) {
        session->setEventHandler({});
        session = discussion;
        viewingArchive = false;
    }
    if (!session || session->getInfo().id != request.sessionId) {
        saveSession();
        historyCancellation = std::make_shared<std::atomic<bool>>(false);
        auto selected = co_await registry.loadAsync(request.sessionId, std::nullopt,
            [cancel = historyCancellation] { return cancel->load(); });
        historyCancellation.reset();
        if (closing) co_return nullptr;
        detachSession();
        session = std::move(selected);
    }
    if (!session->getInfo().discussionId.empty()) {
        if (!discussion || discussion->getInfo().id != session->getInfo().discussionId)
            discussion = co_await registry.loadAsync(session->getInfo().discussionId, std::nullopt);
        implementation = session;
        viewingArchive = session->getInfo().frozen;
    } else {
        const auto target = registry.resumeTarget(*session);
        if (target != session->getInfo().id) {
            discussion = session;
            session->setEventHandler({});
            if (implementation && implementation->getInfo().id == target) session = implementation;
            else session = co_await registry.loadAsync(target, std::nullopt);
            implementation = session;
        }
    }
    const auto& restored = session->getInfo();
    providerConnection.restoreSelection(ProviderSelection{restored.provider, restored.model, restored.reasoning, restored.contextLimit});
    permissions->setMode(session->getInfo().mode);
    selectedMode = session->getInfo().mode;
    subscribe();
    try { co_await sendHistory(request); }
    catch (...) { close(); throw; } // A partial transfer must never leave a usable, stale client view.
    (discussion ? discussion : session)->recordTerminalOpen(terminalSessionId);
    co_return nullptr;
}

asio::awaitable<nlohmann::json> Integration::switchImplementation(const IntegrationRequest& request) {
    requireIdleSession();
    if (!btwRuns.empty()) throw std::logic_error("Wait for BTW conversations before switching Implement");
    if (!session) throw std::logic_error("Start a Discussion before opening Implement");
    auto parent = discussion ? discussion : session;
    if (parent->getInfo().id != request.sessionId) throw std::invalid_argument("Discussion is not attached");
    auto target = parent;
    if (request.type == "implementation.close") {
        const auto& run = ensureImplementationRun();
        if (viewingArchive || !run.fullyReviewed())
            throw std::logic_error("Validate all implementation steps before closing");
        auto cleanup = std::async(std::launch::async, [directory = session->storageDirectory()] {
            ImplementationWorktree(directory).close();
        });
        co_await awaitFuture(cleanup); cleanup.get();
        registry.freezeImplementation(*parent, *session);
    }
    if (request.type == "implementation.open") target = registry.createImplementation(*parent, request.objectiveId);
    else if (request.type == "implementation.view" && !request.branchId.empty()) {
        const auto& ids = parent->getInfo().implementationIds;
        if (std::find(ids.begin(), ids.end(), request.branchId) == ids.end())
            throw std::invalid_argument("Implementation does not belong to this Discussion");
        if (implementation && implementation->getInfo().id == request.branchId) target = implementation;
        else target = co_await registry.loadAsync(request.branchId, std::nullopt);
    }
    saveSession();
    session->setEventHandler({});
    session = std::move(target);
    discussion = parent;
    if (session != parent) implementation = session;
    viewingArchive = session->getInfo().frozen;
    const auto& restored = session->getInfo();
    providerConnection.restoreSelection(ProviderSelection{restored.provider, restored.model, restored.reasoning, restored.contextLimit});
    selectedMode = session->getInfo().mode;
    permissions->setMode(*selectedMode);
    subscribe();
    try { co_await sendHistory(request); }
    catch (...) { close(); throw; }
    co_return nullptr;
}

asio::awaitable<nlohmann::json> Integration::navigateSession(const IntegrationRequest& request) {
    requireIdleSession();
    if (!session) throw std::logic_error("Start a conversation before using /undo or /redo");
    if (!btwRuns.empty()) throw std::logic_error("Wait for BTW conversations before using /undo or /redo");
    const auto draft = co_await session->navigate(request.type == "session.redo");
    permissions->setMode(session->getInfo().mode);
    selectedMode = session->getInfo().mode;
    permissions->setManualActions(session->manualActions());
    try { co_await sendHistory(request, draft); }
    catch (...) { close(); throw; }
    co_return nullptr;
}

asio::awaitable<void> Integration::sendHistory(const IntegrationRequest& request, const std::optional<Message>& draft,
    std::shared_ptr<Session> source) {
    if (!source) source = session;
    auto& history = source->history();
    const bool detail = request.type == "session.activity_detail";
    const bool activities = request.type == "session.activities" || detail;
    auto page = activities ? SessionHistoryService::Page{} : history.page(request.direction, request.anchor, request.clean,
        source->getInfo().inheritedMessages,
        implementationRun && runSessionId == source->getInfo().id ? implementationRun->planHistoryEnd() : std::nullopt);
    // Resolve invalid groups before starting a transfer.
    auto group = detail ? history.detail(request.groupId, request.activityIndex) :
        activities ? history.activity(request.groupId) : nlohmann::json(nullptr);
    const auto count = (activities ? std::size_t{1} : page.count) + (draft ? 1 : 0);
    const auto manualStates = source->manualApprovalStates();
    co_await sendHistoryFrame({{"type", "session.attach_started"}, {"requestId", request.requestId},
        {"sessionId", source->getInfo().id}, {"session", source->getInfo()}, {"messageCount", count},
        {"purpose", draft ? "navigation" : detail ? "activity-detail" : activities ? "activities" : "page"}, {"groupId", request.groupId},
        {"activityIndex", detail ? nlohmann::json(request.activityIndex) : nlohmann::json(nullptr)},
        {"page", {{"first", page.first}, {"last", page.last}, {"hasOlder", page.hasOlder}, {"hasNewer", page.hasNewer},
            {"start", page.start}, {"end", page.end}, {"total", page.total}}},
        {"info", runtimeInfo()}});
    for (std::size_t index = 0; index < count && !closing; ++index) {
        auto record = draft && index == 0 ? nlohmann::json(*draft) : activities ? std::move(group) : page.record(index - (draft ? 1 : 0));
        if (record.contains("activities")) for (auto& item : record["activities"]) {
            if (!item.contains("result")) continue;
            auto& metadata = item["result"]["metadata"];
            const auto id = metadata.value("refused_message_id", "");
            if (!id.empty()) metadata["manually_approved"] = manualStates.value(id, false);
        }
        HistoryChunks chunks(request.requestId, source->getInfo().id, index, record.dump());
        while (auto chunk = chunks.next()) {
            if (closing) co_return;
            // Same serialized writer as normal replies, without runtime info inflating bounded frames.
            co_await sendHistoryFrame(std::move(*chunk));
            co_await asio::post(io, asio::use_awaitable);
            if (pingDue && !closing) co_await sendReply(makePing());
        }
    }
    co_await sendHistoryFrame({{"type", "session.attached"}, {"requestId", request.requestId},
        {"sessionId", source->getInfo().id}, {"messageCount", count}, {"info", runtimeInfo()}});
}

asio::awaitable<void> Integration::sendHistoryFrame(nlohmann::json frame) {
    if (closing) co_return;
    if (frame.dump().size() + 1 > HistoryChunks::MAX_FRAME_BYTES)
        throw std::invalid_argument("History frame exceeds the size limit");
    co_await connection.send(std::move(frame));
}

asio::awaitable<nlohmann::json> Integration::nextReasoningEffort(const IntegrationRequest& request) {
    if (!reasoningEffortsLoader && !providers) throw std::runtime_error("Reasoning levels are unavailable for this model");
    const auto selected = providerConnection.selection();
    const auto model = selected.model;
    const auto provider = selected.provider;
    auto loading = std::async(std::launch::async, [load = reasoningEffortsLoader, connections = providers, model, provider] {
        return connections ? connections->create(provider)->getReasoningEfforts(model) : load(model);
    });
    co_await awaitFuture(loading);
    const auto efforts = loading.get();
    if (closing) co_return nullptr;
    if (efforts.empty()) throw std::runtime_error("Reasoning levels are unavailable for this model");
    const auto current = providerConnection.selection();
    if (current.provider != provider || current.model != model)
        throw std::runtime_error("Model changed while loading reasoning levels; retry");
    auto next = std::find(efforts.begin(), efforts.end(), current.reasoning);
    if (next == efforts.end() || ++next == efforts.end()) next = efforts.begin();
    providerConnection.setReasoning(*next);
    applyCurrentSelection();
    co_return nlohmann::json{{"type", "reasoning.updated"}, {"requestId", request.requestId}};
}

SessionInfo Integration::makeSessionInfo(const IntegrationRequest& request) {
    const auto selected = providerConnection.selection();
    SessionInfo info;
    info.workspace = request.workspace;
    info.model = selected.model;
    info.provider = selected.provider;
    info.reasoning = selected.reasoning;
    info.contextLimit = selected.contextLimit;
    info.mode = permissions->mode();
    if (!info.workspace.is_absolute() || !std::filesystem::is_directory(info.workspace))
        throw std::invalid_argument("Session workspace must be an existing absolute directory");
    if (!permissions->isConfigured()) permissions->configure(info.workspace, permissions->mode());
    if (std::filesystem::weakly_canonical(info.workspace) != permissions->root())
        info.configurationWorkspace = permissions->root();
    if (info.model.empty()) throw std::invalid_argument("Session requires a model");
    if (info.reasoning.empty()) throw std::invalid_argument("Session requires a reasoning effort");
    return info;
}

bool Integration::consumePreparationCancellation() {
    const bool cancelled = preparingCancellation && preparingCancellation->load();
    preparingCancellation.reset();
    return cancelled;
}

asio::awaitable<std::optional<std::string>> Integration::generateTitle(const SessionInfo& info, const std::string& text) {
    if (!preparingCancellation) preparingCancellation = std::make_shared<std::atomic<bool>>(false);
    auto title = std::async(std::launch::async,
        [generate = titleGenerator, connections = providers, provider = info.provider, model = info.model,
         text, cancel = preparingCancellation] {
            core::CancellationCheck check = [cancel] { return cancel->load(); };
            core::checkCancellation(check);
            if (generate) return generate(model, text, check);
            if (!connections) throw std::runtime_error("No title provider is configured");
            auto client = connections->create(provider);
            client->setCancellationCheck(check);
            return generateSessionTitle(*client, model, text);
        });
    co_await awaitFuture(title);
    std::string result;
    std::exception_ptr error;
    try { result = title.get(); } catch (...) { error = std::current_exception(); }
    const bool cancelled = consumePreparationCancellation();
    if (closing || cancelled) co_return std::nullopt;
    if (error) std::rethrow_exception(error);
    co_return result;
}

asio::awaitable<bool> Integration::createSession(SessionInfo info, const std::string& text) {
    const auto title = co_await generateTitle(info, text);
    if (!title) co_return false;
    info.title = *title;
    session = registry.create(std::move(info));
    session->recordTerminalOpen(terminalSessionId);
    subscribe();
    co_return true;
}

nlohmann::json Integration::cancelledReply(const IntegrationRequest& request, const SessionInfo& info,
    std::chrono::steady_clock::time_point startedAt) const {
    nlohmann::json reply{{"type", "execution.cancelled"}, {"requestId", request.requestId},
        {"messages", nlohmann::json::array()}, {"error", "Execution cancelled"}};
    if (session) reply["sessionId"] = info.id;
    else {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt).count();
        reply["metadata"] = GenerationMetadata{info.model, info.reasoning, static_cast<std::uint64_t>(elapsed)};
    }
    return reply;
}

asio::awaitable<void> Integration::checkImageInput(bool hasNewImages) {
    bool needed = hasNewImages;
    if (session && (session->agentReady() || session->hasStoredImages())) {
        if (session->isExecuting()) needed = needed || session->hasActiveImages();
        else {
            if (!session->agentReady()) {
                if (!preparingCancellation) preparingCancellation = std::make_shared<std::atomic<bool>>(false);
                if (!co_await session->prepareAgent([cancel = preparingCancellation] { return cancel->load(); }))
                    throw core::Cancelled();
            }
            needed = needed || session->hasActiveImages();
        }
    }
    if (!needed) co_return;
    if (!providers) throw ImageInputError(ImageSupport::UNKNOWN);
    requireImageSupport(co_await loadImageSupport());
}

asio::awaitable<ImageSupport> Integration::loadImageSupport() {
    const auto selected = providerConnection.selection();
    if (!preparingCancellation) preparingCancellation = std::make_shared<std::atomic<bool>>(false);
    auto updates = std::make_shared<AsyncQueue<std::optional<GenerationRetryStatus>>>(io.get_executor());
    const auto publish = [updates, executor = io.get_executor()](std::optional<GenerationRetryStatus> status) {
        asio::post(executor, [updates, status = std::move(status)] { updates->push(status); });
    };
    auto job = std::async(std::launch::async, [providers = providers, selected, cancel = preparingCancellation, publish] {
        try {
            auto provider = providers->create(selected.provider);
            provider->setCancellationCheck([cancel] { return cancel->load(); });
            ImageSupport support = ImageSupport::UNKNOWN;
            runWithRetry([&] {
                support = provider->getModelInfo(selected.model).imageInput;
            }, [cancel] { return cancel->load(); }, [publish](const auto& status) { publish(status); });
            publish(std::nullopt);
            return support;
        } catch (...) {
            publish(std::nullopt);
            throw;
        }
    });
    while (const auto status = co_await updates->receive())
        co_await sendReply({{"type", "execution.retry"}, {"retry", {{"attempt", status->attempt},
            {"limit", 10}, {"seconds", status->seconds}, {"reason", status->reason}}}});
    co_await awaitFuture(job);
    const auto support = job.get();
    if (closing || preparingCancellation->load()) throw core::Cancelled();
    preparingCancellation.reset();
    co_return support;
}

std::vector<Session::LoadedSkill> Integration::loadSkills(const IntegrationRequest& request) const {
    if (request.skills.empty()) return {};
    const auto workspace = session ? session->getInfo().configurationDirectory() :
        permissions->isConfigured() ? permissions->root() : std::filesystem::path(request.workspace);
    if (!workspace.is_absolute() || !std::filesystem::is_directory(workspace))
        throw std::invalid_argument("Session workspace must be an existing absolute directory");
    const auto root = permissions->isConfigured() ? permissions->root() : std::filesystem::canonical(workspace);
    const auto relative = std::filesystem::weakly_canonical(workspace).lexically_relative(root);
    if ((!session || session->getInfo().configurationWorkspace.empty()) && (relative.empty() || *relative.begin() == ".."))
        throw std::invalid_argument("Skill is outside the integration workspace");
    const SkillCatalog catalog(workspace);
    std::vector<Session::LoadedSkill> loaded;
    for (const auto& invocation : request.skills)
        loaded.push_back({invocation, catalog.load(invocation), catalog.displayArguments(invocation)});
    return loaded;
}

asio::awaitable<nlohmann::json> Integration::sendMessage(const IntegrationRequest& request) {
    if (request.text.find_first_not_of(" \r\n\t") == std::string::npos && request.images.empty()) throw std::invalid_argument("Message must not be empty");
    requireIdleSession();
    if (viewingArchive) throw std::logic_error("Archived Implement is read-only");
    if (session && !session->getInfo().discussionId.empty() && ensureImplementationRun().exists())
        throw std::logic_error("Use the read-only review chat after implementation starts");
    if (request.implementationRequest && (!session || session->getInfo().discussionId.empty()))
        throw std::logic_error("Open Implement before requesting its plan");
    if (session && !session->getInfo().discussionId.empty() &&
        (request.saneRequest || (request.messageMode && *request.messageMode != ExecutionMode::SANE)))
        throw std::logic_error("Implement accepts ordinary Sane messages only");
    auto loadedSkills = loadSkills(request);
    if (!request.model.empty()) providerConnection.setModel(request.model);
    if (request.reasoning) providerConnection.setReasoning(*request.reasoning);
    co_await checkImageInput(!request.images.empty());
    if (providers) providers->requireAuth(providerConnection.selection().provider);
    if (session && session->getInfo().discussionId.empty() && !session->getInfo().activeImplementationId.empty()) {
        const auto branch = implementation && implementation->getInfo().id == session->getInfo().activeImplementationId ? implementation :
            co_await registry.loadAsync(session->getInfo().activeImplementationId, std::nullopt);
        registry.freezeImplementation(*session, *branch);
        implementation.reset();
    }
    if (request.messageMode) selectedMode = *request.messageMode;
    const auto startedAt = std::chrono::steady_clock::now();
    const bool created = !session;
    if (created) {
        const auto info = makeSessionInfo(request);
        if (!co_await createSession(info, request.text.empty() ? "Discuss attached images" : request.text)) {
            if (closing) co_return nullptr;
            co_return cancelledReply(request, info, startedAt);
        }
        providerConnection.restoreSelection(ProviderSelection{info.provider, info.model, info.reasoning, info.contextLimit});
    }
    if (!session->agentReady()) {
        if (!preparingCancellation) preparingCancellation = std::make_shared<std::atomic<bool>>(false);
        applyCurrentSelection();
        co_await sendReply({{"type", "execution.preparing"}, {"requestId", request.requestId}, {"sessionId", session->getInfo().id}});
        const auto prepared = co_await session->prepareAgent([cancel = preparingCancellation] { return cancel->load(); });
        if (closing) { consumePreparationCancellation(); co_return nullptr; }
        if (!prepared) {
            consumePreparationCancellation();
            co_return cancelledReply(request, session->getInfo(), startedAt);
        }
        if (session->hasActiveImages()) co_await checkImageInput(false);
    }
    if (!session->isExecuting() && consumePreparationCancellation())
        co_return cancelledReply(request, session->getInfo(), startedAt);
    applyCurrentSelection();
    if (providers) permissions->setPermissionMode(providers->permissionMode(),
        providers->permissionVerifier(permissions->root().string(), request.text), providers->permissionConfidence());
    permissions->setManualActions(session->manualActions());
    preparingCancellation = std::make_shared<std::atomic<bool>>(false);
    co_await session->prepareTurn();
    if (closing) { consumePreparationCancellation(); co_return nullptr; }
    if (consumePreparationCancellation()) co_return cancelledReply(request, session->getInfo(), startedAt);
    permissions->setMode(selectedMode.value_or(permissions->mode()));
    session->sendMessage(request.text, permissions->mode(), startedAt, request.images, std::move(loadedSkills),
        request.saneRequest, request.implementationRequest);
    co_return nlohmann::json{{"type", created ? "session.created" : "message.accepted"},
        {"requestId", request.requestId}, {"session", session->getInfo()}};
}

void Integration::startReader() {
    auto self = shared_from_this();
    asio::co_spawn(io, readRequests(), [self](std::exception_ptr error) {
        if (error && !self->closing) {
            try { std::rethrow_exception(error); }
            catch (const std::exception& exception) { std::cerr << "[sosd] Client read error: " << exception.what() << '\n'; }
        }
        self->close();
    });
}

asio::awaitable<nlohmann::json> Integration::compactSession(const IntegrationRequest& request) {
    requireIdleSession();
    if (!session) throw std::logic_error("Start a conversation before compacting");
    if (!providers && !summaryProvider) throw std::runtime_error("Compaction provider is unavailable");
    applyCurrentSelection();
    const auto info = session->getInfo();
    co_await sendReply({{"type", "compaction.started"}, {"requestId", request.requestId}, {"sessionId", info.id}});
    if (!co_await session->prepareAgent([cancel = summaryCancellation] { return cancel->load(); })) {
        summaryCancellation.reset();
        co_return nlohmann::json{{"type", "compaction.cancelled"}, {"requestId", request.requestId}, {"sessionId", info.id}};
    }
    auto result = co_await generateCompaction(info, request.requestId);
    if (!result)
        co_return nlohmann::json{{"type", "compaction.cancelled"}, {"requestId", request.requestId}, {"sessionId", info.id}};
    const auto summary = result->summary;
    session->applyCompaction(std::move(*result));
    co_return nlohmann::json{{"type", "compaction.completed"}, {"requestId", request.requestId}, {"sessionId", info.id},
        {"messages", std::vector<Message>{summary}}};
}

asio::awaitable<std::optional<CompactionResult>> Integration::generateCompaction(const SessionInfo& info, const std::string& requestId) {
    const auto summaryId = "compaction_" + info.id + "_" + std::to_string(session->messageCount() + 1);
    auto conversation = session->activeConversation();
    auto updates = std::make_shared<AsyncQueue<std::optional<std::size_t>>>(io.get_executor());
    const auto publish = [updates, executor = io.get_executor()](std::optional<std::size_t> attempt) {
        asio::post(executor, [updates, attempt] { updates->push(attempt); });
    };
    auto job = std::async(std::launch::async,
        [conversation = std::move(conversation), info, summaryId, cancel = summaryCancellation,
         factory = summaryProvider, connections = providers, limitOverride = providerConnection.selection().contextLimit, publish]() mutable {
            try {
                core::CancellationCheck check = [cancel] { return cancel->load(); };
                core::checkCancellation(check);
                auto provider = factory ? factory(info) : connections->create(info.provider);
                if (!provider) throw std::runtime_error("Compaction requires a provider");
                provider->setCancellationCheck(check);
                const auto limit = limitOverride ? limitOverride : provider->getContextLimit(info.model);
                auto result = CompactionService{}.compact(conversation, *provider, info, summaryId, check,
                    CompactionService::recentBudget(limit), false, [publish](std::size_t attempt) { publish(attempt); });
                publish(std::nullopt);
                return result;
            } catch (...) {
                publish(std::nullopt);
                throw;
            }
        });
    while (const auto attempt = co_await updates->receive())
        co_await sendReply({{"type", "compaction.progress"}, {"attempt", *attempt}, {"limit", CompactionService::MAX_SUMMARY_ATTEMPTS}, {"sessionId", info.id}, {"requestId", requestId}});
    co_await awaitFuture(job);
    std::optional<CompactionResult> result;
    std::exception_ptr failure;
    try { result.emplace(job.get()); } catch (...) { failure = std::current_exception(); }
    const bool cancelled = summaryCancellation->load();
    summaryCancellation.reset();
    if (closing || cancelled) co_return std::nullopt;
    if (failure) std::rethrow_exception(failure);
    co_return std::move(result);
}

asio::awaitable<nlohmann::json> Integration::handleRequest(const IntegrationRequest& request) {
    try { co_return co_await handle(request); }
    catch (const GenerationRetriesExhausted&) {
        preparingCancellation.reset();
        co_return nlohmann::json{{"type", "request.failed"}, {"requestId", request.requestId},
            {"code", "catalog_unavailable"},
            {"error", "Could not load the model catalog after 10 retries. Your message was not sent. Please try again."}};
    }
    catch (const ImageInputError& error) {
        preparingCancellation.reset();
        co_return nlohmann::json{{"type", "request.failed"}, {"requestId", request.requestId},
            {"code", "image_input"}, {"error", error.what()}};
    }
    catch (const std::exception& error) {
        if (request.type == "message.send" || request.type == "message.check") preparingCancellation.reset();
        if (request.type == "session.attach" || request.type == "session.preview") historyCancellation.reset();
        if ((request.type == "session.compact" || request.type == "session.recap") && !request.blockedBySummary)
            summaryCancellation.reset();
        co_return nlohmann::json{{"type", "request.failed"}, {"requestId", request.requestId}, {"error", error.what()}};
    }
}

asio::awaitable<nlohmann::json> Integration::recordAgentEvent(const AgentEvent& event) {
    nlohmann::json reply;
    try {
        session->record(event);
        if (implementationExecuting && implementationRun) {
            auto activity = event.messages;
            if (event.type == AgentEventType::COMPACTION_STARTED || event.type == AgentEventType::COMPACTION_FAILED ||
                event.type == AgentEventType::COMPACTION_CANCELLED) {
                const auto status = event.type == AgentEventType::COMPACTION_STARTED ? "started" :
                    event.type == AgentEventType::COMPACTION_CANCELLED ? "cancelled" : "failed";
                activity.push_back(Message{"compaction-status-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()),
                    MessageRole::SYSTEM, {TextContent{std::string(status) + (event.error.empty() ? "" : "\n" + event.error)}}});
            }
            implementationRun->recordActivity(activity);
            if (!activity.empty()) co_await sendReply({{"type", "implementation.updated"},
                {"sessionId", session->getInfo().id}, {"implementation", implementationRun->view()}});
        }
        reply = {{"type", eventName(event.type)}, {"sessionId", session->getInfo().id},
            {"messages", event.messages}, {"error", event.error}};
        for (auto& message : reply["messages"]) for (auto& part : message["content"]) {
            if (part.value("type", "") != "tool_call") continue;
            if (const auto script = SessionHistoryView::skillScript(part.at("toolName"), part.at("arguments")))
                part.update({{"skill_script", script->name}, {"skill_script_path", script->path}});
        }
        for (auto& message : reply["messages"])
            if (message.contains("images")) for (auto& image : message["images"]) image.erase("data");
        if (!event.errorCode.empty()) reply["code"] = event.errorCode;
        if (event.retry) reply["retry"] = {{"attempt", event.retry->attempt}, {"limit", 10},
            {"seconds", event.retry->seconds}, {"reason", event.retry->reason}, {"error", event.retry->error}};
        if (event.metadata) reply["metadata"] = *event.metadata;
        if (event.type == AgentEventType::COMPACTION_PROGRESS) {
            reply["attempt"] = event.compactionAttempt;
            reply["limit"] = CompactionService::MAX_SUMMARY_ATTEMPTS;
        }
        if (event.type == AgentEventType::COMPACTION_STARTED || event.type == AgentEventType::COMPACTION_COMPLETED ||
            event.type == AgentEventType::COMPACTION_FAILED || event.type == AgentEventType::COMPACTION_CANCELLED ||
            event.type == AgentEventType::COMPACTION_PROGRESS)
            reply["automatic"] = true;
    } catch (const std::exception& error) {
        std::cerr << "[sosd] Session save failed: " << error.what() << '\n';
        close();
    }
    if (event.isTerminal()) {
        if (event.type == AgentEventType::EXECUTION_COMPLETED)
            if (auto plan = session->latestImplementationPlan()) reply["messages"].push_back(*plan);
        co_await session->finishExecution();
        co_await finishImplementationStep(event);
        co_await finishAutoPlan(event);
        if (selectedMode) permissions->setMode(*selectedMode);
    }
    co_return reply;
}

asio::awaitable<nlohmann::json> Integration::dispatch(Event& event) {
    if (const auto* workspace = std::get_if<WorkspaceRequest>(&event)) co_return co_await handleWorkspace(*workspace);
    if (std::holds_alternative<ImplementationNext>(event)) co_return co_await startImplementationStep();
    if (const auto* btw = std::get_if<BtwEvent>(&event)) co_return co_await recordBtwEvent(*btw);
    // Agent events must still be drained and persisted after disconnect.
    if (const auto* agent = std::get_if<AgentEvent>(&event)) co_return co_await recordAgentEvent(*agent);
    if (closing) co_return nullptr;
    if (auto* restored = std::get_if<McpRestored>(&event)) {
        mcpRestoring = false;
        if (!restored->hasServers && restored->errors.empty()) co_return nullptr;
        co_return nlohmann::json{{"type", "mcp.restored"}, {"errors", restored->errors}};
    }
    if (std::holds_alternative<Ping>(event)) co_return pingDue ? makePing() : nlohmann::json(nullptr);
    if (const auto* request = std::get_if<IntegrationRequest>(&event)) co_return co_await handleRequest(*request);
    if (const auto* permission = std::get_if<PermissionEvent>(&event)) {
        if (implementationExecuting && permission->type == PermissionEventType::RESOLVED && permission->decision == PermissionDecision::NO) {
            implementationStopping = true;
            session->cancel();
        }
        nlohmann::json reply = *permission;
        if (session) reply["sessionId"] = session->getInfo().id;
        co_return reply;
    }
    if (const auto* question = std::get_if<QuestionEvent>(&event)) {
        if (!session || !session->isExecuting()) co_return nullptr;
        co_return nlohmann::json{{"type", "question.requested"}, {"questionId", question->id},
            {"question", question->question}, {"options", question->options}, {"sessionId", session->getInfo().id}};
    }
    co_return nullptr;
}

asio::awaitable<void> Integration::sendReply(nlohmann::json reply) {
    if (reply.is_null() || closing) co_return;
    reply["info"] = runtimeInfo();
    try { co_await connection.send(std::move(reply)); }
    catch (...) { close(); }
}

asio::awaitable<void> Integration::run() {
    startReader();
    co_await sendReply({{"type", "daemon.ready"}, {"protocolVersion", daemon_config::PROTOCOL_VERSION}});
    schedulePing();
    while (!closing || (session && session->isExecuting()) || !btwRuns.empty()) {
        auto event = co_await events->receive();
        co_await sendReply(co_await dispatch(event));
    }
    saveSession();
    detachSession();
    registry.unwatchWorkspace(this);
    co_await providerConnection.drain();
    if (mcpOAuth) {
        mcpOAuth->cancelled->store(true);
        co_await awaitFuture(mcpOAuth->completion);
        mcpOAuth.reset();
    }
}
