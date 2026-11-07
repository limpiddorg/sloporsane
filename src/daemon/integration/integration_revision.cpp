#include "integration.hpp"
#include "../session/implementation_worktree.hpp"
#include "../transport/await_future.hpp"

asio::awaitable<void> Integration::acceptImplementationRevision(const IntegrationRequest& request) {
    auto& run = ensureImplementationRun();
    if (request.revision != run.revisionVersion() || !run.hasRevisionProposal()) throw std::logic_error("This revision proposal is no longer current");
    if (!run.reviewBranch.empty() && btwRuns.contains(request.sessionId + "/" + run.reviewBranch))
        throw std::logic_error("Wait for review to finish before accepting the revision");
    if (!run.revisionBranch.empty() && btwRuns.contains(request.sessionId + "/" + run.revisionBranch))
        throw std::logic_error("Wait for plan discussion to finish before accepting the plan");
    if (run.view().value("proposed_plan_markdown", "") != "" && !run.planningRevision())
        throw std::logic_error("Open the revised Implementation Plan before accepting it");
    if (session->isExecuting()) {
        implementationStopping = true;
        co_await stopExecution(); // Wait for a verified partial snapshot before any rollback.
    }
    const auto rollback = run.prepareRevision(request.revision);
    const auto first = run.reviewIndex();
    co_await sendReply({{"type", "implementation.updated"}, {"sessionId", request.sessionId}, {"implementation", run.view()}});
    auto job = std::async(std::launch::async, [directory = session->storageDirectory(), first, token = request.revision, rollback] {
        ImplementationWorktree checkout(directory);
        if (!checkout.exists()) throw std::runtime_error("Revision requires the isolated execution checkout");
        checkout.revise(first, token, rollback);
    });
    co_await awaitFuture(job); job.get();
    const auto reviewStart = session->messageCount();
    session->incorporateAcceptedReview(run.reviewBranch, run.view().at("proposed_rework").get<std::string>());
    if (!run.revisionBranch.empty())
        session->incorporateAcceptedReview(run.revisionBranch, run.view().at("proposed_rework").get<std::string>());
    auto construction = run.reviewConstruction(first);
    if (construction) {
        const auto accepted = session->btwStore()->readFrom(reviewStart);
        construction->insert(construction->end(), accepted.begin(), accepted.end());
        run.saveReviewConstruction(first, *construction);
    }
    run.commitRevision(request.revision);
    co_await sendReply({{"type", "implementation.updated"}, {"sessionId", request.sessionId}, {"implementation", run.view()}});
    implementationStopping = false;
    events->push(ImplementationNext{});
}

asio::awaitable<void> Integration::seedImplementationRevision() {
    auto& run = ensureImplementationRun();
    if (!run.needsRevisionSeed()) co_return;
    const auto step = run.next()->first;
    const auto generation = run.currentGeneration();
    const auto source = run.revisionSource();
    // The first step's old result is already present after rollback. A later
    // contribution is replayed onto its newly corrected predecessor.
    const auto preserve = source.state.substeps.front().snapshot &&
        run.baseline().has_value();
    auto job = std::async(std::launch::async, [directory = session->storageDirectory(), step, generation, source, preserve] {
        return ImplementationWorktree(directory).replay(step, generation, source, preserve);
    });
    co_await awaitFuture(job); run.seedRevision(job.get());
}
