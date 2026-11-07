#include "implementation_run.hpp"
#include "../prompts.hpp"
#include "implementation_changes.hpp"
#include "../snapshots/git_snapshot.hpp"
#include <algorithm>
#include <limits>

using Json = nlohmann::json;

void to_json(Json& json, const ImplementationItem& item) {
    json = {{"complete", item.complete}, {"reviewed", item.reviewed}, {"explanation", item.explanation}, {"diffs", item.diffs},
        {"started", item.started}, {"announced", item.announced}, {"finalized", item.finalized}, {"unnecessary", item.unnecessary}, {"activity", item.activity}};
    if (item.snapshot) json["snapshot"] = *item.snapshot;
    if (item.durationMs) json["duration_ms"] = *item.durationMs;
    if (item.modelStatistics) json["model_statistics"] = *item.modelStatistics;
}
void from_json(const Json& json, ImplementationItem& item) {
    json.at("complete").get_to(item.complete);
    json.at("reviewed").get_to(item.reviewed);
    json.at("explanation").get_to(item.explanation);
    json.at("diffs").get_to(item.diffs);
    if (json.contains("snapshot")) item.snapshot = json.at("snapshot").get<TurnSnapshot>();
    json.at("started").get_to(item.started);
    json.at("announced").get_to(item.announced);
    json.at("finalized").get_to(item.finalized);
    json.at("unnecessary").get_to(item.unnecessary);
    json.at("activity").get_to(item.activity);
    if (json.contains("duration_ms")) item.durationMs = json.at("duration_ms").get<std::uint64_t>();
    if (json.contains("model_statistics")) item.modelStatistics = json.at("model_statistics").get<ModelCallMetadata>();
}
void to_json(Json& json, const ImplementationStepState& step) {
    json = {{"substeps", step.substeps}, {"whole", step.whole}, {"validation", step.validation}, {"generation", step.generation}, {"identity", step.identity},
        {"display_revision", step.displayRevision}, {"progress_boundary", step.progressBoundary}, {"conclusion", step.conclusion}, {"review_branch", step.reviewBranch}};
}
void from_json(const Json& json, ImplementationStepState& step) {
    json.at("substeps").get_to(step.substeps);
    json.at("whole").get_to(step.whole);
    json.at("validation").get_to(step.validation);
    json.at("generation").get_to(step.generation);
    json.at("identity").get_to(step.identity);
    json.at("display_revision").get_to(step.displayRevision);
    json.at("progress_boundary").get_to(step.progressBoundary);
    json.at("conclusion").get_to(step.conclusion);
    json.at("review_branch").get_to(step.reviewBranch);
}
ImplementationRun::ImplementationRun(const std::filesystem::path& directory)
    : storageDirectory(directory), db(directory / "implementation.sqlite"), boundaries(directory) {
    db.execute("CREATE TABLE IF NOT EXISTS implementation_state (id INTEGER PRIMARY KEY CHECK(id=1), payload TEXT NOT NULL)");
    db.execute("CREATE TABLE IF NOT EXISTS review_construction (identity TEXT NOT NULL, generation INTEGER NOT NULL, messages TEXT NOT NULL, PRIMARY KEY(identity,generation))");
    SessionDatabase::Statement read(db, "SELECT payload FROM implementation_state WHERE id=1");
    if (!read.step()) return;
    const auto value = Json::parse(read.text(0));
    const auto version = value.at("version").get<int>();
    if (version != 7) throw std::runtime_error("Unsupported implementation state");
    plan = value.at("plan").get<ImplementationPlan>();
    steps = value.at("steps").get<std::vector<ImplementationStepState>>();
    status = value.at("status"); error = value.at("error"); pace = value.at("pace");
    value.at("review_branch").get_to(reviewBranch);
    value.at("checkout_pending").get_to(checkoutPending);
    value.at("active_turn").get_to(activeTurn);
    workStep = value.at("work_step"); workSubstep = value.at("work_substep");
    reviewStep = value.at("review_step"); reviewSubstep = value.at("review_substep");
    value.at("boundary_cursor").get_to(boundaryCursor);
    value.at("activity_substep").get_to(activitySubstep);
    value.at("revision").get_to(revision);
    value.at("proposed_rework").get_to(proposedRework);
    value.at("accepted_rework").get_to(acceptedRework);
    value.at("revision_history").get_to(revisionHistory);
    if (value.contains("proposed_revision") && !value["proposed_revision"].is_null()) proposedRevision = value["proposed_revision"].get<ImplementationRevisionProposal>();
    if (value.contains("revision_rollback") && !value["revision_rollback"].is_null()) revisionRollback = value["revision_rollback"].get<TurnSnapshot>();
    value.at("revision_sources").get_to(revisionSources);
    value.at("revision_archive").get_to(revisionArchive);
    value.at("revision_planning").get_to(revisionPlanning);
    value.at("revision_branch").get_to(revisionBranch);
    value.at("plan_versions").get_to(planVersions);
    if (steps.size() != plan->steps.size() || workStep > steps.size() || reviewStep > workStep)
        throw std::runtime_error("Invalid implementation cursors");
    if (workStep < steps.size()) applyBoundaries(std::numeric_limits<std::int64_t>::max());
}
Json ImplementationRun::data() const {
    if (!plan) return nullptr;
    return {{"version", 7}, {"checkout_pending", checkoutPending}, {"plan", *plan}, {"steps", steps}, {"status", status}, {"error", error}, {"pace", pace},
        {"revision", revision}, {"proposed_rework", proposedRework}, {"accepted_rework", acceptedRework}, {"revision_history", revisionHistory},
        {"proposed_revision", proposedRevision ? Json(*proposedRevision) : Json(nullptr)}, {"revision_rollback", revisionRollback ? Json(*revisionRollback) : Json(nullptr)},
        {"revision_sources", revisionSources}, {"revision_archive", revisionArchive},
        {"revision_planning", revisionPlanning}, {"revision_branch", revisionBranch}, {"plan_versions", planVersions},
        {"work_step", workStep}, {"work_substep", workSubstep}, {"review_step", reviewStep}, {"review_substep", reviewSubstep},
        {"review_branch", reviewBranch}, {"active_turn", activeTurn}, {"boundary_cursor", boundaryCursor}, {"activity_substep", activitySubstep}};
}
void ImplementationRun::save() {
    SessionDatabase::Statement write(db, "INSERT INTO implementation_state(id,payload) VALUES(1,?) ON CONFLICT(id) DO UPDATE SET payload=excluded.payload");
    write.bind(1, data().dump()); write.step();
}
void ImplementationRun::setReviewBranch(std::size_t step, const std::string& id) {
    steps.at(step).reviewBranch = reviewBranch = id;
    save();
}
std::vector<std::string> ImplementationRun::previousReviewBranches(std::size_t step) const {
    std::vector<std::string> result;
    for (std::size_t index = 0; index < step; ++index)
        if (!steps.at(index).reviewBranch.empty()) result.push_back(steps[index].reviewBranch);
    return result;
}
void ImplementationRun::saveReviewConstruction(std::size_t step, const std::vector<Message>& messages) {
    const auto& built = steps.at(step);
    SessionDatabase::Statement write(db, "INSERT OR REPLACE INTO review_construction(identity,generation,messages) VALUES(?,?,?)");
    write.bind(1, built.identity); write.bind(2, static_cast<std::int64_t>(built.generation));
    write.bind(3, Json(messages).dump()); write.step();
}
std::optional<std::vector<Message>> ImplementationRun::reviewConstruction(std::size_t step, bool latestGeneration) {
    const auto& built = steps.at(step);
    SessionDatabase::Statement read(db, latestGeneration
        ? "SELECT messages FROM review_construction WHERE identity=? ORDER BY generation DESC LIMIT 1"
        : "SELECT messages FROM review_construction WHERE identity=? AND generation=?");
    read.bind(1, built.identity);
    if (!latestGeneration) read.bind(2, static_cast<std::int64_t>(built.generation));
    if (!read.step()) return {};
    return Json::parse(read.text(0)).get<std::vector<Message>>();
}
TurnSnapshot ImplementationRun::reviewFiles(std::size_t step) const {
    const auto& built = steps.at(step);
    if (!built.whole.complete || built.substeps.empty() || !built.substeps.front().snapshot)
        throw std::logic_error("No completed historical state for this step");
    return *built.substeps.front().snapshot;
}
std::optional<std::size_t> ImplementationRun::reviewHistoryEnd(std::size_t step) const {
    if (step + 1 < steps.size() && !steps[step + 1].substeps.empty() && steps[step + 1].substeps.front().snapshot)
        return steps[step + 1].substeps.front().snapshot->start;
    return {};
}
void ImplementationRun::start(const ImplementationPlan& accepted, const std::string& selectedPace) {
    if (selectedPace != "step_by_step" && selectedPace != "everything") throw std::invalid_argument("Invalid implementation pace");
    if (plan) throw std::logic_error("Implementation has already started; resume it instead");
    plan = accepted; pace = selectedPace;
    for (const auto& step : accepted.steps) steps.push_back({std::vector<ImplementationItem>(step.substeps.size())});
    for (std::size_t i = 0; i < steps.size(); ++i) steps[i].identity = "step-" + std::to_string(i);
    planVersions = {{1, steps.front().identity, accepted}};
    checkoutPending = true;
    status = "running"; save();
}
void ImplementationRun::resume() {
    if (hasRevisionProposal()) throw std::logic_error("Accept or dismiss the revision proposal before continuing construction");
    if (!plan || (status != "interrupted" && status != "failed")) throw std::logic_error("Implementation is not interrupted");
    if (workStep < steps.size()) {
        const auto& boundary = steps[workStep].substeps.at(workSubstep).snapshot;
        if (boundary && std::any_of(boundary->edits.begin(), boundary->edits.end(), [](const auto& edit) { return !edit.complete; }))
            throw std::runtime_error("An interrupted file mutation has no verified outcome. Resume is blocked to preserve review evidence.");
    }
    status = workStep == steps.size() ? "completed" : pace == "step_by_step" && workStep > reviewStep ? "waiting" : "running";
    error.clear(); save();
}
void ImplementationRun::begin(const TurnSnapshot& snapshot) {
    executionStarted = std::chrono::steady_clock::now();
    activeTurn = snapshot.id;
    activityElapsedMs = 0;
    auto& boundary = steps.at(workStep).substeps.at(workSubstep).snapshot;
    if (!boundary) {
        boundary = snapshot;
        boundary->messages.clear(); boundary->edits.clear();
    }
    applyBoundaries(std::numeric_limits<std::int64_t>::max());
    save();
}
void ImplementationRun::recover(const std::optional<TurnSnapshot>& snapshot) {
    if (status != "running") return;
    if (activeTurn && snapshot && snapshot->id == activeTurn && workStep < steps.size()) {
        auto recovered = substepCombined(*snapshot);
        recovered.messages.clear();
        steps.at(workStep).substeps.at(workSubstep).snapshot = std::move(recovered);
    }
    activeTurn = 0;
    stop("Implementation interrupted. Resume explicitly; inspect any partial work before continuing.");
}
std::optional<std::pair<std::size_t, std::size_t>> ImplementationRun::next() const {
    if (!plan || status != "running" || workStep >= steps.size()) return {};
    return {{workStep, workSubstep}};
}
TurnSnapshot ImplementationRun::substepCombined(const TurnSnapshot& latest) const {
    auto result = latest;
    result.messages.clear();
    const auto& previous = steps.at(workStep).substeps.at(workSubstep).snapshot;
    if (previous) {
        result.before = previous->before;
        result.edits.insert(result.edits.begin(), previous->edits.begin(), previous->edits.end());
    }
    return result;
}
std::optional<TurnSnapshot> ImplementationRun::baseline() const {
    if (!next()) return {};
    return steps.at(workStep).substeps.at(workSubstep).snapshot;
}
void ImplementationRun::finish(const TurnSnapshot& snapshot, const ImplementationCompletion& completion,
    std::vector<ImplementationDiff> diffs, bool completed, const std::string& failure) {
    if (!next()) throw std::logic_error("No implementation step is running");
    auto& step = steps.at(workStep);
    auto merged = substepCombined(snapshot);
    merged.messages.clear();
    step.substeps.at(workSubstep).snapshot = std::move(merged);
    activeTurn = 0;
    executionStarted.reset();
    activityRoutes.clear();
    if (!completed) { status = "interrupted"; error = failure; save(); return; }
    if (completion.substeps.size() != step.substeps.size())
        throw std::invalid_argument("Completion must describe every planned substep in order");
    finishCapturedChanges(step, *step.substeps.at(workSubstep).snapshot, diffs);
    for (std::size_t i = 0; i < step.substeps.size(); ++i) {
        if (!completion.substeps[i].explanation.empty()) step.substeps[i].explanation = completion.substeps[i].explanation;
        if (step.substeps[i].explanation.find_first_not_of(" \r\n\t") == std::string::npos)
            throw std::invalid_argument("Each substep needs a final or captured explanation");
        if (!step.substeps[i].diffs.empty()) step.substeps[i].unnecessary = false;
        step.substeps[i].complete = true;
    }
    step.whole.complete = true;
    step.whole.explanation = completion.explanation;
    step.conclusion = completion.conclusion;
    step.whole.diffs = std::move(diffs);
    step.validation.complete = step.validation.announced = true;
    if (revisionSources.contains(workStep)) revisionSources.at(workStep).seeded = true;
    ++workStep; workSubstep = activitySubstep = 0;
    status = hasRevisionProposal() ? "revision_pending" : workStep == steps.size() ? "completed" : pace == "step_by_step" ? "waiting" : "running";
    if (workStep == steps.size()) acceptedRework.clear();
    save();
}
void ImplementationRun::stop(const std::string& reason) {
    executionStarted.reset();
    if (plan) { status = "interrupted"; error = reason; save(); }
}

void ImplementationRun::applyBoundaries(std::int64_t through) {
    auto& step = steps.at(workStep);
    for (const auto& boundary : boundaries.read(workStep, currentGeneration())) {
        if (boundary.id <= boundaryCursor || boundary.id > through) continue;
        const auto& progress = boundary.progress;
        if (progress.substep) {
            auto& part = step.substeps.at(*progress.substep);
            if (progress.status == "started") { part.started = true; part.announced = part.finalized = part.unnecessary = false; }
            if (progress.status == "correcting") {
                part.unnecessary = false;
                if (step.displayRevision && !step.validation.started) {
                    part.started = true; part.announced = part.finalized = false;
                }
            }
            if (progress.status == "completed" || progress.status == "unnecessary") {
                part.announced = true;
                part.finalized = true;
                part.unnecessary = progress.status == "unnecessary";
                part.explanation = progress.explanation;
            }
        }
        if (boundary.owner) {
            auto& part = step.substeps.at(*boundary.owner);
            part.diffs.insert(part.diffs.end(), boundary.diffs.begin(), boundary.diffs.end());
        }
        if (boundary.activeSubstep && (!boundary.validating || (step.displayRevision && !step.validation.started))) activitySubstep = *boundary.activeSubstep;
        if (!step.displayRevision) step.validation.started = boundary.validating;
        else if (progress.status == "validating" || progress.status == "finished") step.validation.started = true;
        boundaryCursor = boundary.id;
    }
}

void ImplementationRun::finishCapturedChanges(ImplementationStepState& step, const TurnSnapshot& snapshot, const std::vector<ImplementationDiff>& diffs) {
    applyBoundaries(std::numeric_limits<std::int64_t>::max());
    const auto evidence = boundaries.read(workStep, currentGeneration());
    if (evidence.empty() || evidence.back().progress.status != "finished")
        throw std::logic_error("No captured step validation completion");
    const auto& submitted = evidence.back().snapshot;
    if (submitted.id != snapshot.id || submitted.root != snapshot.root || submitted.after != snapshot.after || Json(submitted.edits) != Json(snapshot.edits))
        throw std::runtime_error("Files changed after step completion; the captured evidence must be reviewed before resuming");
    auto owned = preserveImplementationOwnership(evidence, diffs, step.substeps.size());
    for (std::size_t i = 0; i < owned.size(); ++i) step.substeps[i].diffs = std::move(owned[i]);
}

void ImplementationRun::recordCapturedActivity(const Message& message) {
    auto& step = steps.at(workStep);
    for (const auto& content : message.content) if (const auto* result = std::get_if<ToolResultContent>(&content);
        result && !result->isError && result->metadata.contains("implementation_boundary"))
        applyBoundaries(result->metadata.at("implementation_boundary").get<std::int64_t>());
    if (message.role == MessageRole::SYSTEM || message.role == MessageRole::USER) return;
    std::optional<std::size_t> route = step.validation.started ? std::nullopt : std::optional(activitySubstep);
    std::vector<std::pair<std::optional<std::size_t>, Message>> fragments;
    for (const auto& content : message.content) {
        if (const auto* call = std::get_if<ToolCallContent>(&content)) {
            if (call->toolName == "report_implementation_progress") {
                const auto status = call->arguments.value("status", "");
                if (status == "validating") route.reset();
                else if ((status == "started" || (step.displayRevision && status == "correcting")) && !step.validation.started && call->arguments.contains("substep") &&
                    call->arguments["substep"].is_number_integer() && call->arguments["substep"] >= 0) {
                    const auto index = call->arguments["substep"].get<std::size_t>();
                    if (index < step.substeps.size()) route = index;
                }
            }
            activityRoutes[call->callId] = route;
        }
        if (const auto* result = std::get_if<ToolResultContent>(&content); result && activityRoutes.contains(result->callId)) {
            route = activityRoutes.at(result->callId);
            activityRoutes.erase(result->callId);
        }
        if (fragments.empty() || fragments.back().first != route) {
            auto fragment = message;
            fragment.content.clear();
            fragment.metadata.reset(); fragment.modelCall.reset();
            fragments.emplace_back(route, std::move(fragment));
        }
        fragments.back().second.content.push_back(content);
    }
    if (fragments.empty()) fragments.emplace_back(route, message);
    fragments.front().second.modelCall = message.modelCall;
    fragments.back().second.metadata = message.metadata;
    const auto owner = fragments.front().first;
    recordStatistics(message, owner ? step.substeps.at(*owner) : step.validation);
    for (auto& [target, fragment] : fragments)
        (target ? step.substeps.at(*target) : step.validation).activity.push_back(std::move(fragment));
}

void ImplementationRun::recordStatistics(const Message& message, ImplementationItem& item) {
    auto& whole = steps.at(workStep).whole;
    if (message.elapsedMs) {
        const auto elapsed = std::max(activityElapsedMs, *message.elapsedMs);
        const auto delta = elapsed - activityElapsedMs;
        item.durationMs = item.durationMs.value_or(0) + delta;
        whole.durationMs = whole.durationMs.value_or(0) + delta;
        activityElapsedMs = elapsed;
    }
    if (message.modelCall) {
        for (auto* target : {&item, &whole}) {
            if (target->modelStatistics) target->modelStatistics->accumulate(*message.modelCall);
            else target->modelStatistics = *message.modelCall;
        }
    }
}

void ImplementationRun::recordActivity(const std::vector<Message>& messages) {
    if (!next() || messages.empty()) return;
    auto& step = steps.at(workStep);
    for (const auto& message : messages) {
        if (message.compaction || message.id.starts_with("compaction-status-")) {
            auto& target = step.validation.started ? step.validation : step.substeps.at(activitySubstep);
            target.activity.push_back(message);
        } else recordCapturedActivity(message);
    }
    save();
}
const ImplementationItem& ImplementationRun::item(std::size_t step, std::size_t substep) const {
    const auto& selected = steps.at(step);
    const auto& item = substep == selected.substeps.size() ? selected.whole : selected.substeps.at(substep);
    if (!item.complete) throw std::logic_error("This change is not complete yet");
    return item;
}
std::optional<TurnSnapshot> ImplementationRun::importBoundary(std::size_t step, std::size_t substep) const {
    if (hasRevisionProposal() || status == "revising") throw std::logic_error("Accept or dismiss the revision proposal before validating");
    item(step, substep);
    const auto& selected = steps.at(step);
    if (substep != selected.substeps.size()) return {};
    if (!selected.whole.complete || selected.whole.reviewed || step != reviewStep)
        throw std::logic_error("Finalize completed steps in planned order");
    return selected.substeps.front().snapshot;
}
void ImplementationRun::review(std::size_t step, std::size_t substep, bool validate) {
    if (hasRevisionProposal() || status == "revising") throw std::logic_error("Accept or dismiss the revision proposal before validating");
    item(step, substep);
    auto& selected = steps.at(step);
    if (!selected.whole.complete) throw std::logic_error("Wait for the entire step before validating substeps");
    if (selected.whole.reviewed) throw std::logic_error("Final step validation cannot be undone");
    if (substep < selected.substeps.size()) selected.substeps[substep].reviewed = validate;
    else {
        if (step != reviewStep) throw std::logic_error("Finalize steps in planned order");
        if (!validate) throw std::logic_error("Final step validation cannot be undone");
        for (auto& part : selected.substeps) part.reviewed = true;
        selected.whole.reviewed = true; ++reviewStep;
        proposedRework.clear();
        if (revision) ++revision;
        if (status == "waiting") status = "running";
    }
    reviewSubstep = 0;
    if (reviewStep < steps.size()) {
        const auto& current = steps[reviewStep].substeps;
        while (reviewSubstep < current.size() && current[reviewSubstep].reviewed) ++reviewSubstep;
    }
    save();
}
std::string ImplementationRun::instruction() const {
    if (!next()) throw std::logic_error("No next implementation step");
    const auto& step = plan->steps.at(workStep);
    if (revisionSources.contains(workStep)) return correctionInstruction();
    return "Execute the ENTIRE step " + std::to_string(workStep + 1) +
        " of the accepted implementation plan in this turn, including all its substeps and technical validations. Do not implement later steps.\nAccepted plan:\n" + plan->markdown() +
        "\nCurrent step: " + step.title + "\nObjective: " + step.objective +
        (baseline() ? "\nExecution was interrupted. Inspect preserved outcomes and current code before continuing; do not blindly repeat work.\n" + resumeProgress() : "") +
        "\nPreserve unrelated user changes. Complete the step, perform checks if needed and correct failures in this same turn. "
        "The harness activates the first substep before execution and preserves the active boundary on resume; do not announce started again. Report completed with 2-4 concrete result bullets when a substep's work is done. "
        "After completed or unnecessary, the harness automatically activates the first remaining substep, or enters validation when none remain. Do not send a separate started or validating announcement for these automatic transitions; continue directly with useful work. "
        "If current code makes a planned substep no longer useful, report status unnecessary immediately with a concrete reason, without fabricated work. Restarting or completing that substep clears this provisional designation. "
        "Use zero-based substep indexes. These boundaries capture real file states and preserve ownership. Do not redistribute changes at completion. "
        "After ALL planned substeps are completed or explicitly unnecessary, validation is activated automatically. "
        "This starts Validating the step. If checks are needed, run them in this phase. Entering validation does not require new checks. Normally this phase makes NO edits. "
        "If a check fails, report status correcting with the original substep index BEFORE editing its repair; report completed for that target when the repair is done, then rerun affected checks. "
        "A repair updates only its declared target; unchanged contributions retain their existing substep ownership. All checks and repair activity remain under Validating the step. "
        "Wait for completion results before working on the automatically activated successor. Group independent calls only when no intervening result is needed. "
        "No systematic inspect_implementation_changes call is needed; inspect only when useful to resolve uncertainty. "
        "Write the final step explanation as a concise, neutral description of what was added and what the step contains, including how its parts fit together. Do not phrase it as a reply to the user, a progress report, or a self-evaluation. "
        "Then call complete_implementation_step with a concise global explanation and one object per planned substep, in order. Use {} to reuse its latest completed/unnecessary explanation; provide explanation only when it needs revision. "
        "Do NOT submit hunk assignments. The daemon builds corrected intermediate states from captured construction ownership, including repairs in their target substeps. "
        "Never fabricate changes for a substep with no distinct file changes. Report checks without claiming success if blocked. "
        "Successful completion ends execution immediately; make it the last call." + completionGuidance();
}

std::string ImplementationRun::completionGuidance() const {
    std::string text = "\nTechnical validation is the final phase of building this step, not a read-only review or a stopping point. "
        "During construction, do not run tests, builds, linters or verification-only diff/status inspections, or repeatedly check whether everything is correct. Reserve those checks for Validating the step; reads needed to implement the current substep remain appropriate. "
        "Keep validation proportional to this step: if tests, builds or other checks are needed, perform them in validation, not during construction. "
        "Prefer existing tests and short targeted checks. Do not build an ad-hoc test framework or a large DOM/timer simulation merely to compensate for an unavailable browser; report the unverified interaction instead unless the plan or a concrete risk requires that check. "
        "If no additional check is needed, explain why and complete the step without inventing checks. "
        "Reuse successful earlier results when intervening changes cannot invalidate them. Do not reread all files, audit the whole implementation or repeat still-valid checks for reassurance. "
        "Inspect specific code or broaden checks only to resolve a concrete uncertainty or regression risk, or satisfy an explicit plan requirement. "
        "After a repair, rerun affected checks rather than all previous validations. The last step is not a fresh global audit. "
        "Finish ALL planned in-scope work, repair missing or incomplete contributions, rerun affected checks if needed, and submit complete_implementation_step before ending normally. "
        "A prose message, audit, progress report or promise to resume does not complete the step and must not replace that submission. "
        "Do not stop to request a new Implement activation: execution is already authorized for this step in Sane. "
        "Cancellation, denied actions or genuine unresolved blockers must remain explicit interruptions, never fabricated completion. "
        "Required checks must pass EXCEPT failures demonstrated on the pre-implementation baseline and unrelated to these changes. "
        "Record those failures and the baseline evidence in the step explanation; they do NOT block technical completion and do NOT authorize unrelated repairs. "
        "Never claim a failing check passed. New regressions, unexplained failures or unperformed required checks still block completion. Human validation remains separate. ";
    text += "Keep completion concise: describe the coherent result and actual check outcomes once; reuse captured substep explanations with {} rather than restating them. Do not add a separate recap before submitting. ";
    if (workStep + 1 != steps.size()) return text + "Use an empty conclusion in complete_implementation_step.";
    text += "This is the LAST step. Supply a conclusion in complete_implementation_step: a readable recap of the ENTIRE implementation, what is now possible, "
        "the checks actually performed and any remaining caveats (including pre-existing failures from earlier steps). "
        "Use a few paragraphs or bullets, slightly fuller than a step explanation, without repeating every substep; at most 6000 characters.\nPrevious completed steps:\n";
    for (std::size_t i = 0; i < workStep; ++i) {
        text += plan->steps[i].title + "\n" + steps[i].whole.explanation + "\n";
        for (const auto& part : steps[i].substeps) text += part.explanation + "\n";
    }
    return text;
}

std::string ImplementationRun::resumeProgress() const {
    const auto& step = steps.at(workStep);
    Json progress = {{"validating", step.validation.started}, {"substeps", Json::array()}};
    for (std::size_t i = 0; i < step.substeps.size(); ++i) {
        const auto& part = step.substeps[i];
        progress["substeps"].push_back({{"substep", i}, {"started", part.started}, {"announced", part.announced},
            {"unnecessary", part.unnecessary}, {"explanation", part.explanation}});
    }
    const auto evidence = boundaries.read(workStep, currentGeneration());
    const auto lastProgress = std::find_if(evidence.rbegin(), evidence.rend(), [](const auto& boundary) {
        return boundary.progress.status != "mutation";
    });
    if (lastProgress != evidence.rend()) progress["last_boundary"] = lastProgress->progress;
    return "Preserved construction progress (do not restart completed work; remain in validation if validating is true):\n" + progress.dump();
}
Json ImplementationRun::view() const {
    auto value = data();
    if (value.is_null()) return value;
    value["conclusion"] = !steps.empty() && std::all_of(steps.begin(), steps.end(), [](const auto& step) { return step.whole.complete; })
        ? steps.back().conclusion : "";
    value.erase("revision_history");
    for (const auto key : {"revision_archive", "revision_sources", "revision_rollback"}) value.erase(key);
    value["preparing"] = status == "revising" || (status == "running" && activeTurn == 0);
    if (status == "revising") {
        value["work_step"] = reviewStep;
        value["work_substep"] = 0;
    }
    value["version_count"] = revisionArchive.size() + 1;
    value["version_request"] = acceptedRework;
    value["accepted_plan_markdown"] = plan->markdown();
    for (std::size_t i = 0; i < planVersions.size(); ++i) value["plan_versions"][i]["markdown"] = planVersions[i].plan.markdown();
    value["history_count"] = history().size();
    value["proposed_plan_markdown"] = proposedRevision && proposedRevision->plan ? proposedRevision->plan->markdown() : "";
    if (status != "revising" && workStep < steps.size())
        value["work_substep"] = steps[workStep].validation.started ? steps[workStep].substeps.size() : activitySubstep;
    if (status == "running" && activeTurn && executionStarted && workStep < steps.size()) {
        const auto& step = steps[workStep];
        const auto& item = step.validation.started ? step.validation : step.substeps.at(activitySubstep);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - *executionStarted).count();
        value["active_duration_ms"] = item.durationMs.value_or(0) +
            (static_cast<std::uint64_t>(elapsed) > activityElapsedMs ? elapsed - activityElapsedMs : 0);
    }
    for (std::size_t i = 0; i < steps.size(); ++i) {
        auto& step = value["steps"][i];
        step["preparing"] = value["preparing"].get<bool>() && i == (status == "revising" ? reviewStep : workStep);
        step["repository_root"] = steps[i].substeps[0].snapshot ? steps[i].substeps[0].snapshot->root : "";
        step["repository_tree"] = steps[i].substeps[0].snapshot ? steps[i].substeps[0].snapshot->before : "";
        step["revising"] = revisionSources.contains(i) && !steps[i].whole.complete;
        step["history_count"] = history(i).size();
        step["correction_request"] = "";
        for (const auto& archive : revisionArchive)
            if (archive.revision == steps[i].displayRevision) step["correction_request"] = archive.request;
        step["previous_versions"] = Json::array();
        step["construction_history"] = Json::array();
        for (const auto& archive : revisionArchive) for (std::size_t j = 0; j < archive.steps.size(); ++j) {
            const auto& previous = archive.steps[j];
            if (previous.identity == steps[i].identity && previous.whole.complete) {
                Json parts = Json::array();
                for (std::size_t k = 0; k < previous.substeps.size(); ++k)
                    parts.push_back({{"title", archive.plan.steps[j].substeps[k].title},
                        {"explanation", previous.substeps[k].explanation}, {"activity", previous.substeps[k].activity}});
                step["construction_history"].push_back({{"revision", archive.revision}, {"request", archive.request},
                    {"title", archive.plan.steps[j].title}, {"explanation", previous.whole.explanation}, {"substeps", parts},
                    {"validation_activity", previous.validation.activity}});
            }
            if (j != archive.reworkedStep) continue; // Successors are replayed, not superseded menu entries.
            if (previous.identity != steps[i].identity || previous.whole.reviewed ||
                (!previous.whole.complete && (previous.substeps.empty() || !previous.substeps.front().snapshot))) continue;
            Json titles = Json::array();
            for (const auto& part : archive.plan.steps[j].substeps) titles.push_back(part.title);
            step["previous_versions"].push_back({{"revision", archive.revision}, {"plan_number", archive.planNumber}, {"title", archive.plan.steps[j].title}, {"substeps", titles}});
        }
        if (status == "revising" && i >= reviewStep) {
            step["revising"] = true; step["whole"]["complete"] = false;
            for (auto& part : step["substeps"]) part["complete"] = false;
        }
        for (auto& part : step["substeps"]) part.erase("snapshot");
        step["validation"].erase("snapshot");
    }
    return value;
}
Json ImplementationRun::versions() const {
    Json result = Json::array();
    std::string request;
    for (const auto& archive : revisionArchive) {
        Json value{{"status", "archived"}, {"pace", pace}, {"plan", archive.plan}, {"steps", archive.steps},
            {"work_step", archive.steps.size()}, {"work_substep", 0}, {"review_step", archive.reworkedStep},
            {"review_substep", 0}, {"error", ""}, {"revision", archive.revision},
            {"accepted_plan_markdown", archive.plan.markdown()}, {"version_request", request}};
        const bool complete = std::all_of(archive.steps.begin(), archive.steps.end(), [](const auto& step) { return step.whole.complete; });
        value["conclusion"] = complete && !archive.steps.empty() ? archive.steps.back().conclusion : "";
        for (std::size_t i = 0; i < archive.steps.size(); ++i) {
            auto& step = value["steps"][i];
            step["correction_request"] = "";
            for (const auto& previous : revisionArchive)
                if (previous.revision == archive.steps[i].displayRevision) step["correction_request"] = previous.request;
            const auto& snapshot = archive.steps[i].substeps.front().snapshot;
            step["repository_root"] = snapshot ? snapshot->root : "";
            step["repository_tree"] = snapshot ? snapshot->before : "";
            for (auto& part : step["substeps"]) part.erase("snapshot");
            step["whole"].erase("snapshot");
            step["validation"].erase("snapshot");
        }
        result.push_back(std::move(value));
        request = archive.request;
    }
    return result;
}
TurnSnapshot ImplementationRun::repositoryBaseline(std::size_t step, const std::string& tree) const {
    if (tree.empty()) throw std::invalid_argument("Repository baseline changed or is unavailable");
    const auto find = [&](const auto& candidates) -> std::optional<TurnSnapshot> {
        if (step >= candidates.size()) return {};
        auto snapshot = candidates[step].substeps.front().snapshot;
        if (!snapshot || snapshot->root.empty() || snapshot->before.empty()) {
            for (const auto& candidate : candidates) {
                const auto& baseline = candidate.substeps.front().snapshot;
                if (baseline && !baseline->root.empty() && !baseline->before.empty()) { snapshot = baseline; break; }
            }
        }
        if (snapshot && !snapshot->root.empty() && snapshot->before == tree) return snapshot;
        return {};
    };
    if (auto snapshot = find(steps)) return *snapshot;
    // Historical browsing resolves only persisted baselines, never the current checkout.
    for (const auto& archive : revisionArchive)
        if (auto snapshot = find(archive.steps)) return *snapshot;
    throw std::invalid_argument("Repository baseline changed or is unavailable");
}
Json ImplementationRun::repositoryDirectory(std::size_t step, const std::string& tree, const std::string& path) const {
    const auto snapshot = repositoryBaseline(step, tree);
    Json entries = Json::array();
    for (const auto& entry : GitSnapshot::archive(snapshot.root).children({tree}, path))
        entries.push_back({{"name", entry.name}, {"directory", entry.directory}});
    return entries;
}
std::string ImplementationRun::reviewContext(std::size_t step, std::size_t substep, const std::string& path,
    std::optional<std::size_t> hunk) const {
    const auto& change = item(step, substep);
    std::string selection;
    if (!path.empty()) {
        const auto file = std::find_if(change.diffs.begin(), change.diffs.end(), [&](const auto& diff) { return diff.path == path; });
        if (file == change.diffs.end()) throw std::invalid_argument("Selected review file is not part of this item");
        selection = "\nCurrently displayed file: " + path;
        if (hunk) {
            std::vector<std::size_t> offsets;
            for (std::size_t offset = 0; offset < file->patch.size();) {
                if (file->patch.compare(offset, 3, "@@ ") == 0) offsets.push_back(offset);
                const auto next = file->patch.find('\n', offset);
                if (next == std::string::npos) break;
                offset = next + 1;
            }
            if (offsets.empty()) offsets.push_back(0); // Binary or metadata-only diff.
            if (*hunk >= offsets.size()) throw std::invalid_argument("Invalid selected review hunk");
            const auto end = *hunk + 1 < offsets.size() ? offsets[*hunk + 1] : file->patch.size();
            selection += "\nCurrently displayed hunk (zero-based " + std::to_string(*hunk) + "):\n" + file->patch.substr(offsets[*hunk], end - offsets[*hunk]);
        }
    } else if (hunk) throw std::invalid_argument("A selected review hunk requires a file");
    return prompts::implementationPhaseAnnouncement(ImplementationPhase::REVIEW) +
        "\nInspected step " + std::to_string(step + 1) + ": " + plan->steps.at(step).title +
        "\nInspected item: " + (substep == steps[step].substeps.size() ? "Step summary" : plan->steps[step].substeps[substep].title) +
        selection + "\nExplanation:\n" + change.explanation + "\nExact preserved diffs (JSON evidence):\n" + Json(change.diffs).dump() +
        "\nAccepted implementation plan:\n" + plan->markdown() +
        "\nAccepted plan JSON (copy the validated prefix exactly when revising):\n" + Json(*plan).dump() +
        "\nReview cursor (zero-based): " + std::to_string(reviewStep) + "; inspected step: " + std::to_string(step) +
        "\nAccepted step identities and substep counts: " + [&] { Json result = Json::array(); for (std::size_t i = 0; i < steps.size(); ++i) result.push_back({{"step", i}, {"identity", steps[i].identity}, {"substeps", steps[i].substeps.size()}}); return result.dump(); }() +
        (revisionPlanning ? "\nA revised Implementation Plan is already open. Keep this step review informative and read-only; changes to the candidate belong to its separate planning discussion. Do not propose local rework here." : canRework(step) ? "\nThe user CAN request changes here. Read-only review prohibits direct file edits, NOT structured proposals. For an explicit modification request, call propose_step_rework if the accepted plan remains accurate, or propose_implementation_revision with a complete candidate plan if it no longer is. Do not tell the user to leave Implement or start another conversation. Do not add ritual confirmation before proposing: the UI obtains execution authorization. Questions alone do not authorize changes. Validated steps stay immutable; needed corrections to their behavior belong to this unvalidated step." :
            "\nThis selection cannot be reworked directly. Validated history is immutable. If changes are needed, direct the user to review the first unvalidated step within this Implement workspace, not to leave it. Answer questions read-only.");
}
