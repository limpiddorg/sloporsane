#include "implementation_worktree.hpp"
#include "worktree_listing.hpp"
#include "../snapshots/git_snapshot.hpp"
#include <agent-runtime/common/path_resolution.hpp>
#include <agent-runtime/core/process.hpp>
#include <fstream>

extern char** environ;
namespace fs = std::filesystem;
using Json = nlohmann::json;

ImplementationWorktree::ImplementationWorktree(const fs::path& directory, GitProcessRunner runner)
    : storageDirectory(directory), runGitProcess(std::move(runner)), db(directory / "worktree.sqlite") {
    db.execute("CREATE TABLE IF NOT EXISTS checkout (id INTEGER PRIMARY KEY CHECK(id=1), payload TEXT NOT NULL)");
    db.execute("CREATE TABLE IF NOT EXISTS step_commits (step INTEGER PRIMARY KEY, tree TEXT NOT NULL, revision TEXT NOT NULL)");
    db.execute("CREATE TABLE IF NOT EXISTS imports (step INTEGER PRIMARY KEY, source_before TEXT NOT NULL, source_after TEXT NOT NULL, expected TEXT NOT NULL, target TEXT NOT NULL, applied INTEGER NOT NULL DEFAULT 0)");
    db.execute("CREATE TABLE IF NOT EXISTS revisions (token INTEGER PRIMARY KEY, first_step INTEGER NOT NULL, payload TEXT NOT NULL, applied INTEGER NOT NULL DEFAULT 0)");
    db.execute("CREATE TABLE IF NOT EXISTS revision_replays (token INTEGER NOT NULL, step INTEGER NOT NULL, payload TEXT NOT NULL, PRIMARY KEY(token,step))");
    SessionDatabase::Statement read(db, "SELECT payload FROM checkout WHERE id=1");
    if (!read.step()) return;
    const auto state = Json::parse(read.text(0));
    if (state.at("version") != 1) throw std::runtime_error("Unsupported implementation checkout version");
    root = state.at("root").get<std::string>();
    workspace = state.at("workspace").get<std::string>();
    worktree = state.at("worktree").get<std::string>();
    branch = state.at("branch"); head = state.at("head");
    privateRepository = state.at("private_repository").get<std::string>();
    ready = state.at("ready"); closed = state.at("closed");
}

void ImplementationWorktree::save() {
    const auto value = Json{{"version", 1}, {"root", root.string()}, {"workspace", workspace.string()},
        {"worktree", worktree.string()}, {"private_repository", privateRepository.string()},
        {"branch", branch}, {"head", head}, {"ready", ready}, {"closed", closed}}.dump();
    SessionDatabase::Statement write(db, "INSERT INTO checkout(id,payload) VALUES(1,?) ON CONFLICT(id) DO UPDATE SET payload=excluded.payload");
    write.bind(1, value); write.step();
}

std::string ImplementationWorktree::git(const fs::path& cwd, const std::vector<std::string>& arguments) const {
    std::vector<std::string> args;
    for (auto variable = environ; *variable; ++variable) {
        const std::string entry(*variable);
        if (entry.starts_with("GIT_")) args.insert(args.end(), {"-u", entry.substr(0, entry.find('='))});
    }
    args.insert(args.end(), {"GIT_TERMINAL_PROMPT=0", "GIT_OPTIONAL_LOCKS=0", "LC_ALL=C", "git",
        "-c", "core.hooksPath=/dev/null", "-c", "core.fsmonitor=false", "-c", "gc.auto=0",
        "-c", "maintenance.auto=false", "-c", "fetch.writeCommitGraph=false",
        "-c", "commit.gpgSign=false", "-c", "user.name=SOS", "-c", "user.email=sos@localhost"});
    const bool usePrivateRepository = !privateRepository.empty() && cwd == root;
    if (usePrivateRepository) args.push_back("--git-dir=" + privateRepository.string());
    args.insert(args.end(), arguments.begin(), arguments.end());
    core::ProcessOptions options;
    // Large checkout cleanup and object transfers can exceed the generic timeout.
    options.timeout = std::chrono::minutes(10);
    const auto directory = usePrivateRepository ? privateRepository.parent_path() : cwd;
    const auto result = runGitProcess ? runGitProcess(args, directory, options) :
        core::runProcess("/usr/bin/env", args, directory, {}, {}, options);
    if (result.exitCode != 0 || result.stopped || result.timedOut) {
        auto operation = arguments.empty() ? std::string("unknown") : arguments.front();
        if (operation == "worktree" && arguments.size() > 1) operation += " " + arguments[1];
        const auto reason = result.timedOut ? "timed out after 600 seconds" : result.stopped ? "stopped" :
            "exited with code " + std::to_string(result.exitCode);
        throw std::runtime_error("Implementation Git " + operation + " " + reason +
            (result.error.empty() ? "" : ": " + result.error));
    }
    auto output = result.output;
    if (!output.empty() && output.back() == '\n') output.pop_back();
    return output;
}

fs::path ImplementationWorktree::executionDirectory() const {
    if (!ready || closed || !fs::is_directory(worktree))
        throw std::runtime_error("Implementation checkout is unavailable; no work will run in the user workspace");
    return fs::canonical(worktree / workspace.lexically_relative(root));
}

fs::path ImplementationWorktree::start(const fs::path& original, const std::string& id) {
    if (ready) return executionDirectory();
    if (closed) throw std::logic_error("Implementation checkout is archived");
    if (root.empty()) initialize(original, id);
    save(); // Creation intent survives an interrupted worktree add.
    populate();
    return executionDirectory();
}

void ImplementationWorktree::initialize(const fs::path& original, const std::string& id) {
    if (id.empty() || id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos)
        throw std::invalid_argument("Invalid implementation identity");
    const auto found = GitSnapshot::discover(original);
    const auto originalRoot = found.value_or(fs::canonical(original));
    const auto revision = found ? git(*found, {"rev-parse", "--verify", "HEAD"}) : "";
    if (found && git(*found, {"rev-parse", "--show-object-format"}) != "sha1")
        throw std::runtime_error("Implementation snapshots currently require a SHA-1 Git repository");
    const auto checkout = fs::weakly_canonical(PathResolution::worktreesDirectory() / id);
    const auto relative = checkout.lexically_relative(originalRoot);
    if (!relative.empty() && *relative.begin() != "..") throw std::runtime_error("Implementation storage must be outside the user repository");
    if (fs::exists(checkout)) throw std::runtime_error("Implementation checkout path already exists; refusing to replace it");
    const auto name = "sos/" + id;
    if (found && !git(*found, {"for-each-ref", "--format=%(refname)", "refs/heads/" + name}).empty())
        throw std::runtime_error("Implementation branch already exists; refusing to replace it");
    const auto seed = found ? fs::path{} : fs::weakly_canonical(PathResolution::snapshotsDirectory() / ("implementation-" + id) / "source.git");
    if (!seed.empty()) {
        const auto relativeSeed = seed.lexically_relative(originalRoot);
        if ((!relativeSeed.empty() && *relativeSeed.begin() != "..") || fs::exists(seed))
            throw std::runtime_error("Private implementation repository must be new and outside the user workspace");
    }
    root = originalRoot; workspace = fs::canonical(original); worktree = checkout;
    privateRepository = seed;
    head = revision; branch = name;
}

bool ImplementationWorktree::registered() const {
    const auto listing = git(root, {"worktree", "list", "--porcelain"});
    for (const auto& record : worktreeListingFields(listing)) {
        if (record.starts_with("worktree ") && fs::path(record.substr(9)) == worktree) return true;
    }
    return false;
}

void ImplementationWorktree::populate() {
    fs::create_directories(worktree.parent_path());
    fs::permissions(worktree.parent_path(), fs::perms::owner_all);
    if (!privateRepository.empty()) initializePrivateRepository();
    if (registered()) git(root, {"worktree", "remove", "--force", worktree.string()});
    // Only our recorded ref is touched, never the user's branch or index.
    git(root, {"update-ref", "-d", "refs/heads/" + branch});
    git(root, {"worktree", "add", "--no-checkout", "-b", branch, worktree.string(), head});
    fs::permissions(worktree, fs::perms::owner_all);
    GitSnapshot execution(worktree);
    execution.excludeInternalFiles();
    auto source = privateRepository.empty() ? GitSnapshot(root, execution.storageDirectory()) :
        GitSnapshot::directory(root, execution.storageDirectory());
    source.copyTo(worktree);
    // Baseline the private copy, not the potentially changing source workspace.
    const auto initial = execution.capture();
    const auto baseline = commit(initial.tree, head, "Initial workspace state");
    git(worktree, {"read-tree", baseline}); // Private worktree index only; no checkout filters/hooks.
    head = baseline; ready = true; save();
}

void ImplementationWorktree::initializePrivateRepository() {
    fs::create_directories(privateRepository.parent_path());
    fs::permissions(privateRepository.parent_path(), fs::perms::owner_all);
    if (!fs::exists(privateRepository / "HEAD"))
        git(root, {"init", "--bare", "--template=", "--object-format=sha1", privateRepository.string()});
    git(root, {"config", "sos.privateWorkspace", "true"});
    if (head.empty()) {
        const auto tree = git(root, {"mktree"});
        head = git(root, {"commit-tree", tree, "-m", "Private implementation repository"});
        git(root, {"update-ref", "refs/heads/bootstrap", head});
        git(root, {"symbolic-ref", "HEAD", "refs/heads/bootstrap"});
        save();
    }
    fs::create_directories(privateRepository / "info");
    std::ofstream excludes(privateRepository / "info/exclude");
    excludes << ".sos/\n";
    excludes.close();
    if (!excludes) throw std::runtime_error("Cannot write private implementation exclusions");
}

std::string ImplementationWorktree::commit(const std::string& tree, const std::string& parent, const std::string& label) {
    const auto archive = GitSnapshot::archive(worktree);
    // Transfer raw snapshot objects without FETCH_HEAD or a user-index operation.
    git(root, {"fetch", "--no-write-fetch-head", "--no-tags", "--no-recurse-submodules",
        (archive.storageDirectory() / "repo.git").string(),
        "refs/snapshots/" + tree + ":refs/sos/" + branch.substr(4) + "/trees/" + tree});
    const auto revision = git(root, {"commit-tree", tree, "-p", parent, "-m", label});
    git(root, {"update-ref", "refs/heads/" + branch, revision});
    git(root, {"update-ref", "refs/sos/" + branch.substr(4) + "/history", revision});
    return revision;
}

void ImplementationWorktree::seal(std::size_t step, const TurnSnapshot& snapshot) {
    if (snapshot.root != worktree.string() || snapshot.after.empty()) throw std::runtime_error("Step has no isolated snapshot");
    SessionDatabase::Statement existing(db, "SELECT tree FROM step_commits WHERE step=?");
    existing.bind(1, static_cast<std::int64_t>(step));
    if (existing.step()) {
        if (existing.text(0) == snapshot.after) return;
        requireReworkable(step);
    }
    const auto revision = commit(snapshot.after, head, "Implementation step " + std::to_string(step + 1));
    SessionDatabase::Transaction transaction(db);
    SessionDatabase::Statement write(db, "INSERT INTO step_commits(step,tree,revision) VALUES(?,?,?) ON CONFLICT(step) DO UPDATE SET tree=excluded.tree,revision=excluded.revision");
    write.bind(1, static_cast<std::int64_t>(step)); write.bind(2, snapshot.after); write.bind(3, revision); write.step();
    head = revision; save(); transaction.commit();
}

void ImplementationWorktree::requireReworkable(std::size_t step) {
    if (closed || !ready) throw std::runtime_error("Implementation checkout is unavailable");
    SessionDatabase::Statement imported(db, "SELECT step FROM imports WHERE step=?");
    imported.bind(1, static_cast<std::int64_t>(step));
    if (imported.step()) throw std::runtime_error("An imported or pending-import step cannot be replaced");
    SessionDatabase::Statement later(db, "SELECT step FROM step_commits WHERE step>? LIMIT 1");
    later.bind(1, static_cast<std::int64_t>(step));
    if (later.step()) throw std::runtime_error("Reconcile downstream steps before replacing a sealed step");
}

std::optional<std::pair<GitSnapshotId, GitSnapshotId>> ImplementationWorktree::pendingImport(
    std::size_t step, const TurnSnapshot& snapshot, const GitSnapshot& target) {
    SessionDatabase::Statement existing(db, "SELECT source_before,source_after,expected,target,applied FROM imports WHERE step=?");
    existing.bind(1, static_cast<std::int64_t>(step));
    if (existing.step()) {
        if (existing.text(0) != snapshot.before || existing.text(1) != snapshot.after)
            throw std::runtime_error("Step changed after import preparation");
        if (existing.integer(4)) return {};
        return std::pair{GitSnapshotId{existing.text(2)}, GitSnapshotId{existing.text(3)}};
    }
    const auto prepared = target.prepareImport({snapshot.before}, {snapshot.after});
    SessionDatabase::Statement write(db, "INSERT INTO imports(step,source_before,source_after,expected,target) VALUES(?,?,?,?,?)");
    write.bind(1, static_cast<std::int64_t>(step)); write.bind(2, snapshot.before); write.bind(3, snapshot.after);
    write.bind(4, prepared.first.tree); write.bind(5, prepared.second.tree); write.step();
    return prepared;
}

void ImplementationWorktree::import(std::size_t step, const TurnSnapshot& snapshot) {
    if (closed) throw std::logic_error("Implementation checkout is archived");
    if (snapshot.root != worktree.string() || snapshot.before.empty() || snapshot.after.empty())
        throw std::runtime_error("Step has no isolated import boundary");
    const auto archive = GitSnapshot::archive(worktree);
    auto target = privateRepository.empty() ? GitSnapshot(root, archive.storageDirectory()) :
        GitSnapshot::directory(root, archive.storageDirectory());
    const auto pending = pendingImport(step, snapshot, target);
    if (!pending) return;
    const auto conflicts = target.restore(pending->first, pending->second, true);
    if (!conflicts.empty()) {
        std::string message = "Cannot import this step: workspace changed since import preparation. Nothing was imported; the step remains unvalidated. Files:";
        for (const auto& path : conflicts) message += "\n" + path.generic_string();
        throw std::runtime_error(message);
    }
    SessionDatabase::Statement done(db, "UPDATE imports SET applied=1 WHERE step=?");
    done.bind(1, static_cast<std::int64_t>(step)); done.step();
}

void ImplementationWorktree::close() {
    if (!exists() || closed) return;
    if (registered()) git(root, {"worktree", "remove", "--force", worktree.string()});
    git(root, {"update-ref", "-d", "refs/heads/" + branch});
    closed = true; save(); // Snapshot objects and refs/sos history remain pinned.
}
