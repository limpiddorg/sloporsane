#include "implementation_run.hpp"
#include "implementation_changes.hpp"
#include <algorithm>
#include <numeric>
#include <set>
#include <limits>

using Json = nlohmann::json;

void to_json(Json& json, const ImplementationRevisionProposal& proposal) {
    json = {{"request", proposal.request}, {"retained_steps", Json::array()}, {"substep_owners", proposal.substepOwners}};
    for (const auto& step : proposal.retainedSteps) json["retained_steps"].push_back(step ? Json(*step) : Json(nullptr));
    if (proposal.plan) json["plan"] = *proposal.plan;
}
void from_json(const Json& json, ImplementationRevisionProposal& proposal) {
    proposal.request = json.at("request").get<std::string>();
    if (json.contains("plan")) proposal.plan = json.at("plan").get<ImplementationPlan>();
    for (const auto& step : json.value("retained_steps", Json::array()))
        proposal.retainedSteps.push_back(step.is_null() ? std::nullopt : std::optional(step.get<std::size_t>()));
    proposal.substepOwners = json.value("substep_owners", std::vector<std::vector<std::size_t>>{});
}

bool ImplementationRun::canRework(std::size_t step) const {
    return plan && step == reviewStep && step < steps.size() &&
        steps[step].whole.complete && !steps[step].whole.reviewed && workStep > step && status != "revising";
}

void ImplementationRun::validateRevision(const ImplementationRevisionProposal& proposal) const {
    if (proposal.request.find_first_not_of(" \t\r\n") == std::string::npos || proposal.request.size() > 8000)
        throw std::invalid_argument("Invalid revision request");
    const auto& candidate = proposal.plan.value_or(*plan);
    if (candidate.steps.size() <= reviewStep || proposal.retainedSteps.size() != candidate.steps.size() ||
        proposal.substepOwners.size() != candidate.steps.size()) throw std::invalid_argument("Map every revised step and its retained substeps");
    std::set<std::size_t> retained;
    for (std::size_t i = 0; i < candidate.steps.size(); ++i) {
        const auto source = proposal.retainedSteps[i];
        if (i <= reviewStep && source != std::optional(i)) throw std::invalid_argument("Preserve the validated prefix and the reviewed step identity");
        if (i < reviewStep && Json(candidate.steps[i]) != Json(plan->steps[i])) throw std::logic_error("Validated plan steps are immutable");
        if (!source) {
            if (!proposal.substepOwners[i].empty()) throw std::invalid_argument("A new step has no retained substeps");
            continue;
        }
        if (*source >= steps.size() || !retained.insert(*source).second || (i > reviewStep && *source <= reviewStep))
            throw std::invalid_argument("Invalid or duplicate retained step");
        const auto& owners = proposal.substepOwners[i];
        if (owners.size() != steps[*source].substeps.size() || std::any_of(owners.begin(), owners.end(),
            [&](auto owner) { return owner >= candidate.steps[i].substeps.size(); })) throw std::invalid_argument("Map each retained contribution to a revised substep");
        if (i < reviewStep) for (std::size_t j = 0; j < owners.size(); ++j)
            if (owners[j] != j) throw std::logic_error("Validated substep ownership is immutable");
    }
}

void ImplementationRun::proposeRework(std::size_t step, std::uint64_t expected, const std::string& request) {
    proposeRevision(step, expected, ImplementationRevisionProposal{request});
}
void ImplementationRun::proposeRevision(std::size_t step, std::uint64_t expected, ImplementationRevisionProposal proposal) {
    if (expected != revision || !canRework(step)) throw std::logic_error("The reviewed result changed; request a new proposal");
    if (revisionPlanning && !proposal.plan) throw std::logic_error("Update the candidate plan in its planning workspace, not through local rework");
    if (!proposal.plan) {
        proposal.retainedSteps.clear(); proposal.substepOwners.clear();
        for (std::size_t i = 0; i < steps.size(); ++i) {
            proposal.retainedSteps.push_back(i);
            std::vector<std::size_t> owners(steps[i].substeps.size()); std::iota(owners.begin(), owners.end(), 0);
            proposal.substepOwners.push_back(std::move(owners));
        }
    }
    validateRevision(proposal);
    proposedRework = proposal.request; proposedRevision = std::move(proposal); ++revision;
    if (status != "running") status = revisionPlanning ? "revision_planning" : "revision_pending";
    save();
}
void ImplementationRun::rejectRevision(std::uint64_t expected) {
    if (expected != revision || !hasRevisionProposal() || status == "revising") throw std::logic_error("This revision proposal is no longer current");
    proposedRework.clear(); proposedRevision.reset(); revisionPlanning = false; revisionBranch.clear(); ++revision;
    if (status == "revision_pending" || status == "revision_planning" || (status == "interrupted" && error == "Construction paused for the revision proposal"))
        status = workStep == steps.size() ? "completed" : pace == "step_by_step" ? "waiting" : "running";
    error.clear();
    save();
}

void ImplementationRun::openRevisionPlan(std::uint64_t expected) {
    if (expected != revision || !proposedRevision || !proposedRevision->plan || status == "revising")
        throw std::logic_error("This plan revision proposal is no longer current");
    if (!revisionPlanning) revisionBranch.clear();
    revisionPlanning = true; status = "revision_planning"; error.clear(); ++revision; save();
}

std::string ImplementationRun::revisionPlanContext() const {
    if (!revisionPlanning || !proposedRevision || !proposedRevision->plan) throw std::logic_error("Open the revision plan before discussing it");
    return reviewContext(reviewStep, steps[reviewStep].substeps.size()) +
        "\nYou are now discussing a candidate revised Implementation Plan, NOT reviewing or executing code. "
        "The user accepted opening this planning workspace, NOT construction. All file access remains read-only. "
        "Discuss questions on the LEFT; publish complete candidates on the RIGHT using propose_implementation_revision. "
        "Publish sufficiently specified requested adjustments in the same turn, without ritual confirmation. "
        "Do not propose local rework or edit files. Execution starts only when the user accepts the plan in the UI. "
        "Keep responses and requests concise. Preserve the validated prefix and retained contribution mappings.\nCurrent candidate JSON:\n" + Json(*proposedRevision).dump();
}

void ImplementationRun::captureRevisionSources() {
    revisionSources.clear();
    for (std::size_t i = reviewStep; i < steps.size(); ++i) {
        auto source = steps[i];
        if (source.substeps.empty() || !source.substeps[0].snapshot) continue;
        const auto snapshot = *source.substeps[0].snapshot;
        if (!snapshot.workspaces.empty()) throw std::logic_error("Cannot revise work spanning multiple execution workspaces");
        if (!source.whole.complete) {
            auto evidence = boundaries.read(i, source.generation);
            if (!evidence.empty()) {
                auto interval = snapshot;
                const auto& last = evidence.back(); interval.before = last.snapshot.after;
                if (snapshot.edits.size() < last.snapshot.edits.size()) throw std::runtime_error("Incomplete revision evidence");
                interval.edits.erase(interval.edits.begin(), interval.edits.begin() + last.snapshot.edits.size());
                auto diffs = TurnFiles(storageDirectory).diffs(interval);
                if (!diffs.empty()) {
                    if (!last.activeSubstep) throw std::runtime_error("Partial code changes have no captured owner");
                    ImplementationBoundary tail; tail.owner = last.activeSubstep; tail.diffs = std::move(diffs); evidence.push_back(std::move(tail));
                }
                const auto owned = preserveImplementationOwnership(evidence, TurnFiles(storageDirectory).diffs(snapshot), source.substeps.size());
                for (std::size_t j = 0; j < owned.size(); ++j) source.substeps[j].diffs = owned[j];
            }
        }
        revisionSources.emplace(i, ImplementationRevisionSource{std::move(source)});
    }
}

TurnSnapshot ImplementationRun::prepareRevision(std::uint64_t expected) {
    if (expected != revision || !hasRevisionProposal()) throw std::logic_error("This revision proposal is no longer current");
    if (status == "revising" && revisionRollback) return *revisionRollback;
    if (!canRework(reviewStep)) throw std::logic_error("Wait for a built, unvalidated step before revising");
    if (!proposedRevision) {
        ImplementationRevisionProposal proposal{proposedRework};
        for (std::size_t i = 0; i < steps.size(); ++i) {
            proposal.retainedSteps.push_back(i); std::vector<std::size_t> owners(steps[i].substeps.size()); std::iota(owners.begin(), owners.end(), 0);
            proposal.substepOwners.push_back(std::move(owners));
        }
        proposedRevision = std::move(proposal);
    }
    validateRevision(*proposedRevision);
    captureRevisionSources();
    const auto& current = *steps[reviewStep].substeps.front().snapshot;
    TurnSnapshot rollback = current; rollback.before = rollback.after = current.after; rollback.edits.clear(); rollback.messages.clear();
    for (std::size_t i = reviewStep + 1; i < steps.size(); ++i) {
        if (steps[i].substeps.empty() || !steps[i].substeps[0].snapshot) continue;
        const auto& next = *steps[i].substeps[0].snapshot;
        if (next.root != rollback.root || next.before != rollback.after || next.after.empty()) throw std::runtime_error("Non-contiguous revision snapshots");
        rollback.after = next.after; rollback.edits.insert(rollback.edits.end(), next.edits.begin(), next.edits.end());
    }
    rollback.complete = true; revisionRollback = rollback; status = "revising"; save();
    return rollback;
}

void ImplementationRun::commitRevision(std::uint64_t expected) {
    if (expected != revision || status != "revising" || !proposedRevision || !revisionRollback) throw std::logic_error("No prepared revision to commit");
    const auto proposal = *proposedRevision;
    revisionArchive.push_back({*plan, steps, proposal.request, revision, planVersions.back().number, reviewStep});
    revisionHistory[reviewStep].push_back(steps[reviewStep]);
    const auto candidate = proposal.plan.value_or(*plan);
    const bool retainCurrent = Json(candidate.steps[reviewStep].substeps) == Json(plan->steps[reviewStep].substeps) &&
        [&] { const auto& owners = proposal.substepOwners[reviewStep]; for (std::size_t i = 0; i < owners.size(); ++i) if (owners[i] != i) return false; return true; }();
    auto originalSources = std::move(revisionSources); revisionSources.clear();
    std::vector<ImplementationStepState> replacement;
    for (std::size_t i = 0; i < candidate.steps.size(); ++i) {
        const auto sourceIndex = proposal.retainedSteps[i];
        if (i < reviewStep) { replacement.push_back(steps[i]); continue; }
        ImplementationStepState state{std::vector<ImplementationItem>(candidate.steps[i].substeps.size()), {}, {}, revision,
            sourceIndex ? steps[*sourceIndex].identity : "step-" + std::to_string(revision) + "-" + std::to_string(i)};
        state.displayRevision = expected;
        if (sourceIndex && i == reviewStep) state.reviewBranch = steps[reviewStep].reviewBranch;
        if (sourceIndex && originalSources.contains(*sourceIndex)) {
            auto source = originalSources.at(*sourceIndex); source.owners = proposal.substepOwners[i];
            state.whole = source.state.whole; state.whole.complete = state.whole.reviewed = false;
            state.validation = source.state.validation; state.validation.complete = state.validation.announced = state.validation.started = false;
            for (std::size_t j = 0; j < source.owners.size(); ++j) {
                auto& target = state.substeps[source.owners[j]]; const auto& old = source.state.substeps[j];
                target.activity.insert(target.activity.end(), old.activity.begin(), old.activity.end());
                if (old.durationMs) target.durationMs = target.durationMs.value_or(0) + *old.durationMs;
                if (old.modelStatistics) {
                    if (target.modelStatistics) target.modelStatistics->accumulate(*old.modelStatistics);
                    else target.modelStatistics = old.modelStatistics;
                }
            }
            if (i == reviewStep && retainCurrent) {
                state = steps[reviewStep]; state.whole.complete = state.whole.reviewed = false;
                state.validation.complete = state.validation.announced = false;
                for (auto& part : state.substeps) part.complete = part.reviewed = false;
                source.seeded = true;
            }
            if (i == reviewStep && !retainCurrent) state.substeps.front().snapshot = source.state.substeps.front().snapshot;
            revisionSources.emplace(i, std::move(source));
        }
        const auto clearProgress = [](ImplementationItem& item) {
            item.complete = item.reviewed = item.started = item.announced = item.finalized = item.unnecessary = false;
            item.explanation.clear(); item.activity.clear(); item.diffs.clear();
        };
        state.displayRevision = expected;
        clearProgress(state.whole); clearProgress(state.validation);
        for (auto& part : state.substeps) clearProgress(part);
        replacement.push_back(std::move(state));
    }
    if (proposal.plan) planVersions.push_back({planVersions.size() + 1, steps[reviewStep].identity, candidate});
    plan = candidate; steps = std::move(replacement); acceptedRework = proposal.request;
    revisionPlanning = false;
    proposedRework.clear(); proposedRevision.reset(); revisionRollback.reset(); ++revision;
    workStep = reviewStep; workSubstep = activitySubstep = 0; boundaryCursor = activeTurn = 0; activityRoutes.clear();
    if (retainCurrent) {
        const auto evidence = boundaries.read(workStep, currentGeneration());
        if (!evidence.empty()) steps[workStep].progressBoundary = boundaryCursor = evidence.back().id;
    }
    status = "running"; error.clear(); save();
}
void ImplementationRun::acceptRework(std::uint64_t expected) { prepareRevision(expected); commitRevision(expected); }

bool ImplementationRun::needsRevisionSeed() const { return next() && revisionSources.contains(workStep) && !revisionSources.at(workStep).seeded; }
void ImplementationRun::seedRevision(const ImplementationRevisionSeed& seed) {
    if (!needsRevisionSeed()) throw std::logic_error("No revision seed is pending");
    boundaries.seed(workStep, currentGeneration(), seed.boundaries);
    auto& source = revisionSources.at(workStep); source.seeded = true; source.conflicts = seed.conflicts;
    steps[workStep].substeps.front().snapshot = seed.snapshot;
    const auto evidence = boundaries.read(workStep, currentGeneration());
    if (!evidence.empty()) steps[workStep].progressBoundary = boundaryCursor = evidence.back().id;
    save();
}
std::string ImplementationRun::correctionInstruction() const {
    const auto& source = revisionSources.at(workStep);
    return "Correct or re-examine the existing contribution of step " + std::to_string(workStep + 1) + " ONLY. Never rewrite validated history or implement later steps.\nAccepted request:\n" + acceptedRework +
        "\nAccepted plan:\n" + plan->markdown() + "\nPreserved previous contribution by original substep (JSON evidence):\n" + Json(source.state.substeps).dump() +
        "\nOriginal-to-current substep owners: " + Json(source.owners).dump() + "\nReplay conflicts needing targeted reconciliation:\n" + source.conflicts +
        "\nExisting contributions were preserved/replayed against the corrected predecessor where possible. Inspect current code. Retain unaffected work; do not blindly rebuild. "
        "For EVERY planned substep, report correcting with its zero-based CURRENT index before edits or re-examination, then completed with an updated explanation (or unnecessary if genuinely obsolete). "
        "Use correcting, not started: this is a revision with retained construction ownership. A step can be coherent without code edits, but must still be examined and technically checked. "
        "If replay conflicted, adapt the preserved contribution to current code in its mapped substep; never silently drop it. "
        "Group ordered progress and work calls when no intervening result is needed; wait for results before dependent decisions. "
        "Report validating with substep null; if checks are needed, run them in this phase. If no additional check is needed, explain why and complete the step. No systematic diff inspection is needed. "
        "Then complete_implementation_step with one object per current substep: {} reuses its latest completed/unnecessary explanation; provide explanation only to revise it. "
        "Do not submit hunk assignments. Final diffs compare the step's corrected starting state to its corrected result. Never claim blocked checks passed. Successful submission ends execution immediately; put it last." + completionGuidance();
}
Json ImplementationRun::history(std::optional<std::size_t> step) const {
    Json result = Json::array();
    if (step && *step >= steps.size()) return result;
    for (const auto& archive : revisionArchive) for (std::size_t i = 0; i < archive.steps.size(); ++i)
        if ((!step || archive.steps[i].identity == steps[*step].identity) && archive.steps[i].whole.complete)
            result.push_back({{"revision", archive.revision}, {"request", archive.request}, {"plan", archive.plan.steps[i]}, {"state", archive.steps[i]}});
    return result;
}
