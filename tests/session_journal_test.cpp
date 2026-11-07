#include <daemon/session/session_journal.hpp>
#include <daemon/session/session_history_view.hpp>
#include <daemon/session/session.hpp>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <agent-runtime/conversation/messages/message_json.hpp>
#include <gtest/gtest.h>
#include <fstream>
#include <chrono>
#include <iostream>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using Json = nlohmann::json;
namespace fs = std::filesystem;
class UnusedProvider : public Provider {
public:
    ProviderFormat getFormat() const override { return ProviderFormat::OPENAI_RESPONSES; }
    nlohmann::json generate(const nlohmann::json&, const std::string&,
        const std::vector<ToolDefinition>&, const std::string&) override {
        throw std::logic_error("Restoring context must not generate");
    }
};
class SessionJournalTest : public testing::Test {
protected:
    fs::path directory;
    void SetUp() override {
        std::string pattern = "/tmp/sos-journal-XXXXXX";
        ASSERT_NE(mkdtemp(pattern.data()), nullptr);
        directory = pattern;
    }
    void TearDown() override { std::error_code ignored; fs::remove_all(directory, ignored); }
    fs::path journalPath() const { return directory / "messages.jsonl"; }
    fs::path indexPath() const { return directory / "messages.index.jsonl"; }
    static Message text(std::string id, MessageRole role = MessageRole::USER) {
        return {id, role, {TextContent{"Text for " + id}}};
    }
    static Message refused(std::string id, const Json& action) {
        ToolResultContent result{"call-" + id, std::string(100000, 'x'), true};
        result.metadata = {{"refused_action", action}, {"refused_message_id", id},
            {"permissions", Json::array({{{"decision", "ai_refused"}}})}};
        return {id, MessageRole::TOOL, {result}};
    }
    static Message approval(std::string id, std::string target, bool approved) {
        Message message{id, MessageRole::SYSTEM, {}};
        message.manualApproval = ManualApprovalChange{target, approved};
        return message;
    }
    static std::string line(std::size_t sequence, std::vector<Message> messages) {
        return Json{{"sequence", sequence}, {"updatedAt", sequence}, {"messages", messages}}.dump() + '\n';
    }
    static void append(const fs::path& path, const std::string& value) {
        std::ofstream stream(path, std::ios::binary | std::ios::app);
        stream << value;
    }
};
}

TEST_F(SessionJournalTest, SkillActivityNameSurvivesLazyProjectionWithoutBodyReads) {
    SessionJournal journal(directory);
    std::vector<Message> history{text("user"),
        {"call", MessageRole::ASSISTANT, {ToolCallContent{"skill-call", "skill",
            {{"name", "check-api"}, {"input", std::string(100000, 'x')}}}}},
        {"result", MessageRole::TOOL, {ToolResultContent{"skill-call", "Loaded", false}}}};
    journal.append(history, 1);
    auto brief = journal.summaries();
    SessionHistoryView view;
    view.setReader([](std::size_t) -> Message { throw std::logic_error("Activity must not load bodies"); });
    view.update(brief);
    const auto row = view.activities(brief, "user/activity/0")["activities"][0];
    EXPECT_EQ(row["arguments"], Json({{"name", "check-api"}}));
    EXPECT_LT(row.dump().size(), 1000);
}

TEST_F(SessionJournalTest, FileMutationPathsSurviveLazyHistoryWithoutLoadingDiffsOrArguments) {
    const std::string patch = "*** Begin Patch\n*** Update File: src/old.cpp\n*** Move to: src/new.cpp\n@@\n-old\n+"
        + std::string(100000, 'x') + "\n*** Add File: tests/new.cpp\n+test\n*** End Patch";
    ToolResultContent applied{"patch", "Success", false};
    applied.metadata = {{"files", Json::array({
        {{"filePath", "/repo/src/old.cpp"}, {"relativePath", "src/new.cpp"}, {"movePath", "/repo/src/new.cpp"},
            {"type", "move"}, {"patch", std::string(100000, 'd')}},
        {{"filePath", "/repo/tests/new.cpp"}, {"relativePath", "tests/new.cpp"}, {"type", "add"}, {"patch", "+test"}}})}};
    std::vector<Message> history{text("user"),
        {"calls", MessageRole::ASSISTANT, {
            ToolCallContent{"patch", "apply_patch", {{"patchText", patch}}},
            ToolCallContent{"edit", "edit", {{"filePath", "src/edit.cpp"}, {"oldString", "secret"}, {"newString", "new"}}},
            ToolCallContent{"write", "write", {{"filePath", "src/write.cpp"}, {"content", "secret"}}}}},
        {"results", MessageRole::TOOL, {applied, ToolResultContent{"edit", "Failed", true}, ToolResultContent{"write", "Done", false}}}};
    {
        SessionJournal journal(directory);
        journal.append(history, 1);
    }
    SessionJournal journal(directory);
    const auto summaries = journal.summaries();
    int reads = 0;
    SessionHistoryView view;
    view.setReader([&](std::size_t position) { ++reads; return journal.reader()(position); });
    view.update(summaries);
    const auto items = view.activities(summaries, "user/activity/0").at("activities");
    ASSERT_EQ(items.size(), 3);
    EXPECT_EQ(items[0].at("arguments"), Json({{"filePaths", {"src/new.cpp", "tests/new.cpp"}}}));
    EXPECT_EQ(items[0].at("result").at("metadata").at("files")[0].at("relativePath"), "src/new.cpp");
    EXPECT_FALSE(items[0].at("result").at("metadata").at("files")[0].contains("patch"));
    EXPECT_EQ(items[1].at("arguments"), Json({{"filePath", "src/edit.cpp"}}));
    EXPECT_EQ(items[2].at("arguments"), Json({{"filePath", "src/write.cpp"}}));
    EXPECT_EQ(reads, 0);
    EXPECT_LT(items.dump().size(), 2000);
    const auto detail = view.activityDetail(summaries, "user/activity/0", 0).at("activities")[0];
    EXPECT_EQ(detail.at("arguments").at("patchText"), patch);
    EXPECT_EQ(detail.at("result").at("metadata").at("files")[0].at("patch"), std::string(100000, 'd'));
    EXPECT_GT(reads, 0);
    EXPECT_TRUE(SessionHistoryView::fileArguments("apply_patch", {{"patchText", 42}}).empty());
    EXPECT_TRUE(SessionHistoryView::fileArguments("write", {{"filePath", Json::array()}}).empty());
}

TEST_F(SessionJournalTest, GrepTitlesUseOnlyStoredSummariesWithoutLegacyBodyFallback) {
    ToolResultContent replayed{"replay", std::string(100000, 'x'), false};
    replayed.metadata = {{"approved_action", {{"id", "approval"}, {"tool", "grep"},
        {"arguments", {{"pattern", "FIXME"}, {"path", "src"}}}}}};
    {
        SessionJournal journal(directory);
        journal.append({text("user"),
            {"calls", MessageRole::ASSISTANT, {
                ToolCallContent{"grep", "grep", {{"pattern", "TODO"}, {"path", "src"}}},
                ToolCallContent{"replay", "rerun_approved_action", {{"id", "approval"}}}}},
            {"results", MessageRole::TOOL, {ToolResultContent{"grep", std::string(100000, 'x'), false}, replayed}}}, 1);
    }
    const auto checkTitles = [](SessionJournal& journal, bool hasPattern) {
        const auto items = journal.historyIndex().activity("user/activity/0", journal.reader()).at("activities");
        ASSERT_EQ(items.size(), 2);
        EXPECT_EQ(items[0].at("arguments"), hasPattern ? Json({{"pattern", "TODO"}}) : Json::object());
        EXPECT_EQ(items[1].at("arguments"), hasPattern ? Json({{"pattern", "FIXME"}}) : Json::object());
        for (const auto& item : items) EXPECT_FALSE(item.at("result").contains("output"));
        EXPECT_LT(items.dump().size(), 1000);
    };
    {
        SessionJournal journal(directory);
        checkTitles(journal, true);
        EXPECT_EQ(journal.bytesRead(), 0);
    }
    // Existing databases were saved before grep patterns were retained.
    {
        SessionDatabase database(directory / "session.sqlite");
        SessionDatabase::Statement query(database, "SELECT position,summary FROM messages ORDER BY position");
        while (query.step()) {
            auto brief = Json::parse(query.text(1)).get<Message>();
            for (auto& part : brief.content) {
                if (auto* call = std::get_if<ToolCallContent>(&part); call && call->toolName == "grep")
                    call->arguments.erase("pattern");
                if (auto* result = std::get_if<ToolResultContent>(&part); result && result->metadata.contains("approved_action"))
                    result->metadata["approved_action"].erase("arguments");
            }
            SessionDatabase::Statement update(database, "UPDATE messages SET summary=? WHERE position=?");
            update.bind(1, Json(brief).dump());
            update.bind(2, query.integer(0));
            update.step();
        }
    }
    SessionJournal legacy(directory);
    checkTitles(legacy, false);
    EXPECT_EQ(legacy.bytesRead(), 0);
    const auto detail = legacy.historyIndex().detail("user/activity/0", 0, legacy.reader());
    EXPECT_EQ(detail.at("activities")[0].at("arguments").at("pattern"), "TODO");
}

TEST_F(SessionJournalTest, GlobPatternIsAvailableFromSummariesAfterReopenAndReplay) {
    ToolResultContent replayed{"replay", std::string(100000, 'x'), false};
    replayed.metadata = {{"approved_action", {{"id", "approval"}, {"tool", "glob"},
        {"arguments", {{"pattern", "**/*.hpp"}, {"path", "src"}}}}}};
    {
        SessionJournal journal(directory);
        journal.append({text("user"),
            {"calls", MessageRole::ASSISTANT, {
                ToolCallContent{"glob", "glob", {{"pattern", "**/*.cpp"}, {"path", "src"}}},
                ToolCallContent{"replay", "rerun_approved_action", {{"id", "approval"}}}}},
            {"results", MessageRole::TOOL, {ToolResultContent{"glob", std::string(100000, 'x'), false}, replayed}}}, 1);
    }
    SessionJournal journal(directory);
    const auto items = journal.historyIndex().activity("user/activity/0", journal.reader()).at("activities");
    ASSERT_EQ(items.size(), 2);
    EXPECT_EQ(items[0].at("arguments"), Json({{"pattern", "**/*.cpp"}}));
    EXPECT_EQ(items[1].at("arguments"), Json({{"pattern", "**/*.hpp"}}));
    for (const auto& item : items) EXPECT_FALSE(item.at("result").contains("output"));
    EXPECT_EQ(journal.bytesRead(), 0);
    EXPECT_LT(items.dump().size(), 1000);
}

TEST_F(SessionJournalTest, BashMainCommandIsAvailableFromSummariesAfterReopenAndReplay) {
    ToolResultContent replayed{"replay", std::string(100000, 'x'), false};
    replayed.metadata = {{"approved_action", {{"id", "approval"}, {"tool", "bash"},
        {"arguments", {{"mainCommand", "pytest"}, {"command", "python -m pytest"}}}}}, {"exit", 0}};
    {
        SessionJournal journal(directory);
        journal.append({text("user"),
            {"calls", MessageRole::ASSISTANT, {
                ToolCallContent{"bash", "bash", {{"mainCommand", "git"}, {"command", "git status"}}},
                ToolCallContent{"replay", "rerun_approved_action", {{"id", "approval"}}},
                ToolCallContent{"old", "bash", {{"command", "echo legacy"}}}}},
            {"results", MessageRole::TOOL, {ToolResultContent{"bash", std::string(100000, 'x'), false}, replayed}}}, 1);
    }
    SessionJournal journal(directory);
    const auto items = journal.historyIndex().activity("user/activity/0", journal.reader()).at("activities");
    ASSERT_EQ(items.size(), 3);
    EXPECT_EQ(items[0].at("arguments"), Json({{"mainCommand", "git"}}));
    EXPECT_EQ(items[1].at("arguments"), Json({{"mainCommand", "pytest"}}));
    EXPECT_EQ(items[1].at("result").at("metadata").at("exit"), 0);
    EXPECT_TRUE(items[2].at("arguments").empty());
    EXPECT_EQ(journal.bytesRead(), 0);
    EXPECT_LT(items.dump().size(), 1500);
}

TEST_F(SessionJournalTest, ImagesAreDeduplicatedTransactionalAndLoadedOnlyForContext) {
    const auto image = validateImage({"", "image/png", "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a6a0AAAAASUVORK5CYII=", 0, 0, "pixel.png", "clipboard"});
    auto first = text("one"); first.images = {image};
    auto second = text("two"); second.images = {image};
    {
        SessionJournal journal(directory);
        EXPECT_THROW(journal.append({first, first}, 1), std::runtime_error);
        EXPECT_FALSE(journal.hasImages());
        EXPECT_EQ(journal.count(), 0);
        journal.append({first, second}, 2);
        EXPECT_TRUE(journal.hasImages());
        EXPECT_TRUE(journal.summaries()[0].images[0].data.empty());
        EXPECT_TRUE(journal.reader({}, false)(0).images[0].data.empty());
        SessionDatabase db(directory / "session.sqlite");
        SessionDatabase::Statement count(db, "SELECT COUNT(*) FROM images");
        ASSERT_TRUE(count.step()); EXPECT_EQ(count.integer(0), 1);
    }
    SessionJournal restored(directory);
    EXPECT_EQ(restored.reader()(1).images[0].data, image.data);
    EXPECT_EQ(restored.readImage(image.id).data, image.data);
    EXPECT_THROW(restored.readImage("img_other_session"), std::invalid_argument);
    SessionHistoryService history(std::make_shared<SessionJournal>(directory));
    auto page = history.page("latest", "", false);
    EXPECT_EQ(page.record(0)["images"][0]["id"], image.id);
    EXPECT_FALSE(page.record(0)["images"][0].contains("data"));
}

TEST_F(SessionJournalTest, BoundedPlanHistoryNeverPagesIntoConstructionAfterReopen) {
    {
        SessionJournal journal(directory);
        std::vector<Message> messages;
        for (int i = 0; i < 35; ++i) messages.push_back(text("planning-" + std::to_string(i)));
        for (int i = 0; i < 50; ++i) messages.push_back(text("construction-" + std::to_string(i)));
        journal.append(messages, 1);
    }
    SessionHistoryService history(std::make_shared<SessionJournal>(directory));
    auto latest = history.page("latest", "", false, 0, 35);
    EXPECT_EQ(latest.last, "planning-34");
    EXPECT_FALSE(latest.hasNewer);
    EXPECT_TRUE(latest.hasOlder);
    for (std::size_t i = 0; i < latest.count; ++i)
        EXPECT_EQ(latest.record(i).dump().find("construction-"), std::string::npos);
    auto older = history.page("before", latest.first, false, 0, 35);
    EXPECT_TRUE(older.hasNewer);
    auto newer = history.page("after", older.last, false, 0, 35);
    EXPECT_EQ(newer.last, "planning-34");
    EXPECT_FALSE(newer.hasNewer);
    EXPECT_THROW(history.page("after", "construction-0", false, 0, 35), std::invalid_argument);
    EXPECT_EQ(history.page().last, "construction-49");
}

TEST_F(SessionJournalTest, LastDisplayRecapSurvivesReopenAndIgnoresInvisibleJournalEntries) {
    {
        SessionJournal journal(directory);
        EXPECT_FALSE(journal.historyIndex().lastIsRecap());
        std::vector<Message> history;
        for (int i = 0; i < 100; ++i) history.push_back(text("user-" + std::to_string(i)));
        auto recap = text("recap", MessageRole::ASSISTANT);
        recap.recap = true;
        history.push_back(recap);
        history.push_back(text("invisible-system", MessageRole::SYSTEM));
        journal.append(history, 1);
    }
    SessionJournal reopened(directory);
    const auto before = reopened.summaryRowsRead();
    EXPECT_TRUE(reopened.historyIndex().lastIsRecap());
    EXPECT_EQ(reopened.summaryRowsRead() - before, 1);
    EXPECT_EQ(reopened.bytesRead(), 0); // No bodies or full history scan.
    // Reading an old page never changes the tail used by the idle check.
    reopened.historyIndex().page("before", "recap");
    EXPECT_TRUE(reopened.historyIndex().lastIsRecap());
    auto checkpoint = text("checkpoint", MessageRole::ASSISTANT);
    checkpoint.compaction = CompactionMetadata{{}, "sane"};
    reopened.append({checkpoint}, 2);
    EXPECT_FALSE(reopened.historyIndex().lastIsRecap());
    auto recap = text("recap-2", MessageRole::ASSISTANT);
    recap.recap = true;
    reopened.append({recap}, 3);
    EXPECT_TRUE(reopened.historyIndex().lastIsRecap());
    reopened.append({text("new-user")}, 4);
    EXPECT_FALSE(reopened.historyIndex().lastIsRecap());
}

TEST_F(SessionJournalTest, ObjectivePlanRemainsAvailableAcrossOrdinaryDiscussionAndReopen) {
    auto request = text("request");
    request.userMetadata = UserMessageMetadata{"sane", true};
    auto middle = text("intermediate", MessageRole::ASSISTANT);
    auto plan = text("plan", MessageRole::ASSISTANT);
    plan.objectivePlan = true;
    {
        SessionJournal journal(directory);
        journal.append({request, middle}, 1);
        EXPECT_FALSE(journal.objectivePlanReady());
        journal.append({plan}, 2);
        EXPECT_TRUE(journal.objectivePlanReady());
    }
    SessionJournal reopened(directory);
    EXPECT_TRUE(reopened.objectivePlanReady());
    reopened.append({Message{"tool-only", MessageRole::ASSISTANT, {ToolCallContent{"call", "read", {{"filePath", "x"}}}}}}, 3);
    EXPECT_TRUE(reopened.objectivePlanReady()); // Activities are not assistant replies.
    reopened.append({text("later", MessageRole::ASSISTANT)}, 4);
    EXPECT_TRUE(reopened.objectivePlanReady());
    auto next = text("discussion");
    reopened.append({next}, 5);
    EXPECT_TRUE(reopened.objectivePlanReady());
    EXPECT_TRUE(reopened.reader()(2).objectivePlan);
    EXPECT_TRUE(reopened.reader()(0).userMetadata->saneRequest);
}

TEST_F(SessionJournalTest, RecoveryReadsOnlyPendingCallsAndCurrentPermissionState) {
    const Json action{{"tool", "bash"}, {"arguments", {{"command", "echo yes"}}}};
    {
        SessionJournal writer(directory);
        std::vector<Message> messages;
        for (int i = 0; i < 3000; ++i) messages.push_back(text("old-" + std::to_string(i)));
        messages.push_back(refused("refused", action));
        messages.push_back(approval("approve", "refused", true));
        messages.push_back({"call", MessageRole::ASSISTANT, {
            ToolCallContent{"finished", "bash", {}}, ToolCallContent{"pending", "bash", {}}}});
        messages.push_back({"result", MessageRole::TOOL, {ToolResultContent{"finished", "ok", false}}});
        writer.append(messages, 1);
    }
    auto journal = std::make_shared<SessionJournal>(directory);
    EXPECT_EQ(journal->recovery().interruptedCalls(), std::vector<std::string>{"pending"});
    EXPECT_EQ(journal->recovery().approvedActions(), std::vector<Json>{action});
    EXPECT_EQ(journal->recovery().refusedAction("refused"), action);
    EXPECT_TRUE(journal->recovery().approvalStates()["refused"].get<bool>());
    const auto changes = journal->recovery().changesSinceLastUser();
    ASSERT_EQ(changes.size(), 1);
    EXPECT_TRUE(changes.front().approved);
    SessionInfo info;
    Session session(info, directory, {}, journal);
    EXPECT_EQ(session.manualActions(), std::vector<Json>{action});
    session.setManualApproval("refused", false);
    EXPECT_TRUE(session.manualActions().empty());
    EXPECT_FALSE(session.manualApprovalStates()["refused"].get<bool>());
    EXPECT_FALSE(session.agentReady());
    EXPECT_EQ(journal->bytesRead(), 0);
    EXPECT_EQ(journal->summaryRowsRead(), 0);
}

TEST_F(SessionJournalTest, RecoveryKeepsExactActionIdentityAndChronologicalAudit) {
    const Json action{{"tool", "bash"}, {"arguments", {{"command", "echo yes"}}}};
    auto other = action;
    other["working_directory"] = "/other";
    SessionJournal journal(directory);
    journal.append({text("user"), refused("first", action), refused("second", action), refused("other", other),
        approval("a1", "first", true), approval("a2", "second", false), approval("a3", "first", true)}, 1);
    const auto& recovery = journal.recovery();
    EXPECT_EQ(recovery.approvedActions(), std::vector<Json>{action});
    EXPECT_EQ(recovery.approvalStates(), (Json{{"first", true}, {"second", true}, {"other", false}}));
    const auto changes = recovery.changesSinceLastUser();
    ASSERT_EQ(changes.size(), 3);
    EXPECT_TRUE(changes[0].approved);
    EXPECT_FALSE(changes[1].approved);
    EXPECT_TRUE(changes[2].approved);
    EXPECT_EQ(changes[1].action, action);
    journal.append({text("next")}, 2);
    EXPECT_TRUE(recovery.changesSinceLastUser().empty());
    EXPECT_TRUE(recovery.approved(action));
    EXPECT_FALSE(recovery.approved(other));
    EXPECT_THROW(recovery.refusedAction("missing"), std::invalid_argument);
    EXPECT_EQ(journal.bytesRead(), 0);
    EXPECT_EQ(journal.summaryRowsRead(), 0);
}

TEST_F(SessionJournalTest, RecoveryProjectionRollsBackWithItsSourceMessages) {
    const Json action{{"tool", "write"}, {"arguments", {{"filePath", "file"}}}};
    SessionJournal journal(directory);
    journal.append({refused("refused", action), approval("approved", "refused", true),
        {"call", MessageRole::ASSISTANT, {ToolCallContent{"pending", "bash", {}}}}}, 1);
    EXPECT_THROW(journal.append({approval("revoked", "refused", false),
        {"result", MessageRole::TOOL, {ToolResultContent{"pending", "ok", false}}}, text("call")}, 2), std::runtime_error);
    EXPECT_EQ(journal.count(), 3);
    EXPECT_TRUE(journal.recovery().approved(action));
    EXPECT_EQ(journal.recovery().changesSinceLastUser().size(), 1);
    EXPECT_EQ(journal.recovery().interruptedCalls(), std::vector<std::string>{"pending"});
    EXPECT_THROW(journal.append({approval("invalid", "missing", true)}, 2), std::invalid_argument);
    EXPECT_EQ(journal.count(), 3);
}

TEST_F(SessionJournalTest, RecoveryHandlesOutOfOrderResultsAndCancellation) {
    SessionJournal journal(directory);
    journal.append({{"early", MessageRole::TOOL, {ToolResultContent{"done", "ok", false}}},
        {"call", MessageRole::ASSISTANT, {ToolCallContent{"pending", "bash", {}}, ToolCallContent{"done", "bash", {}}}}}, 1);
    EXPECT_EQ(journal.recovery().interruptedCalls(), std::vector<std::string>{"pending"});
    EXPECT_THROW(journal.recovery().interruptedCalls([] { return true; }), core::Cancelled);
    journal.append({{"result", MessageRole::TOOL, {ToolResultContent{"pending", "interrupted", true}}}}, 2);
    EXPECT_TRUE(journal.recovery().interruptedCalls().empty());
    EXPECT_TRUE(SessionJournal(directory).recovery().interruptedCalls().empty());
}

TEST_F(SessionJournalTest, PreviousSchemasAreRejectedWithoutChangingMessagesOrVersion) {
    SessionJournal(directory).append({text("user")}, 1);
    for (int schema = 1; schema < 6; ++schema) {
        SessionDatabase database(directory / "session.sqlite");
        database.execute(("PRAGMA user_version=" + std::to_string(schema)).c_str());
        EXPECT_THROW(SessionJournal{directory}, std::runtime_error);
        SessionDatabase::Statement version(database, "PRAGMA user_version");
        ASSERT_TRUE(version.step());
        EXPECT_EQ(version.integer(0), schema);
        SessionDatabase::Statement message(database, "SELECT id FROM messages");
        ASSERT_TRUE(message.step());
        EXPECT_EQ(message.text(0), "user");
    }
}

TEST_F(SessionJournalTest, IndexedPagesSkipLargeBodiesAndMatchTheInMemoryView) {
    std::vector<Message> history;
    SessionJournal writer(directory);
    const auto add = [&](Message message) {
        writer.append({message}, history.size() + 1);
        history.push_back(std::move(message));
    };
    for (int turn = 0; turn < 150; ++turn) {
        const auto id = std::to_string(turn);
        add(text("user-" + id));
        add({"call-" + id, MessageRole::ASSISTANT, {ToolCallContent{"call-" + id, "bash", {{"command", "echo test"}}}}});
        add({"result-" + id, MessageRole::TOOL, {ToolResultContent{"call-" + id, std::string(200000, 'x'), false}}});
        add(text("answer-" + id, MessageRole::ASSISTANT));
    }
    const auto started = std::chrono::steady_clock::now();
    SessionJournal loaded(directory);
    EXPECT_EQ(loaded.bytesRead(), 0); // No journal read to open an up-to-date index.
    SessionHistoryView disk, memory;
    disk.update(loaded.summaries());
    disk.setReader(loaded.reader());
    memory.update(history);
    auto page = disk.page();
    for (const auto record : page.records)
        EXPECT_EQ(disk.record(loaded.summaries(), record, page.loadedGroups.contains(record)),
                  memory.record(history, record, page.loadedGroups.contains(record)));
    const auto pageBytes = loaded.bytesRead();
    EXPECT_LT(pageBytes, loaded.sizeBytes() / 100);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    std::cout << "Indexed resume: " << elapsed << " ms, " << pageBytes << " journal bytes read / "
              << loaded.sizeBytes() << " stored bytes\n";
    EXPECT_EQ(loaded.lastUserMessage(), "Text for user-149");
    EXPECT_EQ(disk.activities(loaded.summaries(), "user-0/activity/0"), memory.activities(history, "user-0/activity/0"));
    EXPECT_EQ(loaded.bytesRead(), pageBytes + Json(history[596]).dump().size());
    EXPECT_EQ(disk.activityDetail(loaded.summaries(), "user-0/activity/0", 0),
              memory.activityDetail(history, "user-0/activity/0", 0));
    while (page.hasOlder) {
        const auto anchor = page.first;
        page = disk.page("before", anchor);
        EXPECT_EQ(page.records, memory.page("before", anchor).records);
        for (const auto record : page.records)
            EXPECT_EQ(disk.record(loaded.summaries(), record, false), memory.record(history, record, false));
    }
    EXPECT_EQ(page.first, "user-0");
    const auto beforeFullRead = loaded.bytesRead();
    const auto fullStarted = std::chrono::steady_clock::now();
    EXPECT_EQ(loaded.readAll().size(), history.size());
    EXPECT_EQ(loaded.bytesRead() - beforeFullRead, loaded.sizeBytes());
    std::cout << "Full journal read: " << std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - fullStarted).count() << " ms, " << loaded.sizeBytes() << " bytes\n";
}

TEST_F(SessionJournalTest, PersistentProjectionMatchesReferenceAfterEveryAppendAndReopen) {
    std::vector<Message> input{text("system", MessageRole::SYSTEM)};
    for (int turn = 0; turn < 8; ++turn) {
        const auto suffix = std::to_string(turn);
        input.push_back(text("user" + suffix));
        Message response{"response" + suffix, MessageRole::ASSISTANT, {
            ReasoningContent{"thinking"}, TextContent{"first"}, TextContent{"second"},
            ToolCallContent{"read" + suffix, "read", {{"filePath", "file"}}}}};
        response.elapsedMs = 1000;
        if (turn % 3 != 0) response.modelCall = ModelCallMetadata{"model", "high", 500, 25};
        input.push_back(response);
        ToolResultContent result{"read" + suffix, "File not found", true};
        result.metadata["permissions"] = Json::array({{{"decision", "ai_refused"}, {"id", "permission"}}});
        input.push_back({"result" + suffix, MessageRole::TOOL, {result}});
        input.back().elapsedMs = 1500;
        input.push_back({"tail" + suffix, MessageRole::ASSISTANT,
            {TextContent{"progress"}, ReasoningContent{"more reasoning"}, ToolCallContent{"late" + suffix, "bash", {}}}});
        input.back().elapsedMs = 2000;
        if (turn % 2 == 0) input.back().modelCall = ModelCallMetadata{"model", "high", 300, {}};
        auto answer = text("answer" + suffix, MessageRole::ASSISTANT);
        answer.metadata = GenerationMetadata{"model", "high", 2500};
        answer.elapsedMs = 2500;
        input.push_back(answer);
        if (turn > 0) input.push_back({"late-result" + suffix, MessageRole::TOOL,
            {ToolResultContent{"late" + std::to_string(turn - 1), "late output", false}}});
        if (turn == 3) {
            auto checkpoint = text("checkpoint", MessageRole::ASSISTANT);
            checkpoint.compaction = CompactionMetadata{{"user3", "answer3"}, "sane"};
            input.push_back(checkpoint);
        }
    }
    std::vector<Message> history;
    SessionHistoryView reference;
    auto journal = std::make_shared<SessionJournal>(directory);
    const auto compare = [&](const std::string& direction, const std::string& anchor) {
        SessionHistoryService service(journal);
        const auto expected = reference.page(direction, anchor);
        const auto page = service.page(direction, anchor);
        ASSERT_EQ(page.count, expected.records.size());
        EXPECT_EQ(page.first, expected.first);
        EXPECT_EQ(page.last, expected.last);
        EXPECT_EQ(page.hasOlder, expected.hasOlder);
        EXPECT_EQ(page.hasNewer, expected.hasNewer);
        for (std::size_t i = 0; i < page.count; ++i) {
            const auto item = reference.record(history, expected.records[i], expected.loadedGroups.contains(expected.records[i]));
            EXPECT_EQ(page.record(i), item);
            if (item.at("role") != "activity") continue;
            const auto id = item.at("id").get<std::string>();
            EXPECT_EQ(service.activity(id), reference.activities(history, id));
            for (std::size_t j = 0; j < item.at("count").get<std::size_t>(); ++j)
                EXPECT_EQ(service.detail(id, j), reference.activityDetail(history, id, j));
        }
    };
    for (const auto& message : input) {
        SCOPED_TRACE(message.id);
        journal->append({message}, 1);
        history.push_back(message);
        reference.update(history);
        compare("latest", "");
        journal = std::make_shared<SessionJournal>(directory);
        compare("latest", "");
    }
    for (const auto& message : history) if (message.role == MessageRole::USER) {
        compare("before", message.id);
        compare("after", message.id);
    }
}

TEST_F(SessionJournalTest, ProjectionReopensWithoutReadingBodiesAndResolvesPendingCalls) {
    {
        SessionJournal journal(directory);
        journal.append({text("user"), {"call", MessageRole::ASSISTANT, {ToolCallContent{"call", "bash", {}}}}}, 1);
    }
    auto journal = std::make_shared<SessionJournal>(directory);
    SessionHistoryService service(journal);
    EXPECT_EQ(journal->summaryRowsRead(), 0);
    EXPECT_EQ(service.activity("user/activity/0")["count"], 1);
    EXPECT_EQ(journal->summaryRowsRead(), 1);
    EXPECT_EQ(journal->bytesRead(), 0);
    // Pending links persist through reopening and are updated by a later result.
    journal->append({{"result", MessageRole::TOOL, {ToolResultContent{"call", "done", false}}}}, 2);
    EXPECT_EQ(service.detail("user/activity/0", 0)["activities"][0]["result"]["output"], "done");
}

TEST_F(SessionJournalTest, ProjectionRollbackDoesNotLeaveRecordsOrChangedGroupState) {
    auto journal = std::make_shared<SessionJournal>(directory);
    journal->append({text("user"), {"call", MessageRole::ASSISTANT, {ToolCallContent{"call", "bash", {}}}}}, 1);
    SessionHistoryService service(journal);
    const auto before = service.activity("user/activity/0");
    EXPECT_THROW(journal->append({{"result", MessageRole::TOOL, {ToolResultContent{"call", "done", false}}},
        text("next"), text("user")}, 2), std::runtime_error);
    EXPECT_EQ(service.page().last, "user");
    EXPECT_EQ(service.activity("user/activity/0"), before);
    journal->append({{"result", MessageRole::TOOL, {ToolResultContent{"call", "done", false}}}}, 3);
    EXPECT_EQ(service.detail("user/activity/0", 0)["activities"][0]["result"]["output"], "done");
}

TEST_F(SessionJournalTest, SkillSquaresAreDistinctWithoutReadingBodiesOrSummaries) {
    const std::vector<Message> history{text("user"),
        {"skill", MessageRole::ASSISTANT, {ToolCallContent{"s", "skill", {{"name", "test"}}}}},
        {"result", MessageRole::TOOL, {ToolResultContent{"s", "loaded", false}}}};
    {
        SessionJournal journal(directory);
        journal.append(history, 1);
    }
    auto journal = std::make_shared<SessionJournal>(directory);
    const auto collapsed = [&] {
        return journal->historyIndex().record(1, false, [](std::size_t) -> Message {
            throw std::logic_error("Collapsed group must not read bodies");
        });
    };
    EXPECT_EQ(collapsed()["kinds"], Json::array({"skill"}));
    EXPECT_EQ(journal->bytesRead(), 0);
    const auto reads = journal->summaryRowsRead();
    EXPECT_EQ(reads, 0);
    EXPECT_EQ(collapsed()["kinds"], Json::array({"skill"}));
    EXPECT_EQ(journal->summaryRowsRead(), reads);
    journal->append({{"bash", MessageRole::ASSISTANT, {ToolCallContent{"b", "bash", {{"command", "true"}}}}}}, 2);
    EXPECT_EQ(collapsed()["kinds"], Json::array({"tools", "skill"}));
    SessionHistoryView reference;
    auto mixed = history;
    mixed.push_back({"bash", MessageRole::ASSISTANT, {ToolCallContent{"b", "bash", {{"command", "true"}}}}});
    reference.update(mixed);
    EXPECT_EQ(reference.record(mixed, 1, false)["kinds"], Json::array({"tools", "skill"}));
}

TEST_F(SessionJournalTest, LatestPageReadsNoGlobalSummariesRegardlessOfHistoryLength) {
    std::uintmax_t expectedBytes = 0;
    for (const auto length : {30, 3000}) {
        const auto folder = directory / std::to_string(length);
        fs::create_directory(folder);
        {
            SessionJournal writer(folder);
            std::vector<Message> messages;
            for (int i = 0; i < length; ++i) messages.push_back(text("old" + std::to_string(i)));
            for (int i = 0; i < 20; ++i) messages.push_back(text("last" + std::to_string(i)));
            writer.append(messages, 1);
        }
        auto journal = std::make_shared<SessionJournal>(folder);
        EXPECT_EQ(journal->bytesRead(), 0);
        SessionHistoryService service(journal);
        const auto page = service.page();
        ASSERT_EQ(page.first, "last0");
        ASSERT_EQ(page.last, "last19");
        for (std::size_t i = 0; i < page.count; ++i) page.record(i);
        EXPECT_EQ(journal->summaryRowsRead(), 0);
        if (expectedBytes) EXPECT_EQ(journal->bytesRead(), expectedBytes);
        expectedBytes = journal->bytesRead();
    }
}

TEST_F(SessionJournalTest, CollapsedHundredThousandItemActivityReadsOnlyItsAggregate) {
    {
        SessionJournal writer(directory);
        Message calls{"calls", MessageRole::ASSISTANT, {}};
        for (int i = 0; i < 100000; ++i)
            calls.content.push_back(ToolCallContent{"call" + std::to_string(i), "bash", {{"command", "echo test"}}});
        writer.append({text("user"), calls, text("next-user")}, 1);
    }
    auto journal = std::make_shared<SessionJournal>(directory);
    SessionHistoryService service(journal);
    const auto page = service.page();
    ASSERT_EQ(page.count, 4);
    const auto activity = page.record(1);
    EXPECT_EQ(activity["count"], 100000);
    EXPECT_EQ(activity["kinds"], Json::array({"tools"}));
    EXPECT_FALSE(activity.contains("activities"));
    EXPECT_EQ(journal->bytesRead(), 0);
    EXPECT_EQ(journal->summaryRowsRead(), 0);
    EXPECT_EQ(page.record(3)["count"], 0);
}

TEST_F(SessionJournalTest, HistoryServiceHidesIndexAndReadsPageBodiesLazily) {
    auto journal = std::make_shared<SessionJournal>(directory);
    std::vector<Message> messages;
    for (int i = 0; i < 50; ++i) messages.push_back(text("user-" + std::to_string(i)));
    journal->append(messages, 1);
    SessionHistoryService service(journal);
    const auto before = journal->bytesRead();
    const auto latest = service.page();
    EXPECT_TRUE(latest.hasOlder);
    EXPECT_FALSE(latest.hasNewer);
    EXPECT_EQ(journal->bytesRead(), before);
    SessionHistoryView reference;
    reference.update(messages);
    const auto expected = reference.page();
    ASSERT_EQ(latest.count, expected.records.size());
    for (std::size_t i = 0; i < latest.count; ++i)
        EXPECT_EQ(latest.record(i), reference.record(messages, expected.records[i], expected.loadedGroups.contains(expected.records[i])));
    EXPECT_THROW(latest.record(latest.count), std::out_of_range);
    const auto older = service.page("before", latest.first);
    EXPECT_TRUE(older.hasNewer);
    EXPECT_NE(older.first, latest.first);
    EXPECT_THROW(service.page("before", "missing"), std::invalid_argument);
    journal->append({text("new-user")}, 2);
    EXPECT_EQ(service.page().last, "new-user");
    EXPECT_EQ(latest.last, "user-49");
    EXPECT_NO_THROW(latest.record(0));
}

TEST_F(SessionJournalTest, HistoryGlobalExtentAndDirectSeekStayLazyAndRespectBranchBounds) {
    auto journal = std::make_shared<SessionJournal>(directory);
    std::vector<Message> messages;
    for (int i = 0; i < 100; ++i) messages.push_back(text("user-" + std::to_string(i)));
    journal->append(messages, 1);
    SessionHistoryService service(journal);
    const auto bytes = journal->bytesRead();
    const auto latest = service.page();
    EXPECT_EQ(latest.total, 200); // One message and its activity per user turn.
    EXPECT_EQ(latest.end, latest.total);
    EXPECT_GT(latest.start, 0);
    const auto middle = service.page("at", "80");
    EXPECT_EQ(middle.total, latest.total);
    EXPECT_LE(middle.start, 80);
    EXPECT_GT(middle.end, 80);
    EXPECT_TRUE(middle.hasOlder);
    EXPECT_TRUE(middle.hasNewer);
    const auto branch = service.page("at", "10", false, 20, 80);
    EXPECT_EQ(branch.total, 120);
    EXPECT_EQ(branch.start, 0);
    EXPECT_LT(branch.end, branch.total);
    EXPECT_EQ(journal->bytesRead(), bytes);
    EXPECT_THROW(service.page("at", "-1"), std::invalid_argument);
    EXPECT_THROW(service.page("at", "200"), std::invalid_argument);
    EXPECT_THROW(service.page("at", "1invalid"), std::invalid_argument);
    SessionHistoryService restored(std::make_shared<SessionJournal>(directory));
    EXPECT_EQ(restored.page("at", "80").start, middle.start);
    EXPECT_EQ(restored.page().total, latest.total);
}

TEST_F(SessionJournalTest, HistoryServiceTracksToolResultsAndReopensWithoutAnAgent) {
    auto journal = std::make_shared<SessionJournal>(directory);
    journal->append({text("user"), {"call", MessageRole::ASSISTANT,
        {ToolCallContent{"tool", "bash", {{"command", "echo hello"}}}}}}, 1);
    SessionHistoryService service(journal);
    EXPECT_FALSE(service.activity("user/activity/0")["activities"][0].contains("result"));
    journal->append({{"result", MessageRole::TOOL, {ToolResultContent{"tool", "hello", false}}}}, 2);
    EXPECT_EQ(service.detail("user/activity/0", 0)["activities"][0]["result"]["output"], "hello");
    EXPECT_THROW(service.activity("missing"), std::invalid_argument);
    EXPECT_THROW(service.detail("user/activity/0", 1), std::invalid_argument);
    SessionHistoryService restored(std::make_shared<SessionJournal>(directory));
    EXPECT_EQ(restored.activity("user/activity/0"), service.activity("user/activity/0"));
    EXPECT_EQ(restored.detail("user/activity/0", 0), service.detail("user/activity/0", 0));
}

TEST_F(SessionJournalTest, SkillScriptClassificationSurvivesReopenWithoutChangingMessages) {
    const std::string command = "python3 .sos/skills/demo/scripts/nested/run.py --text=hello";
    Message call{"call", MessageRole::ASSISTANT, {ToolCallContent{"tool", "bash", {{"command", command}}}}};
    {
        SessionJournal journal(directory);
        journal.append({text("user"), call,
            {"result", MessageRole::TOOL, {ToolResultContent{"tool", "hello", false}}}}, 1);
    }
    auto journal = std::make_shared<SessionJournal>(directory);
    SessionHistoryService restored(journal);
    const auto row = restored.activity("user/activity/0")["activities"][0];
    EXPECT_EQ(row["skill_script"], "demo");
    EXPECT_EQ(row["skill_script_path"], "nested/run.py");
    EXPECT_EQ(journal->historyIndex().record(1, false, {})["kinds"], Json::array({"skill"}));
    EXPECT_EQ(row["toolName"], "bash");
    EXPECT_TRUE(row["arguments"].empty());
    EXPECT_EQ(journal->bytesRead(), 0);
    EXPECT_FALSE(Json(call).dump().contains("skill_script"));
    EXPECT_EQ(restored.detail("user/activity/0", 0)["activities"][0]["skill_script_path"], "nested/run.py");
    EXPECT_FALSE(SessionHistoryView::skillScriptCommand("bash", {{"command", "echo hello"}}));
    EXPECT_FALSE(SessionHistoryView::skillScriptCommand("bash", {{"command", command + std::string(8192, 'x')}}));
}

TEST_F(SessionJournalTest, SystemMessagesDoNotCreateActivitiesOrLoseJournalContent) {
    const std::string injected = "Already loaded.\nName: demo\nArguments: {}\nOriginal instructions [literal]";
    Message injection{"skill-user", MessageRole::SYSTEM, {TextContent{injected}}};
    injection.userSkill = "demo";
    const std::vector<Message> messages{text("user"), injection};
    {
        SessionJournal journal(directory);
        journal.append(messages, 1);
    }
    auto journal = std::make_shared<SessionJournal>(directory);
    SessionHistoryService restored(journal);
    const auto activity = restored.activity("user/activity/0");
    EXPECT_TRUE(activity["kinds"].empty());
    EXPECT_TRUE(activity["activities"].empty());
    EXPECT_EQ(journal->bytesRead(), 0);
    EXPECT_EQ(std::get<TextContent>(journal->reader()(1).content[0]).text, injected);
    SessionHistoryView reference;
    reference.update(messages);
    EXPECT_EQ(reference.activities(messages, "user/activity/0"), activity);
}

TEST_F(SessionJournalTest, SyntheticSkillPairRestoresOneActivityAndProviderUserText) {
    Message user{"user", MessageRole::USER, {TextContent{"//demo value"}}};
    user.providerText = "Call the skill \"demo\" with arguments (JSON data):\n{\"subject\":\"value\"}";
    const Json arguments{{"name", "demo"}, {"arguments", {{"subject", "value"}}}};
    Message call{"skill-call", MessageRole::ASSISTANT, {ToolCallContent{"skill-call", "skill", arguments}}};
    call.userSkill = "demo";
    const std::vector<Message> messages{user, call,
        {"result", MessageRole::TOOL, {ToolResultContent{"skill-call", "Exact original skill content", false}}}};
    {
        SessionJournal journal(directory);
        journal.append(messages, 1);
    }
    auto journal = std::make_shared<SessionJournal>(directory);
    SessionHistoryService restored(journal);
    const auto activity = restored.activity("user/activity/0");
    EXPECT_EQ(activity["kinds"], Json::array({"skill"}));
    ASSERT_EQ(activity["activities"].size(), 1);
    const auto& item = activity["activities"][0];
    EXPECT_EQ(item["kind"], "tool");
    EXPECT_EQ(item["toolName"], "skill");
    EXPECT_EQ(item["user_skill"], "demo");
    EXPECT_FALSE(item["result"].contains("output"));
    EXPECT_EQ(journal->bytesRead(), 0);
    const auto detail = restored.detail("user/activity/0", 0)["activities"][0];
    EXPECT_EQ(detail["arguments"], arguments);
    EXPECT_EQ(detail["result"]["output"], "Exact original skill content");
    SessionHistoryView reference;
    reference.update(messages);
    EXPECT_EQ(reference.activities(messages, "user/activity/0"), activity);
    const auto loaded = journal->readAll();
    EXPECT_EQ(Json(loaded), Json(messages));
    Conversation conversation(ProviderFormat::OPENAI_RESPONSES);
    for (const auto& message : loaded) conversation.append(message);
    EXPECT_EQ(conversation.getProviderFormat().dump().find("//demo"), std::string::npos);
    EXPECT_NE(conversation.getProviderFormat().dump().find("Call the skill"), std::string::npos);
}

TEST_F(SessionJournalTest, ActivityTimesAndAiDecisionsSurviveDatabaseResume) {
    std::vector<Message> history{
        text("user"), text("intro", MessageRole::ASSISTANT),
        {"call1", MessageRole::ASSISTANT, {ToolCallContent{"c1", "write", {{"filePath", "file"}}}}},
        {"result1", MessageRole::TOOL, {ToolResultContent{"c1", "Refused", true}}},
        text("progress", MessageRole::ASSISTANT),
        {"call2", MessageRole::ASSISTANT, {ToolCallContent{"c2", "write", {{"filePath", "other"}}}}},
        {"result2", MessageRole::TOOL, {ToolResultContent{"c2", "Written", false}}},
        text("done", MessageRole::ASSISTANT)};
    const std::vector<std::uint64_t> times{0, 1000, 2500, 3500, 4000, 6000, 7000, 9000};
    for (std::size_t i = 1; i < history.size(); ++i) history[i].elapsedMs = times[i];
    history.back().metadata = GenerationMetadata{"model", "none", 9000};
    std::get<ToolResultContent>(history[3].content[0]).metadata["permissions"] = Json::array({
        {{"id", "1"}, {"decision", "ai_refused"}}});
    std::get<ToolResultContent>(history[6].content[0]).metadata["permissions"] = Json::array({
        {{"id", "2"}, {"decision", "ai_accepted"}}});
    SessionJournal(directory).append(history, 1);
    for (const bool rebuild : {false, true}) {
        if (rebuild) fs::remove(directory / "info.json");
        SessionJournal restored(directory);
        EXPECT_EQ(Json(restored.readAll()), Json(history));
        SessionHistoryView view;
        view.update(restored.summaries());
        const auto first = view.activities(restored.summaries(), "user/activity/0");
        const auto second = view.activities(restored.summaries(), "call1/activity/0");
        const auto last = view.activities(restored.summaries(), "call2/activity/0");
        EXPECT_EQ(first["duration_ms"], 2500);
        EXPECT_EQ(second["duration_ms"], 3500);
        EXPECT_FALSE(second["terminal"].get<bool>());
        EXPECT_EQ(last["metadata"]["duration_ms"], 9000);
        EXPECT_EQ(second["activities"][0]["result"]["metadata"]["permissions"][0]["decision"], "ai_refused");
        EXPECT_EQ(second["kinds"], Json::array({"tools", "refused"}));
        const auto page = view.page();
        bool sawRefusedSummary = false;
        for (const auto index : page.records) {
            const auto record = view.record(restored.summaries(), index, false);
            if (record.at("id") == "call1/activity/0") {
                sawRefusedSummary = true;
                EXPECT_FALSE(record.contains("activities"));
                EXPECT_EQ(record["kinds"], Json::array({"tools", "refused"}));
            }
        }
        EXPECT_TRUE(sawRefusedSummary);
        EXPECT_EQ(last["activities"][0]["result"]["metadata"]["permissions"][0]["decision"], "ai_accepted");
    }
}

TEST_F(SessionJournalTest, JsonlFilesAreNotImportedOrModified) {
    const auto first = line(1, {text("one"), text("two", MessageRole::ASSISTANT)});
    append(journalPath(), first);
    SessionJournal journal(directory);
    EXPECT_EQ(journal.count(), 0);
    EXPECT_TRUE(journal.readAll().empty());
    EXPECT_EQ(fs::file_size(journalPath()), first.size());
    append(journalPath(), line(3, {text("legacy-tail")}));
    append(indexPath(), "broken index");
    journal.append({text("three")}, 3);
    SessionJournal reopened(directory);
    EXPECT_EQ(reopened.bytesRead(), 0);
    EXPECT_EQ(reopened.count(), 1);
    EXPECT_EQ(reopened.lastUserMessage(), "Text for three");
}

TEST_F(SessionJournalTest, ModelCallUsageSurvivesResumeAndIsAggregatedOncePerActivity) {
    for (const auto variant : {0, 1, 2, 3}) {
        const auto folder = directory / std::to_string(variant);
        fs::create_directory(folder);
        std::vector<Message> history{
            text("user"),
            {"a1", MessageRole::ASSISTANT, {ReasoningContent{"Thinking"}, TextContent{"Progress"},
                ToolCallContent{"c1", "bash", {{"command", "first"}}}}},
            {"t1", MessageRole::TOOL, {ToolResultContent{"c1", "Long tool", false}}},
            {"a2", MessageRole::ASSISTANT, {ToolCallContent{"c2", "bash", {{"command", "second"}}}}},
            {"t2", MessageRole::TOOL, {ToolResultContent{"c2", "Long tool", false}}},
            text("a3", MessageRole::ASSISTANT)};
        history[1].elapsedMs = 1000;
        history[2].elapsedMs = 9000;
        history[3].elapsedMs = 12000;
        history[4].elapsedMs = 18000;
        history[5].elapsedMs = 19000;
        history[1].modelCall = ModelCallMetadata{"model", "high", 1000, 10};
        history[3].modelCall = ModelCallMetadata{"model", "high", 3000, 90};
        history[5].modelCall = ModelCallMetadata{"model", "high", 1000, 10};
        history[5].metadata = GenerationMetadata{"model", "high", 19000};
        if (variant == 1) history[3].modelCall->outputTokens.reset();
        if (variant == 2) for (auto& message : history) message.modelCall.reset();
        if (variant == 3) history[3].modelCall.reset();
        SessionJournal(folder).append(history, 1);
        for (const bool rebuild : {false, true}) {
            if (rebuild) fs::remove(folder / "info.json");
            SessionJournal restored(folder);
            EXPECT_EQ(Json(restored.readAll()), Json(history));
            SessionHistoryView view;
            view.update(restored.summaries());
            const auto first = view.activities(restored.summaries(), "user/activity/0");
            const auto last = view.activities(restored.summaries(), "a1/activity/2");
            EXPECT_EQ(first["generation"], (Json{{"model", "model"}, {"reasoning", "high"}}));
            EXPECT_EQ(last["generation"], first["generation"]);
            EXPECT_EQ(first["duration_ms"], 1000);
            EXPECT_EQ(last["metadata"]["duration_ms"], 19000);
            if (variant == 2) {
                EXPECT_FALSE(first.contains("model_call"));
                EXPECT_FALSE(last.contains("model_call"));
            } else {
                EXPECT_EQ(first["model_call"]["output_tokens"], 10);
                EXPECT_EQ(first["model_call"]["duration_ms"], 1000);
                EXPECT_EQ(last["model_call"]["duration_ms"], variant == 3 ? 1000 : 4000);
                if (variant == 0) EXPECT_EQ(last["model_call"]["output_tokens"], 100);
                else EXPECT_FALSE(last["model_call"].contains("output_tokens"));
            }
        }
    }
}

TEST_F(SessionJournalTest, ManualApprovalAuditSurvivesResumeAndOnlyReportsOnNextUserTurn) {
    const Json action{{"tool", "bash"}, {"arguments", {{"command", "echo safe > file"}}},
        {"working_directory", directory.string()}, {"workspace", directory.string()}};
    ToolResultContent refused{"call", "Refused", true};
    refused.metadata = {{"refused_action", action}, {"refused_message_id", "refused"},
        {"permissions", Json::array({{{"decision", "ai_refused"}}})}};
    const Message original{"refused", MessageRole::TOOL, {refused}};
    SessionJournal(directory).append({text("system", MessageRole::SYSTEM), text("user"),
        {"call", MessageRole::ASSISTANT, {ToolCallContent{"call", "bash", action["arguments"]}}}, original}, 1);
    SessionInfo info;
    info.id = "manual-test";
    info.workspace = directory;
    info.mode = ExecutionMode::SLOP;
    const auto factory = [](const SessionInfo& info) {
        return std::make_unique<Agent>(std::make_unique<UnusedProvider>(), info.model);
    };
    {
        Session session(info, directory, factory, std::make_shared<SessionJournal>(directory));
        EXPECT_THROW(session.setManualApproval("missing", true), std::invalid_argument);
        EXPECT_THROW(session.setManualApproval("user", true), std::invalid_argument);
        session.setManualApproval("refused", true);
        EXPECT_FALSE(session.agentReady());
        EXPECT_FALSE(session.isExecuting());
        EXPECT_EQ(session.manualActions(), std::vector<Json>{action});
        EXPECT_TRUE(session.manualApprovalStates()["refused"].get<bool>());
        const auto count = session.messageCount();
        session.setManualApproval("refused", true);
        EXPECT_EQ(session.messageCount(), count);
        session.setManualApproval("refused", false);
        EXPECT_TRUE(session.manualActions().empty());
        session.setManualApproval("refused", true);
        EXPECT_EQ(Json(session.getHistory()[3]), Json(original));
    }
    fs::remove(directory / "info.json"); // Approval state is independent of the readable cache.
    Session restored(info, directory, factory, std::make_shared<SessionJournal>(directory));
    EXPECT_EQ(restored.manualActions(), std::vector<Json>{action});
    EXPECT_TRUE(restored.manualApprovalStates()["refused"].get<bool>());
    EXPECT_EQ(restored.activeConversation().getMessages().size(), 4);
    std::vector<AgentEvent> events;
    restored.setEventHandler([&](AgentEvent event) { events.push_back(std::move(event)); });
    restored.sendMessage("Continue", ExecutionMode::SLOP);
    asio::io_context io;
    auto finished = asio::co_spawn(io, restored.finishExecution(), asio::use_future);
    io.run();
    finished.get();
    for (const auto& event : events) restored.record(event);
    const auto messages = restored.getHistory();
    std::vector<std::string> reports;
    for (const auto& message : messages) {
        if (message.id.starts_with("approval_report_")) reports.push_back(std::get<TextContent>(message.content[0]).text);
    }
    ASSERT_EQ(reports.size(), 1);
    EXPECT_NE(reports[0].find("accepted"), std::string::npos);
    EXPECT_NE(reports[0].find("refused"), std::string::npos);
    EXPECT_NE(reports[0].find("echo safe > file"), std::string::npos);
    EXPECT_EQ(Json(messages[3]), Json(original));
    events.clear();
    restored.sendMessage("Continue again", ExecutionMode::SLOP);
    io.restart();
    auto again = asio::co_spawn(io, restored.finishExecution(), asio::use_future);
    io.run();
    again.get();
    for (const auto& event : events) for (const auto& message : event.messages)
        EXPECT_FALSE(message.id.starts_with("approval_report_"));
}

TEST_F(SessionJournalTest, CorruptDatabaseIsNotReplacedByLegacyHistory) {
    append(journalPath(), line(1, {text("legacy")}));
    { std::ofstream stream(directory / "session.sqlite"); stream << "not a database"; }
    EXPECT_THROW(SessionJournal{directory}, std::runtime_error);
    std::ifstream stream(directory / "session.sqlite");
    std::string data((std::istreambuf_iterator<char>(stream)), {});
    EXPECT_EQ(data, "not a database");
}

TEST_F(SessionJournalTest, JsonlContentsAreIgnoredIncludingIncompleteTails) {
    append(journalPath(), line(1, {text("one")}) + "{\"sequence\":2");
    const auto bytes = fs::file_size(journalPath());
    SessionJournal journal(directory);
    EXPECT_TRUE(journal.lastUserMessage().empty());
    EXPECT_EQ(fs::file_size(journalPath()), bytes);
    journal.append({text("two")}, 2);
    EXPECT_EQ(SessionJournal(directory).count(), 1);
}

TEST_F(SessionJournalTest, ReadsIndividualMessagesAndRespectsCancellation) {
    SessionJournal writer(directory);
    writer.append({text("one"), {"huge", MessageRole::ASSISTANT, {TextContent{std::string(4000000, 'x')}}}, text("two")}, 1);
    SessionJournal loaded(directory);
    EXPECT_EQ(loaded.bytesRead(), 0);
    auto read = loaded.reader();
    EXPECT_EQ(read(0).id, "one");
    EXPECT_EQ(read(2).id, "two");
    EXPECT_LT(loaded.bytesRead(), 1000);
    EXPECT_THROW(loaded.reader([] { return true; })(0), core::Cancelled);
    EXPECT_THROW(SessionJournal(directory, [] { return true; }), core::Cancelled);
    EXPECT_THROW(read(3), std::out_of_range);
    EXPECT_FALSE(fs::exists(journalPath()));
    EXPECT_FALSE(fs::exists(indexPath()));
}

TEST_F(SessionJournalTest, FailedAppendRollsBackEveryMessageAndMetadata) {
    SessionJournal journal(directory);
    SessionInfo info;
    info.id = "test";
    info.title = "Before";
    journal.save({text("one")}, info);
    journal.summaries(); // Exercise rollback with the legacy projector already loaded.
    info.title = "After";
    EXPECT_THROW(journal.save({text("two"), text("one")}, info), std::runtime_error);
    EXPECT_EQ(journal.count(), 1);
    EXPECT_EQ(journal.summaries().size(), 1);
    EXPECT_EQ(journal.info()->title, "Before");
    SessionJournal reopened(directory);
    EXPECT_EQ(reopened.count(), 1);
    EXPECT_EQ(reopened.info()->title, "Before");
    journal.save({text("two")}, info);
    EXPECT_EQ(journal.count(), 2);
}

TEST_F(SessionJournalTest, CancelledInitializationIsRetryable) {
    EXPECT_THROW(SessionJournal(directory, [] { return true; }), core::Cancelled);
    SessionJournal journal(directory);
    journal.append({text("one")}, 1);
    EXPECT_EQ(SessionJournal(directory).lastUserMessage(), "Text for one");
}

TEST_F(SessionJournalTest, InfoCacheCanBeRegeneratedAndDoesNotOverrideDatabase) {
    SessionJournal journal(directory);
    SessionInfo info;
    info.id = "test";
    info.model = "persisted-model";
    info.mode = ExecutionMode::SLOP;
    journal.save({text("one")}, info);
    journal.publishInfo();
    { std::ofstream stream(directory / "info.json"); stream << "broken"; }
    SessionJournal restored(directory);
    ASSERT_TRUE(restored.info());
    EXPECT_EQ(restored.info()->model, "persisted-model");
    restored.publishInfo();
    std::ifstream stream(directory / "info.json");
    auto cache = Json::parse(stream).get<SessionInfo>();
    EXPECT_EQ(cache.mode, ExecutionMode::SLOP);
    EXPECT_EQ(cache.model, "persisted-model");
}

TEST_F(SessionJournalTest, FutureSchemaIsRejectedWithoutLegacyFallback) {
    {
        SessionJournal journal(directory);
        journal.append({text("one")}, 1);
    }
    {
        SessionDatabase database(directory / "session.sqlite");
        database.execute("PRAGMA user_version=999");
    }
    append(journalPath(), line(1, {text("legacy")}));
    EXPECT_THROW(SessionJournal{directory}, std::runtime_error);
    SessionDatabase database(directory / "session.sqlite");
    SessionDatabase::Statement version(database, "PRAGMA user_version");
    ASSERT_TRUE(version.step());
    EXPECT_EQ(version.integer(0), 999);
    SessionDatabase::Statement original(database, "SELECT id FROM messages");
    ASSERT_TRUE(original.step());
    EXPECT_EQ(original.text(0), "one");
}

TEST_F(SessionJournalTest, MetadataCacheDoesNotSeedDatabaseAndProviderReplayPersists) {
    auto message = text("one");
    auto& content = std::get<TextContent>(message.content.front());
    content.provider = ProviderFormat::OPENAI_RESPONSES;
    content.providerData = {{"opaque", "preserved"}};
    SessionInfo info;
    info.id = "legacy";
    info.mode = ExecutionMode::SLOP;
    info.model = "legacy-model";
    const auto serialized = Json(info).dump();
    { std::ofstream stream(directory / "info.json"); stream << serialized; }
    append(journalPath(), line(1, {message}));
    SessionJournal journal(directory);
    EXPECT_FALSE(journal.info());
    EXPECT_EQ(journal.count(), 0);
    journal.save({message}, info);
    ASSERT_TRUE(journal.info());
    EXPECT_EQ(journal.info()->mode, ExecutionMode::SLOP);
    EXPECT_EQ(journal.info()->model, "legacy-model");
    EXPECT_EQ(Json(journal.reader()(0)), Json(message));
    std::ifstream stream(directory / "info.json");
    EXPECT_EQ(std::string((std::istreambuf_iterator<char>(stream)), {}), serialized);
}

TEST_F(SessionJournalTest, ProcessExitRecoversCommittedWalAndDiscardsUncommittedWrites) {
    {
        SessionJournal journal(directory);
        journal.append({text("one")}, 1);
    }
    for (const bool commit : {false, true}) {
        const auto child = fork();
        ASSERT_GE(child, 0);
        if (child == 0) {
            try {
                if (commit) {
                    SessionJournal journal(directory);
                    journal.append({text("committed")}, 2);
                    _exit(0); // Deliberately bypass SQLite close/checkpoint.
                }
                SessionDatabase database(directory / "session.sqlite");
                SessionDatabase::Transaction transaction(database);
                database.execute("INSERT INTO messages SELECT 1,'uncommitted',role,summary,body,2 FROM messages WHERE position=0");
                _exit(0); // Deliberately bypass transaction rollback/destructors.
            } catch (...) { _exit(1); }
        }
        int status = 0;
        ASSERT_EQ(waitpid(child, &status, 0), child);
        ASSERT_TRUE(WIFEXITED(status));
        ASSERT_EQ(WEXITSTATUS(status), 0);
        SessionJournal restored(directory);
        EXPECT_EQ(restored.count(), commit ? 2 : 1);
        EXPECT_EQ(restored.reader()(0).id, "one");
        if (commit) EXPECT_EQ(restored.reader()(1).id, "committed");
        EXPECT_EQ(restored.historyIndex().page("latest", "").last, commit ? "committed" : "one");
    }
}

TEST_F(SessionJournalTest, CompactedContextRestoresOnlyReferencedBodiesAndDoesNotConstructAgentOnOpen) {
    auto journal = std::make_shared<SessionJournal>(directory);
    std::vector<Message> before{
        text("instructions", MessageRole::SYSTEM), text("old"),
        {"discarded", MessageRole::ASSISTANT, {TextContent{std::string(4000000, 'x')}}},
        text("retained"), text("retained-answer", MessageRole::ASSISTANT)};
    for (const auto& message : before) journal->append({message}, 1);
    Message summary{"checkpoint", MessageRole::ASSISTANT, {TextContent{"Summary"}}};
    summary.compaction = CompactionMetadata{{"retained", "retained-answer"}, "slop"};
    journal->append({summary}, 2);
    const auto tail = text("after");
    journal->append({tail}, 3);
    auto expected = CompactionService::restore(before, summary);
    expected.append(tail);
    SessionInfo info;
    info.id = "session";
    info.workspace = directory;
    int constructed = 0;
    Session session(info, directory, [&](const SessionInfo& selected) {
        ++constructed;
        return std::make_unique<Agent>(std::make_unique<UnusedProvider>(), selected.model);
    }, journal);
    EXPECT_FALSE(session.agentReady());
    EXPECT_EQ(constructed, 0);
    EXPECT_EQ(journal->bytesRead(), 0);
    asio::io_context io;
    auto prepare = asio::co_spawn(io, session.prepareAgent({}), asio::use_future);
    io.run();
    EXPECT_TRUE(prepare.get());
    EXPECT_TRUE(session.agentReady());
    EXPECT_EQ(constructed, 1);
    EXPECT_EQ(Json(session.activeConversation().getMessages()), Json(expected.getMessages()));
    EXPECT_LT(journal->bytesRead(), 10000);
}

TEST_F(SessionJournalTest, PreparingAgentIsOffThreadCancellableAndRetryable) {
    auto journal = std::make_shared<SessionJournal>(directory);
    journal->append({text("instructions", MessageRole::SYSTEM), text("user")}, 1);
    SessionInfo info;
    info.id = "session";
    info.workspace = directory;
    std::promise<void> release;
    const auto released = release.get_future().share();
    const auto mainThread = std::this_thread::get_id();
    Session session(info, directory, [&](const SessionInfo& selected) {
        EXPECT_NE(std::this_thread::get_id(), mainThread);
        if (released.wait_for(std::chrono::seconds(2)) != std::future_status::ready)
            throw std::runtime_error("Asio was blocked during preparation");
        return std::make_unique<Agent>(std::make_unique<UnusedProvider>(), selected.model);
    }, journal);
    asio::io_context io;
    std::atomic<bool> cancelled = false;
    auto prepare = asio::co_spawn(io, session.prepareAgent([&] { return cancelled.load(); }), asio::use_future);
    asio::steady_timer timer(io, std::chrono::milliseconds(10));
    timer.async_wait([&](const auto&) { cancelled = true; release.set_value(); });
    io.run();
    EXPECT_FALSE(prepare.get());
    EXPECT_FALSE(session.agentReady());
    EXPECT_EQ(session.messageCount(), 2);
    io.restart();
    auto retry = asio::co_spawn(io, session.prepareAgent({}), asio::use_future);
    io.run();
    EXPECT_TRUE(retry.get());
    EXPECT_EQ(session.activeConversation().getMessages().size(), 2);
}
