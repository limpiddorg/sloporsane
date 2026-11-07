#include <daemon/session/agent_worktree.hpp>
#include <daemon/session/worktree_listing.hpp>
#include <daemon/session/implementation_worktree.hpp>
#include <daemon/session/session_journal.hpp>
#include <daemon/snapshots/turn_files.hpp>
#include <agent-runtime/tools/fs/common.hpp>
#include <agent-runtime/common/path_resolution.hpp>
#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <fstream>

namespace {
namespace fs = std::filesystem;
class AgentWorktreeTest : public testing::Test {
protected:
    fs::path directory, repo;
    std::optional<std::string> oldData;
    void SetUp() override {
        directory = fs::current_path() / "build-ai" / ("agent-worktree-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        repo = directory / "repo";
        fs::create_directories(repo);
        if (const auto* data = std::getenv("XDG_DATA_HOME")) oldData = data;
        setenv("XDG_DATA_HOME", (directory / "data").c_str(), 1);
        git(repo, {"init", "--template="});
        git(repo, {"config", "user.name", "Test"});
        git(repo, {"config", "user.email", "test@example.invalid"});
        fs_tools::writeText(repo / "file", "committed\n");
        git(repo, {"add", "."});
        git(repo, {"-c", "commit.gpgSign=false", "commit", "-m", "Base"});
    }
    void TearDown() override {
        if (oldData) setenv("XDG_DATA_HOME", oldData->c_str(), 1); else unsetenv("XDG_DATA_HOME");
        fs::remove_all(directory);
    }
    std::string git(const fs::path& cwd, const std::vector<std::string>& args) {
        const auto result = core::runProcess("git", args, cwd);
        if (result.exitCode) throw std::runtime_error(result.error);
        return result.output;
    }
    std::string readBytes(const fs::path& path) {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) throw std::runtime_error("Cannot read test file: " + path.string());
        return {std::istreambuf_iterator<char>(stream), {}};
    }
};

TEST_F(AgentWorktreeTest, CreatesFromHeadWithoutTouchingDirtyFilesIndexOrHooks) {
    fs_tools::writeText(repo / "file", "staged\n"); git(repo, {"add", "file"});
    fs_tools::writeText(repo / "file", "dirty\n");
    fs::create_directories(repo / ".sos");
    fs_tools::writeText(repo / ".sos/mcps.json", "local configuration");
    const auto status = git(repo, {"status", "--porcelain"});
    const auto index = readBytes(repo / ".git/index");
    fs_tools::writeText(repo / ".git/hooks/post-checkout", "#!/bin/sh\necho called > '" + (directory / "hook").string() + "'\n");
    fs::permissions(repo / ".git/hooks/post-checkout", fs::perms(0755));
    AgentWorktree worktrees(repo);
    const auto target = worktrees.create("feature");
    EXPECT_EQ(target.filename(), "feature");
    EXPECT_EQ(target.parent_path().parent_path(), fs::canonical(PathResolution::workspacesDirectory()));
    EXPECT_TRUE(target.parent_path().filename().string().starts_with("repo-"));
    EXPECT_EQ(target.parent_path().filename().string().size(), 5 + 16);
    EXPECT_EQ(git(target, {"branch", "--show-current"}), "sos/feature\n");
    EXPECT_EQ(fs_tools::readText(target / "file"), "committed\n");
    EXPECT_FALSE(fs::exists(target / ".sos"));
    EXPECT_FALSE(fs::exists(directory / "hook"));
    EXPECT_EQ(git(repo, {"status", "--porcelain"}), status);
    EXPECT_EQ(readBytes(repo / ".git/index"), index);
    EXPECT_EQ(git(target, {"rev-parse", "HEAD"}), git(repo, {"rev-parse", "HEAD"}));
    EXPECT_THROW(worktrees.create("feature"), std::runtime_error);
    EXPECT_THROW(worktrees.create("../escape"), std::invalid_argument);
}

TEST_F(AgentWorktreeTest, SharesRepositoryStorageFromLinkedWorktreesAndSymlinks) {
    const auto first = AgentWorktree(repo).create("first");
    const auto second = AgentWorktree(first).create("second");
    EXPECT_EQ(first.parent_path(), second.parent_path());
    const auto alias = directory / "alias";
    fs::create_directory_symlink(repo, alias);
    const auto third = AgentWorktree(alias).create("third");
    EXPECT_EQ(first.parent_path(), third.parent_path());
}

TEST_F(AgentWorktreeTest, SeparatesClonesWithTheSameRepositoryName) {
    const auto clone = directory / "other" / "repo";
    fs::create_directories(clone.parent_path());
    git(repo, {"clone", "--no-hardlinks", repo.string(), clone.string()});
    const auto first = AgentWorktree(repo).create("test");
    const auto second = AgentWorktree(clone).create("test");
    EXPECT_NE(first.parent_path(), second.parent_path());
    EXPECT_TRUE(second.parent_path().filename().string().starts_with("repo-"));
    EXPECT_EQ(git(second, {"branch", "--show-current"}), "sos/test\n");
}

TEST_F(AgentWorktreeTest, KeepsLegacyWorktreesSelectableWithoutMovingOrRenamingThem) {
    const auto legacy = directory / "data" / "sos" / "agent-workspaces" / "legacy";
    fs::create_directories(legacy.parent_path());
    git(repo, {"worktree", "add", "-b", "sos/agent/legacy", legacy.string()});
    const auto fresh = AgentWorktree(legacy).create("fresh");
    EXPECT_EQ(AgentWorktree(repo).select(legacy), fs::canonical(legacy));
    EXPECT_EQ(git(legacy, {"branch", "--show-current"}), "sos/agent/legacy\n");
    EXPECT_NE(legacy.parent_path(), fresh.parent_path());
    EXPECT_EQ(AgentWorktree(repo).list().size(), 3);
}

TEST_F(AgentWorktreeTest, RemovesCleanWorktreeButKeepsItsBranch) {
    AgentWorktree worktrees(repo);
    const auto target = worktrees.create("delete");
    EXPECT_TRUE(worktrees.list().front().value("primary", false));
    worktrees.remove(target, {repo});
    EXPECT_FALSE(fs::exists(target));
    EXPECT_EQ(worktrees.list().size(), 1);
    EXPECT_FALSE(git(repo, {"show-ref", "--verify", "refs/heads/sos/delete"}).empty());
}

TEST_F(AgentWorktreeTest, RejectsPrimaryOpenedDirtyAndLockedWorktreeDeletion) {
    AgentWorktree worktrees(repo);
    const auto target = worktrees.create("protected");
    EXPECT_THROW(worktrees.remove(repo, {}), std::logic_error);
    EXPECT_THROW(worktrees.remove(target, {target}), std::logic_error);
    fs::create_directories(target / "subdir");
    EXPECT_THROW(worktrees.remove(target, {target / "subdir"}), std::logic_error);
    fs::remove(target / "subdir");
    fs_tools::writeText(target / "file", "dirty\n");
    EXPECT_THROW(worktrees.remove(target, {}), std::runtime_error);
    EXPECT_EQ(fs_tools::readText(target / "file"), "dirty\n");
    git(target, {"checkout", "--", "file"});
    fs_tools::writeText(target / "untracked", "keep\n");
    EXPECT_THROW(worktrees.remove(target, {}), std::runtime_error);
    fs::remove(target / "untracked");
    git(repo, {"worktree", "lock", target.string()});
    EXPECT_THROW(worktrees.remove(target, {}), std::runtime_error);
    EXPECT_TRUE(fs::exists(target));
}

TEST_F(AgentWorktreeTest, ListsAndSelectsManualWorktreesButRejectsOtherRepositories) {
    const auto manual = directory / "manual";
    git(repo, {"worktree", "add", "--detach", manual.string()});
    AgentWorktree worktrees(repo);
    EXPECT_EQ(worktrees.list().size(), 2);
    EXPECT_EQ(worktrees.select(manual), fs::canonical(manual));
    EXPECT_EQ(AgentWorktree(manual).select(repo), fs::canonical(repo));
    const auto foreign = directory / "foreign";
    fs::create_directories(foreign); git(foreign, {"init", "--template="});
    EXPECT_THROW(worktrees.select(foreign), std::invalid_argument);
    EXPECT_THROW(worktrees.select(repo / "file"), std::invalid_argument);
}

TEST_F(AgentWorktreeTest, ListsAndSelectsPathsWithSpacesAndGitEscapes) {
    const auto manual = directory / "manual space-é-\t-\n-\"-\\";
    git(repo, {"worktree", "add", "--detach", manual.string()});
    AgentWorktree worktrees(repo);
    EXPECT_EQ(worktrees.list().size(), 2);
    EXPECT_EQ(worktrees.select(manual), fs::canonical(manual));
    git(repo, {"config", "core.quotePath", "false"});
    EXPECT_EQ(worktrees.select(manual), fs::canonical(manual));
}

TEST(WorktreeListingTest, DecodesQuotedPathsAndPreservesOtherFields) {
    const auto fields = worktreeListingFields("worktree \"/tmp/a\\n\\t\\\"\\\\\\303\\251\"\nHEAD abc\ndetached\n\nworktree /tmp/plain space\nbare");
    ASSERT_EQ(fields.size(), 6);
    EXPECT_EQ(fields[0], "worktree /tmp/a\n\t\"\\é");
    EXPECT_EQ(fields[1], "HEAD abc");
    EXPECT_EQ(fields[2], "detached");
    EXPECT_EQ(fields[4], "worktree /tmp/plain space");
    EXPECT_EQ(fields[5], "bare");
    EXPECT_THROW(worktreeListingFields("worktree \"/tmp/\\q\""), std::runtime_error);
}

TEST(WorktreeListingTest, ReadsRawMultilinePathsFromOlderGit) {
    const auto fields = worktreeListingFields("worktree /tmp/first\n\nsecond\nHEAD 0123456789012345678901234567890123456789\ndetached\n");
    ASSERT_EQ(fields.size(), 3);
    EXPECT_EQ(fields[0], "worktree /tmp/first\n\nsecond");
    EXPECT_EQ(fields[2], "detached");
}

TEST_F(AgentWorktreeTest, ImplementImportsOnlyIntoSelectedAgentWorktree) {
    const auto active = AgentWorktree(repo).create("active");
    const auto store = directory / "implementation"; fs::create_directories(store);
    ImplementationWorktree implementation(store);
    const auto isolated = implementation.start(active, "test-implement");
    GitSnapshot snapshots(isolated);
    TurnSnapshot step; step.root = isolated.string(); step.before = snapshots.capture().tree;
    fs_tools::writeText(isolated / "file", "implemented\n");
    step.after = snapshots.capture().tree; step.complete = true;
    implementation.seal(0, step); implementation.import(0, step);
    EXPECT_EQ(fs_tools::readText(active / "file"), "implemented\n");
    EXPECT_EQ(fs_tools::readText(repo / "file"), "committed\n");
    implementation.close();
    EXPECT_TRUE(fs::is_directory(active));
}

TEST_F(AgentWorktreeTest, UndoRedoRetainsBothWorkspaceRootsAndPreflightsAllConflicts) {
    const auto active = AgentWorktree(repo).create("undo");
    const auto store = directory / "conversation"; fs::create_directories(store);
    SessionInfo info; info.id = "test"; info.workspace = repo;
    SessionJournal journal(store); journal.save({}, info);
    TurnFiles files(store); auto snapshot = files.prepare(repo, repo);
    snapshot.start = journal.count(); files.begin(snapshot);
    journal.append({Message{"user", MessageRole::USER, {TextContent{"Switch and work"}}}}, 1);
    files.mutate({}, [&] { fs_tools::writeText(repo / "file", "source change\n"); });
    files.switchWorkspace(active, active);
    files.mutate({}, [&] { fs_tools::writeText(active / "file", "target change\n"); });
    files.finish();
    fs_tools::writeText(repo / "file", "external change\n");
    EXPECT_THROW(TurnFiles::navigate(store, false), std::runtime_error);
    EXPECT_EQ(fs_tools::readText(active / "file"), "target change\n");
    fs_tools::writeText(repo / "file", "source change\n");
    TurnFiles::navigate(store, false);
    EXPECT_EQ(fs_tools::readText(repo / "file"), "committed\n");
    EXPECT_EQ(fs_tools::readText(active / "file"), "committed\n");
    TurnFiles::navigate(store, true);
    EXPECT_EQ(fs_tools::readText(repo / "file"), "source change\n");
    EXPECT_EQ(fs_tools::readText(active / "file"), "target change\n");
}
}
