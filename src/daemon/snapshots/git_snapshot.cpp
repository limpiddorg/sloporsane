#include "git_snapshot.hpp"
#include "private_copy.hpp"
#include <agent-runtime/common/path_resolution.hpp>
#include <agent-runtime/core/process.hpp>
#include <openssl/sha.h>
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/file.h>
#include <unistd.h>

extern char** environ;

namespace {
namespace fs = std::filesystem;

class StorageLock {
public:
    explicit StorageLock(const fs::path& directory) {
        fs::create_directories(directory);
        fd = ::open((directory / "lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) throw std::runtime_error("Cannot open snapshot lock");
        while (flock(fd, LOCK_EX) != 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            throw std::runtime_error("Cannot acquire snapshot lock");
        }
    }
    ~StorageLock() { flock(fd, LOCK_UN); ::close(fd); }
    StorageLock(const StorageLock&) = delete;
    StorageLock& operator=(const StorageLock&) = delete;
private:
    int fd;
};

std::string digest(const std::string& text) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(text.data()), text.size(), hash);
    constexpr char HEX[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : hash) { result += HEX[byte >> 4]; result += HEX[byte & 15]; }
    return result;
}

bool objectId(const std::string& text) {
    return text.size() == 40 && text.find_first_not_of("0123456789abcdef") == std::string::npos;
}

std::string line(std::string text) {
    if (!text.empty() && text.back() == '\n') text.pop_back();
    return text;
}

// Strip inherited Git routing/index/config overrides; never run through a shell.
// Source queries retain global ignore configuration. Private storage uses no global config.
core::ProcessResult runGit(const fs::path& cwd, const std::vector<std::string>& arguments,
                           bool isolated = false, const core::ProcessOptions& options = {}) {
    std::vector<std::string> args;
    for (auto variable = environ; *variable; ++variable) {
        const std::string entry(*variable);
        if (entry.starts_with("GIT_")) {
            args.push_back("-u");
            args.push_back(entry.substr(0, entry.find('=')));
        }
    }
    args.insert(args.end(), {"GIT_OPTIONAL_LOCKS=0", "GIT_TERMINAL_PROMPT=0", "LC_ALL=C"});
    if (isolated) args.insert(args.end(), {"GIT_CONFIG_NOSYSTEM=1", "GIT_CONFIG_GLOBAL=/dev/null"});
    args.insert(args.end(), {"git", "-c", "core.fsmonitor=false", "-c", "core.hooksPath=/dev/null",
                             "-c", "core.autocrlf=false", "-c", "gc.auto=0"});
    args.insert(args.end(), arguments.begin(), arguments.end());
    auto budget = options;
    if (std::find(arguments.begin(), arguments.end(), "hash-object") != arguments.end())
        budget.timeout = std::chrono::minutes(10); // Batch hashing reads the entire eligible tree.
    auto result = core::runProcess("/usr/bin/env", args, cwd, {}, {}, budget);
    if (result.timedOut || result.stopped) {
        const auto command = std::find_if(arguments.begin(), arguments.end(), [](const auto& arg) { return !arg.starts_with("-"); });
        throw std::runtime_error("Snapshot Git " + (command == arguments.end() ? std::string("command") : *command) +
            (result.timedOut ? " timed out after " + std::to_string(budget.timeout.count() / 1000) + " seconds" : " stopped"));
    }
    return result;
}

std::string success(core::ProcessResult result) {
    if (result.exitCode != 0) throw std::runtime_error("Snapshot Git command failed: " + result.error);
    return result.output;
}

std::vector<std::string> records(const std::string& text) {
    std::vector<std::string> result;
    std::size_t offset = 0;
    while (offset < text.size()) {
        const auto end = text.find('\0', offset);
        if (end == std::string::npos) throw std::runtime_error("Incomplete Git path listing");
        result.push_back(text.substr(offset, end - offset));
        offset = end + 1;
    }
    return result;
}

bool internalData(const fs::path& path) {
    for (const auto& part : path) if (part == ".sos") return true;
    return false;
}

bool privateWorkspace(const fs::path& directory) {
    const auto result = runGit(directory, {"config", "--local", "--bool", "sos.privateWorkspace"});
    if (result.exitCode == 1) return false;
    return line(success(result)) == "true";
}

void validatePath(const fs::path& path) {
    if (path.empty() || path.is_absolute()) throw std::runtime_error("Invalid snapshot path");
    for (const auto& part : path)
        if (part == ".." || part == "." || part == ".git") throw std::runtime_error("Unsafe snapshot path");
}

void writeBytes(const fs::path& path, const std::string& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    if (!stream) throw std::runtime_error("Cannot write snapshot file: " + path.string());
}

std::vector<std::string> privateArgs(const fs::path& storage) {
    return {"--git-dir=" + (storage / "repo.git").string()};
}

std::string quotePath(const std::string& path) {
    std::string result = "\"";
    for (const unsigned char byte : path) {
        if (byte < 32 || byte >= 127 || byte == '\\' || byte == '"') {
            result += '\\';
            result += char('0' + (byte >> 6));
            result += char('0' + ((byte >> 3) & 7));
            result += char('0' + (byte & 7));
        } else result += char(byte);
    }
    return result + "\"\n";
}
}

std::optional<fs::path> GitSnapshot::discover(const fs::path& directory) {
    const auto location = fs::canonical(directory);
    auto parent = location;
    while (!fs::exists(parent / ".git")) {
        if (parent == parent.root_path()) return std::nullopt;
        parent = parent.parent_path();
    }
    // Bare repositories and ordinary non-repository directories have no worktree.
    const auto result = runGit(location, {"rev-parse", "--show-toplevel"});
    if (result.exitCode == 128 && (result.error.starts_with("fatal: not a git repository") ||
                                 result.error.starts_with("fatal: this operation must be run in a work tree")))
        return std::nullopt;
    const auto root = line(success(result));
    if (root.empty()) throw std::runtime_error("Git returned an empty worktree root");
    return fs::canonical(root);
}

GitSnapshot::GitSnapshot(const fs::path& directory) {
    const auto found = discover(directory);
    if (!found) throw std::invalid_argument("Snapshots require a Git worktree");
    worktree = *found;
    excludeInternalData = privateWorkspace(worktree);
    storage = fs::weakly_canonical(PathResolution::snapshotsDirectory() / digest(worktree.string()));
    excludeInternalData = excludeInternalData || fs::exists(storage / "exclude-internal-data");
    const auto relative = storage.lexically_relative(worktree);
    if (!relative.empty() && *relative.begin() != "..")
        throw std::invalid_argument("Snapshot storage must be outside the captured worktree");
    const StorageLock lock(storage);
    initialize();
}

std::string GitSnapshot::git(const std::vector<std::string>& arguments, const std::optional<fs::path>& input) const {
    auto args = privateArgs(storage);
    args.insert(args.end(), arguments.begin(), arguments.end());
    core::ProcessOptions options;
    options.inputFile = input;
    return success(runGit(storage, args, true, options));
}

GitSnapshot::GitSnapshot(const fs::path& directory, const fs::path& objectStorage) {
    const auto found = discover(directory);
    if (!found) throw std::invalid_argument("Snapshots require a Git worktree");
    worktree = *found;
    excludeInternalData = privateWorkspace(worktree);
    storage = fs::canonical(objectStorage);
    excludeInternalData = excludeInternalData || fs::exists(storage / "exclude-internal-data");
    const auto relative = storage.lexically_relative(worktree);
    if (!relative.empty() && *relative.begin() != "..")
        throw std::invalid_argument("Snapshot storage must be outside the captured worktree");
    if (!fs::exists(storage / "repo.git/HEAD")) throw std::runtime_error("Snapshot archive is missing");
}

GitSnapshot GitSnapshot::archive(const fs::path& formerRoot) {
    GitSnapshot result;
    result.worktree = formerRoot.lexically_normal();
    result.storage = fs::weakly_canonical(PathResolution::snapshotsDirectory() / digest(result.worktree.string()));
    if (!fs::exists(result.storage / "repo.git/HEAD")) throw std::runtime_error("Snapshot archive is missing");
    return result;
}

GitSnapshot GitSnapshot::directory(const fs::path& directory, const fs::path& objectStorage) {
    GitSnapshot result;
    result.worktree = fs::canonical(directory);
    result.storage = fs::canonical(objectStorage);
    const auto relative = result.storage.lexically_relative(result.worktree);
    if (!relative.empty() && *relative.begin() != "..")
        throw std::invalid_argument("Snapshot storage must be outside the captured directory");
    if (!fs::exists(result.storage / "repo.git/HEAD")) throw std::runtime_error("Snapshot archive is missing");
    result.directoryOnly = true;
    result.excludeInternalData = true;
    return result;
}

std::pair<GitSnapshotId, GitSnapshotId> GitSnapshot::prepareImport(const GitSnapshotId& before, const GitSnapshotId& after) const {
    const StorageLock lock(storage);
    std::vector<GitSnapshotChange> mutations;
    std::vector<fs::path> problems;
    for (const auto& change : changes(before, after)) {
        if (!safeParents(change.path)) { problems.push_back(change.path); continue; }
        const auto status = fs::symlink_status(worktree / change.path);
        if (fs::exists(status) && !fs::is_regular_file(status) && !fs::is_symlink(status)) {
            problems.push_back(change.path); continue;
        }
        const auto current = inspect(change.path);
        if (current == change.after) continue;
        auto target = change.after;
        if (current != change.before) {
            if (!current || !change.before || !target || current->mode == "120000" ||
                change.before->mode == "120000" || target->mode == "120000") {
                problems.push_back(change.path); continue;
            }
            if (current->mode != change.before->mode && target->mode != change.before->mode && current->mode != target->mode) {
                problems.push_back(change.path); continue;
            }
            const auto ours = storage / "merge-current", base = storage / "merge-base", theirs = storage / "merge-step";
            writeBytes(ours, git({"cat-file", "blob", current->blob}));
            writeBytes(base, git({"cat-file", "blob", change.before->blob}));
            writeBytes(theirs, git({"cat-file", "blob", target->blob}));
            auto merged = runGit(storage, {"merge-file", "-p", ours.string(), base.string(), theirs.string()}, true);
            if (merged.exitCode != 0) { problems.push_back(change.path); continue; }
            writeBytes(ours, merged.output);
            target->blob = line(git({"hash-object", "-w", "--no-filters", ours.string()}));
            if (target->mode == change.before->mode) target->mode = current->mode;
        }
        mutations.push_back({change.path, current, target});
    }
    if (!problems.empty()) {
        std::string message = "Cannot import this step because of conflicting workspace changes. Nothing was imported; the step remains unvalidated. Files:";
        for (const auto& path : problems) message += "\n" + path.generic_string();
        throw std::runtime_error(message);
    }
    const auto tree = [&](bool forward) {
        git({"read-tree", "--empty"});
        std::string index;
        for (const auto& change : mutations) if (const auto& file = forward ? change.after : change.before; file)
            index += file->mode + " " + file->blob + '\t' + change.path.generic_string() + '\0';
        const auto input = storage / "input";
        writeBytes(input, index);
        git({"update-index", "-z", "--index-info"}, input);
        const auto id = line(git({"write-tree"}));
        git({"update-ref", "refs/snapshots/" + id, id});
        return GitSnapshotId{id};
    };
    return {tree(false), tree(true)};
}

std::pair<GitSnapshotId, GitSnapshotId> GitSnapshot::contributionTrees(const GitSnapshotId& before, const GitSnapshotId& after,
    const std::vector<ImplementationDiff>& diffs) const {
    const StorageLock lock(storage);
    const auto original = files(before), final = files(after);
    const auto tree = [&](bool forward) {
        git({"read-tree", "--empty"});
        std::string index;
        for (const auto& diff : diffs) {
            fs::path path(diff.path);
            if (path.is_absolute()) continue; // Tool-journaled ignored files are handled separately.
            validatePath(path);
            const auto& source = forward ? final : original;
            const bool opaque = diff.patch.find("GIT binary patch") != std::string::npos || diff.patch.find("Binary") != std::string::npos;
            if (opaque) {
                if (source.contains(path)) index += source.at(path).mode + " " + source.at(path).blob + '\t' + path.generic_string() + '\0';
                continue;
            }
            const auto& text = forward ? diff.after : diff.before;
            if (text.empty() && !source.contains(path)) continue;
            const auto mode = source.contains(path) ? source.at(path).mode : final.contains(path) ? final.at(path).mode : "100644";
            const auto input = storage / "contribution-text"; writeBytes(input, text);
            const auto blob = line(git({"hash-object", "-w", "--no-filters", input.string()}));
            index += mode + " " + blob + '\t' + path.generic_string() + '\0';
        }
        const auto input = storage / "input"; writeBytes(input, index);
        git({"update-index", "-z", "--index-info"}, input);
        const auto id = line(git({"write-tree"})); git({"update-ref", "refs/snapshots/" + id, id});
        return GitSnapshotId{id};
    };
    return {tree(false), tree(true)};
}

void GitSnapshot::initialize() const {
    fs::permissions(storage, fs::perms::owner_all);
    if (!fs::exists(storage / "repo.git/HEAD"))
        success(runGit(storage, {"init", "--bare", "--template=", "--object-format=sha1", (storage / "repo.git").string()}, true));
}

bool GitSnapshot::ignored(const fs::path& relative) const {
    if (excludeInternalData && internalData(relative)) return true;
    bool nestedRepository = false;
    auto parent = worktree;
    for (const auto& part : relative.parent_path()) {
        parent /= part;
        if (fs::exists(parent / ".git")) nestedRepository = true;
    }
    if (directoryOnly || nestedRepository) {
        auto args = privateArgs(storage);
        args.insert(args.end(), {"--work-tree=" + worktree.string(), "check-ignore", "--no-index", "-q", "--", "./" + relative.generic_string()});
        const auto result = runGit(worktree, args);
        if (result.exitCode == 1) return false;
        success(result);
        return true;
    }
    const auto result = runGit(worktree, {"check-ignore", "--no-index", "-q", "--", "./" + relative.generic_string()});
    if (result.exitCode == 1) return false;
    success(result);
    return true;
}

bool GitSnapshot::safeParents(const fs::path& relative) const {
    validatePath(relative);
    auto current = worktree;
    for (const auto& part : relative.parent_path()) {
        current /= part;
        const auto status = fs::symlink_status(current);
        if (fs::exists(status) && !fs::is_directory(status)) return false;
    }
    return true;
}

std::optional<GitSnapshotFile> GitSnapshot::inspect(const fs::path& relative) const {
    if (!safeParents(relative)) throw std::runtime_error("Unsafe snapshot parent: " + relative.string());
    const auto path = worktree / relative;
    const auto status = fs::symlink_status(path);
    if (!fs::exists(status)) return std::nullopt;
    std::string mode;
    fs::path source = path;
    if (fs::is_symlink(status)) {
        mode = "120000";
        source = storage / "link-target";
        writeBytes(source, fs::read_symlink(path).native());
    } else if (fs::is_regular_file(status)) {
        mode = (status.permissions() & fs::perms::owner_exec) != fs::perms::none ? "100755" : "100644";
    } else throw std::runtime_error("Unsupported snapshot file type: " + relative.string());
    const auto blob = line(git({"hash-object", "-w", "--no-filters", "--", source.string()}));
    if (!objectId(blob)) throw std::runtime_error("Invalid snapshot blob ID");
    return GitSnapshotFile{mode, blob};
}

std::vector<std::string> GitSnapshot::eligiblePaths() const {
    if (directoryOnly) {
        return directoryPaths();
    }
    const auto candidates = records(success(runGit(worktree, {"ls-files", "--cached", "--others", "--exclude-standard", "-z"})));
    const std::set<std::string> unique(candidates.begin(), candidates.end());
    std::string ignoreInput;
    for (const auto& name : unique) ignoreInput += "./" + name + '\0';
    const auto input = storage / "input";
    writeBytes(input, ignoreInput);
    core::ProcessOptions options;
    options.inputFile = input;
    const auto ignoreResult = runGit(worktree, {"check-ignore", "--no-index", "-z", "--stdin"}, false, options);
    if (ignoreResult.exitCode != 1) success(ignoreResult);
    const auto excluded = records(ignoreResult.output);
    const std::set<std::string> ignoredPaths(excluded.begin(), excluded.end());
    std::vector<std::string> paths;
    for (const auto& name : unique) {
        if (ignoredPaths.contains("./" + name) || (excludeInternalData && internalData(name))) continue;
        if (fs::is_directory(fs::symlink_status(worktree / name))) {
            // A gitlink is a directory by design; a tracked file replaced by a
            // directory remains unsupported. Do not turn a failed checkpoint
            // into a successful snapshot of an incompatible file-type transition.
            const auto index = records(success(runGit(worktree, {"--literal-pathspecs", "ls-files", "--stage", "-z", "--", name})));
            for (const auto& entry : index) {
                const auto tab = entry.find('\t');
                if (tab != std::string::npos && entry.substr(tab + 1) == name && !entry.starts_with("160000 "))
                    throw std::runtime_error("Unsupported snapshot file type: " + name);
            }
            const auto nested = directoryPaths(name);
            paths.insert(paths.end(), nested.begin(), nested.end());
        } else paths.push_back(name);
    }
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    return paths;
}

std::vector<std::string> GitSnapshot::directoryPaths(const fs::path& relative) const {
    std::vector<fs::path> pending{relative};
    std::vector<std::string> result;
    while (!pending.empty()) {
        std::vector<std::string> candidates;
        std::string ignoreInput;
        for (const auto& directory : pending) {
            for (const auto& entry : fs::directory_iterator(worktree / directory)) {
                const auto path = directory / entry.path().filename();
                if (entry.path().filename() == ".git" || internalData(path)) continue;
                candidates.push_back(path.generic_string());
                ignoreInput += "./" + path.generic_string() + '\0';
            }
        }
        pending.clear();
        if (candidates.empty()) continue;
        const auto ignoredPaths = excludedDirectoryPaths(ignoreInput);
        for (const auto& path : candidates) {
            if (ignoredPaths.contains("./" + path)) continue;
            if (fs::is_directory(fs::symlink_status(worktree / path))) pending.emplace_back(path);
            else result.push_back(path);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::set<std::string> GitSnapshot::excludedDirectoryPaths(const std::string& candidates) const {
    const auto input = storage / "input";
    writeBytes(input, candidates);
    core::ProcessOptions options;
    options.inputFile = input;
    // Explicit private Git routing sees nested .gitignore files, but never treats
    // embedded repositories/worktree .git files as opaque gitlinks. Batch by depth
    // and prune ignored directories before visiting their (potentially huge) contents.
    auto args = privateArgs(storage);
    args.insert(args.end(), {"--work-tree=" + worktree.string(), "check-ignore", "--no-index", "-z", "--stdin"});
    const auto ignored = runGit(worktree, args, false, options);
    if (ignored.exitCode != 1) success(ignored);
    const auto excluded = records(ignored.output);
    return {excluded.begin(), excluded.end()};
}

void GitSnapshot::copyTo(const fs::path& destination) const {
    const StorageLock lock(storage);
    for (const auto& name : eligiblePaths()) {
        const fs::path path(name);
        if (internalData(path)) continue;
        if (!safeParents(path)) throw std::runtime_error("Unsafe private copy parent: " + name);
        const auto status = fs::symlink_status(worktree / path);
        if (!fs::exists(status)) continue;
        fs::create_directories((destination / path).parent_path());
        copyPrivateFile(worktree / path, destination / path);
    }
}
void GitSnapshot::exportTo(const GitSnapshotId& snapshot, const fs::path& destination, const std::function<void()>& check) const {
    fs::create_directories(destination);
    if (!fs::is_empty(destination)) throw std::runtime_error("Historical review destination must be empty");
    const auto entries = files(snapshot);
    for (const auto& [path, file] : entries) {
        if (check) check();
        if (internalData(path)) continue;
        // Symlinks are created last, so no historical path can redirect writes.
        if (file.mode == "120000") continue;
        fs::create_directories((destination / path).parent_path());
        writeBytes(destination / path, git({"cat-file", "blob", file.blob}));
    }
    for (const auto& [path, file] : entries) if (file.mode == "120000") {
        if (check) check();
        if (internalData(path)) continue;
        const fs::path target(git({"cat-file", "blob", file.blob}));
        const auto relative = (path.parent_path() / target).lexically_normal();
        // Never allow an historical link to escape into the live checkout.
        if (target.is_absolute() || relative.empty() || *relative.begin() == "..") continue;
        fs::create_directories((destination / path).parent_path());
        fs::create_symlink(target, destination / path);
    }
}

void GitSnapshot::excludeInternalFiles() {
    const StorageLock lock(storage);
    writeBytes(storage / "exclude-internal-data", "implementation\n");
    excludeInternalData = true;
}

void GitSnapshot::stageFiles(const std::vector<std::string>& paths) const {
    const auto input = storage / "input";
    std::vector<std::pair<std::string, std::string>> entries;
    std::string hashInput;
    fs::create_directories(storage / "links");
    for (const auto& name : paths) {
        const fs::path path(name);
        validatePath(path);
        if (!safeParents(path)) throw std::runtime_error("Unsafe snapshot parent: " + name);
        auto source = worktree / path;
        const auto status = fs::symlink_status(source);
        if (!fs::exists(status)) continue;
        std::string mode;
        if (fs::is_symlink(status)) {
            mode = "120000";
            const auto target = fs::read_symlink(source).native();
            source = storage / "links" / std::to_string(entries.size());
            writeBytes(source, target);
        } else if (fs::is_regular_file(status)) {
            mode = (status.permissions() & fs::perms::owner_exec) != fs::perms::none ? "100755" : "100644";
        } else throw std::runtime_error("Unsupported snapshot file type: " + name);
        entries.emplace_back(mode, name);
        hashInput += quotePath(source.string());
    }
    writeBytes(input, hashInput);
    std::istringstream hashes(git({"hash-object", "-w", "--no-filters", "--stdin-paths"}, input));
    std::string index;
    for (const auto& [mode, name] : entries) {
        std::string blob;
        if (!std::getline(hashes, blob) || !objectId(blob)) throw std::runtime_error("Invalid snapshot blob listing");
        index += mode + " " + blob + '\t' + name + '\0';
    }
    writeBytes(input, index);
    git({"update-index", "-z", "--index-info"}, input);
}

GitSnapshotId GitSnapshot::capture() const {
    const StorageLock lock(storage);
    git({"read-tree", "--empty"});
    stageFiles(eligiblePaths());
    const auto tree = line(git({"write-tree"}));
    if (!objectId(tree)) throw std::runtime_error("Invalid snapshot tree ID");
    // Pin trees so snapshots survive reopening and Git garbage collection.
    git({"update-ref", "refs/snapshots/" + tree, tree});
    return {tree};
}

GitSnapshot::Files GitSnapshot::files(const GitSnapshotId& snapshot) const {
    if (!objectId(snapshot.tree)) throw std::invalid_argument("Invalid snapshot ID");
    Files result;
    for (const auto& record : records(git({"ls-tree", "-r", "-z", snapshot.tree}))) {
        const auto tab = record.find('\t');
        const auto space = record.find(' ');
        const auto hash = record.find(' ', space + 1);
        if (tab == std::string::npos || hash == std::string::npos || record.substr(space + 1, hash - space - 1) != "blob")
            throw std::runtime_error("Unsupported snapshot tree entry");
        const fs::path path(record.substr(tab + 1));
        validatePath(path);
        const GitSnapshotFile file{record.substr(0, space), record.substr(hash + 1, tab - hash - 1)};
        if (!objectId(file.blob) || (file.mode != "100644" && file.mode != "100755" && file.mode != "120000"))
            throw std::runtime_error("Unsupported snapshot tree entry");
        result.emplace(path, file);
    }
    return result;
}

std::vector<std::string> GitSnapshot::paths(const GitSnapshotId& snapshot) const {
    std::vector<std::string> result;
    for (const auto& [path, file] : files(snapshot)) result.push_back(path.generic_string());
    return result;
}

std::vector<GitSnapshotEntry> GitSnapshot::children(const GitSnapshotId& snapshot, const std::string& directory) const {
    if (!objectId(snapshot.tree)) throw std::invalid_argument("Invalid snapshot ID");
    if (!directory.empty()) {
        validatePath(fs::path(directory));
        if (directory.find('\0') != std::string::npos || directory.ends_with('/') || directory.find("//") != std::string::npos)
            throw std::invalid_argument("Invalid snapshot directory");
    }
    auto tree = snapshot.tree;
    if (!directory.empty()) {
        // A new directory has no neighbours in the baseline. Resolve its literal
        // entry first: an absent path is empty, but an unavailable snapshot fails.
        const auto entries = records(git({"--literal-pathspecs", "ls-tree", "-z", snapshot.tree, "--", directory}));
        if (entries.empty()) return {};
        if (entries.size() != 1 || !entries.front().starts_with("040000 tree ") ||
            entries.front().substr(entries.front().find('\t') + 1) != directory)
            throw std::runtime_error("Snapshot path is not a directory");
        tree += ":" + directory;
    }
    std::vector<GitSnapshotEntry> result;
    // No -r: Git reads only the requested tree object, never its descendants or blobs.
    for (const auto& record : records(git({"ls-tree", "-z", tree}))) {
        const auto tab = record.find('\t'), space = record.find(' '), hash = record.find(' ', space + 1);
        if (tab == std::string::npos || hash == std::string::npos)
            throw std::runtime_error("Invalid snapshot directory entry");
        const auto name = record.substr(tab + 1), kind = record.substr(space + 1, hash - space - 1);
        validatePath(fs::path(name));
        if (name.find('/') != std::string::npos || (kind != "blob" && kind != "tree"))
            throw std::runtime_error("Unsupported snapshot directory entry");
        result.push_back({name, kind == "tree"});
    }
    return result;
}

std::vector<GitSnapshotChange> GitSnapshot::changes(const GitSnapshotId& before, const GitSnapshotId& after) const {
    const auto oldFiles = files(before), newFiles = files(after);
    std::set<fs::path> paths;
    for (const auto& [path, file] : oldFiles) paths.insert(path);
    for (const auto& [path, file] : newFiles) paths.insert(path);
    std::vector<GitSnapshotChange> result;
    for (const auto& path : paths) {
        const auto old = oldFiles.find(path), next = newFiles.find(path);
        const std::optional<GitSnapshotFile> a = old == oldFiles.end() ? std::nullopt : std::make_optional(old->second);
        const std::optional<GitSnapshotFile> b = next == newFiles.end() ? std::nullopt : std::make_optional(next->second);
        if (a != b) result.push_back({path, a, b});
    }
    return result;
}

void GitSnapshot::apply(const std::vector<GitSnapshotChange>& mutations, bool forward) const {
    for (const auto& change : mutations) {
        const auto& file = forward ? change.after : change.before;
        const auto destination = worktree / change.path;
        if (!file) { fs::remove(destination); continue; }
        const auto temporary = storage / "restore-blob";
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) throw std::runtime_error("Cannot stage snapshot blob");
        core::ProcessOptions options;
        options.captureOutput = false;
        options.onStdout = [&](std::string_view bytes) {
            stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            if (!stream) throw std::runtime_error("Cannot stage snapshot blob");
        };
        auto args = privateArgs(storage);
        args.insert(args.end(), {"cat-file", "blob", file->blob});
        success(runGit(storage, args, true, options));
        stream.close();
        if (!stream) throw std::runtime_error("Cannot finish staging snapshot blob");
        fs::create_directories(destination.parent_path());
        // Unlink first: never follow an existing symlink or alter its target/hard-link peers.
        fs::remove(destination);
        if (file->mode == "120000") {
            std::ifstream input(temporary, std::ios::binary);
            const std::string target(std::istreambuf_iterator<char>(input), {});
            fs::create_symlink(target, destination);
        } else {
            fs::copy_file(temporary, destination);
            fs::permissions(destination, file->mode == "100755" ? fs::perms(0755) : fs::perms(0644));
        }
    }
}

bool GitSnapshot::covers(const fs::path& path) const {
    const auto relative = path.lexically_relative(worktree);
    for (const auto& part : relative) if (part == ".git") return false;
    return !relative.empty() && !relative.is_absolute() && *relative.begin() != ".." && !ignored(relative);
}

std::vector<fs::path> GitSnapshot::conflicts(const GitSnapshotId& expected, const GitSnapshotId& target, bool resume) const {
    const StorageLock lock(storage);
    return checkConflicts(expected, target, resume);
}

std::vector<fs::path> GitSnapshot::checkConflicts(const GitSnapshotId& expected, const GitSnapshotId& target, bool resume) const {
    const auto mutations = changes(expected, target);
    std::vector<fs::path> conflicts;
    for (const auto& change : mutations) {
        if (change.after) git({"cat-file", "-e", change.after->blob});
        if (!safeParents(change.path) || ignored(change.path)) { conflicts.push_back(change.path); continue; }
        const auto status = fs::symlink_status(worktree / change.path);
        if (fs::exists(status) && !fs::is_regular_file(status) && !fs::is_symlink(status)) {
            conflicts.push_back(change.path);
            continue;
        }
        const auto current = inspect(change.path);
        if (current != change.before && (!resume || current != change.after)) conflicts.push_back(change.path);
    }
    return conflicts;
}

std::vector<fs::path> GitSnapshot::restore(const GitSnapshotId& expected, const GitSnapshotId& target, bool resume) const {
    const StorageLock lock(storage);
    const auto problems = checkConflicts(expected, target, resume);
    if (!problems.empty()) return problems;
    auto mutations = changes(expected, target);
    if (resume) std::erase_if(mutations, [&](const auto& change) { return inspect(change.path) == change.after; });
    try { apply(mutations, true); }
    catch (...) {
        try { apply(mutations, false); }
        catch (...) { throw std::runtime_error("Snapshot restore and rollback failed; worktree may be partially restored"); }
        throw;
    }
    return {};
}

std::vector<ImplementationDiff> GitSnapshot::diffs(const GitSnapshotId& before, const GitSnapshotId& after) const {
    std::vector<ImplementationDiff> result;
    for (const auto& change : changes(before, after)) {
        result.push_back({change.path.string(), git({"--literal-pathspecs", "diff", "--no-ext-diff", "--no-textconv", "--no-renames",
            "--binary", "--no-color", "--unified=3", before.tree, after.tree, "--", change.path.string()})});
        auto& diff = result.back();
        if (diff.patch.find("GIT binary patch") != std::string::npos) continue;
        diff.before = change.before ? git({"cat-file", "blob", change.before->blob}) : "";
        diff.after = change.after ? git({"cat-file", "blob", change.after->blob}) : "";
        try {
            if (diff.before.find('\0') != std::string::npos || diff.after.find('\0') != std::string::npos)
                throw std::runtime_error("Binary contents");
            (void)nlohmann::json({diff.before, diff.after}).dump();
        } catch (const std::exception&) {
            diff.before.clear(); diff.after.clear();
            try { (void)nlohmann::json(diff.patch).dump(); }
            catch (const std::exception&) { diff.patch = "Non-UTF-8 contents changed (preserved in snapshot blobs).\n"; }
        }
    }
    return result;
}
