#include "implementation_boundaries.hpp"
#include <algorithm>

using Json = nlohmann::json;

namespace {
std::optional<std::size_t> optionalIndex(const Json& json, const char* key) {
    return json.contains(key) && !json.at(key).is_null() ? std::optional(json.at(key).get<std::size_t>()) : std::nullopt;
}
Json indexJson(const std::optional<std::size_t>& index) { return index ? Json(*index) : Json(nullptr); }
}

void to_json(Json& json, const ImplementationProgress& progress) {
    json = {{"substep", indexJson(progress.substep)}, {"status", progress.status}, {"explanation", progress.explanation}};
    if (progress.completion) json["completion"] = *progress.completion;
}
void from_json(const Json& json, ImplementationProgress& progress) {
    progress = {optionalIndex(json, "substep"), json.at("status"), json.value("explanation", "")};
    if (json.contains("completion")) progress.completion = json.at("completion").get<ImplementationCompletion>();
}
void to_json(Json& json, const ImplementationBoundary& boundary) {
    json = {{"progress", boundary.progress}, {"snapshot", boundary.snapshot}, {"owner", indexJson(boundary.owner)},
        {"active_substep", indexJson(boundary.activeSubstep)}, {"validating", boundary.validating}, {"diffs", boundary.diffs}};
}
void from_json(const Json& json, ImplementationBoundary& boundary) {
    json.at("progress").get_to(boundary.progress);
    json.at("snapshot").get_to(boundary.snapshot);
    boundary.owner = optionalIndex(json, "owner");
    boundary.activeSubstep = optionalIndex(json, "active_substep");
    boundary.validating = json.at("validating");
    json.at("diffs").get_to(boundary.diffs);
}

ImplementationBoundaries::ImplementationBoundaries(const std::filesystem::path& directory)
    : directory(directory), db(directory / "implementation.sqlite") {
    // Validate existing storage before creating anything: this connection is
    // constructed before ImplementationRun gets to decode its state.
    SessionDatabase::Statement stateTable(db, "SELECT 1 FROM sqlite_schema WHERE type='table' AND name='implementation_state'");
    if (stateTable.step()) {
        SessionDatabase::Statement state(db, "SELECT payload FROM implementation_state WHERE id=1");
        if (state.step() && Json::parse(state.text(0)).at("version") != 7)
            throw std::runtime_error("Unsupported implementation state");
    }
    bool exists = false, generation = false;
    { SessionDatabase::Statement columns(db, "PRAGMA table_info(implementation_boundaries)");
      while (columns.step()) { exists = true; generation |= columns.text(1) == "generation"; } }
    if (exists && !generation) throw std::runtime_error("Unsupported implementation boundaries");
    db.execute("CREATE TABLE IF NOT EXISTS implementation_boundaries (id INTEGER PRIMARY KEY, step INTEGER NOT NULL, payload TEXT NOT NULL, generation INTEGER NOT NULL)");
}

std::vector<ImplementationBoundary> ImplementationBoundaries::read(std::size_t step, std::uint64_t generation) const {
    SessionDatabase::Statement query(db, "SELECT id,payload FROM implementation_boundaries WHERE step=? AND generation=? ORDER BY id");
    query.bind(1, static_cast<std::int64_t>(step));
    query.bind(2, static_cast<std::int64_t>(generation));
    std::vector<ImplementationBoundary> result;
    while (query.step()) {
        auto boundary = Json::parse(query.text(1)).get<ImplementationBoundary>();
        boundary.id = query.integer(0);
        result.push_back(std::move(boundary));
    }
    return result;
}

void ImplementationBoundaries::requireActiveSubstep(std::size_t step, bool command, std::uint64_t generation) const {
    const auto evidence = read(step, generation);
    // Shell checks have no declared file paths. Permit them during technical
    // validation, but retain snapshot checks against unowned source changes.
    if (!evidence.empty() && command && evidence.back().validating && evidence.back().progress.status != "finished") return;
    if (evidence.empty() || !evidence.back().activeSubstep || evidence.back().progress.status == "finished")
        throw std::logic_error("Before editing, report started for a construction substep or correcting for its validation repair target");
}

void ImplementationBoundaries::advance(ImplementationBoundary& boundary, std::size_t count,
    const std::vector<ImplementationBoundary>& previous) const {
    const auto& progress = boundary.progress;
    const auto& status = progress.status;
    if (progress.substep && *progress.substep >= count) throw std::invalid_argument("Unknown planned substep");
    if (status == "validating" || status == "finished") {
        if (progress.substep) throw std::invalid_argument("Step validation has no substep index");
        if (progress.completion && progress.completion->substeps.size() != count)
            throw std::invalid_argument("Completion must describe every planned substep in order");
        std::vector<bool> announced(count);
        for (const auto& entry : previous) if (entry.progress.substep) {
            const auto& reported = entry.progress.status;
            if (reported == "started") announced[*entry.progress.substep] = false;
            if (reported == "completed" || reported == "unnecessary") announced[*entry.progress.substep] = true;
        }
        if (std::any_of(announced.begin(), announced.end(), [](bool done) { return !done; }))
            throw std::logic_error("Complete or explicitly skip every planned substep before validating the step");
        if (status == "finished" && !boundary.validating)
            throw std::logic_error("Start step validation before completing the step; perform checks there if needed");
        boundary.validating = true;
        boundary.activeSubstep.reset();
    } else if (status == "started" || status == "correcting") {
        if (!progress.substep) throw std::invalid_argument("A construction or correction needs its target substep");
        if ((status == "correcting") != boundary.validating)
            throw std::logic_error("Use started during construction, or correcting during step validation");
        boundary.activeSubstep = progress.substep;
    } else if (status == "completed" || status == "unnecessary") {
        if (!progress.substep) throw std::invalid_argument("A substep announcement needs an index");
        if (status == "completed" && boundary.activeSubstep != progress.substep)
            throw std::logic_error("Start the substep or its correction before completing it");
        if (boundary.activeSubstep == progress.substep) boundary.activeSubstep.reset();
    } else throw std::invalid_argument("Unknown implementation progress status");
}

std::int64_t ImplementationBoundaries::capture(std::size_t step, std::size_t count,
    const ImplementationProgress& requested, const TurnSnapshot& snapshot, std::uint64_t generation, std::int64_t progressAfter,
    const std::vector<std::string>& mutationPaths, bool autoAdvance) {
    SessionDatabase::Transaction transaction(db);
    const auto previous = read(step, generation);
    auto progress = requested;
    if (progress.status == "activate") {
        // Internal turn setup, not a model announcement. Resume existing evidence
        // unchanged; revisions still require an explicit correction target.
        if (!autoAdvance || !previous.empty()) return previous.empty() ? 0 : previous.back().id;
        if (!count) throw std::invalid_argument("Cannot activate an empty step");
        progress = {0, "started", "Activated from the accepted plan."};
    }
    ImplementationBoundary boundary;
    boundary.progress = progress;
    boundary.snapshot = snapshot;
    auto interval = snapshot;
    if (!previous.empty()) {
        const auto& last = previous.back();
        if (last.progress.status == "finished" && snapshot.id == last.snapshot.id)
            throw std::logic_error("The step has already been submitted in this turn");
        boundary.owner = boundary.activeSubstep = last.activeSubstep;
        boundary.validating = last.validating;
        if (snapshot.root != last.snapshot.root) throw std::runtime_error("Implementation snapshot root changed");
        interval.before = last.snapshot.after;
        if (snapshot.edits.size() < last.snapshot.edits.size() ||
            Json(std::vector<SnapshotEdit>(snapshot.edits.begin(), snapshot.edits.begin() + last.snapshot.edits.size())) != Json(last.snapshot.edits))
            throw std::runtime_error("Implementation edit evidence was truncated or changed");
        interval.edits.erase(interval.edits.begin(), interval.edits.begin() + last.snapshot.edits.size());
    }
    boundary.diffs = TurnFiles(directory).diffs(interval);
    if (progress.status == "mutation" && !progress.explanation.empty()) for (auto& diff : boundary.diffs) {
        std::size_t hunkCount = diff.patch.starts_with("@@ -") ? 1 : 0;
        for (std::size_t at = 0; (at = diff.patch.find("\n@@ -", at)) != std::string::npos; at += 5) ++hunkCount;
        diff.hunkExplanations.assign(std::max(std::size_t{1}, hunkCount), progress.explanation);
    }
    if (progress.status == "mutation" && boundary.diffs.empty()) return previous.empty() ? 0 : previous.back().id;
    if (!mutationPaths.empty()) {
        std::vector<std::filesystem::path> ordered;
        for (const auto& name : mutationPaths) {
            const std::filesystem::path path(name);
            ordered.push_back(std::filesystem::weakly_canonical(path.parent_path()) / path.filename());
        }
        const auto rank = [&](const std::string& path) {
            const auto absolute = std::filesystem::path(path).is_absolute() ? std::filesystem::path(path) : std::filesystem::path(snapshot.root) / path;
            return std::find(ordered.begin(), ordered.end(), absolute.lexically_normal()) - ordered.begin();
        };
        std::stable_sort(boundary.diffs.begin(), boundary.diffs.end(), [&](const auto& left, const auto& right) {
            return rank(left.path) < rank(right.path);
        });
    }
    auto currentProgress = previous;
    if (progressAfter) {
        std::erase_if(currentProgress, [&](const auto& entry) { return entry.id <= progressAfter; });
        bool validating = false;
        for (auto& entry : currentProgress) {
            if (entry.progress.status == "validating") validating = true;
            if (entry.progress.status == "correcting" && !validating) entry.progress.status = "started";
        }
        if (progress.status == "finished" && !validating)
            throw std::logic_error("Start technical validation for this revised result before submitting it");
    }
    if (progress.status != "mutation") advance(boundary, count, currentProgress);
    // A failed validation capture can leave real changes without an owner. A
    // validated, explicit repair target must be able to recover that pending
    // interval; otherwise every correcting announcement repeats the same refusal.
    // Never reassign contributions already owned by an active substep.
    if (!boundary.diffs.empty() && !boundary.owner && progress.status == "correcting")
        boundary.owner = boundary.activeSubstep;
    if (!boundary.diffs.empty() && !boundary.owner)
        throw std::logic_error("Declare the active substep before editing; during validation use correcting with its target substep");
    const auto append = [&](const ImplementationBoundary& entry) {
        SessionDatabase::Statement insert(db, "INSERT INTO implementation_boundaries(step,payload,generation) VALUES(?,?,?) RETURNING id");
        insert.bind(1, static_cast<std::int64_t>(step));
        insert.bind(2, Json(entry).dump());
        insert.bind(3, static_cast<std::int64_t>(generation));
        insert.step();
        const auto id = insert.integer(0);
        insert.step();
        return id;
    };
    auto id = append(boundary);
    if (autoAdvance && !boundary.validating && (progress.status == "completed" || progress.status == "unnecessary")) {
        currentProgress.push_back(boundary);
        std::vector<bool> done(count);
        for (const auto& entry : currentProgress) if (entry.progress.substep) {
            const auto& status = entry.progress.status;
            if (status == "started" || status == "correcting") done[*entry.progress.substep] = false;
            if (status == "completed" || status == "unnecessary") done[*entry.progress.substep] = true;
        }
        const auto next = std::find(done.begin(), done.end(), false);
        ImplementationBoundary transition;
        transition.snapshot = snapshot;
        transition.validating = boundary.validating;
        transition.activeSubstep = boundary.activeSubstep;
        transition.progress = next == done.end()
            ? ImplementationProgress{std::nullopt, "validating", "Construction finished; perform additional checks only if needed."}
            : ImplementationProgress{static_cast<std::size_t>(next - done.begin()), "started", "Activated from the accepted plan."};
        advance(transition, count, currentProgress);
        // Same snapshot: this transition owns no edits and is atomic with completion.
        id = append(transition);
    }
    transaction.commit();
    return id;
}

void ImplementationBoundaries::seed(std::size_t step, std::uint64_t generation, const std::vector<ImplementationBoundary>& evidence) {
    SessionDatabase::Transaction transaction(db);
    if (!read(step, generation).empty()) return; // Idempotent after interrupted publication.
    for (const auto& boundary : evidence) {
        SessionDatabase::Statement insert(db, "INSERT INTO implementation_boundaries(step,payload,generation) VALUES(?,?,?)");
        insert.bind(1, static_cast<std::int64_t>(step)); insert.bind(2, Json(boundary).dump());
        insert.bind(3, static_cast<std::int64_t>(generation)); insert.step();
    }
    transaction.commit();
}
