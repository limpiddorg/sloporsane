#include <daemon/snapshots/git_snapshot.hpp>
#include <daemon/session/implementation_run.hpp>
#include <daemon/session/implementation_changes.hpp>
#include <agent-runtime/common/path_resolution.hpp>
#include <agent-runtime/core/process.hpp>
#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <fstream>

namespace {
namespace fs = std::filesystem;

class GitSnapshotTest : public testing::Test {
protected:
    fs::path directory, repo;
    std::optional<std::string> oldData, oldConfig;
    void SetUp() override {
        directory = fs::temp_directory_path() / ("sos-snapshots-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        repo = directory / "worktree";
        fs::create_directories(repo);
        if (const auto* value = std::getenv("XDG_DATA_HOME")) oldData = value;
        if (const auto* value = std::getenv("XDG_CONFIG_HOME")) oldConfig = value;
        setenv("XDG_DATA_HOME", (directory / "data").c_str(), 1);
        setenv("XDG_CONFIG_HOME", (directory / "config").c_str(), 1);
        git({"init", "--template="});
        git({"config", "user.name", "Snapshot test"});
        git({"config", "user.email", "snapshot@example.invalid"});
    }
    void TearDown() override {
        if (oldData) setenv("XDG_DATA_HOME", oldData->c_str(), 1); else unsetenv("XDG_DATA_HOME");
        if (oldConfig) setenv("XDG_CONFIG_HOME", oldConfig->c_str(), 1); else unsetenv("XDG_CONFIG_HOME");
        fs::remove_all(directory);
    }
    std::string git(const std::vector<std::string>& args) {
        const auto result = core::runProcess("git", args, repo);
        if (result.exitCode) throw std::runtime_error(result.error);
        return result.output;
    }
    void write(const fs::path& path, const std::string& bytes) {
        fs::create_directories((repo / path).parent_path());
        std::ofstream file(repo / path, std::ios::binary);
        file.write(bytes.data(), bytes.size());
    }
    std::string read(const fs::path& path) {
        std::ifstream file(repo / path, std::ios::binary);
        return {std::istreambuf_iterator<char>(file), {}};
    }
};

TEST_F(GitSnapshotTest, DiscoversRootWithoutCreatingAnythingOutsideGit) {
    fs::create_directories(repo / "nested/deeper");
    EXPECT_EQ(GitSnapshot::discover(repo / "nested/deeper"), fs::canonical(repo));
    fs::create_directory(directory / "plain");
    EXPECT_FALSE(GitSnapshot::discover(directory / "plain"));
    EXPECT_THROW(GitSnapshot(directory / "plain"), std::invalid_argument);
    EXPECT_FALSE(fs::exists(PathResolution::snapshotsDirectory()));
}

TEST_F(GitSnapshotTest, HistoricalReviewExportNeverReadsLaterChangesOrFollowsExternalLinks) {
    write("src/state.txt", "step-one\n");
    fs::create_symlink("state.txt", repo / "src/local");
    fs::create_symlink((directory / "external").string(), repo / "external-link");
    std::ofstream(directory / "external") << "private";
    GitSnapshot snapshots(repo);
    const auto step = snapshots.capture();
    write("src/state.txt", "step-two\n");
    write("future.txt", "later file");
    const auto destination = directory / "historical";
    snapshots.exportTo(step, destination);
    std::ifstream file(destination / "src/state.txt");
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(file), {}), "step-one\n");
    EXPECT_FALSE(fs::exists(destination / "future.txt"));
    EXPECT_FALSE(fs::is_symlink(destination / "external-link"));
    EXPECT_TRUE(fs::is_symlink(destination / "src/local"));
    EXPECT_EQ(read("src/state.txt"), "step-two\n");
    EXPECT_THROW(snapshots.exportTo(step, destination), std::runtime_error);
}

TEST_F(GitSnapshotTest, CapturesUnbornRepositoryAndPinsPersistentTrees) {
    write("file", "before");
    GitSnapshot snapshots(repo);
    EXPECT_EQ(snapshots.storageDirectory().parent_path(), fs::canonical(PathResolution::snapshotsDirectory()));
    const auto before = snapshots.capture();
    EXPECT_EQ(before.tree, snapshots.capture().tree);
    write("file", "after");
    const auto after = snapshots.capture();
    GitSnapshot reopened(repo);
    const auto changes = reopened.changes(before, after);
    ASSERT_EQ(changes.size(), 1);
    EXPECT_EQ(changes[0].path, "file");
    EXPECT_NE(changes[0].before->blob, changes[0].after->blob);
    const auto diffs = reopened.diffs(before, after);
    ASSERT_EQ(diffs.size(), 1);
    EXPECT_EQ(diffs[0].path, "file");
    EXPECT_NE(diffs[0].patch.find("-before"), std::string::npos);
    EXPECT_NE(diffs[0].patch.find("+after"), std::string::npos);
    const auto gc = core::runProcess("git", {"--git-dir=" + (snapshots.storageDirectory() / "repo.git").string(), "gc", "--prune=now"}, directory);
    ASSERT_EQ(gc.exitCode, 0) << gc.error;
    EXPECT_TRUE(reopened.restore(after, before).empty());
    EXPECT_EQ(read("file"), "before");
    EXPECT_TRUE(reopened.restore(before, after).empty());
    EXPECT_EQ(read("file"), "after");
}

TEST_F(GitSnapshotTest, RepositoryPathsStayAtTheHistoricalBaseline) {
    write("README.md", "readme");
    write("src/base.cpp", "base");
    write(".gitignore", "ignored/\n");
    write("ignored/cache", "cache");
    GitSnapshot snapshots(repo);
    const auto before = snapshots.capture();
    write("later.cpp", "later");
    fs::remove(repo / "README.md");
    const std::vector<std::string> expected{".gitignore", "README.md", "src/base.cpp"};
    EXPECT_EQ(snapshots.paths(before), expected);
    EXPECT_EQ(GitSnapshot(repo).paths(before), expected);
}

TEST_F(GitSnapshotTest, RepositoryDirectoryListsOnlyImmediateHistoricalChildren) {
    write("README.md", "base"); write("src/base.cpp", "base"); write("src/deep/hidden.cpp", "hidden");
    fs::create_symlink("src", repo / "link");
    GitSnapshot snapshots(repo);
    const auto before = snapshots.capture();
    const auto root = snapshots.children(before, "");
    ASSERT_EQ(root.size(), 3);
    EXPECT_EQ(root[0].name, "README.md"); EXPECT_FALSE(root[0].directory);
    EXPECT_EQ(root[1].name, "link"); EXPECT_FALSE(root[1].directory);
    EXPECT_EQ(root[2].name, "src"); EXPECT_TRUE(root[2].directory);
    write("src/later.cpp", "later"); fs::remove(repo / "src/base.cpp");
    const auto src = GitSnapshot::archive(snapshots.root()).children(before, "src");
    ASSERT_EQ(src.size(), 2);
    EXPECT_EQ(src[0].name, "base.cpp"); EXPECT_FALSE(src[0].directory);
    EXPECT_EQ(src[1].name, "deep"); EXPECT_TRUE(src[1].directory);
    const auto deep = snapshots.children(before, "src/deep");
    ASSERT_EQ(deep.size(), 1); EXPECT_EQ(deep[0].name, "hidden.cpp");
    for (const auto* path : {"..", "src/../src", "/src", "src/", "src//deep", ".git", "link", "README.md"})
        EXPECT_ANY_THROW(snapshots.children(before, path)) << path;
    EXPECT_ANY_THROW(snapshots.children({"HEAD"}, ""));
}

TEST_F(GitSnapshotTest, NewDirectoriesHaveNoHistoricalNeighboursButMissingSnapshotsStillFail) {
    write("README.md", "base"); write("src/base.cpp", "base");
    GitSnapshot snapshots(repo);
    const auto before = snapshots.capture();
    write("src/snake/model.cpp", "new"); write("new/deep/model.cpp", "new");
    write("src/[literal]/model.cpp", "literal");
    for (const auto* path : {"missing", "src/snake", "new/deep", "src/[literal]"})
        EXPECT_TRUE(snapshots.children(before, path).empty()) << path;
    const auto after = snapshots.capture();
    const auto literal = snapshots.children(after, "src/[literal]");
    ASSERT_EQ(literal.size(), 1);
    EXPECT_EQ(literal.front().name, "model.cpp");
    EXPECT_ANY_THROW(snapshots.children({std::string(40, '0')}, ""));
    EXPECT_ANY_THROW(snapshots.children({std::string(40, '0')}, "src/snake"));
}

TEST_F(GitSnapshotTest, ImplementationPublishesOnlyBaselineIdentityAndLoadsDirectoriesOnDemand) {
    write("README.md", "base"); write("src/base.cpp", "base");
    GitSnapshot snapshots(repo);
    TurnSnapshot boundary; boundary.id = 1; boundary.root = snapshots.root().string();
    boundary.before = snapshots.capture().tree;
    boundary.edits.push_back({(repo / "README.md").string(), {}, {}, true});
    const auto storage = directory / "implementation"; fs::create_directory(storage);
    ImplementationRun run(storage);
    const auto plan = nlohmann::json::parse(R"({"steps":[{"title":"Model","objective":"Expose model","substeps":[{"title":"State","intention":"Represent state","changes":"Define model"}],"validation":["Run checks"]}]})").get<ImplementationPlan>();
    run.start(plan, "step_by_step"); run.begin(boundary);
    EXPECT_TRUE(run.baseline()->edits.empty()); // Work already journaled in this same turn must not be duplicated.
    const nlohmann::json expected = {{{"name", "README.md"}, {"directory", false}}, {{"name", "src"}, {"directory", true}}};
    EXPECT_FALSE(run.view()["steps"][0].contains("repository_files"));
    EXPECT_EQ(run.view()["steps"][0]["repository_tree"], boundary.before);
    EXPECT_EQ(run.repositoryDirectory(0, boundary.before, ""), expected);
    write("later.cpp", "later");
    ImplementationRun restored(storage);
    EXPECT_EQ(restored.repositoryDirectory(0, boundary.before, ""), expected);
    EXPECT_EQ(restored.repositoryDirectory(0, boundary.before, "src"),
        (nlohmann::json::array({{{"name", "base.cpp"}, {"directory", false}}})));
    EXPECT_ANY_THROW(restored.repositoryDirectory(1, boundary.before, ""));
    EXPECT_ANY_THROW(restored.repositoryDirectory(0, std::string(40, 'a'), ""));
    EXPECT_ANY_THROW(restored.repositoryDirectory(0, boundary.before, "../"));
    EXPECT_EQ(restored.view()["steps"][0]["repository_root"], snapshots.root().string());
    fs::rename(snapshots.storageDirectory(), snapshots.storageDirectory().string() + "-temporarily-unavailable");
    EXPECT_EQ(restored.view()["steps"][0]["repository_tree"], boundary.before); // No Git access during state publication.
}

TEST_F(GitSnapshotTest, ImplementationBoundariesPreserveGitIgnoredBinaryAndUserIndex) {
    write(".gitignore", "ignored\n"); write("model", "base\n"); write("ignored", "cached\n");
    git({"add", ".gitignore", "model"});
    const auto index = read(".git/index"), head = read(".git/HEAD");
    const auto storage = directory / "boundary-evidence"; fs::create_directory(storage);
    TurnFiles files(storage); files.begin(files.prepare(repo, repo));
    ImplementationBoundaries boundaries(storage);
    boundaries.capture(0, 1, {0, "started", ""}, files.boundary());
    files.mutate({repo / "model", repo / "ignored"}, [&] { write("model", "broken\n"); write("ignored", "updated\n"); });
    boundaries.capture(0, 1, {0, "completed", "Built"}, files.boundary());
    boundaries.capture(0, 1, {{}, "validating", ""}, files.boundary());
    boundaries.capture(0, 1, {0, "correcting", ""}, files.boundary());
    files.mutate({repo / "model", repo / "ignored"}, [&] { write("model", "corrected\n"); write("ignored", std::string("\0binary", 7)); });
    boundaries.capture(0, 1, {0, "completed", "Repaired"}, files.boundary());
    boundaries.capture(0, 1, {{}, "finished", "Checks passed"}, files.boundary());
    files.finish();
    const auto final = files.diffs(files.boundary());
    const auto owned = preserveImplementationOwnership(boundaries.read(0), final, 1);
    ASSERT_EQ(final.size(), 2); ASSERT_EQ(owned[0].size(), 2);
    EXPECT_EQ(owned[0][0].before, "base\n"); EXPECT_EQ(owned[0][0].after, "corrected\n");
    EXPECT_NE(owned[0][1].patch.find("Binary contents changed"), std::string::npos);
    EXPECT_EQ(read(".git/index"), index); EXPECT_EQ(read(".git/HEAD"), head);
}

TEST_F(GitSnapshotTest, ValidationCommandChangesCanBeRecoveredWithoutReassigningConstruction) {
    write("model", "base\n");
    git({"add", "model"});
    const auto index = read(".git/index"), head = read(".git/HEAD");
    const auto storage = directory / "validation-recovery"; fs::create_directory(storage);
    TurnFiles files(storage); files.begin(files.prepare(repo, repo));
    ImplementationBoundaries boundaries(storage);
    boundaries.capture(0, 2, {0, "started", "Build model"}, files.boundary());
    files.mutate({repo / "model"}, [&] { write("model", "built\n"); });
    boundaries.capture(0, 2, {0, "completed", "Built"}, files.boundary());
    boundaries.capture(0, 2, {1, "unnecessary", "No wiring"}, files.boundary());
    boundaries.capture(0, 2, {{}, "validating", "Check"}, files.boundary());
    bool executed = false;
    EXPECT_THROW(files.mutate({}, [&] { executed = true; write("generated.txt", "test output\n"); },
        [&](const auto& snapshot) { boundaries.capture(0, 2, {{}, "mutation", ""}, snapshot); }), std::logic_error);
    EXPECT_TRUE(executed);
    ASSERT_NO_THROW(boundaries.capture(0, 2, {1, "correcting", "Account for test output"}, files.boundary()));
    boundaries.capture(0, 2, {1, "completed", "Checked output"}, files.boundary());
    EXPECT_NO_THROW(files.mutate({}, [] {}, [&](const auto& snapshot) {
        boundaries.capture(0, 2, {{}, "mutation", ""}, snapshot);
    }));
    boundaries.capture(0, 2, {{}, "finished", "Passed"}, files.boundary());
    files.finish();
    const auto owned = preserveImplementationOwnership(boundaries.read(0), files.diffs(files.boundary()), 2);
    ASSERT_EQ(owned[0].size(), 1); ASSERT_EQ(owned[1].size(), 1);
    EXPECT_EQ(owned[0][0].path, "model");
    EXPECT_EQ(owned[0][0].before, "base\n"); EXPECT_EQ(owned[0][0].after, "built\n");
    EXPECT_EQ(owned[1][0].path, "generated.txt");
    EXPECT_EQ(owned[1][0].after, "test output\n");
    EXPECT_EQ(read(".git/index"), index); EXPECT_EQ(read(".git/HEAD"), head);
}

TEST_F(GitSnapshotTest, LeavesUserIndexHeadAndConfigurationUntouched) {
    write("tracked", "committed");
    git({"add", "tracked"});
    git({"commit", "-m", "initial"});
    write("tracked", "staged");
    git({"add", "tracked"});
    write("tracked", "unstaged");
    const auto index = read(".git/index"), head = read(".git/HEAD"), config = read(".git/config");
    const auto refs = git({"show-ref"});
    GitSnapshot snapshots(repo);
    const auto before = snapshots.capture();
    write("tracked", "new");
    const auto after = snapshots.capture();
    ASSERT_TRUE(snapshots.restore(after, before).empty());
    EXPECT_EQ(read("tracked"), "unstaged");
    EXPECT_EQ(read(".git/index"), index);
    EXPECT_EQ(read(".git/HEAD"), head);
    EXPECT_EQ(read(".git/config"), config);
    EXPECT_EQ(git({"show-ref"}), refs);
}

TEST_F(GitSnapshotTest, RestoresAddDeleteRenameBinaryAndLiteralPathsInBothDirections) {
    const std::string binary("\0first\r\n\xff", 9);
    write("old", binary);
    write(":(glob)* weird\nname", "before");
    GitSnapshot snapshots(repo);
    const auto before = snapshots.capture();
    fs::rename(repo / "old", repo / "renamed");
    write("new/sub/file", "new");
    write(":(glob)* weird\nname", "after");
    const auto after = snapshots.capture();
    ASSERT_EQ(snapshots.changes(before, after).size(), 4);
    const auto patches = snapshots.diffs(before, after);
    ASSERT_EQ(patches.size(), 4);
    for (const auto& patch : patches) {
        EXPECT_FALSE(patch.patch.empty()) << patch.path;
        EXPECT_NO_THROW(nlohmann::json(patch).dump());
        if (patch.path == "old" || patch.path == "renamed") {
            EXPECT_TRUE(patch.before.empty()); EXPECT_TRUE(patch.after.empty());
        } else if (patch.path == ":(glob)* weird\nname") {
            EXPECT_EQ(patch.before, "before"); EXPECT_EQ(patch.after, "after");
        }
    }
    EXPECT_TRUE(snapshots.restore(after, before).empty());
    EXPECT_EQ(read("old"), binary);
    EXPECT_EQ(read(":(glob)* weird\nname"), "before");
    EXPECT_FALSE(fs::exists(repo / "renamed"));
    EXPECT_FALSE(fs::exists(repo / "new/sub/file"));
    EXPECT_TRUE(snapshots.restore(before, after).empty());
    EXPECT_EQ(read("renamed"), binary);
    EXPECT_EQ(read("new/sub/file"), "new");
    EXPECT_FALSE(fs::exists(repo / "old"));
}

TEST_F(GitSnapshotTest, ExcludesIgnoredFilesEvenWhenTrackedAndDoesNotTouchUnrelatedFiles) {
    write("tracked-secret", "old");
    git({"add", "tracked-secret"});
    write(".gitignore", "tracked-secret\nignored/\n");
    write(".git/info/exclude", "local-secret\n");
    write("ignored/cache", "cache");
    write("local-secret", "secret");
    write("regular", "before");
    GitSnapshot snapshots(repo);
    const auto before = snapshots.capture();
    write("tracked-secret", "changed");
    write("ignored/cache", "changed");
    write("local-secret", "changed");
    write("regular", "after");
    const auto after = snapshots.capture();
    ASSERT_EQ(snapshots.changes(before, after).size(), 1);
    write("unrelated", "human");
    EXPECT_TRUE(snapshots.restore(after, before).empty());
    EXPECT_EQ(read("unrelated"), "human");
    EXPECT_EQ(read("tracked-secret"), "changed");
    EXPECT_EQ(read("ignored/cache"), "changed");
    EXPECT_EQ(read("local-secret"), "changed");
}

TEST_F(GitSnapshotTest, RefusesAllChangesWhenOneFileConflictsInEitherDirection) {
    write("a", "before");
    write("b", "before");
    GitSnapshot snapshots(repo);
    const auto before = snapshots.capture();
    write("a", "after");
    write("b", "after");
    const auto after = snapshots.capture();
    write("b", "human");
    EXPECT_EQ(snapshots.restore(after, before), std::vector<fs::path>{"b"});
    EXPECT_EQ(read("a"), "after");
    EXPECT_EQ(read("b"), "human");
    write("b", "after");
    EXPECT_TRUE(snapshots.restore(after, before).empty());
    fs::remove(repo / "a");
    EXPECT_EQ(snapshots.restore(before, after), std::vector<fs::path>{"a"});
    EXPECT_EQ(read("b"), "before");
}

TEST_F(GitSnapshotTest, ProtectsNewlyIgnoredFilesAndSymlinkAncestors) {
    write("folder/file", "before");
    GitSnapshot snapshots(repo);
    const auto before = snapshots.capture();
    write("folder/file", "after");
    const auto after = snapshots.capture();
    write(".gitignore", "folder/\n");
    EXPECT_EQ(snapshots.restore(after, before), std::vector<fs::path>{"folder/file"});
    EXPECT_EQ(read("folder/file"), "after");
    fs::remove(repo / ".gitignore");
    fs::rename(repo / "folder", directory / "outside");
    fs::create_directory_symlink(directory / "outside", repo / "folder");
    EXPECT_EQ(snapshots.restore(after, before), std::vector<fs::path>{"folder/file"});
    EXPECT_EQ(read("folder/file"), "after");
}

TEST_F(GitSnapshotTest, CapturesLinksWithoutFollowingTargetsAndPreservesExecutableBit) {
    write("../external", "outside");
    fs::create_symlink("../external", repo / "link");
    write("script", "script");
    fs::permissions(repo / "script", fs::perms(0755));
    GitSnapshot snapshots(repo);
    const auto before = snapshots.capture();
    fs::remove(repo / "link");
    fs::create_symlink("missing", repo / "link");
    fs::permissions(repo / "script", fs::perms(0644));
    const auto after = snapshots.capture();
    EXPECT_TRUE(snapshots.restore(after, before).empty());
    EXPECT_EQ(fs::read_symlink(repo / "link"), "../external");
    EXPECT_EQ(read("../external"), "outside");
    EXPECT_NE(fs::status(repo / "script").permissions() & fs::perms::owner_exec, fs::perms::none);
    EXPECT_TRUE(snapshots.restore(before, after).empty());
    EXPECT_EQ(fs::read_symlink(repo / "link"), "missing");
}

TEST_F(GitSnapshotTest, DoesNotRunUserFiltersOrNormalizeLineEndings) {
    write(".gitattributes", "*.txt text eol=crlf filter=unsafe\n");
    git({"config", "filter.unsafe.clean", "touch filter-ran; cat"});
    git({"config", "filter.unsafe.smudge", "touch filter-ran; cat"});
    write("raw.txt", "mixed\r\nraw\n");
    GitSnapshot snapshots(repo);
    const auto before = snapshots.capture();
    write("raw.txt", "changed");
    const auto after = snapshots.capture();
    EXPECT_TRUE(snapshots.restore(after, before).empty());
    EXPECT_EQ(read("raw.txt"), "mixed\r\nraw\n");
    EXPECT_FALSE(fs::exists(repo / "filter-ran"));
}

TEST_F(GitSnapshotTest, StreamsLargeBinaryBlobsBeyondProcessCaptureLimit) {
    const std::string bytes(9 * 1024 * 1024, '\0');
    write("large", bytes);
    GitSnapshot snapshots(repo);
    const auto before = snapshots.capture();
    write("large", "small");
    const auto after = snapshots.capture();
    EXPECT_TRUE(snapshots.restore(after, before).empty());
    EXPECT_EQ(read("large"), bytes);
}

TEST_F(GitSnapshotTest, SupportsLinkedWorktreesWithSeparateStorage) {
    write("file", "initial");
    git({"add", "file"});
    git({"commit", "-m", "initial"});
    git({"worktree", "add", "-b", "linked", (directory / "linked").string()});
    GitSnapshot primary(repo), linked(directory / "linked");
    EXPECT_NE(primary.storageDirectory(), linked.storageDirectory());
    const auto before = linked.capture();
    write("../linked/file", "changed");
    const auto after = linked.capture();
    EXPECT_TRUE(linked.restore(after, before).empty());
    EXPECT_EQ(read("../linked/file"), "initial");
    EXPECT_EQ(read("file"), "initial");
}

TEST_F(GitSnapshotTest, RejectsInvalidIdsAndUnsupportedDirectoryReplacementWithoutMutation) {
    write("file", "before");
    GitSnapshot snapshots(repo);
    const auto before = snapshots.capture();
    EXPECT_THROW(snapshots.changes({"--all"}, before), std::invalid_argument);
    fs::remove(repo / "file");
    write("file/nested", "after");
    const auto after = snapshots.capture();
    EXPECT_FALSE(snapshots.restore(after, before).empty());
    EXPECT_EQ(read("file/nested"), "after");
}

TEST_F(GitSnapshotTest, CapturesDeletedTrackedFilesAndProtectsCollidingAdditions) {
    write("tracked", "original");
    git({"add", "tracked"});
    GitSnapshot snapshots(repo);
    const auto before = snapshots.capture();
    fs::remove(repo / "tracked");
    const auto empty = snapshots.capture();
    ASSERT_EQ(snapshots.changes(before, empty).size(), 1);
    EXPECT_TRUE(snapshots.restore(empty, before).empty());
    EXPECT_EQ(read("tracked"), "original");
    EXPECT_TRUE(snapshots.restore(before, empty).empty());
    write("tracked", "human replacement");
    EXPECT_EQ(snapshots.restore(empty, before), std::vector<fs::path>{"tracked"});
    EXPECT_EQ(read("tracked"), "human replacement");
}

TEST_F(GitSnapshotTest, HonorsGlobalExcludes) {
    write("../global-ignore", "private-*\n");
    write("../config/git/config", "[core]\n\texcludesFile = " + (directory / "global-ignore").string() + "\n");
    write("private-note", "secret");
    GitSnapshot snapshots(repo);
    const auto before = snapshots.capture();
    write("private-note", "changed");
    EXPECT_TRUE(snapshots.changes(before, snapshots.capture()).empty());
}

TEST_F(GitSnapshotTest, RejectsStorageInsideWorktreeAndFlattensNestedRepositories) {
    setenv("XDG_DATA_HOME", (repo / "data").c_str(), 1);
    EXPECT_THROW(GitSnapshot{repo}, std::invalid_argument);
    setenv("XDG_DATA_HOME", (directory / "data").c_str(), 1);
    fs::create_directory(repo / "nested");
    const auto init = core::runProcess("git", {"init", "--template="}, repo / "nested");
    ASSERT_EQ(init.exitCode, 0);
    write("nested/file", "embedded contents");
    write("nested/.gitignore", "cache/\n");
    write("nested/cache/file", "ignored");
    GitSnapshot snapshots(repo);
    const auto before = snapshots.capture();
    const auto paths = snapshots.paths(before);
    EXPECT_NE(std::find(paths.begin(), paths.end(), "nested/file"), paths.end());
    EXPECT_EQ(std::find(paths.begin(), paths.end(), "nested/cache/file"), paths.end());
    write("nested/file", "changed");
    EXPECT_TRUE(snapshots.restore(snapshots.capture(), before).empty());
    EXPECT_EQ(read("nested/file"), "embedded contents");
}

TEST_F(GitSnapshotTest, IgnoresInheritedIndexOverrides) {
    const auto* current = std::getenv("GIT_INDEX_FILE");
    const std::optional<std::string> previous = current ? std::make_optional<std::string>(current) : std::nullopt;
    struct RestoreEnvironment {
        std::optional<std::string> previous;
        ~RestoreEnvironment() {
            if (previous) setenv("GIT_INDEX_FILE", previous->c_str(), 1);
            else unsetenv("GIT_INDEX_FILE");
        }
    } restore{previous};
    const auto poisoned = directory / "unrelated-index";
    write("../unrelated-index", "must not be touched");
    setenv("GIT_INDEX_FILE", poisoned.c_str(), 1);
    write("file", "contents");
    GitSnapshot snapshots(repo);
    EXPECT_FALSE(snapshots.capture().tree.empty());
    EXPECT_EQ(read("../unrelated-index"), "must not be touched");
}

TEST(GitSnapshotProcessTest, StreamsOnlyStdoutWhenCaptureIsDisabled) {
    std::string output;
    core::ProcessOptions options;
    options.captureOutput = false;
    options.onStdout = [&](std::string_view bytes) { output += bytes; };
    const auto result = core::runProcess("/bin/sh", {"-c", "printf content; printf diagnostic >&2"},
                                         fs::temp_directory_path(), {}, {}, options);
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_EQ(output, "content");
    EXPECT_TRUE(result.output.empty());
}
}
