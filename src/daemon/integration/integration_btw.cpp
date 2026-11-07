#include "integration.hpp"
#include "../transport/await_future.hpp"
#include "../session/history_chunks.hpp"
#include "../snapshots/git_snapshot.hpp"
#include <agent-runtime/conversation/messages/message_json.hpp>
#include <asio/post.hpp>
#include <algorithm>

namespace {
std::string branchKey(const std::string& session, const std::string& branch) { return session + "/" + branch; }
std::vector<Message> previousReviewSummaries(const std::filesystem::path& directory,
    const std::vector<std::string>& previous, const ProviderSelection& selection, Provider& provider,
    core::CancellationCheck cancelled) {
    std::vector<Message> summaries;
    SessionJournal store(directory);
    for (const auto& id : previous) {
        core::checkCancellation(cancelled);
        auto earlier = store.readBtw(id);
        const auto messages = currentReviewMessages(earlier);
        if (messages.empty()) continue;
        if (earlier.reviewSummary.empty()) {
            earlier.reviewSummary = CompactionService::summarizeReview(messages, provider, selection, cancelled);
            core::checkCancellation(cancelled);
            store.saveBtw(earlier);
        }
        summaries.push_back(Message{"previous-review-" + id, MessageRole::SYSTEM,
            {TextContent{"Summary of a previous validated step review (historical context):\n" + earlier.reviewSummary}}});
    }
    std::size_t bytes = 0;
    for (const auto& message : summaries) bytes += std::get<TextContent>(message.content.front()).text.size();
    if (bytes > std::min<std::uint64_t>(8000, selection.contextLimit.value_or(32768) / 8)) {
        auto source = summaries;
        for (auto& message : source) message.role = MessageRole::USER;
        auto merged = CompactionService::summarizeReview(source, provider, selection, cancelled);
        summaries = {Message{"previous-review-merged", MessageRole::SYSTEM, {TextContent{std::move(merged)}}}};
    }
    return summaries;
}
void retainReviewInstructions(Conversation& compacted, const Conversation& original) {
    const auto retain = [&](const Message& message) {
        if (std::none_of(compacted.getMessages().begin(), compacted.getMessages().end(),
            [&](const auto& retained) { return retained.id == message.id; })) compacted.append(message);
    };
    for (const auto& message : original.getMessages()) if (message.id == "btw-instructions") retain(message);
    for (auto it = original.getMessages().rbegin(); it != original.getMessages().rend(); ++it)
        if (it->id.starts_with("review-selection-")) { retain(*it); break; }
}
Agent::ContextPreparation stepReviewPreparation(const std::filesystem::path& directory,
    std::vector<std::string> previous, ProviderSelection selection, SessionInfo info, AgentEventHandler emit) {
    return [directory, previous = std::move(previous), selection = std::move(selection), info = std::move(info),
            emit = std::move(emit), prepared = false, sequence = std::size_t{0}]
        (const CompactionContext& context, Provider& provider, core::CancellationCheck cancelled) mutable -> std::optional<Conversation> {
        core::checkCancellation(cancelled);
        std::vector<Message> summaries;
        if (!prepared && !previous.empty() && std::none_of(context.conversation.getMessages().begin(),
            context.conversation.getMessages().end(), [](const auto& message) { return message.id.starts_with("previous-review-"); })) {
            summaries = previousReviewSummaries(directory, previous, selection, provider, cancelled);
        }
        std::optional<Conversation> compacted;
        CompactionService service;
        if (service.needCompaction(context, summaries)) {
            info.model = selection.model; info.reasoning = selection.reasoning; info.contextLimit = context.model.contextLimit;
            compacted = Session::compactBeforeGeneration(context, provider, info,
                "compaction_review_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                "_" + std::to_string(++sequence), emit, cancelled);
            retainReviewInstructions(*compacted, context.conversation);
        }
        if (summaries.empty()) {
            if (compacted && service.needCompaction({*compacted, context.model, context.tools}))
                throw std::runtime_error("Review context still exceeds the budget after restoring review instructions");
            prepared = true;
            return compacted;
        }
        Conversation result(ProviderFormat::OPENAI_RESPONSES);
        for (const auto& message : (compacted ? *compacted : context.conversation).getMessages()) result.append(message);
        for (const auto& summary : summaries) result.append(summary);
        if (service.needCompaction({result, context.model, context.tools}))
            throw std::runtime_error("Review context and summaries still exceed the context budget after compaction");
        result.rebuildProviderContext(result.getMessages().size());
        prepared = true;
        return result;
    };
}
void appendReviewSkills(BtwConversation& branch, const std::vector<Session::LoadedSkill>& skills) {
    if (skills.empty()) return;
    auto& user = branch.messages.back();
    std::string instruction;
    for (const auto& skill : skills) {
        if (!instruction.empty()) instruction += "\n\n";
        instruction += "Call the skill " + nlohmann::json(skill.invocation.name).dump() +
            " with input (JSON string data):\n" + nlohmann::json(skill.invocation.input).dump();
    }
    user.providerText = instruction;
    const auto userId = user.id;
    for (std::size_t index = 0; index < skills.size(); ++index) {
        const auto& skill = skills[index];
        const auto id = "skill_" + userId + "_" + std::to_string(index);
        Message call{id, MessageRole::ASSISTANT, {ToolCallContent{id, "skill", nlohmann::json(skill.invocation)}}};
        call.userSkill = skill.invocation.name;
        branch.messages.push_back(std::move(call));
        branch.messages.push_back(Message{id + "_result", MessageRole::TOOL,
            {ToolResultContent{id, skill.content, false, {}, skill.metadata}}});
    }
}
nlohmann::json branchInfo(const BtwConversation& branch) {
    auto result = nlohmann::json{{"id", branch.id}, {"anchor", branch.anchor}, {"pageAnchor", branch.pageAnchor}, {"standalone", branch.standalone}, {"status", branch.status}, {"error", branch.error},
        {"model", branch.model}, {"reasoning", branch.reasoning}, {"provider", branch.provider}, {"review", branch.review}, {"revision_planning", branch.revisionPlanning}};
    if (branch.reviewStep) result["review_step"] = *branch.reviewStep;
    return result;
}
std::vector<nlohmann::json> displayMessages(const BtwConversation& branch) {
    std::vector<nlohmann::json> result;
    for (const auto& message : branch.messages) {
        if (branch.review) {
            if (message.role != MessageRole::SYSTEM || message.id.starts_with("compaction-status-")) result.push_back(nlohmann::json(message));
            continue;
        }
        if (message.role != MessageRole::USER && message.role != MessageRole::ASSISTANT) continue;
        auto content = nlohmann::json::array();
        for (const auto& part : message.content)
            if (const auto* text = std::get_if<TextContent>(&part))
                content.push_back({{"type", "text"}, {"text", text->text}});
        if (!content.empty()) result.push_back({{"id", message.id},
            {"role", message.role == MessageRole::USER ? "user" : "assistant"}, {"content", content},
            {"metadata", message.metadata ? nlohmann::json(*message.metadata) : nlohmann::json(nullptr)}});
    }
    return result;
}
}

asio::awaitable<void> Integration::sendBtw(const BtwConversation& branch, const std::string& sessionId) {
    const auto transfer = "btw-transfer-" + std::to_string(++btwTransfer);
    const auto messages = displayMessages(branch);
    co_await sendReply({{"type", "btw.started"}, {"requestId", transfer}, {"sessionId", sessionId},
        {"session", {{"id", sessionId}}}, {"branch", branchInfo(branch)}, {"messageCount", messages.size()}});
    for (std::size_t i = 0; i < messages.size(); ++i) {
        HistoryChunks chunks(transfer, sessionId, i, messages[i].dump());
        while (auto chunk = chunks.next()) {
            if (closing) co_return;
            (*chunk)["type"] = "btw.chunk";
            try { co_await sendHistoryFrame(std::move(*chunk)); }
            catch (...) { close(); co_return; }
            co_await asio::post(io, asio::use_awaitable);
            if (pingDue && !closing) co_await sendReply(makePing());
        }
    }
    co_await sendReply({{"type", "btw.finished"}, {"requestId", transfer}, {"sessionId", sessionId},
        {"messageCount", messages.size()}});
}

asio::awaitable<nlohmann::json> Integration::handleBtw(const IntegrationRequest& request) {
    if (!session) throw std::invalid_argument("Start a principal conversation before using /btw");
    if (request.sessionId != session->getInfo().id) throw std::invalid_argument("Session is not attached");
    const auto sessionId = session->getInfo().id;
    auto store = session->btwStore();
    if (request.type == "btw.list" || request.type == "btw.history") {
        auto branches = request.type == "btw.list" ? store->listBtw() : std::vector{store->readBtw(request.branchId)};
        for (auto& branch : branches) {
            if (branch.status == "running" && !btwRuns.contains(branchKey(sessionId, branch.id))) {
                branch.status = "interrupted";
                branch.error = "BTW interrupted when the application closed. Continue with /btw " + branch.id + " message.";
                store->saveBtw(branch);
            }
            co_await sendBtw(branch, sessionId);
        }
        co_return nlohmann::json{{"type", "btw.listed"}, {"requestId", request.requestId}, {"sessionId", sessionId}};
    }
    if (request.branchId.empty()) {
        if (session->history().page().last.empty()) throw std::logic_error("Start a principal conversation before using /btw");
        if (!session->agentReady()) {
            btwPreparationCancellation = std::make_shared<std::atomic<bool>>(false);
            bool prepared;
            try { prepared = co_await session->prepareAgent([cancel = btwPreparationCancellation] { return cancel->load(); }); }
            catch (...) { btwPreparationCancellation.reset(); throw; }
            btwPreparationCancellation.reset();
            if (!prepared || closing) co_return nullptr;
        }
    }
    const auto run = startBtw(request);
    co_await sendReply({{"type", "btw.accepted"}, {"requestId", request.requestId}, {"sessionId", sessionId},
        {"branchId", run->branch.id}});
    co_await sendBtw(run->branch, sessionId);
    co_return nullptr;
}

std::shared_ptr<Integration::BtwRun> Integration::startBtw(const IntegrationRequest& request) {
    const auto sessionId = session->getInfo().id;
    auto store = session->btwStore();
    if (request.text.find_first_not_of(" \r\n\t") == std::string::npos) throw std::invalid_argument("BTW message must not be empty");
    if (!providers && !btwProvider) throw std::runtime_error("Connect a provider before using /btw");
    const auto selection = providerConnection.selection();
    if (providers && !btwProvider) providers->requireAuth(selection.provider);
    BtwConversation branch;
    const bool planning = request.type == "implementation.revision.review";
    const bool review = request.type == "implementation.review" || planning;
    // Load all skills before creating/changing a branch or starting generation.
    const auto skills = loadSkills(request);
    if (request.branchId.empty()) {
        branch.standalone = !session->isExecuting();
        branch.review = review;
        branch.revisionPlanning = planning;
        branch.snapshot = review ? std::vector<Message>{} : session->btwSnapshot();
        if (branch.standalone) {
            branch.anchor = store->historyIndex().lastRecordId();
            branch.pageAnchor = session->history().page().last;
        } else {
            for (auto it = branch.snapshot.rbegin(); it != branch.snapshot.rend(); ++it)
                if (it->role == MessageRole::USER) { branch.anchor = it->id; break; }
            branch.pageAnchor = branch.anchor;
        }
    } else {
        if (btwRuns.contains(branchKey(sessionId, request.branchId)))
            throw std::logic_error("This BTW conversation is still answering. Wait before continuing it.");
        branch = store->readBtw(request.branchId);
        if (branch.review != review) throw std::logic_error("Review and BTW conversations cannot be mixed");
        if (branch.revisionPlanning != planning) throw std::logic_error("Plan discussion and step review cannot be mixed");
        branch.messages = freezeBtwContext(std::move(branch.messages));
    }
    if (review) {
        auto& state = ensureImplementationRun();
        if (!planning) branch.reviewStep = request.stepIndex;
        if (planning) branch.snapshot = session->reviewSnapshot();
        else {
            const auto key = state.reviewConstructionKey(request.stepIndex);
            if (branch.contextConstructionKey != key) { branch.activeContext.clear(); branch.contextMessageCount = 0; }
            branch.contextConstructionKey = key;
            auto construction = state.reviewConstruction(request.stepIndex);
            if (!construction) {
                construction = session->reviewSnapshot(state.reviewHistoryEnd(request.stepIndex));
                state.saveReviewConstruction(request.stepIndex, *construction);
            }
            branch.snapshot = std::move(*construction);
        }
        branch.messages.push_back(Message{"review-selection-" + std::to_string(branch.messages.size()), MessageRole::SYSTEM,
            {TextContent{planning ? state.revisionPlanContext() : state.reviewContext(request.stepIndex, request.substepIndex, request.reviewPath, request.reviewHunk)}}});
    }
    branch.model = selection.model;
    branch.reasoning = selection.reasoning;
    branch.provider = selection.provider;
    branch.status = "running";
    branch.reviewSummary.clear();
    branch.error.clear();
    branch.messages.push_back(Message{"btw-user-" + std::to_string(branch.messages.size()), MessageRole::USER,
        {TextContent{request.text}}});
    appendReviewSkills(branch, skills);
    // A separate reader connection avoids sharing the principal's SQLite connection with a worker.
    auto imageReader = [directory = session->storageDirectory(), snapshot = branch.snapshot, history = branch.messages](const std::string& id) {
        for (const auto* messages : {&snapshot, &history})
            for (const auto& message : *messages)
                for (const auto& image : message.images) if (image.id == id) return image;
        return SessionJournal(directory).readImage(id);
    };
    auto run = std::make_shared<BtwRun>();
    auto workspace = session->getInfo().executionDirectory();
    auto authorized = executionPermissionRoot();
    std::vector<std::filesystem::path> historicalRoots;
    std::optional<TurnSnapshot> historical;
    std::function<void(const std::function<void()>&)> prepareFiles;
    if (review && !planning) {
        historical = ensureImplementationRun().reviewFiles(request.stepIndex);
        run->reviewDirectory = session->storageDirectory() / ("review-files-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const auto relative = workspace.lexically_relative(historical->root);
        if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
            throw std::logic_error("Historical review workspace is outside its preserved root");
        auto sourceRoot = session->getInfo().workspace;
        for (const auto& component : relative) if (component != ".") sourceRoot = sourceRoot.parent_path();
        historicalRoots = {historical->root, sourceRoot, std::filesystem::weakly_canonical(sourceRoot)};
        workspace = run->reviewDirectory / relative;
        authorized = run->reviewDirectory;
        prepareFiles = [snapshot = *historical, destination = run->reviewDirectory, ready = std::make_shared<bool>(false)](const auto& check) {
            if (*ready) return;
            GitSnapshot::archive(snapshot.root).exportTo({snapshot.after}, destination, check);
            *ready = true;
        };
    }
    auto agent = makeBtwAgent(branch, btwProvider ? btwProvider(selection) : providers->create(selection.provider), selection,
        workspace, std::move(imageReader), authorized,
        review && (planning || !ensureImplementationRun().planningRevision()) && ensureImplementationRun().canRework(request.stepIndex), historicalRoots, std::move(prepareFiles));
    if (request.branchId.empty()) branch = store->createBtw(std::move(branch));
    else store->saveBtw(branch);
    run->owner = session;
    run->branch = std::move(branch);
    run->agent = std::move(agent);
    if (review) {
        run->reviewStep = request.stepIndex;
        run->reviewRevision = ensureImplementationRun().revisionVersion();
    }
    const auto key = branchKey(sessionId, run->branch.id);
    btwRuns[key] = run;
    run->agent->setEventHandler([weak = std::weak_ptr(run), executor = io.get_executor(), queue = events](AgentEvent event) {
        if (auto run = weak.lock()) asio::post(executor, [queue, run, event = std::move(event)]() mutable {
            queue->push(BtwEvent{run, std::move(event)});
        });
    });
    if (review) {
        AgentEventHandler emit = [weak = std::weak_ptr(run), executor = io.get_executor(), queue = events](AgentEvent event) {
            if (auto run = weak.lock()) asio::post(executor, [queue, run, event = std::move(event)]() mutable {
                queue->push(BtwEvent{run, std::move(event)});
            });
        };
        run->agent->setContextPreparation(stepReviewPreparation(session->storageDirectory(),
            planning ? std::vector<std::string>{} : ensureImplementationRun().previousReviewBranches(request.stepIndex),
            selection, session->getInfo(), std::move(emit)));
    }
    try { run->execution = run->agent->run(); }
    catch (const std::exception& error) {
        run->branch.status = "failed";
        run->branch.error = error.what();
        store->saveBtw(run->branch);
        btwRuns.erase(key);
    }
    return run;
}

asio::awaitable<nlohmann::json> Integration::recordBtwEvent(const BtwEvent& notification) {
    const auto& run = notification.run;
    const auto& event = notification.event;
    const auto sessionId = run->owner->getInfo().id;
    if (event.retry) co_return nlohmann::json{{"type", "btw.retry"}, {"sessionId", sessionId},
        {"branchId", run->branch.id}, {"retry", {{"attempt", event.retry->attempt}, {"limit", 10},
            {"seconds", event.retry->seconds}, {"reason", event.retry->reason}, {"error", event.retry->error}}}};
    if (event.type == AgentEventType::COMPACTION_STARTED || event.type == AgentEventType::COMPACTION_FAILED ||
        event.type == AgentEventType::COMPACTION_CANCELLED) {
        const auto status = event.type == AgentEventType::COMPACTION_STARTED ? "started" :
            event.type == AgentEventType::COMPACTION_CANCELLED ? "cancelled" : "failed";
        run->branch.messages.push_back(Message{"compaction-status-" + std::to_string(run->branch.messages.size()),
            MessageRole::SYSTEM, {TextContent{std::string(status) + (event.error.empty() ? "" : "\n" + event.error)}}});
    }
    run->branch.messages.insert(run->branch.messages.end(), event.messages.begin(), event.messages.end());
    if (run->branch.review)
        for (const auto& message : event.messages)
            for (const auto& content : message.content)
                if (const auto* result = std::get_if<ToolResultContent>(&content);
                    result && !result->isError && result->metadata.contains("implementation_revision"))
                    run->reworkProposal = result->metadata.at("implementation_revision").get<ImplementationRevisionProposal>();
    if (event.isTerminal()) {
        co_await awaitFuture(run->execution);
        try { run->execution.get(); } catch (...) { /* The event carries the error. */ }
        run->branch.status = event.type == AgentEventType::EXECUTION_COMPLETED ? "completed" :
            event.type == AgentEventType::EXECUTION_CANCELLED ? "interrupted" : "failed";
        run->branch.error = event.error;
        if (run->branch.review) {
            run->branch.activeContext = run->agent->getMessages();
            run->branch.contextMessageCount = run->branch.messages.size();
        }
        btwRuns.erase(branchKey(sessionId, run->branch.id));
    }
    try { run->owner->btwStore()->saveBtw(run->branch); }
    catch (...) {
        if (event.persisted) event.persisted->set_exception(std::current_exception());
        close(); throw;
    }
    if (event.persisted) event.persisted->set_value();
    bool proposed = false;
    if (event.type == AgentEventType::EXECUTION_COMPLETED && run->branch.review && session == run->owner) {
        if (run->reworkProposal) {
            try {
                auto& state = ensureImplementationRun();
                state.proposeRevision(run->reviewStep, run->reviewRevision, *run->reworkProposal);
                proposed = true;
                co_await sendReply({{"type", "implementation.updated"}, {"sessionId", sessionId}, {"implementation", state.view()}});
            } catch (const std::exception& error) {
                run->branch.error = error.what();
                run->owner->btwStore()->saveBtw(run->branch);
            }
        }
    }
    if (!closing) co_await sendBtw(run->branch, sessionId);
    if (proposed && session && session->isExecuting()) {
        implementationStopping = true;
        co_await stopExecution();
    }
    co_return nullptr;
}
