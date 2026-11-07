#include <daemon/session/implementation_worktree.hpp>
#include <daemon/session/session_journal.hpp>
#include <daemon/snapshots/git_snapshot.hpp>
#include <daemon/snapshots/private_copy.hpp>
#include <agent-runtime/common/path_resolution.hpp>
#include <agent-runtime/core/process.hpp>
#include <gtest/gtest.h>
#include <cstdlib>
#include <fstream>
#include <chrono>
#include <sys/stat.h>

namespace {
namespace fs = std::filesystem;
class ImplementationWorktreeTest : public testing::Test {
protected:
    fs::path directory, repo, session, plainSource;
    std::optional<std::string> oldData, oldConfig;
    void SetUp() override {
        directory = fs::current_path() / "build-ai" / ("worktree-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        repo = directory / "repository"; session = directory / "session";
        fs::create_directories(repo); fs::create_directories(session);
        if (const auto* value = std::getenv("XDG_DATA_HOME")) oldData = value;
        if (const auto* value = std::getenv("XDG_CONFIG_HOME")) oldConfig = value;
        setenv("XDG_DATA_HOME", (directory / "data").c_str(), 1);
        setenv("XDG_CONFIG_HOME", (directory / "config").c_str(), 1);
        git({"init", "--template="});
        git({"config", "user.name", "Test"}); git({"config", "user.email", "test@example.invalid"});
        write(repo / ".gitignore", "ignored/\n");
        write(repo / "file", "one\ntwo\nthree\nfour\nfive\nsix\nseven\n");
        git({"add", "."}); git({"commit", "-m", "Base"});
    }
    void TearDown() override {
        if (oldData) setenv("XDG_DATA_HOME", oldData->c_str(), 1); else unsetenv("XDG_DATA_HOME");
        if (oldConfig) setenv("XDG_CONFIG_HOME", oldConfig->c_str(), 1); else unsetenv("XDG_CONFIG_HOME");
        fs::remove_all(directory);
        if (!plainSource.empty()) fs::remove_all(plainSource);
    }
    std::string git(const std::vector<std::string>& args) {
        const auto result = core::runProcess("git", args, repo);
        if (result.exitCode) throw std::runtime_error(result.error);
        return result.output;
    }
    void write(const fs::path& path, const std::string& bytes) {
        fs::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary); stream.write(bytes.data(), bytes.size());
    }
    std::string read(const fs::path& path) {
        std::ifstream stream(path, std::ios::binary); return {std::istreambuf_iterator<char>(stream), {}};
    }
    TurnSnapshot step(const fs::path& checkout, const std::function<void()>& edit) {
        GitSnapshot snapshots(checkout);
        TurnSnapshot result; result.root = checkout.string(); result.before = snapshots.capture().tree;
        edit(); result.after = snapshots.capture().tree; result.complete = true;
        return result;
    }
    ImplementationPlan plan(std::size_t count) {
        ImplementationPlan result;
        for (std::size_t i = 0; i < count; ++i) result.steps.push_back({"Step " + std::to_string(i), "A coherent outcome", {{"Code", "Implement", "Code changes"}, {"Checks", "Verify", "Tests"}}, {"Check"}});
        return result;
    }
    TurnSnapshot build(ImplementationRun& run, ImplementationWorktree& work, const fs::path& checkout,
        const std::vector<std::function<void()>>& actions) {
        const auto index = run.next().value().first;
        TurnFiles files(session); files.begin(files.prepare(checkout, checkout)); run.begin(files.boundary());
        ImplementationBoundaries boundaries(session);
        const auto evidence = boundaries.read(index, run.currentGeneration());
        const bool correcting = !evidence.empty() && evidence.back().validating;
        const auto report = [&](std::optional<std::size_t> part, const std::string& status) {
            boundaries.capture(index, actions.size(), {part, status, "Explained"}, run.substepCombined(files.boundary()), run.currentGeneration(), run.currentProgressBoundary());
        };
        for (std::size_t i = 0; i < actions.size(); ++i) {
            report(i, correcting ? "correcting" : "started");
            files.mutate({checkout / "file", checkout / "later", checkout / "ignored/cache"}, actions[i]);
            report(i, "completed");
        }
        report({}, "validating"); report({}, "finished"); files.finish();
        const auto snapshot = files.boundary(), whole = run.substepCombined(snapshot);
        ImplementationCompletion completion{"Checked", std::vector<ImplementationSubstepResult>(actions.size()), "Global recap with validation caveats"};
        for (auto& part : completion.substeps) part.explanation = "Checked contribution";
        run.finish(snapshot, completion, files.diffs(whole), true); work.seal(index, whole); return whole;
    }
    void makePlainDirectory() {
        // build-ai is itself inside this project's Git repository: a plain fixture
        // must have no Git ancestor, while all private storage stays in build-ai.
        fs::remove_all(repo / ".git");
        plainSource = fs::temp_directory_path() / ("sos-plain-" + directory.filename().string());
        fs::create_directories(plainSource);
        fs::copy(repo, plainSource / "workspace", fs::copy_options::recursive | fs::copy_options::copy_symlinks);
        repo = plainSource / "workspace";
        ASSERT_FALSE(GitSnapshot::discover(repo));
    }
};

TEST_F(ImplementationWorktreeTest, ConclusionPersistsAndIsExposedOnlyAfterAllStepsAreBuilt) {
    ImplementationRun run(session); run.start(plan(2), "everything");
    ImplementationWorktree work(session); const auto checkout = work.start(repo, "conclusion");
    EXPECT_NE(run.instruction().find("do NOT block technical completion"), std::string::npos);
    build(run, work, checkout, {[] {}, [] {}});
    EXPECT_EQ(run.view()["conclusion"], "");
    EXPECT_NE(run.instruction().find("This is the LAST step"), std::string::npos);
    EXPECT_NE(run.instruction().find("Checked contribution"), std::string::npos);
    build(run, work, checkout, {[] {}, [] {}});
    EXPECT_EQ(run.view()["conclusion"], "Global recap with validation caveats");
    EXPECT_EQ(ImplementationRun(session).view()["conclusion"], run.view()["conclusion"]);
    const auto old = nlohmann::json{{"explanation", "Historical"}, {"substeps", nlohmann::json::array()}}.get<ImplementationCompletion>();
    EXPECT_TRUE(old.conclusion.empty());
}

TEST_F(ImplementationWorktreeTest, RestartPreservesBuiltStepsAndInterruptedConstructionContent) {
    ImplementationWorktree work(session);
    const auto checkout = work.start(repo, "restart-content");
    nlohmann::json built, partial;
    std::optional<TurnSnapshot> latest;
    {
        ImplementationRun run(session); run.start(plan(2), "everything");
        build(run, work, checkout, {[&] { write(checkout / "file", "first step\n"); }, [] {}});
        built = run.view()["steps"][0];
        ASSERT_FALSE(built["substeps"][0]["diffs"].empty());
        TurnFiles files(session); files.begin(files.prepare(checkout, checkout)); run.begin(files.boundary());
        ImplementationBoundaries boundaries(session);
        const auto report = [&](std::size_t substep, const std::string& status) {
            const auto id = boundaries.capture(1, 2, {substep, status, "Preserved explanation"}, files.boundary());
            ToolResultContent result{"progress", "Reported"}; result.metadata = {{"implementation_boundary", id}};
            run.recordActivity({Message{"progress-" + std::to_string(id), MessageRole::TOOL, {result}}});
        };
        report(0, "started");
        run.recordActivity({Message{"working", MessageRole::ASSISTANT, {TextContent{"Building the next step"}}}});
        files.mutate({checkout / "file"}, [&] { write(checkout / "file", "partial second step\n"); });
        report(0, "completed"); report(1, "started");
        partial = run.view()["steps"][1];
        latest = SessionJournal(session).navigationTurn(false);
        // No finish/stop event: reopen exactly the durable state left mid-run.
    }
    ImplementationRun restored(session); restored.recover(latest);
    EXPECT_EQ(restored.status, "interrupted");
    const auto view = restored.view();
    EXPECT_EQ(view["steps"][0], built);
    EXPECT_EQ(view["steps"][1]["substeps"], partial["substeps"]);
    EXPECT_EQ(view["steps"][1]["whole"]["diffs"], partial["whole"]["diffs"]);
    EXPECT_EQ(view["work_step"], 1);
    EXPECT_FALSE(view["steps"][1]["whole"]["complete"]);
    work.close();
}

TEST_F(ImplementationWorktreeTest, DirtyBaselineDoesNotTouchUserBranchIndexOrHooks) {
    write(repo / "staged", "staged"); git({"add", "staged"}); write(repo / "staged", "unstaged");
    fs::remove(repo / "file"); write(repo / "new", "new"); write(repo / "ignored/cache", "secret");
    write(repo / ".git/hooks/post-checkout", "#!/bin/sh\necho invoked > '" + (directory / "hook").string() + "'\n");
    fs::permissions(repo / ".git/hooks/post-checkout", fs::perms(0755));
    git({"config", "commit.gpgSign", "true"});
    const auto status = git({"status", "--porcelain=v1"}), index = read(repo / ".git/index"), head = git({"rev-parse", "HEAD"});
    ImplementationWorktree work(session);
    const auto checkout = work.start(repo, "dirty");
    EXPECT_EQ(read(checkout / "staged"), "unstaged"); EXPECT_EQ(read(checkout / "new"), "new");
    EXPECT_FALSE(fs::exists(checkout / "file")); EXPECT_FALSE(fs::exists(checkout / "ignored/cache"));
    EXPECT_FALSE(fs::exists(directory / "hook"));
    EXPECT_EQ(git({"status", "--porcelain=v1"}), status); EXPECT_EQ(read(repo / ".git/index"), index);
    EXPECT_EQ(git({"rev-parse", "HEAD"}), head);
    work.close(); EXPECT_FALSE(fs::exists(checkout));
    EXPECT_TRUE(git({"for-each-ref", "refs/heads/sos/dirty"}).empty());
    EXPECT_FALSE(git({"for-each-ref", "refs/sos/dirty"}).empty());
}

TEST_F(ImplementationWorktreeTest, ImportsOnlySelectedStepPreservesConcurrentChangesAndSurvivesCleanup) {
    ImplementationWorktree work(session); const auto checkout = work.start(repo, "steps");
    const auto index = read(repo / ".git/index"), head = git({"rev-parse", "HEAD"});
    const auto first = step(checkout, [&] { write(checkout / "file", "ONE\ntwo\nthree\nfour\nfive\nsix\nseven\n"); });
    work.seal(0, first);
    const auto second = step(checkout, [&] { write(checkout / "later", "next step"); });
    work.seal(1, second);
    EXPECT_FALSE(fs::exists(repo / "later")); EXPECT_TRUE(read(repo / "file").starts_with("one"));
    write(repo / "file", "one\ntwo\nthree\nfour\nfive\nsix\nSEVEN\n"); write(repo / "unrelated", "mine");
    work.import(0, first);
    EXPECT_EQ(read(repo / "file"), "ONE\ntwo\nthree\nfour\nfive\nsix\nSEVEN\n");
    EXPECT_FALSE(fs::exists(repo / "later")); EXPECT_EQ(read(repo / "unrelated"), "mine");
    ImplementationWorktree reopened(session); reopened.import(0, first); reopened.import(1, second);
    EXPECT_EQ(read(repo / "later"), "next step"); EXPECT_EQ(read(repo / ".git/index"), index);
    EXPECT_EQ(git({"rev-parse", "HEAD"}), head);
    const auto diffs = GitSnapshot(checkout).diffs({first.before}, {first.after});
    reopened.close(); reopened.close();
    EXPECT_EQ(GitSnapshot::archive(checkout).diffs({first.before}, {first.after})[0].patch, diffs[0].patch);
    EXPECT_FALSE(GitSnapshot::archive(checkout).paths({first.before}).empty());
}

TEST_F(ImplementationWorktreeTest, ReplacesUnimportedLastStepAndKeepsOriginalDiffBoundary) {
    ImplementationWorktree work(session); const auto checkout = work.start(repo, "rework");
    const auto first = step(checkout, [&] { write(checkout / "file", "old implementation\n"); });
    work.seal(0, first);
    auto corrected = step(checkout, [&] { write(checkout / "file", "corrected implementation\n"); });
    corrected.before = first.before;
    work.seal(0, corrected);
    EXPECT_FALSE(GitSnapshot::archive(checkout).diffs({first.before}, {first.after}).empty());
    ImplementationWorktree restored(session); restored.import(0, corrected);
    EXPECT_EQ(read(repo / "file"), "corrected implementation\n");
    const auto forbidden = step(checkout, [&] { write(checkout / "file", "forbidden\n"); });
    EXPECT_THROW(restored.seal(0, forbidden), std::runtime_error);
    restored.close();
}

TEST_F(ImplementationWorktreeTest, DoesNotReplaceStepWithBuiltSuccessors) {
    ImplementationWorktree work(session); const auto checkout = work.start(repo, "successors");
    const auto first = step(checkout, [&] { write(checkout / "file", "first\n"); }); work.seal(0, first);
    const auto second = step(checkout, [&] { write(checkout / "later", "later\n"); }); work.seal(1, second);
    const auto corrected = step(checkout, [&] { write(checkout / "file", "corrected\n"); });
    EXPECT_THROW(work.seal(0, corrected), std::runtime_error);
    work.close();
}

TEST_F(ImplementationWorktreeTest, EverythingRevisionRebasesSharedFilesAndIgnoredContributionsWithoutHumanAutoValidation) {
    ImplementationWorktree work(session); const auto checkout = work.start(repo, "reconcile");
    ImplementationRun run(session); run.start(plan(2), "everything");
    const auto first = build(run, work, checkout, {[&] { write(checkout / "file", "ONE\ntwo\nthree\nfour\nfive\nsix\nseven\n"); }, [] {}});
    const auto second = build(run, work, checkout, {[&] { write(checkout / "file", "ONE\ntwo\nthree\nfour\nfive\nsix\nSEVEN\n"); write(checkout / "ignored/cache", "retained cache"); }, [] {}});
    ASSERT_TRUE(run.canRework(0));
    run.proposeRework(0, run.revisionVersion(), "Use NEW in the first step");
    const auto token = run.revisionVersion();
    const auto rollback = run.prepareRevision(token);
    work.revise(0, token, rollback); work.revise(0, token, rollback); // Durable/idempotent rollback.
    EXPECT_EQ(read(checkout / "file"), "ONE\ntwo\nthree\nfour\nfive\nsix\nseven\n");
    EXPECT_FALSE(fs::exists(checkout / "ignored/cache"));
    run.commitRevision(token);
    EXPECT_FALSE(run.view()["steps"][1]["whole"]["complete"]);
    EXPECT_EQ(run.view()["steps"][0]["previous_versions"].size(), 1);
    EXPECT_TRUE(run.view()["steps"][1]["previous_versions"].empty());
    ImplementationRun restored(session);
    EXPECT_EQ(restored.view()["steps"][0]["previous_versions"].size(), 1);
    EXPECT_TRUE(restored.view()["steps"][1]["previous_versions"].empty());
    const auto corrected = build(run, work, checkout, {[&] { write(checkout / "file", "NEW\ntwo\nthree\nfour\nfive\nsix\nseven\n"); }, [] {}});
    ASSERT_TRUE(run.needsRevisionSeed());
    const auto seeded = work.replay(1, run.currentGeneration(), run.revisionSource(), false);
    EXPECT_TRUE(seeded.conflicts.empty()) << seeded.conflicts;
    EXPECT_EQ(read(checkout / "file"), "NEW\ntwo\nthree\nfour\nfive\nsix\nSEVEN\n");
    EXPECT_EQ(read(checkout / "ignored/cache"), "retained cache");
    run.seedRevision(seeded);
    const auto revisedSecond = build(run, work, checkout, {[] {}, [] {}}); // Re-examined without further edits.
    EXPECT_EQ(revisedSecond.before, corrected.after);
    EXPECT_FALSE(run.view()["steps"][0]["whole"]["reviewed"]);
    EXPECT_FALSE(run.view()["steps"][1]["whole"]["reviewed"]);
    EXPECT_EQ(run.history(1).size(), 1);
    EXPECT_EQ(run.history().size(), 2);
    EXPECT_TRUE(run.view()["steps"][1]["previous_versions"].empty());
    EXPECT_FALSE(GitSnapshot::archive(checkout).diffs({second.before}, {second.after}).empty());
    work.import(0, corrected); run.review(0, 2, true);
    work.import(1, revisedSecond); run.review(1, 2, true);
    EXPECT_EQ(read(repo / "file"), "NEW\ntwo\nthree\nfour\nfive\nsix\nSEVEN\n");
    EXPECT_TRUE(run.fullyReviewed()); work.close();
}

TEST_F(ImplementationWorktreeTest, ReplayConflictPreservesCodeAndReplaysIndependentSubsteps) {
    ImplementationWorktree work(session); const auto checkout = work.start(repo, "replay-conflict");
    ImplementationRun run(session); run.start(plan(2), "everything");
    build(run, work, checkout, {[&] { write(checkout / "file", "first\n"); }, [] {}});
    build(run, work, checkout, {[&] { write(checkout / "file", "dependent\n"); }, [&] { write(checkout / "later", "independent\n"); }});
    run.proposeRework(0, run.revisionVersion(), "Change the first implementation");
    const auto token = run.revisionVersion(); work.revise(0, token, run.prepareRevision(token)); run.commitRevision(token);
    build(run, work, checkout, {[&] { write(checkout / "file", "corrected\n"); }, [] {}});
    const auto seed = work.replay(1, run.currentGeneration(), run.revisionSource(), false);
    EXPECT_FALSE(seed.conflicts.empty()); EXPECT_EQ(read(checkout / "file"), "corrected\n");
    EXPECT_EQ(read(checkout / "later"), "independent\n");
    run.seedRevision(seed);
    EXPECT_NE(run.instruction().find("dependent"), std::string::npos);
    build(run, work, checkout, {[&] { write(checkout / "file", "adapted\n"); }, [] {}});
    const auto value = run.view();
    EXPECT_EQ(value["steps"][1]["substeps"][0]["diffs"][0]["before"], "corrected\n");
    EXPECT_EQ(value["steps"][1]["substeps"][1]["diffs"][0]["after"], "independent\n");
    work.close();
}

TEST_F(ImplementationWorktreeTest, RevisedPlanRemapsSubstepsRemovesBuiltStepsAndAddsNewWork) {
    ImplementationWorktree work(session); const auto checkout = work.start(repo, "plan-revision");
    auto original = plan(3); ImplementationRun run(session); run.start(original, "everything");
    build(run, work, checkout, {[&] { write(checkout / "file", "existing\n"); }, [] {}});
    build(run, work, checkout, {[&] { write(checkout / "removed", "obsolete\n"); }, [] {}});
    build(run, work, checkout, {[&] { write(checkout / "third", "retained\n"); }, [] {}});
    auto revised = original; revised.steps[0].substeps[1].title = "uv packaging";
    revised.steps[1] = original.steps[2]; revised.steps[2].title = "New work";
    run.proposeRevision(0, run.revisionVersion(), {"Launch with uv run snake", revised, {0, 2, std::nullopt}, {{0, 0}, {0, 1}, {}}});
    const auto token = run.revisionVersion(); work.revise(0, token, run.prepareRevision(token)); run.commitRevision(token);
    ASSERT_TRUE(run.needsRevisionSeed());
    run.seedRevision(work.replay(0, run.currentGeneration(), run.revisionSource(), true));
    EXPECT_EQ(read(checkout / "file"), "existing\n"); EXPECT_FALSE(fs::exists(checkout / "removed"));
    build(run, work, checkout, {[] {}, [&] { write(checkout / "pyproject.toml", "[project.scripts]\nsnake = 'snake:main'\n"); }});
    run.seedRevision(work.replay(1, run.currentGeneration(), run.revisionSource(), false));
    build(run, work, checkout, {[] {}, [] {}});
    EXPECT_EQ(read(checkout / "third"), "retained\n"); EXPECT_FALSE(fs::exists(checkout / "removed"));
    EXPECT_FALSE(run.needsRevisionSeed());
    build(run, work, checkout, {[&] { write(checkout / "new", "new work\n"); }, [] {}});
    EXPECT_EQ(run.view()["plan"]["steps"][2]["title"], "New work");
    EXPECT_EQ(run.history().size(), 3);
    ASSERT_EQ(run.view()["plan_versions"].size(), 2);
    EXPECT_EQ(run.view()["plan_versions"][0]["plan"], nlohmann::json(original));
    EXPECT_EQ(run.view()["plan_versions"][1]["plan"], nlohmann::json(revised));
    auto third = revised; third.steps[0].validation.push_back("Check uv launch");
    run.proposeRevision(0, run.revisionVersion(), {"Refine launch validation", third, {0, 1, 2}, {{0, 1}, {0, 1}, {0, 1}}});
    run.openRevisionPlan(run.revisionVersion());
    ImplementationRun restored(session);
    EXPECT_TRUE(restored.planningRevision());
    EXPECT_EQ(restored.view()["plan_versions"].size(), 2); // Planning has not accepted #3.
    const auto nextToken = restored.revisionVersion();
    work.revise(0, nextToken, restored.prepareRevision(nextToken)); restored.commitRevision(nextToken);
    const auto versions = ImplementationRun(session).view()["plan_versions"];
    ASSERT_EQ(versions.size(), 3);
    EXPECT_EQ(versions[1]["plan"], nlohmann::json(revised));
    EXPECT_EQ(versions[2]["number"], 3);
    EXPECT_EQ(versions[2]["beforeStep"], versions[1]["beforeStep"]);
    EXPECT_EQ(versions[2]["plan"], nlohmann::json(third));
    work.close();
}

TEST_F(ImplementationWorktreeTest, ValidatedPrefixRemainsImmutableAndInterruptedFutureWorkIsPreserved) {
    ImplementationWorktree work(session); const auto checkout = work.start(repo, "partial-revision");
    const auto original = plan(3); ImplementationRun run(session); run.start(original, "everything");
    const auto first = build(run, work, checkout, {[&] { write(checkout / "file", "accepted\n"); }, [] {}});
    work.import(0, first); run.review(0, 2, true);
    build(run, work, checkout, {[&] { write(checkout / "later", "reviewed result\n"); }, [] {}});
    TurnFiles partial(session); partial.begin(partial.prepare(checkout, checkout)); run.begin(partial.boundary());
    ImplementationBoundaries(session).capture(2, 2, {0, "started", "Partial"}, partial.boundary());
    partial.mutate({checkout / "unfinished"}, [&] { write(checkout / "unfinished", "preserved partial\n"); }); partial.finish();
    run.finish(partial.boundary(), {}, {}, false, "Interrupted");
    auto invalid = original; invalid.steps[0].title = "Rewrite validated history";
    EXPECT_THROW(run.proposeRevision(1, run.revisionVersion(), {"Invalid", invalid, {0, 1, 2}, {{0, 1}, {0, 1}, {0, 1}}}), std::logic_error);
    run.proposeRework(1, run.revisionVersion(), "Correct the current result");
    const auto token = run.revisionVersion();
    const auto rollback = run.prepareRevision(token);
    ImplementationRun recovered(session); work.revise(1, token, rollback); recovered.commitRevision(token);
    EXPECT_FALSE(fs::exists(checkout / "unfinished")); EXPECT_EQ(read(repo / "file"), "accepted\n");
    build(recovered, work, checkout, {[&] { write(checkout / "later", "corrected result\n"); }, [] {}});
    recovered.seedRevision(work.replay(2, recovered.currentGeneration(), recovered.revisionSource(), false));
    EXPECT_EQ(read(checkout / "unfinished"), "preserved partial\n");
    build(recovered, work, checkout, {[] {}, [] {}});
    EXPECT_TRUE(recovered.view()["steps"][0]["whole"]["reviewed"]);
    EXPECT_TRUE(recovered.view()["steps"][0]["previous_versions"].empty());
    EXPECT_EQ(recovered.view()["steps"][1]["previous_versions"].size(), 1);
    EXPECT_TRUE(recovered.view()["steps"][2]["previous_versions"].empty());
    EXPECT_EQ(recovered.view()["review_step"], 1); work.close();
}

TEST_F(ImplementationWorktreeTest, ConflictBlocksEntireStepWithoutMarkersAndCanBeRetried) {
    ImplementationWorktree work(session); const auto checkout = work.start(repo, "conflict");
    const auto snapshot = step(checkout, [&] { write(checkout / "file", "THEIRS\n"); write(checkout / "added", "new"); });
    write(repo / "file", "MINE\n");
    try { work.import(0, snapshot); FAIL() << "Expected conflict"; }
    catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("file"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("remains unvalidated"), std::string::npos);
    }
    EXPECT_EQ(read(repo / "file"), "MINE\n"); EXPECT_FALSE(fs::exists(repo / "added"));
    write(repo / "file", "one\ntwo\nthree\nfour\nfive\nsix\nseven\n");
    work.import(0, snapshot); EXPECT_EQ(read(repo / "file"), "THEIRS\n");
    work.close();
}

TEST_F(ImplementationWorktreeTest, PreservesBinaryModeSymlinkAndDeletion) {
    write(repo / "deleted", "gone"); git({"add", "deleted"}); git({"commit", "-m", "Deletion base"});
    ImplementationWorktree work(session); const auto checkout = work.start(repo, "types");
    const auto snapshot = step(checkout, [&] {
        write(checkout / "binary", std::string("\0\1\2", 3));
        write(checkout / "executable", "script"); fs::permissions(checkout / "executable", fs::perms(0755));
        fs::create_symlink("file", checkout / "link"); fs::remove(checkout / "deleted");
    });
    work.import(0, snapshot);
    EXPECT_EQ(read(repo / "binary"), std::string("\0\1\2", 3));
    EXPECT_NE(fs::status(repo / "executable").permissions() & fs::perms::owner_exec, fs::perms::none);
    EXPECT_EQ(fs::read_symlink(repo / "link"), "file"); EXPECT_FALSE(fs::exists(repo / "deleted"));
    work.close();
}

TEST_F(ImplementationWorktreeTest, MissingCheckoutNeverFallsBackToUserWorkspace) {
    ImplementationWorktree work(session); const auto checkout = work.start(repo, "missing");
    fs::remove_all(checkout);
    EXPECT_THROW(ImplementationWorktree(session).executionDirectory(), std::runtime_error);
    work.close();
    EXPECT_EQ(git({"worktree", "list", "--porcelain"}).find(checkout.string()), std::string::npos);
}

TEST_F(ImplementationWorktreeTest, CollisionNeverReplacesAnExistingBranchEvenOnRetry) {
    git({"branch", "sos/collision"});
    const auto original = git({"rev-parse", "sos/collision"});
    ImplementationWorktree work(session);
    EXPECT_THROW(work.start(repo, "collision"), std::runtime_error);
    EXPECT_THROW(work.start(repo, "collision"), std::runtime_error);
    EXPECT_EQ(git({"rev-parse", "sos/collision"}), original);
    EXPECT_FALSE(fs::exists(PathResolution::worktreesDirectory() / "collision"));
}

TEST_F(ImplementationWorktreeTest, ResumesJournaledPartialImportWithoutReapplyingChanges) {
    ImplementationWorktree work(session); const auto checkout = work.start(repo, "recovery");
    const auto snapshot = step(checkout, [&] { write(checkout / "a", "first"); write(checkout / "b", "second"); });
    const auto archive = GitSnapshot::archive(checkout);
    GitSnapshot target(repo, archive.storageDirectory());
    const auto intent = target.prepareImport({snapshot.before}, {snapshot.after});
    {
        SessionDatabase db(session / "worktree.sqlite");
        SessionDatabase::Statement insert(db, "INSERT INTO imports(step,source_before,source_after,expected,target) VALUES(0,?,?,?,?)");
        insert.bind(1, snapshot.before); insert.bind(2, snapshot.after);
        insert.bind(3, intent.first.tree); insert.bind(4, intent.second.tree); insert.step();
    }
    write(repo / "a", "first"); // Simulate a process exit after the first file was installed.
    ImplementationWorktree reopened(session); reopened.import(0, snapshot);
    EXPECT_EQ(read(repo / "a"), "first"); EXPECT_EQ(read(repo / "b"), "second");
    write(repo / "a", "later user edit"); reopened.import(0, snapshot);
    EXPECT_EQ(read(repo / "a"), "later user edit");
    reopened.close();
}

TEST_F(ImplementationWorktreeTest, PlainDirectoryUsesPrivateGitRespectsExclusionsAndImportsSelectedStep) {
    makePlainDirectory();
    write(repo / ".gitignore", "ignored/\n!.sos/\n!.sos/**\n");
    write(repo / "ignored/cache", "cache"); write(repo / ".sos/auth.json", "private");
    write(repo / "nested/.gitignore", "secret*\n!secret-public\n");
    write(repo / "nested/secret.key", "secret"); write(repo / "nested/secret-public", "included");
    write(repo / "nested/.sos/state", "internal");
    ImplementationWorktree work(session); const auto checkout = work.start(repo, "plain");
    EXPECT_FALSE(fs::exists(repo / ".git")); EXPECT_TRUE(fs::exists(checkout / ".git"));
    EXPECT_FALSE(fs::exists(checkout / "ignored/cache")); EXPECT_FALSE(fs::exists(checkout / ".sos"));
    EXPECT_FALSE(fs::exists(checkout / "nested/secret.key")); EXPECT_FALSE(fs::exists(checkout / "nested/.sos"));
    EXPECT_EQ(read(checkout / "nested/secret-public"), "included");
    const auto first = step(checkout, [&] {
        write(checkout / "file", "ONE\ntwo\nthree\nfour\nfive\nsix\nseven\n");
        write(checkout / "binary", std::string("\0binary", 7));
        fs::create_symlink("file", checkout / "link");
        fs::remove(checkout / "nested/secret-public");
        write(checkout / ".sos/tool-output/result", "generated");
    });
    work.seal(0, first);
    const auto second = step(checkout, [&] { write(checkout / "later", "next step"); }); work.seal(1, second);
    write(repo / "file", "one\ntwo\nthree\nfour\nfive\nsix\nSEVEN\n");
    ImplementationWorktree reopened(session); reopened.import(0, first); reopened.import(0, first);
    EXPECT_EQ(read(repo / "file"), "ONE\ntwo\nthree\nfour\nfive\nsix\nSEVEN\n");
    EXPECT_EQ(read(repo / "binary"), std::string("\0binary", 7)); EXPECT_EQ(fs::read_symlink(repo / "link"), "file");
    EXPECT_FALSE(fs::exists(repo / "nested/secret-public")); EXPECT_FALSE(fs::exists(repo / "later"));
    EXPECT_EQ(read(repo / ".sos/auth.json"), "private"); EXPECT_FALSE(fs::exists(repo / ".sos/tool-output"));
    reopened.import(1, second); EXPECT_EQ(read(repo / "later"), "next step");
    reopened.close(); EXPECT_FALSE(fs::exists(repo / ".git")); EXPECT_FALSE(fs::exists(checkout));
    EXPECT_FALSE(GitSnapshot::archive(checkout).diffs({first.before}, {first.after}).empty());
}

TEST_F(ImplementationWorktreeTest, PlainDirectoryConflictPreservesEntireWorkspaceAndCanBeRetried) {
    makePlainDirectory();
    ImplementationWorktree work(session); const auto checkout = work.start(repo, "plain-conflict");
    const auto snapshot = step(checkout, [&] { write(checkout / "file", "step\n"); write(checkout / "added", "step addition"); });
    write(repo / "file", "user\n");
    EXPECT_THROW(work.import(0, snapshot), std::runtime_error);
    EXPECT_EQ(read(repo / "file"), "user\n"); EXPECT_FALSE(fs::exists(repo / "added"));
    EXPECT_FALSE(fs::exists(repo / ".git"));
    write(repo / "file", "one\ntwo\nthree\nfour\nfive\nsix\nseven\n");
    ImplementationWorktree(session).import(0, snapshot);
    EXPECT_EQ(read(repo / "file"), "step\n"); EXPECT_EQ(read(repo / "added"), "step addition");
    work.close();
}

TEST_F(ImplementationWorktreeTest, PrivateCopyPreservesBytesPermissionsAndLinksWithoutSharingWrites) {
    const auto source = repo / "source", target = directory / "copy";
    write(source, std::string("\0binary\r\n", 9));
    fs::permissions(source, fs::perms(0750));
    copyPrivateFile(source, target); // Native clone when supported, ordinary copy otherwise.
    EXPECT_EQ(read(source), read(target));
    EXPECT_EQ(fs::status(source).permissions(), fs::status(target).permissions());
    EXPECT_FALSE(fs::equivalent(source, target)); // A hard link is never a valid optimization.
    write(target, "private write");
    EXPECT_EQ(read(source), std::string("\0binary\r\n", 9));
    write(source, "user write");
    EXPECT_EQ(read(target), "private write");
    EXPECT_THROW(copyPrivateFile(source, target), std::runtime_error);
    fs::create_symlink("absent", repo / "dangling");
    copyPrivateFile(repo / "dangling", directory / "copied-link");
    EXPECT_EQ(fs::read_symlink(directory / "copied-link"), "absent");
    EXPECT_FALSE(copyPrivateFile(source, directory / "fallback", false));
    EXPECT_EQ(read(directory / "fallback"), "user write");
    write(directory / "fallback", "ordinary private write");
    EXPECT_EQ(read(source), "user write");
}

TEST_F(ImplementationWorktreeTest, GitRootWithNestedRepositoryExcludesInternalDataAndPreservesNestedIndex) {
    const auto nested = repo / "deep/project";
    fs::create_directories(nested);
    ASSERT_EQ(core::runProcess("git", {"init", "--template="}, nested).exitCode, 0);
    write(nested / "code", "original");
    ASSERT_EQ(core::runProcess("git", {"add", "code"}, nested).exitCode, 0);
    ASSERT_EQ(core::runProcess("git", {"-c", "user.name=Test", "-c", "user.email=test@example.invalid",
        "commit", "-m", "Nested base"}, nested).exitCode, 0);
    git({"add", "deep/project"}); // A tracked gitlink must also be flattened into working files.
    const auto index = read(nested / ".git/index");
    write(repo / ".sos/private", "internal");
    ImplementationWorktree work(session);
    const auto checkout = work.start(repo, "nested-git-root");
    EXPECT_EQ(read(checkout / "deep/project/code"), "original");
    EXPECT_FALSE(fs::exists(checkout / "deep/project/.git"));
    EXPECT_FALSE(fs::exists(checkout / ".sos"));
    const auto snapshot = step(checkout, [&] {
        write(checkout / "deep/project/code", "changed");
        write(checkout / ".sos/generated", "internal output");
    });
    work.seal(0, snapshot); work.import(0, snapshot);
    EXPECT_EQ(read(nested / "code"), "changed");
    EXPECT_EQ(read(nested / ".git/index"), index);
    EXPECT_FALSE(fs::exists(repo / ".sos/generated"));
    EXPECT_EQ(read(repo / ".sos/private"), "internal");
    work.close();
}

TEST_F(ImplementationWorktreeTest, MixedTreeClonesNestedRepositoriesAndWorktreesThenImportsOnlyValidatedStep) {
    makePlainDirectory();
    write(directory / "global-ignore", "global-private*\n");
    write(directory / "config/git/config", "[core]\n\texcludesFile = " + (directory / "global-ignore").string() + "\n");
    const auto nested = repo / "ordinary/deep/project";
    fs::create_directories(nested);
    const auto run = [&](const fs::path& cwd, const std::vector<std::string>& args) {
        const auto result = core::runProcess("git", args, cwd);
        if (result.exitCode) throw std::runtime_error(result.error);
        return result.output;
    };
    run(nested, {"init", "--template="});
    run(nested, {"config", "user.name", "Test"});
    run(nested, {"config", "user.email", "test@example.invalid"});
    write(nested / "code", "original\n");
    write(nested / ".gitignore", "cache/\nsecret*\n!secret-public\n");
    write(nested / "cache/file", "ignored");
    write(nested / "secret-key", "secret");
    write(nested / "secret-public", "included");
    write(nested / "global-private-key", "globally excluded");
    write(nested / ".sos/private", "internal");
    run(nested, {"add", "code", ".gitignore"});
    run(nested, {"commit", "-m", "Nested base"});
    const auto linked = repo / "another/deep/linked";
    fs::create_directories(linked.parent_path());
    run(nested, {"worktree", "add", "-b", "linked", linked.string()});
    const auto index = read(nested / ".git/index"), linkedGit = read(linked / ".git");
    const auto head = run(nested, {"rev-parse", "HEAD"});
    write(repo / ".gitignore", "ordinary/ignored/\n");
    write(repo / "ordinary/ignored/tree/file", "excluded subtree");
    fs::create_symlink(nested, repo / "external-link");
    ImplementationWorktree work(session);
    const auto checkout = work.start(repo, "mixed");
    const auto relative = nested.lexically_relative(repo), linkedRelative = linked.lexically_relative(repo);
    EXPECT_EQ(read(checkout / relative / "code"), "original\n");
    EXPECT_EQ(read(checkout / linkedRelative / "code"), "original\n");
    EXPECT_FALSE(fs::exists(checkout / relative / ".git"));
    EXPECT_FALSE(fs::exists(checkout / linkedRelative / ".git"));
    EXPECT_FALSE(fs::exists(checkout / relative / "cache"));
    EXPECT_FALSE(fs::exists(checkout / relative / "secret-key"));
    EXPECT_FALSE(fs::exists(checkout / relative / "global-private-key"));
    EXPECT_FALSE(fs::exists(checkout / relative / ".sos"));
    EXPECT_FALSE(fs::exists(checkout / "ordinary/ignored"));
    EXPECT_EQ(read(checkout / relative / "secret-public"), "included");
    EXPECT_EQ(fs::read_symlink(checkout / "external-link"), nested);
    const auto first = step(checkout, [&] {
        write(checkout / relative / "code", "first step\n");
        write(checkout / "loose-file", "first addition\n");
    });
    work.seal(0, first);
    const auto second = step(checkout, [&] { write(checkout / linkedRelative / "code", "second step\n"); });
    work.seal(1, second);
    EXPECT_EQ(read(nested / "code"), "original\n");
    EXPECT_FALSE(fs::exists(repo / "loose-file"));
    write(nested / "code", "concurrent edit\n");
    EXPECT_THROW(work.import(0, first), std::runtime_error);
    EXPECT_FALSE(fs::exists(repo / "loose-file"));
    write(nested / "code", "original\n");
    ImplementationWorktree(session).import(0, first);
    EXPECT_EQ(read(nested / "code"), "first step\n");
    EXPECT_EQ(read(linked / "code"), "original\n");
    EXPECT_EQ(read(repo / "loose-file"), "first addition\n");
    work.import(1, second);
    EXPECT_EQ(read(linked / "code"), "second step\n");
    EXPECT_EQ(read(nested / ".git/index"), index);
    EXPECT_EQ(read(linked / ".git"), linkedGit);
    EXPECT_EQ(run(nested, {"rev-parse", "HEAD"}), head);
    work.close();
    EXPECT_FALSE(fs::exists(repo / ".git"));
    EXPECT_FALSE(GitSnapshot::archive(checkout).diffs({first.before}, {first.after}).empty());
}

TEST_F(ImplementationWorktreeTest, ResumePartialPreparationAllowsSlowCleanupAndBaselineHashing) {
    makePlainDirectory();
    const auto fifo = repo / "unsupported";
    ASSERT_EQ(::mkfifo(fifo.c_str(), 0600), 0);
    ImplementationWorktree work(session);
    EXPECT_THROW(work.start(repo, "slow-resume"), std::runtime_error);
    fs::remove(fifo);
    write(repo / "new-source-file", "current source");

    auto executable = core::runProcess("which", {"git"}, directory).output;
    ASSERT_FALSE(executable.empty());
    if (executable.back() == '\n') executable.pop_back();
    const auto tools = directory / "tools";
    const auto previous = std::getenv("PATH");
    const std::string oldPath = previous ? previous : "";
    struct RestorePath {
        std::string value;
        ~RestorePath() { setenv("PATH", value.c_str(), 1); }
    } restore{oldPath};
    write(tools / "git", "#!/bin/sh\n"
        "case \"$*\" in\n"
        "  *'worktree remove --force'*) marker='" + (tools / "cleanup").string() + "';;\n"
        "  *'hash-object -w --no-filters --stdin-paths'*) marker='" + (tools / "hashing").string() + "';;\n"
        "esac\n"
        "if [ -n \"$marker\" ] && [ ! -f \"$marker\" ]; then\n"
        "  touch \"$marker\"\nfi\n"
        "exec '" + executable + "' \"$@\"\n");
    fs::permissions(tools / "git", fs::perms(0755));
    setenv("PATH", (tools.string() + ":" + oldPath).c_str(), 1);

    // Observe the actual process options rather than sleeping beyond the generic
    // timeout twice. Still execute real Git, including both intercepted paths.
    unsigned commands = 0;
    ImplementationWorktree reopened(session, [&](const std::vector<std::string>& args,
            const fs::path& cwd, const core::ProcessOptions& options) {
        ++commands;
        EXPECT_EQ(options.timeout, std::chrono::minutes(10));
        EXPECT_GT(options.timeout, core::ProcessOptions{}.timeout);
        return core::runProcess("/usr/bin/env", args, cwd, {}, {}, options);
    });
    const auto checkout = reopened.start(repo, "slow-resume");
    EXPECT_GT(commands, 0u);
    EXPECT_TRUE(fs::exists(tools / "cleanup"));
    EXPECT_TRUE(fs::exists(tools / "hashing"));
    EXPECT_EQ(read(checkout / "new-source-file"), "current source");
    EXPECT_EQ(ImplementationWorktree(session).executionDirectory(), checkout);
    EXPECT_FALSE(fs::exists(repo / ".git"));
    reopened.close();
}
}
