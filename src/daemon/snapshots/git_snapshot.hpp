#ifndef SOS_GIT_SNAPSHOT_HPP
#define SOS_GIT_SNAPSHOT_HPP

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include "change_diff.hpp"

struct GitSnapshotId {
    std::string tree;
};

struct GitSnapshotFile {
    std::string mode;
    std::string blob;
    bool operator==(const GitSnapshotFile&) const = default;
};

struct GitSnapshotChange {
    std::filesystem::path path; // Relative to the worktree root.
    std::optional<GitSnapshotFile> before;
    std::optional<GitSnapshotFile> after;
};

struct GitSnapshotEntry {
    std::string name;
    bool directory = false;
};

// Local Git object storage, separate from the user's index, refs and configuration.
// No permissions are granted by this component.
class GitSnapshot {
public:
    static std::optional<std::filesystem::path> discover(const std::filesystem::path& directory);
    explicit GitSnapshot(const std::filesystem::path& directory);
    GitSnapshot(const std::filesystem::path& directory, const std::filesystem::path& objectStorage);
    static GitSnapshot archive(const std::filesystem::path& formerRoot);
    static GitSnapshot directory(const std::filesystem::path& directory, const std::filesystem::path& objectStorage);
    // Prepare and journal these trees before restore(..., true) for resumable import.
    std::pair<GitSnapshotId, GitSnapshotId> prepareImport(const GitSnapshotId& before, const GitSnapshotId& after) const;
    std::pair<GitSnapshotId, GitSnapshotId> contributionTrees(const GitSnapshotId& before, const GitSnapshotId& after,
        const std::vector<ImplementationDiff>& diffs) const;
    const std::filesystem::path& root() const { return worktree; }
    const std::filesystem::path& storageDirectory() const { return storage; }
    GitSnapshotId capture() const;
    // Populate an empty private checkout without materializing blobs back to disk.
    void copyTo(const std::filesystem::path& destination) const;
    // Materialize one immutable review tree into an empty private directory.
    void exportTo(const GitSnapshotId& snapshot, const std::filesystem::path& destination,
        const std::function<void()>& check = {}) const;
    // Persist implementation-only exclusions in SOS storage, never user Git config.
    void excludeInternalFiles();
    std::vector<std::string> paths(const GitSnapshotId& snapshot) const;
    std::vector<GitSnapshotEntry> children(const GitSnapshotId& snapshot, const std::string& directory) const;
    std::vector<GitSnapshotChange> changes(const GitSnapshotId& before, const GitSnapshotId& after) const;
    std::vector<ImplementationDiff> diffs(const GitSnapshotId& before, const GitSnapshotId& after) const;
    // Returns conflicting paths without changing any worktree file. Empty means success.
    // Only paths differing between expected and target are restored. Reverse the IDs for redo.
    std::vector<std::filesystem::path> restore(const GitSnapshotId& expected, const GitSnapshotId& target, bool resume = false) const;
    std::vector<std::filesystem::path> conflicts(const GitSnapshotId& expected, const GitSnapshotId& target, bool resume = false) const;
    bool covers(const std::filesystem::path& path) const;
private:
    GitSnapshot() = default;
    using Files = std::map<std::filesystem::path, GitSnapshotFile>;
    std::filesystem::path worktree;
    std::filesystem::path storage;
    bool directoryOnly = false;
    bool excludeInternalData = false;
    std::string git(const std::vector<std::string>& arguments, const std::optional<std::filesystem::path>& input = {}) const;
    void initialize() const;
    bool ignored(const std::filesystem::path& relative) const;
    bool safeParents(const std::filesystem::path& relative) const;
    std::optional<GitSnapshotFile> inspect(const std::filesystem::path& relative) const;
    Files files(const GitSnapshotId& snapshot) const;
    std::vector<std::string> eligiblePaths() const;
    std::vector<std::string> directoryPaths(const std::filesystem::path& relative = {}) const;
    std::set<std::string> excludedDirectoryPaths(const std::string& candidates) const;
    void stageFiles(const std::vector<std::string>& paths) const;
    void apply(const std::vector<GitSnapshotChange>& changes, bool forward) const;
    std::vector<std::filesystem::path> checkConflicts(const GitSnapshotId& expected, const GitSnapshotId& target, bool resume) const;
};

#endif // SOS_GIT_SNAPSHOT_HPP
