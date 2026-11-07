#ifndef SOS_TURN_FILES_HPP
#define SOS_TURN_FILES_HPP

#include "git_snapshot.hpp"
#include "turn_snapshot.hpp"
#include <functional>
#include <memory>

// Owned by a session, used on one worker at a time. No runtime dependency on this type.
class TurnFiles {
public:
    explicit TurnFiles(std::filesystem::path directory);
    TurnSnapshot prepare(const std::filesystem::path& workspace, const std::filesystem::path& authorizedRoot);
    std::int64_t begin(TurnSnapshot snapshot);
    void mutate(const std::vector<std::filesystem::path>& paths, const std::function<void()>& action,
        const std::function<void(const TurnSnapshot&)>& captured = {});
    void finish();
    void switchWorkspace(const std::filesystem::path& workspace, const std::filesystem::path& root);
    TurnSnapshot boundary() const;
    std::vector<ImplementationDiff> diffs(const TurnSnapshot& snapshot) const;
    std::vector<ImplementationDiff> inspect(const std::optional<TurnSnapshot>& baseline = {}) const;
    static void navigate(const std::filesystem::path& directory, bool redo);
    void restoreImplementation(const TurnSnapshot& snapshot, bool forward = false) const;
    TurnSnapshot prepareContribution(const TurnSnapshot& original, const std::vector<ImplementationDiff>& diffs) const;
private:
    void checkpoint();
    SnapshotFileState captureFile(const std::filesystem::path& path) const;
    std::string fileBytes(const SnapshotFileState& state) const;
    void restoreFile(const std::filesystem::path& path, const SnapshotFileState& state) const;
    std::filesystem::path directory, blobs;
    TurnSnapshot turn;
    std::unique_ptr<GitSnapshot> git;
};

#endif // SOS_TURN_FILES_HPP
