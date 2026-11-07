#ifndef SOS_IMPLEMENTATION_WORKTREE_HPP
#define SOS_IMPLEMENTATION_WORKTREE_HPP

#include "session_database.hpp"
#include "../snapshots/turn_snapshot.hpp"
#include "../snapshots/git_snapshot.hpp"
#include "implementation_run.hpp"
#include <agent-runtime/core/process.hpp>

// Execution checkout and durable import journal, independent of the review cursor.
class ImplementationWorktree {
public:
    using GitProcessRunner = std::function<core::ProcessResult(const std::vector<std::string>&,
        const std::filesystem::path&, const core::ProcessOptions&)>;
    explicit ImplementationWorktree(const std::filesystem::path& sessionDirectory, GitProcessRunner runner = {});
    bool exists() const { return !root.empty(); }
    std::filesystem::path start(const std::filesystem::path& workspace, const std::string& sessionId);
    std::filesystem::path executionDirectory() const;
    std::filesystem::path executionRoot() const { return worktree; }
    void seal(std::size_t step, const TurnSnapshot& snapshot);
    void requireReworkable(std::size_t step);
    void revise(std::size_t from, std::uint64_t token, const TurnSnapshot& rollback);
    ImplementationRevisionSeed replay(std::size_t step, std::uint64_t generation, const ImplementationRevisionSource& source, bool preserveExisting);
    void import(std::size_t step, const TurnSnapshot& snapshot);
    void close();
private:
    void initialize(const std::filesystem::path& workspace, const std::string& id);
    void populate();
    void initializePrivateRepository();
    bool registered() const;
    std::optional<std::pair<GitSnapshotId, GitSnapshotId>> pendingImport(
        std::size_t step, const TurnSnapshot& snapshot, const GitSnapshot& target);
    std::string git(const std::filesystem::path& cwd, const std::vector<std::string>& args) const;
    std::string commit(const std::string& tree, const std::string& parent, const std::string& label);
    void save();
    std::filesystem::path storageDirectory;
    GitProcessRunner runGitProcess;
    SessionDatabase db;
    std::filesystem::path root, workspace, worktree;
    std::filesystem::path privateRepository;
    std::string branch, head;
    bool ready = false, closed = false;
};

#endif // SOS_IMPLEMENTATION_WORKTREE_HPP
