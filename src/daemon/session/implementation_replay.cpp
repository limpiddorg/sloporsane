#include "implementation_worktree.hpp"
#include "../snapshots/turn_files.hpp"

using Json = nlohmann::json;

void ImplementationWorktree::revise(std::size_t from, std::uint64_t token, const TurnSnapshot& rollback) {
    executionDirectory();
    if (rollback.root != worktree.string()) throw std::runtime_error("Revision snapshot is outside the execution checkout");
    SessionDatabase::Statement imported(db, "SELECT step FROM imports WHERE step>=? LIMIT 1");
    imported.bind(1, static_cast<std::int64_t>(from));
    if (imported.step()) throw std::runtime_error("Resolve the pending import before revising; imported steps cannot be rewritten");
    bool applied = false;
    {
        SessionDatabase::Statement existing(db, "SELECT first_step,payload,applied FROM revisions WHERE token=?");
        existing.bind(1, static_cast<std::int64_t>(token));
        if (existing.step()) {
            if (existing.integer(0) != static_cast<std::int64_t>(from) || existing.text(1) != Json(rollback).dump()) throw std::runtime_error("Revision intent changed");
            applied = existing.integer(2);
        } else {
            SessionDatabase::Statement insert(db, "INSERT INTO revisions(token,first_step,payload) VALUES(?,?,?)");
            insert.bind(1, static_cast<std::int64_t>(token)); insert.bind(2, static_cast<std::int64_t>(from)); insert.bind(3, Json(rollback).dump()); insert.step();
        }
    }
    if (applied) return;
    TurnFiles(storageDirectory).restoreImplementation(rollback);
    SessionDatabase::Transaction transaction(db);
    SessionDatabase::Statement invalidate(db, "DELETE FROM step_commits WHERE step>=?");
    invalidate.bind(1, static_cast<std::int64_t>(from)); invalidate.step();
    SessionDatabase::Statement done(db, "UPDATE revisions SET applied=1 WHERE token=?");
    done.bind(1, static_cast<std::int64_t>(token)); done.step(); transaction.commit();
    // Old commits stay reachable through SOS history. Neither the user index nor branch is changed.
}

namespace {
struct ReplayJournal {
    ImplementationRevisionSeed seed;
    std::size_t cursor = 0;
    std::optional<TurnSnapshot> intent, interval;
};
void to_json(Json& json, const ReplayJournal& journal) {
    json = {{"seed", journal.seed}, {"cursor", journal.cursor}, {"intent", journal.intent ? Json(*journal.intent) : Json(nullptr)}, {"interval", journal.interval ? Json(*journal.interval) : Json(nullptr)}};
}
void from_json(const Json& json, ReplayJournal& journal) {
    json.at("seed").get_to(journal.seed); json.at("cursor").get_to(journal.cursor);
    if (!json.at("intent").is_null()) journal.intent = json.at("intent").get<TurnSnapshot>();
    if (!json.at("interval").is_null()) journal.interval = json.at("interval").get<TurnSnapshot>();
}
}

ImplementationRevisionSeed ImplementationWorktree::replay(std::size_t step, std::uint64_t generation,
    const ImplementationRevisionSource& source, bool preserveExisting) {
    executionDirectory();
    const auto original = source.state.substeps.front().snapshot.value();
    if (original.root != worktree.string() || source.owners.size() != source.state.substeps.size()) throw std::runtime_error("Invalid preserved contribution");
    ReplayJournal journal;
    {
        SessionDatabase::Statement read(db, "SELECT payload FROM revision_replays WHERE token=? AND step=?");
        read.bind(1, static_cast<std::int64_t>(generation)); read.bind(2, static_cast<std::int64_t>(step));
        if (read.step()) journal = Json::parse(read.text(0)).get<ReplayJournal>();
        else {
            journal.seed.snapshot.root = worktree.string(); journal.seed.snapshot.complete = true;
            journal.seed.snapshot.before = journal.seed.snapshot.after = GitSnapshot(worktree).capture().tree;
            if (preserveExisting) journal.seed.snapshot = original;
        }
    }
    const auto save = [&] {
        SessionDatabase::Statement write(db, "INSERT INTO revision_replays(token,step,payload) VALUES(?,?,?) ON CONFLICT(token,step) DO UPDATE SET payload=excluded.payload");
        write.bind(1, static_cast<std::int64_t>(generation)); write.bind(2, static_cast<std::int64_t>(step)); write.bind(3, Json(journal).dump()); write.step();
    };
    TurnFiles files(storageDirectory);
    while (journal.cursor < source.state.substeps.size()) {
        const auto index = journal.cursor;
        ImplementationBoundary boundary; boundary.owner = source.owners[index]; boundary.validating = true;
        const auto& old = source.state.substeps[index];
        boundary.progress = {source.owners[index], old.complete || old.finalized || old.unnecessary ? "completed" : "correcting", old.explanation};
        if (preserveExisting) boundary.diffs = source.state.substeps[index].diffs;
        else {
            if (!journal.intent) {
                journal.interval = journal.seed.snapshot; journal.interval->before = journal.interval->after; journal.interval->edits.clear();
                try {
                    journal.intent = files.prepareContribution(original, source.state.substeps[index].diffs);
                    save(); // Durable before any replayed file mutation.
                } catch (const std::runtime_error& error) {
                    const std::string message = error.what();
                    if (!message.starts_with("Cannot import this step because") && !message.starts_with("Revision replay conflict:")) throw;
                    journal.seed.conflicts += "Substep " + std::to_string(source.owners[index] + 1) + ": " + message + "\n";
                    journal.interval.reset();
                }
            }
            if (journal.intent) {
                files.restoreImplementation(*journal.intent, true);
                auto interval = *journal.interval; interval.after = GitSnapshot(worktree).capture().tree; interval.edits = journal.intent->edits;
                boundary.diffs = files.diffs(interval);
                journal.seed.snapshot.after = interval.after;
                journal.seed.snapshot.edits.insert(journal.seed.snapshot.edits.end(), interval.edits.begin(), interval.edits.end());
                journal.intent.reset(); journal.interval.reset();
            }
        }
        boundary.snapshot = journal.seed.snapshot;
        journal.seed.boundaries.push_back(std::move(boundary)); ++journal.cursor; save();
    }
    return journal.seed;
}
