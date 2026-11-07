#include "turn_files.hpp"
#include "../session/session_journal.hpp"
#include <agent-runtime/tools/fs/common.hpp>
#include <openssl/sha.h>
#include <fstream>
#include <map>
#include <chrono>
#include <set>
#include <algorithm>
#include <sstream>
#include <agent-runtime/core/process.hpp>

namespace {
namespace fs = std::filesystem;
bool within(const fs::path& path, const fs::path& root) {
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}
std::string hash(const std::string& bytes) {
    unsigned char value[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), value);
    constexpr char HEX[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : value) { result += HEX[byte >> 4]; result += HEX[byte & 15]; }
    return result;
}
void requireSafe(const fs::path& path) {
    if (!path.is_absolute() || fs::weakly_canonical(path.parent_path()) != path.parent_path())
        throw std::runtime_error("Undo path was redirected: " + path.string());
}
std::string readBytes(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot read undo file: " + path.string());
    std::string bytes(std::istreambuf_iterator<char>(stream), {});
    if (stream.bad()) throw std::runtime_error("Cannot read undo file: " + path.string());
    return bytes;
}
void rejectConflicts(const std::vector<fs::path>& conflicts) {
    if (conflicts.empty()) return;
    std::string message = "Undo/redo conflicts; no history changes were applied:";
    for (const auto& path : conflicts) message += "\n" + path.string();
    throw std::runtime_error(message);
}
std::map<fs::path, SnapshotEdit> collapse(const std::vector<SnapshotEdit>& edits) {
    std::map<fs::path, SnapshotEdit> result;
    for (const auto& edit : edits) {
        if (!edit.complete) throw std::runtime_error("An interrupted file mutation has no verified outcome: " + edit.path);
        const auto [found, inserted] = result.emplace(edit.path, edit);
        if (inserted) continue;
        if (found->second.after != edit.before)
            throw std::runtime_error("Non-tool changes between edits prevent undo/redo: " + edit.path);
        found->second.after = edit.after;
    }
    return result;
}
}

TurnFiles::TurnFiles(fs::path directory) : directory(std::move(directory)), blobs(this->directory / "undo-blobs") {}

TurnSnapshot TurnFiles::prepare(const fs::path& workspace, const fs::path& authorizedRoot) {
    TurnSnapshot snapshot;
    if (const auto root = GitSnapshot::discover(workspace)) {
        if (!authorizedRoot.empty() && !within(*root, fs::canonical(authorizedRoot)))
            throw std::runtime_error("Git snapshot root is outside the authorized workspace. Start SOS at the repository root: " + root->string());
        git = std::make_unique<GitSnapshot>(*root);
        snapshot.root = root->string();
        snapshot.before = git->capture().tree;
        snapshot.after = snapshot.before; // Current state until the first operation starts.
    }
    return snapshot;
}

std::int64_t TurnFiles::begin(TurnSnapshot snapshot) {
    turn = SessionJournal(directory).beginTurn(std::move(snapshot));
    return turn.id;
}

SnapshotFileState TurnFiles::captureFile(const fs::path& path) const {
    requireSafe(path);
    const auto status = fs::symlink_status(path);
    if (!fs::exists(status)) return {};
    if (!fs::is_regular_file(status) && !fs::is_symlink(status)) throw std::runtime_error("Unsupported undo file: " + path.string());
    const bool link = fs::is_symlink(status);
    const auto bytes = link ? fs::read_symlink(path).native() : readBytes(path);
    const auto id = hash(bytes);
    fs::create_directories(blobs);
    fs::permissions(blobs, fs::perms::owner_all);
    if (!fs::exists(blobs / id)) {
        fs_tools::writeText(blobs / (id + ".tmp"), bytes);
        fs::rename(blobs / (id + ".tmp"), blobs / id);
    }
    return {true, id, link ? 0u : static_cast<unsigned>(status.permissions()), link ? "symlink" : "file"};
}

void TurnFiles::mutate(const std::vector<fs::path>& paths, const std::function<void()>& action,
    const std::function<void(const TurnSnapshot&)>& captured) {
    const auto first = turn.edits.size();
    std::set<fs::path> actual;
    for (auto path : paths) {
        for (unsigned depth = 0; ; ++depth) {
            if (depth == 40) throw std::runtime_error("Too many symlinks in undo path");
            path = fs::weakly_canonical(path.parent_path()) / path.filename();
            actual.insert(path);
            if (!fs::is_symlink(fs::symlink_status(path))) break;
            const auto target = fs::read_symlink(path);
            path = (target.is_absolute() ? target : path.parent_path() / target).lexically_normal();
        }
    }
    for (const auto& path : actual) {
        if (git && git->covers(path)) continue;
        turn.edits.push_back({path.string(), captureFile(path), {}, false});
    }
    SessionJournal store(directory);
    // Never use a stale checkpoint if execution or capture crashes mid-operation.
    if (git) turn.after.clear();
    store.saveTurn(turn); // Durable before the first filesystem mutation.
    std::exception_ptr failure;
    try { action(); } catch (...) { failure = std::current_exception(); }
    for (auto i = first; i < turn.edits.size(); ++i) {
        turn.edits[i].after = captureFile(turn.edits[i].path);
        turn.edits[i].complete = true;
    }
    checkpoint(); // Includes partial outcomes, even when the action threw.
    if (captured) {
        auto verified = turn;
        verified.complete = true;
        captured(verified); // The mutation lock is still held; never acquire it again via boundary().
    }
    if (failure) std::rethrow_exception(failure);
}

void TurnFiles::checkpoint() {
    if (git) turn.after = git->capture().tree;
    SessionJournal(directory).saveTurn(turn);
}

void TurnFiles::finish() {
    turn.complete = true;
    checkpoint();
}

void TurnFiles::switchWorkspace(const fs::path& workspace, const fs::path& root) {
    // The agent worker waits for this operation, so no tool can mutate concurrently.
    TurnFiles next(directory);
    auto prepared = next.prepare(workspace, root);
    checkpoint();
    auto replacement = turn;
    if (!turn.root.empty()) replacement.workspaces.push_back({turn.root, turn.before, turn.after});
    const auto previous = std::find_if(replacement.workspaces.begin(), replacement.workspaces.end(),
        [&](const auto& snapshot) { return snapshot.root == prepared.root; });
    if (previous != replacement.workspaces.end()) {
        if (previous->after != prepared.before)
            throw std::runtime_error("Worktree changed outside this turn; refusing to merge undo boundaries");
        prepared.before = previous->before;
        replacement.workspaces.erase(previous);
    }
    replacement.root = prepared.root;
    replacement.before = prepared.before;
    replacement.after = prepared.after;
    SessionJournal(directory).saveTurn(replacement);
    turn = std::move(replacement);
    git = std::move(next.git);
}

std::vector<ImplementationDiff> TurnFiles::diffs(const TurnSnapshot& snapshot) const {
    if (!snapshot.complete || (!snapshot.root.empty() && snapshot.after.empty()))
        throw std::runtime_error("Implementation snapshot has no verified final state");
    std::vector<ImplementationDiff> result;
    if (!snapshot.root.empty()) result = GitSnapshot::archive(snapshot.root).diffs({snapshot.before}, {snapshot.after});
    for (const auto& workspace : snapshot.workspaces) {
        auto extra = GitSnapshot::archive(workspace.root).diffs({workspace.before}, {workspace.after});
        result.insert(result.end(), extra.begin(), extra.end());
    }
    for (const auto& [path, edit] : collapse(snapshot.edits)) {
        if (edit.before == edit.after) continue;
        const auto before = edit.before.exists ? fileBytes(edit.before) : "";
        const auto after = edit.after.exists ? fileBytes(edit.after) : "";
        bool binary = before.find('\0') != std::string::npos || after.find('\0') != std::string::npos;
        try { (void)nlohmann::json({before, after}).dump(); }
        catch (const std::exception&) { binary = true; }
        std::string patch = "--- " + path.string() + " (before)\n+++ " + path.string() + " (after)\n";
        if (binary)
            patch += "Binary contents changed (preserved in snapshot blobs).\n";
        else {
            patch = fs_tools::diff(path, before, after);
        }
        if (edit.before.mode != edit.after.mode || edit.before.kind != edit.after.kind)
            patch += "File kind/mode: " + edit.before.kind + ":" + std::to_string(edit.before.mode) + " -> " +
                edit.after.kind + ":" + std::to_string(edit.after.mode) + "\n";
        result.push_back({path.string(), std::move(patch), binary ? "" : before, binary ? "" : after});
    }
    return result;
}

std::vector<ImplementationDiff> TurnFiles::inspect(const std::optional<TurnSnapshot>& baseline) const {
    auto snapshot = boundary();
    if (baseline) {
        snapshot.before = baseline->before;
        snapshot.edits.insert(snapshot.edits.begin(), baseline->edits.begin(), baseline->edits.end());
    }
    return diffs(snapshot);
}

TurnSnapshot TurnFiles::boundary() const {
    std::lock_guard lock(fs_tools::mutationMutex());
    SessionJournal journal(blobs.parent_path());
    auto snapshot = journal.navigationTurn(false);
    if (!snapshot) throw std::logic_error("No active implementation snapshot");
    if (!snapshot->root.empty()) snapshot->after = GitSnapshot(snapshot->root).capture().tree;
    for (auto& edit : snapshot->edits) if (!edit.complete) {
        edit.after = captureFile(edit.path);
        edit.complete = true;
    }
    snapshot->complete = true; // Inspection only; never publish this provisional boundary.
    return *snapshot;
}

std::string TurnFiles::fileBytes(const SnapshotFileState& state) const {
    if (!state.exists) return {};
    if (state.kind != "file" && state.kind != "symlink") throw std::runtime_error("Invalid undo file kind");
    if (state.blob.size() != 64 || state.blob.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::runtime_error("Invalid undo blob ID");
    const auto bytes = readBytes(blobs / state.blob);
    if (hash(bytes) != state.blob) throw std::runtime_error("Corrupt undo blob");
    return bytes;
}

void TurnFiles::restoreFile(const fs::path& path, const SnapshotFileState& state) const {
    requireSafe(path);
    if (!state.exists) { fs::remove(path); return; }
    const auto bytes = fileBytes(state);
    fs::create_directories(path.parent_path());
    const auto temporary = path.parent_path() / (".sos-undo-" + state.blob);
    if (fs::exists(fs::symlink_status(temporary))) throw std::runtime_error("Undo temporary file already exists: " + temporary.string());
    if (state.kind == "symlink") fs::create_symlink(bytes, temporary);
    else {
        fs_tools::writeText(temporary, bytes);
        fs::permissions(temporary, static_cast<fs::perms>(state.mode));
    }
    fs::rename(temporary, path);
}

void TurnFiles::restoreImplementation(const TurnSnapshot& snapshot, bool forward) const {
    if (snapshot.root.empty() || snapshot.before.empty() || snapshot.after.empty() || !snapshot.workspaces.empty())
        throw std::runtime_error("Revision has no verified isolated snapshot");
    const auto edits = collapse(snapshot.edits);
    std::lock_guard lock(fs_tools::mutationMutex());
    GitSnapshot git(snapshot.root);
    const GitSnapshotId expected{forward ? snapshot.before : snapshot.after}, target{forward ? snapshot.after : snapshot.before};
    auto conflicts = git.conflicts(expected, target, true);
    for (const auto& [path, edit] : edits) {
        if (!within(path, fs::path(snapshot.root))) throw std::runtime_error("Revision cannot restore files outside its isolated checkout");
        const auto current = captureFile(path);
        if (current != edit.before && current != edit.after) conflicts.push_back(path);
        (void)fileBytes(forward ? edit.after : edit.before);
    }
    rejectConflicts(conflicts);
    rejectConflicts(git.restore(expected, target, true));
    for (const auto& [path, edit] : edits) {
        const auto& state = forward ? edit.after : edit.before;
        if (captureFile(path) != state) restoreFile(path, state);
    }
}

TurnSnapshot TurnFiles::prepareContribution(const TurnSnapshot& original, const std::vector<ImplementationDiff>& diffs) const {
    GitSnapshot git(original.root);
    const auto contribution = git.contributionTrees({original.before}, {original.after}, diffs);
    const auto prepared = git.prepareImport(contribution.first, contribution.second);
    TurnSnapshot intent; intent.root = original.root; intent.before = prepared.first.tree; intent.after = prepared.second.tree; intent.complete = true;
    const auto originals = collapse(original.edits);
    for (const auto& diff : diffs) {
        const fs::path path(diff.path);
        if (!path.is_absolute()) continue;
        if (!within(path, fs::path(original.root)) || !originals.contains(path)) throw std::runtime_error("Revision contribution has no isolated file evidence");
        const auto current = captureFile(path);
        auto before = originals.at(path).before, after = originals.at(path).after;
        const auto stateFor = [&](const std::string& text, SnapshotFileState state) {
            if (!state.exists) return SnapshotFileState{};
            state.blob = hash(text); fs::create_directories(blobs); fs_tools::writeText(blobs / state.blob, text); return state;
        };
        const bool opaque = diff.patch.find("Binary") != std::string::npos;
        if (!opaque) {
            if (!diff.before.empty()) { before.exists = true; before.kind = "file"; if (!before.mode) before.mode = 0644; }
            if (!diff.after.empty()) { after.exists = true; after.kind = "file"; if (!after.mode) after.mode = 0644; }
            before = stateFor(diff.before, before); after = stateFor(diff.after, after);
        }
        if (current == after) continue;
        if (current != before) {
            if (opaque || !current.exists || !before.exists || !after.exists || current.kind != "file" || before.kind != "file" || after.kind != "file")
                throw std::runtime_error("Revision replay conflict: " + path.string());
            const auto ours = directory / "revision-ours", base = directory / "revision-base", theirs = directory / "revision-theirs";
            fs_tools::writeText(ours, fileBytes(current)); fs_tools::writeText(base, fileBytes(before)); fs_tools::writeText(theirs, fileBytes(after));
            const auto merged = core::runProcess("git", {"merge-file", "-p", ours.string(), base.string(), theirs.string()}, directory);
            if (merged.exitCode) throw std::runtime_error("Revision replay conflict: " + path.string());
            after = stateFor(merged.output, after);
        }
        intent.edits.push_back({path.string(), current, after, true});
    }
    return intent;
}

void TurnFiles::navigate(const fs::path& directory, bool redo) {
    SessionJournal store(directory);
    auto selected = store.navigationTurn(redo);
    if (!selected) throw std::runtime_error(redo ? "Nothing to redo" : "No undo snapshot is available for this turn");
    auto turn = std::move(*selected);
    const bool resume = !turn.operation.empty();
    if (!turn.root.empty() && turn.after.empty())
        throw std::runtime_error("The interrupted turn has no verified current Git snapshot; its file outcome cannot be verified safely");
    for (const auto& workspace : turn.workspaces)
        if (workspace.after.empty()) throw std::runtime_error("A previous workspace has no verified undo snapshot");
    auto info = store.info().value();
    if (!resume && !redo) {
        const auto active = store.readFrom(turn.start);
        if (active.empty()) throw std::runtime_error("No recorded user turn to undo");
        // Older untracked user turns must never be swallowed by the latest supported undo.
        const auto users = std::count_if(active.begin(), active.end(), [](const auto& message) {
            return message.role == MessageRole::USER && !message.recap;
        });
        if (users != 1) throw std::runtime_error("The last turn has no complete undo boundary");
        turn.messages = active;
    }
    TurnFiles files(directory);
    const auto edits = collapse(turn.edits);
    std::unique_ptr<GitSnapshot> git;
    GitSnapshotId expected{redo ? turn.before : turn.after}, target{redo ? turn.after : turn.before};
    std::lock_guard lock(fs_tools::mutationMutex());
    std::vector<fs::path> conflicts;
    if (!turn.root.empty()) {
        git = std::make_unique<GitSnapshot>(turn.root);
        conflicts = git->conflicts(expected, target, resume);
    }
    for (const auto& workspace : turn.workspaces) {
        const auto extra = GitSnapshot(workspace.root).conflicts(
            {redo ? workspace.before : workspace.after}, {redo ? workspace.after : workspace.before}, resume);
        conflicts.insert(conflicts.end(), extra.begin(), extra.end());
    }
    for (const auto& [path, edit] : edits) {
        try {
            const auto current = files.captureFile(path);
            if (current != (redo ? edit.before : edit.after) && (!resume || current != (redo ? edit.after : edit.before)))
                conflicts.push_back(path);
        } catch (const std::exception&) { conflicts.push_back(path); }
    }
    rejectConflicts(conflicts);
    for (const auto& [path, edit] : edits) (void)files.fileBytes(redo ? edit.after : edit.before);
    turn.operation = redo ? "redo" : "undo";
    store.saveTurn(turn); // Restart can finish this intent if filesystem publication was interrupted.
    if (git) rejectConflicts(git->restore(expected, target, resume));
    for (const auto& workspace : turn.workspaces)
        rejectConflicts(GitSnapshot(workspace.root).restore(
            {redo ? workspace.before : workspace.after}, {redo ? workspace.after : workspace.before}, resume));
    for (const auto& [path, edit] : edits) {
        const auto& target = redo ? edit.after : edit.before;
        if (files.captureFile(path) != target) files.restoreFile(path, target);
    }
    turn.undone = !redo;
    turn.operation.clear();
    info.context = {};
    info.mode = parseExecutionMode(redo ? turn.modeAfter : turn.modeBefore);
    info.providerContextStart = redo ? turn.start + turn.messages.size() : turn.start;
    info.updatedAt = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    store.navigate(turn, info);
}
