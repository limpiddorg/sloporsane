#include "integration.hpp"
#include "../session/implementation_changes.hpp"
#include "../session/implementation_worktree.hpp"
#include "../snapshots/git_snapshot.hpp"
#include "../transport/await_future.hpp"
#include <asio/post.hpp>

asio::awaitable<void> Integration::prepareImplementationCheckout() {
    auto job = std::async(std::launch::async, [directory = session->storageDirectory(), info = session->getInfo(), authorized = executionPermissionRoot()] {
        const auto root = GitSnapshot::discover(info.workspace).value_or(std::filesystem::canonical(info.workspace));
        const auto relative = root.lexically_relative(std::filesystem::canonical(authorized));
        if (relative.empty() || *relative.begin() == "..")
            throw std::runtime_error("Implementation source is outside the authorized workspace. Start SOS at the repository root or source directory.");
        return ImplementationWorktree(directory).start(info.workspace, info.id);
    });
    co_await awaitFuture(job);
    session->setExecutionWorkspace(job.get());
    subscribe();
}

asio::awaitable<void> Integration::importImplementationStep(std::size_t step, const TurnSnapshot& boundary) {
    auto job = std::async(std::launch::async, [directory = session->storageDirectory(), step, boundary] {
        ImplementationWorktree checkout(directory);
        if (checkout.exists()) { checkout.seal(step, boundary); checkout.import(step, boundary); }
    });
    co_await awaitFuture(job);
    job.get(); // Exceptions become request.failed and the existing Slop notice.
}

std::filesystem::path Integration::executionPermissionRoot() const {
    const auto& info = session->getInfo();
    if (!info.executionWorkspace.empty()) return ImplementationWorktree(session->storageDirectory()).executionRoot();
    return info.configurationWorkspace.empty() ? permissions->root() : info.workspace;
}

ImplementationRun& Integration::ensureImplementationRun() {
    if (!session || session->getInfo().discussionId.empty()) throw std::logic_error("Open Implement first");
    const auto id = session->getInfo().id;
    if (!implementationRun || runSessionId != id) {
        implementationRun = std::make_unique<ImplementationRun>(session->storageDirectory());
        runSessionId = id;
        if (implementationRun->status == "running" && !session->isExecuting())
            implementationRun->recover(session->latestSnapshot());
    }
    return *implementationRun;
}

asio::awaitable<nlohmann::json> Integration::handleImplementation(const IntegrationRequest& request) {
    if (!session || request.sessionId != session->getInfo().id) throw std::logic_error("Implementation session is not attached");
    auto& run = ensureImplementationRun();
    if (request.type == "implementation.directory") {
        // Snapshot queries are read-only and remain available in frozen archives.
        const auto snapshot = run.repositoryBaseline(request.stepIndex, request.repositoryTree);
        auto job = std::async(std::launch::async, [snapshot, path = request.repositoryPath] {
            auto entries = nlohmann::json::array();
            for (const auto& entry : GitSnapshot::archive(snapshot.root).children({snapshot.before}, path))
                entries.push_back({{"name", entry.name}, {"directory", entry.directory}});
            return entries;
        });
        co_await awaitFuture(job);
        co_return nlohmann::json{{"type", "implementation.directory"}, {"requestId", request.requestId},
            {"sessionId", request.sessionId}, {"tree", request.repositoryTree}, {"path", request.repositoryPath}, {"entries", job.get()}};
    }
    if (request.type == "implementation.history") {
        auto response = nlohmann::json{{"type", "implementation.history"}, {"requestId", request.requestId},
            {"sessionId", request.sessionId}, {"step", request.stepIndex}, {"versions", run.history(request.historyAll ? std::nullopt : std::optional(request.stepIndex))}};
        if (request.historyAll) response["implementations"] = run.versions();
        co_return response;
    }
    if (request.type == "implementation.state") {
        const auto currentReview = run.reviewIndex() < run.stepCount() ? run.stepReviewBranch(run.reviewIndex()) : "";
        if (!currentReview.empty()) {
            IntegrationRequest history;
            history.type = "btw.history"; history.sessionId = request.sessionId; history.branchId = currentReview;
            co_await handleBtw(history);
        }
        if (run.planningRevision() && !run.revisionBranch.empty()) {
            IntegrationRequest history;
            history.type = "btw.history"; history.sessionId = request.sessionId; history.branchId = run.revisionBranch;
            co_await handleBtw(history);
        }
    } else {
        if (viewingArchive || session->getInfo().frozen) throw std::logic_error("Archived implementation is read-only");
        if (request.type == "implementation.start") {
            requireIdleSession();
            acceptImplementationPlan(request.implementationPace);
        } else if (request.type == "implementation.auto_plan") {
            co_return co_await autoPlan(request);
        } else if (request.type == "implementation.rework") {
            co_await acceptImplementationRevision(request);
        } else if (request.type == "implementation.revision.open") {
            if (session->isExecuting()) { implementationStopping = true; co_await stopExecution(); }
            run.openRevisionPlan(request.revision);
        } else if (request.type == "implementation.revision.review") {
            auto planning = request;
            planning.branchId = run.revisionBranch; planning.stepIndex = run.reviewIndex();
            run.revisionPlanContext();
            const auto branch = startBtw(planning);
            run.setRevisionBranch(branch->branch.id);
            co_await sendReply({{"type", "implementation.revision.review.accepted"}, {"requestId", request.requestId}, {"sessionId", request.sessionId}});
            co_await sendBtw(branch->branch, request.sessionId);
        } else if (request.type == "implementation.rework.reject") {
            if (!run.revisionBranch.empty() && btwRuns.contains(request.sessionId + "/" + run.revisionBranch))
                throw std::logic_error("Wait for plan discussion to finish before dismissing it");
            run.rejectRevision(request.revision);
            implementationStopping = false;
            if (!session->isExecuting() && run.next()) events->push(ImplementationNext{});
        } else if (request.type == "implementation.resume") {
            requireIdleSession(); run.resume(); implementationStopping = false;
            events->push(ImplementationNext{});
        } else if (request.type == "implementation.validate" || request.type == "implementation.unvalidate") {
            if ((request.revisionProvided || run.revisionVersion() != 0) &&
                (!request.revisionProvided || request.revision != run.revisionVersion()))
                throw std::logic_error("The reviewed result changed; inspect its current version before validating");
            if (request.type == "implementation.validate") {
                if (!run.stepReviewBranch(request.stepIndex).empty() && btwRuns.contains(request.sessionId + "/" + run.stepReviewBranch(request.stepIndex)))
                    throw std::logic_error("Wait for the review response before validating this step");
                const auto boundary = run.importBoundary(request.stepIndex, request.substepIndex);
                if (boundary) co_await importImplementationStep(request.stepIndex, *boundary);
            }
            run.review(request.stepIndex, request.substepIndex, request.type == "implementation.validate");
            // Publish the confirmed import before queuing any next-step preparation.
            co_await sendReply({{"type", "implementation.updated"}, {"requestId", request.requestId},
                {"sessionId", request.sessionId}, {"implementation", run.view()}});
            if (!session->isExecuting() && run.next()) events->push(ImplementationNext{});
            co_return nullptr;
        } else if (request.type == "implementation.review") {
            auto review = request;
            if (request.stepIndex != run.reviewIndex())
                throw std::logic_error("Review is only available for the first unvalidated step.");
            review.branchId = run.stepReviewBranch(request.stepIndex);
            run.reviewContext(request.stepIndex, request.substepIndex); // Validate selection before starting a worker.
            const auto branch = startBtw(review);
            run.setReviewBranch(request.stepIndex, branch->branch.id);
            co_await sendReply({{"type", "implementation.review.accepted"}, {"requestId", request.requestId},
                {"sessionId", request.sessionId}, {"branchId", branch->branch.id}});
            co_await sendBtw(branch->branch, request.sessionId);
        } else if (request.type == "implementation.review.cancel") {
            const auto key = request.sessionId + "/" + run.reviewBranch;
            if (const auto it = btwRuns.find(key); it != btwRuns.end()) it->second->agent->cancel();
            const auto planning = request.sessionId + "/" + run.revisionBranch;
            if (const auto it = btwRuns.find(planning); it != btwRuns.end()) it->second->agent->cancel();
        }
    }
    co_return nlohmann::json{{"type", "implementation.updated"}, {"requestId", request.requestId},
        {"sessionId", request.sessionId}, {"implementation", run.view()}};
}

void Integration::acceptImplementationPlan(const std::string& pace) {
    auto& run = ensureImplementationRun();
    const auto plan = session->latestImplementationPlan();
    if (!plan) throw std::logic_error("Finalize an implementation plan before starting");
    if (run.exists()) throw std::logic_error("Implementation has already started; resume it instead");
    if (pace != "step_by_step" && pace != "everything") throw std::invalid_argument("Invalid implementation pace");
    run.start(*plan->implementationPlan, pace);
    if (!session->getInfo().autoPlanPace.empty()) session->setAutoPlan("");
    implementationStopping = false;
    events->push(ImplementationNext{});
}

asio::awaitable<nlohmann::json> Integration::autoPlan(const IntegrationRequest& request) {
    requireIdleSession();
    if (request.implementationPace != "step_by_step" && request.implementationPace != "everything")
        throw std::invalid_argument("Invalid implementation pace");
    if (!btwRuns.empty()) throw std::logic_error("Wait for BTW conversations before starting auto plan");
    if (ensureImplementationRun().exists()) throw std::logic_error("Implementation has already started; resume it instead");
    session->setAutoPlan(request.implementationPace);
    implementationStopping = false;
    if (session->implementationPlanReady()) {
        acceptImplementationPlan(request.implementationPace);
        co_return nlohmann::json{{"type", "implementation.updated"}, {"requestId", request.requestId},
            {"sessionId", request.sessionId}, {"implementation", ensureImplementationRun().view()}};
    }
    auto planning = request;
    planning.type = "message.send";
    planning.text = "Implementation plan request";
    planning.implementationRequest = true;
    planning.messageMode = ExecutionMode::SANE;
    planning.model.clear();
    std::exception_ptr failure;
    nlohmann::json result;
    try { result = co_await sendMessage(planning); } catch (...) { failure = std::current_exception(); }
    if (failure) {
        session->setAutoPlan(request.implementationPace, "Auto plan could not start. Retry when ready.");
        std::rethrow_exception(failure);
    }
    co_return result;
}

asio::awaitable<void> Integration::finishAutoPlan(const AgentEvent& event) {
    const auto pace = session->getInfo().autoPlanPace;
    if (pace.empty() || ensureImplementationRun().exists()) co_return;
    try {
        if (event.type == AgentEventType::EXECUTION_COMPLETED && session->implementationPlanReady() && !closing && !implementationStopping) {
            acceptImplementationPlan(pace);
            co_await sendReply({{"type", "implementation.updated"}, {"sessionId", session->getInfo().id},
                {"implementation", ensureImplementationRun().view()}});
            co_return;
        }
        session->setAutoPlan(pace, event.error.empty() ? "Auto plan interrupted or no finalized plan was produced. Retry when ready." : event.error);
    } catch (const std::exception& error) { session->setAutoPlan(pace, error.what()); }
}

asio::awaitable<nlohmann::json> Integration::startImplementationStep() {
    if (closing || implementationStopping || !session || session->isExecuting() || !implementationRun ||
        runSessionId != session->getInfo().id || !implementationRun->next() || implementationRun->hasRevisionProposal()) co_return nullptr;
    auto& run = *implementationRun;
    try {
        if (run.needsCheckout()) {
            co_await prepareImplementationCheckout();
            run.checkoutPrepared();
        }
        if (closing || implementationStopping) throw core::Cancelled();
        if (!session->getInfo().executionWorkspace.empty())
            ImplementationWorktree(session->storageDirectory()).executionDirectory();
        co_await seedImplementationRevision();
        applyCurrentSelection();
        if (providers) providers->requireAuth(providerConnection.selection().provider);
        if (!co_await session->prepareAgent([this] { return closing || implementationStopping; })) throw core::Cancelled();
        const auto text = run.instruction() + (session->getInfo().executionWorkspace.empty() ? "" :
            "\nBuild and test only in the isolated execution checkout: " + session->getInfo().executionDirectory().string() +
            "\nThe user's original workspace is not the execution directory. Do not edit it; SOS imports each step only after human validation.");
        if (providers) permissions->setPermissionMode(providers->permissionMode(),
            providers->permissionVerifier(permissions->root().string(), text), providers->permissionConfidence());
        permissions->setMode(ExecutionMode::SANE);
        permissions->setImplementationPhase(ImplementationPhase::EXECUTION);
        permissions->setManualActions(session->manualActions());
        session->setPermissionHandler([policy = permissions](const ToolPermissionRequest& request, const auto& check) {
            const auto answer = policy->ask(request, check);
            if (answer.decision == PermissionDecision::NO) throw core::Cancelled();
            return answer;
        }, executionPermissionRoot());
        co_await session->prepareTurn();
        if (closing || implementationStopping) throw core::Cancelled();
        implementationExecuting = true;
        configureImplementationEvidence(run);
        session->sendMessage(text, ExecutionMode::SANE, std::chrono::steady_clock::now(), {}, {}, false, false,
            ImplementationPhase::EXECUTION);
        run.begin(session->latestSnapshot().value());
    } catch (const std::exception& error) {
        implementationExecuting = false;
        permissions->setImplementationPhase(ImplementationPhase::PLANNING);
        run.stop(error.what());
    }
    co_return nlohmann::json{{"type", "implementation.updated"}, {"sessionId", session->getInfo().id}, {"implementation", run.view()}};
}

void Integration::configureImplementationEvidence(const ImplementationRun& run) {
    const auto directory = session->storageDirectory();
    const auto baseline = run.baseline();
    session->inspectImplementationChanges([directory, baseline] {
        return implementationChangeEvidence(TurnFiles(directory).inspect(baseline));
    });
    const auto step = run.next()->first, count = run.currentSubstepCount();
    const auto generation = run.currentGeneration();
    const auto progressAfter = run.currentProgressBoundary();
    session->captureImplementationProgress([directory, step, count, baseline, generation, progressAfter](const auto& progress, const auto& snapshot) {
        auto combined = snapshot;
        if (baseline) {
            combined.before = baseline->before;
            combined.edits.insert(combined.edits.begin(), baseline->edits.begin(), baseline->edits.end());
        }
        return ImplementationBoundaries(directory).capture(step, count, progress.template get<ImplementationProgress>(), combined, generation, progressAfter,
            progress.value("paths", std::vector<std::string>{}), progressAfter == 0);
    }, [directory, step, generation](bool command) { ImplementationBoundaries(directory).requireActiveSubstep(step, command, generation); });
}

asio::awaitable<void> Integration::finishImplementationStep(const AgentEvent& event) {
    if (!implementationExecuting || !implementationRun) co_return;
    auto& run = *implementationRun;
    implementationExecuting = false;
    permissions->setImplementationPhase(ImplementationPhase::PLANNING);
    subscribe();
    const bool completed = event.type == AgentEventType::EXECUTION_COMPLETED &&
        session->implementationCompletion().has_value() && !implementationStopping;
    try {
        const auto step = run.next()->first;
        const auto snapshot = session->completedSnapshot();
        auto previousConstruction = run.reviewConstruction(step, true);
        const auto whole = run.substepCombined(snapshot);
        auto job = std::async(std::launch::async, [directory = session->storageDirectory(), whole] {
            TurnFiles files(directory);
            return files.diffs(whole);
        });
        co_await awaitFuture(job);
        auto diffs = job.get();
        const auto reason = implementationStopping && run.hasRevisionProposal() ? "Construction paused for the revision proposal" :
            implementationStopping ? "Execution cancelled or tool authorization refused" :
            !event.error.empty() ? event.error : "The model ended without confirming step completion";
        run.finish(snapshot, session->implementationCompletion().value_or(ImplementationCompletion{}), std::move(diffs), completed, reason);
        if (completed) {
            if (previousConstruction) {
                // A correction must not inherit construction of successors that
                // happened before the user returned to this step in Everything.
                for (const auto& message : session->btwStore()->readFrom(snapshot.start))
                    if (!message.compaction && !message.recap && !message.manualApproval)
                        previousConstruction->push_back(message);
            }
            run.saveReviewConstruction(step, previousConstruction ? *previousConstruction : session->reviewSnapshot());
            auto seal = std::async(std::launch::async, [directory = session->storageDirectory(), step, whole] {
                ImplementationWorktree checkout(directory);
                if (checkout.exists()) checkout.seal(step, whole);
            });
            co_await awaitFuture(seal); seal.get();
        }
    } catch (const std::exception& error) { run.stop(std::string("Cannot finalize implementation diff: ") + error.what()); }
    co_await sendReply({{"type", "implementation.updated"}, {"sessionId", session->getInfo().id}, {"implementation", run.view()}});
    if (completed && run.next() && !closing) events->push(ImplementationNext{});
}
