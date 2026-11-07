#include <daemon/snapshots/turn_files.hpp>
#include <daemon/session/session_journal.hpp>
#include <agent-runtime/core/process.hpp>
#include <agent-runtime/tools/fs/write.hpp>
#include <agent-runtime/tools/fs/edit.hpp>
#include <agent-runtime/tools/fs/apply_patch.hpp>
#include <agent-runtime/tools/fs/common.hpp>
#include <agent-runtime/tools/common/bash.hpp>
#include <agent-runtime/core/cancellation.hpp>
#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <sys/wait.h>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
class TurnFilesTest : public testing::Test {
protected:
    fs::path root, workspace, store;
    std::optional<std::string> data;
    void SetUp() override {
        root = fs::temp_directory_path() / ("sos-turn-files-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root / "work");
        root = fs::canonical(root);
        workspace = root / "work";
        store = root / "session";
        fs::create_directory(store);
        if (const auto* value = std::getenv("XDG_DATA_HOME")) data = value;
        setenv("XDG_DATA_HOME", (root / "data").c_str(), 1);
        SessionInfo info;
        info.workspace = workspace;
        info.id = "test";
        SessionJournal(store).save({Message{"system", MessageRole::SYSTEM, {TextContent{"Instructions"}}}}, info);
    }
    void TearDown() override {
        if (data) setenv("XDG_DATA_HOME", data->c_str(), 1); else unsetenv("XDG_DATA_HOME");
        fs::remove_all(root);
    }
    void write(const fs::path& path, const std::string& text) { fs_tools::writeText(workspace / path, text); }
    std::string read(const fs::path& path) { return fs_tools::readText(workspace / path); }
    void gitInit() {
        const auto result = core::runProcess("git", {"init", "--template="}, workspace);
        ASSERT_EQ(result.exitCode, 0) << result.error;
    }
    void begin(TurnFiles& files, const std::string& id = "user") {
        auto turn = files.prepare(workspace, workspace);
        SessionJournal journal(store);
        turn.start = journal.count();
        files.begin(turn);
        journal.append({Message{id, MessageRole::USER, {TextContent{"Change files"}}}}, 1);
    }
    ToolContext context(TurnFiles& files) {
        ToolContext ctx{workspace, workspace, [](const auto&) {}};
        ctx.mutateFiles = [&](const auto& paths, const auto& action) { files.mutate(paths, action); };
        return ctx;
    }
};

TEST_F(TurnFilesTest, GitCurrentIsPersistedAfterEachEditingToolWithoutFinishingTurn) {
    gitInit();
    {
        TurnFiles files(store);
        begin(files);
        const auto ctx = context(files);
        const auto initial = *SessionJournal(store).navigationTurn(false);
        EXPECT_EQ(initial.before, initial.after);
        WriteTool{}.execute({{"filePath", "a"}, {"content", "first\n"}}, ctx);
        const auto written = *SessionJournal(store).navigationTurn(false);
        EXPECT_NE(written.after, initial.after);
        EditTool{}.execute({{"filePath", "a"}, {"oldString", "first"}, {"newString", "second"}}, ctx);
        const auto edited = *SessionJournal(store).navigationTurn(false);
        EXPECT_NE(edited.after, written.after);
        ApplyPatchTool{}.execute({{"patchText", "*** Begin Patch\n*** Delete File: a\n*** Add File: b\n+third\n*** End Patch"}}, ctx);
        const auto patched = *SessionJournal(store).navigationTurn(false);
        EXPECT_NE(patched.after, edited.after);
        EXPECT_EQ(patched.before, initial.before);
        EXPECT_FALSE(patched.complete);
    } // Simulate restart between tools, without finish().
    write("b", "human change");
    EXPECT_THROW(TurnFiles::navigate(store, false), std::runtime_error);
    EXPECT_EQ(read("b"), "human change");
    write("b", "third\n");
    TurnFiles::navigate(store, false);
    EXPECT_FALSE(fs::exists(workspace / "b"));
    TurnFiles::navigate(store, true);
    EXPECT_EQ(read("b"), "third\n");
}

TEST_F(TurnFilesTest, BashCheckpointsSuccessFailureTimeoutAndCancellation) {
    gitInit();
    for (const auto& kind : {"success", "failure", "timeout", "cancel"}) {
        SCOPED_TRACE(kind);
        TurnFiles files(store);
        begin(files, std::string("user-") + kind);
        auto ctx = context(files);
        const auto old = SessionJournal(store).navigationTurn(false)->after;
        std::string command = "printf changed > bash-file";
        if (std::string(kind) == "failure") command += "; exit 7";
        if (std::string(kind) == "timeout" || std::string(kind) == "cancel") command += "; sleep 5";
        if (std::string(kind) == "cancel") {
            ctx.checkCancelled = [&] { if (fs::exists(workspace / "bash-file")) throw core::Cancelled{}; };
            EXPECT_THROW(BashTool{}.execute({{"command", command}, {"mainCommand", "printf"}}, ctx), core::Cancelled);
        } else {
            const auto result = BashTool{}.execute({{"command", command}, {"mainCommand", "printf"}, {"timeout", 300}}, ctx);
            if (std::string(kind) == "failure") EXPECT_EQ(result.metadata["exit"], 7);
            if (std::string(kind) == "timeout") EXPECT_TRUE(result.metadata["timedOut"]);
        }
        const auto stored = *SessionJournal(store).navigationTurn(false);
        EXPECT_FALSE(stored.after.empty());
        EXPECT_NE(stored.after, old);
        EXPECT_FALSE(stored.complete);
        EXPECT_TRUE(stored.edits.empty()); // Bash is not journaled as an editing tool.
        TurnFiles::navigate(store, false);
        EXPECT_FALSE(fs::exists(workspace / "bash-file"));
    }
}

TEST_F(TurnFilesTest, FailedMutationCapturesPartialGitStateAndFinishRefreshesCurrent) {
    gitInit();
    TurnFiles files(store);
    begin(files);
    EXPECT_THROW(files.mutate({workspace / "partial"}, [&] {
        EXPECT_TRUE(SessionJournal(store).navigationTurn(false)->after.empty());
        write("partial", "partial write");
        throw std::runtime_error("I/O failure");
    }), std::runtime_error);
    const auto partial = *SessionJournal(store).navigationTurn(false);
    EXPECT_FALSE(partial.after.empty());
    write("late", "final state");
    files.finish();
    const auto finished = *SessionJournal(store).navigationTurn(false);
    EXPECT_TRUE(finished.complete);
    EXPECT_NE(finished.after, partial.after);
    EXPECT_EQ(finished.before, partial.before);
    TurnFiles::navigate(store, false);
    EXPECT_FALSE(fs::exists(workspace / "partial"));
    EXPECT_FALSE(fs::exists(workspace / "late"));
}

TEST_F(TurnFilesTest, FailedCheckpointDoesNotLeaveAStaleCurrentUsableForUndo) {
    gitInit();
    TurnFiles files(store);
    begin(files);
    // Directories replacing tracked leaves cannot be captured by this facility.
    WriteTool{}.execute({{"filePath", "a"}, {"content", "before"}}, context(files));
    const auto added = core::runProcess("git", {"add", "a"}, workspace);
    ASSERT_EQ(added.exitCode, 0);
    EXPECT_THROW(files.mutate({}, [&] {
        fs::remove(workspace / "a");
        write("a/child", "unverified");
    }), std::runtime_error);
    EXPECT_TRUE(SessionJournal(store).navigationTurn(false)->after.empty());
    EXPECT_THROW(TurnFiles::navigate(store, false), std::runtime_error);
    EXPECT_EQ(read("a/child"), "unverified");
    EXPECT_EQ(SessionJournal(store).count(), 2);
}

TEST_F(TurnFilesTest, AbruptWorkerExitAfterBashKeepsCheckpointAvailableForUndoAndRedo) {
    gitInit();
    const auto pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        try {
            TurnFiles files(store);
            begin(files);
            BashTool{}.execute({{"command", "printf persisted > bash-file"}, {"mainCommand", "printf"}}, context(files));
            _exit(0); // No destructors and no finish(): the worker disappears mid-turn.
        } catch (...) { _exit(1); }
    }
    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
    EXPECT_FALSE(SessionJournal(store).navigationTurn(false)->complete);
    TurnFiles::navigate(store, false);
    EXPECT_FALSE(fs::exists(workspace / "bash-file"));
    TurnFiles::navigate(store, true);
    EXPECT_EQ(read("bash-file"), "persisted");
}

TEST_F(TurnFilesTest, DeniedBashDoesNotRunTheCheckpointHookAndBashOutsideGitIsNotJournaled) {
    TurnFiles files(store);
    begin(files);
    auto ctx = context(files);
    bool checkpointed = false;
    ctx.mutateFiles = [&](const auto&, const auto&) { checkpointed = true; };
    ctx.ask = [](const auto&) { throw std::runtime_error("Denied"); };
    EXPECT_THROW(BashTool{}.execute({{"command", "printf denied > denied"}, {"mainCommand", "printf"}}, ctx), std::runtime_error);
    EXPECT_FALSE(checkpointed);
    EXPECT_FALSE(fs::exists(workspace / "denied"));
    BashTool{}.execute({{"command", "printf keep > outside-git"}, {"mainCommand", "printf"}}, context(files));
    EXPECT_TRUE(SessionJournal(store).navigationTurn(false)->edits.empty());
    TurnFiles::navigate(store, false);
    EXPECT_EQ(read("outside-git"), "keep");
}

TEST_F(TurnFilesTest, JournalsWriteEditAndPatchWithoutGitAndReplaysWithoutExecution) {
    TurnFiles files(store);
    begin(files);
    const auto ctx = context(files);
    WriteTool{}.execute({{"filePath", "a"}, {"content", "first\n"}}, ctx);
    EditTool{}.execute({{"filePath", "a"}, {"oldString", "first"}, {"newString", "second"}}, ctx);
    ApplyPatchTool{}.execute({{"patchText", "*** Begin Patch\n*** Update File: a\n*** Move to: moved\n@@\n-second\n+third\n*** Add File: new\n+created\n*** End Patch"}}, ctx);
    files.finish();
    write("bash-only", "keep this");
    TurnFiles::navigate(store, false);
    EXPECT_FALSE(fs::exists(workspace / "moved"));
    EXPECT_FALSE(fs::exists(workspace / "new"));
    EXPECT_FALSE(fs::exists(workspace / "a"));
    EXPECT_EQ(read("bash-only"), "keep this");
    EXPECT_EQ(SessionJournal(store).count(), 1);
    TurnFiles::navigate(store, true);
    EXPECT_EQ(read("moved"), "third\n");
    EXPECT_EQ(read("new"), "created\n");
    EXPECT_EQ(SessionJournal(store).count(), 2);
}

TEST_F(TurnFilesTest, GitChangesAndIgnoredExternalEditsShareOnePreflight) {
    gitInit();
    write(".gitignore", "ignored\n");
    write("tracked", "before");
    TurnFiles files(store);
    begin(files);
    const auto ctx = context(files);
    write("tracked", "bash change");
    WriteTool{}.execute({{"filePath", "ignored"}, {"content", "tool change"}}, ctx);
    WriteTool{}.execute({{"filePath", (root / "outside").string()}, {"content", "external"}}, ctx);
    files.finish();
    write("ignored", "human conflict");
    EXPECT_THROW(TurnFiles::navigate(store, false), std::runtime_error);
    EXPECT_EQ(read("tracked"), "bash change");
    EXPECT_EQ(SessionJournal(store).count(), 2);
    EXPECT_FALSE(SessionJournal(store).navigationPending());
    write("ignored", "tool change");
    TurnFiles::navigate(store, false);
    EXPECT_EQ(read("tracked"), "before");
    EXPECT_FALSE(fs::exists(root / "outside"));
    EXPECT_FALSE(fs::exists(workspace / "ignored"));
    TurnFiles::navigate(store, true);
    EXPECT_EQ(read("tracked"), "bash change");
    EXPECT_EQ(read("ignored"), "tool change");
    EXPECT_EQ(read("../outside"), "external");
}

TEST_F(TurnFilesTest, RecordsPartialFailuresAndRefusesInterleavedExternalChanges) {
    write("a", "before");
    TurnFiles files(store);
    begin(files);
    EXPECT_THROW(files.mutate({workspace / "a"}, [&] {
        write("a", "partial");
        throw std::runtime_error("I/O failure");
    }), std::runtime_error);
    // Even without finish (e.g. interrupted generation), completed mutations are known.
    TurnFiles::navigate(store, false);
    EXPECT_EQ(read("a"), "before");
    TurnFiles::navigate(store, true);
    EXPECT_EQ(read("a"), "partial");
    TurnFiles second(store);
    begin(second, "second-user");
    auto ctx = context(second);
    WriteTool{}.execute({{"filePath", "a"}, {"content", "tool one"}}, ctx);
    write("a", "bash interleaved");
    WriteTool{}.execute({{"filePath", "a"}, {"content", "tool two"}}, ctx);
    second.finish();
    EXPECT_THROW(TurnFiles::navigate(store, false), std::runtime_error);
    EXPECT_EQ(read("a"), "tool two");
}

TEST_F(TurnFilesTest, NewTurnDiscardsRedoAndNeverReusesItsIdentifier) {
    TurnFiles first(store);
    begin(first);
    first.finish();
    const auto old = SessionJournal(store).navigationTurn(false)->id;
    TurnFiles::navigate(store, false);
    TurnFiles next(store);
    begin(next, "new-user");
    next.finish();
    EXPECT_FALSE(SessionJournal(store).navigationTurn(true));
    EXPECT_GT(SessionJournal(store).navigationTurn(false)->id, old);
    EXPECT_THROW(TurnFiles::navigate(store, true), std::runtime_error);
}

TEST_F(TurnFilesTest, DurableNavigationIntentCanFinishAfterPartialFilePublication) {
    write("a", "before");
    write("b", "before");
    TurnFiles files(store);
    begin(files);
    auto ctx = context(files);
    WriteTool{}.execute({{"filePath", "a"}, {"content", "after"}}, ctx);
    WriteTool{}.execute({{"filePath", "b"}, {"content", "after"}}, ctx);
    files.finish();
    {
        SessionJournal journal(store);
        auto turn = *journal.navigationTurn(false);
        turn.operation = "undo";
        turn.messages = {journal.readAll().back()};
        journal.saveTurn(turn);
    }
    write("a", "before"); // Simulate a crash after the first file was restored.
    EXPECT_THROW(TurnFiles::navigate(store, true), std::runtime_error);
    TurnFiles::navigate(store, false);
    EXPECT_EQ(read("a"), "before");
    EXPECT_EQ(read("b"), "before");
    EXPECT_FALSE(SessionJournal(store).navigationPending());
    EXPECT_EQ(SessionJournal(store).count(), 1);
}

TEST_F(TurnFilesTest, RestoresSymlinkWritesWithoutFollowingANewRedirect) {
    write("target", "before");
    fs::create_symlink("target", workspace / "link");
    TurnFiles files(store);
    begin(files);
    WriteTool{}.execute({{"filePath", "link"}, {"content", "after"}}, context(files));
    files.finish();
    fs::remove(workspace / "link");
    fs::create_symlink("other", workspace / "link");
    EXPECT_THROW(TurnFiles::navigate(store, false), std::runtime_error);
    EXPECT_EQ(read("target"), "after");
    fs::remove(workspace / "link");
    fs::create_symlink("target", workspace / "link");
    TurnFiles::navigate(store, false);
    EXPECT_EQ(read("target"), "before");
    EXPECT_TRUE(fs::is_symlink(workspace / "link"));
}

TEST_F(TurnFilesTest, PatchDeletionRestoresTheSymlinkRatherThanOnlyItsTarget) {
    write("target", "unchanged");
    fs::create_symlink("target", workspace / "link");
    TurnFiles files(store);
    begin(files);
    ApplyPatchTool{}.execute({{"patchText", "*** Begin Patch\n*** Delete File: link\n*** End Patch"}}, context(files));
    files.finish();
    EXPECT_FALSE(fs::exists(fs::symlink_status(workspace / "link")));
    TurnFiles::navigate(store, false);
    EXPECT_EQ(fs::read_symlink(workspace / "link"), "target");
    EXPECT_EQ(read("target"), "unchanged");
    TurnFiles::navigate(store, true);
    EXPECT_FALSE(fs::exists(fs::symlink_status(workspace / "link")));
    EXPECT_EQ(read("target"), "unchanged");
}

TEST_F(TurnFilesTest, WriteThroughDanglingSymlinkChainJournalsTheCreatedTarget) {
    fs::create_symlink("second", workspace / "first");
    fs::create_symlink("missing", workspace / "second");
    TurnFiles files(store);
    begin(files);
    WriteTool{}.execute({{"filePath", "first"}, {"content", "created"}}, context(files));
    files.finish();
    EXPECT_EQ(read("missing"), "created");
    TurnFiles::navigate(store, false);
    EXPECT_FALSE(fs::exists(workspace / "missing"));
    EXPECT_EQ(fs::read_symlink(workspace / "first"), "second");
    EXPECT_EQ(fs::read_symlink(workspace / "second"), "missing");
}
}
