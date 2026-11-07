#include "model.hpp"
#include "markdown.hpp"
#include <gtest/gtest.h>
#include <ftxui/screen/screen.hpp>
using namespace sos::tui;
namespace {
Json header(int count = 1) {
    return {{"requestId", "r"}, {"sessionId", "s"}, {"session", {{"id", "s"}}}, {"messageCount", count}};
}
Json chunk(std::string value, bool last = true, int part = 0) {
    return {{"type", "session.history_chunk"}, {"requestId", "r"}, {"sessionId", "s"}, {"messageIndex", 0}, {"partIndex", part}, {"last", last}, {"data", value}};
}
Json done(int count = 1) {
    return {{"type", "session.attached"}, {"requestId", "r"}, {"sessionId", "s"}, {"messageCount", count}};
}
}
TEST(FtxuiTransfer, ReassemblesUtf8AndValidatesCount) {
    Transfer transfer(header());
    EXPECT_FALSE(transfer.accept(chunk("{\"role\":\"user\",\"content\":[", false)));
    EXPECT_FALSE(transfer.accept(chunk("{\"type\":\"text\",\"text\":\"été\"}]}", true, 1)));
    EXPECT_TRUE(transfer.accept(done()));
    EXPECT_EQ(transfer.messages[0]["content"][0]["text"], "été");
}
TEST(FtxuiTransfer, RejectsTruncationOrderAndCorrelation) {
    Transfer incomplete(header());
    EXPECT_THROW(incomplete.accept(done()), std::runtime_error);
    EXPECT_THROW(incomplete.accept(chunk("{}", true, 1)), std::runtime_error);
    auto wrong = chunk("{}"); wrong["sessionId"] = "other";
    EXPECT_THROW(incomplete.accept(wrong), std::runtime_error);
    EXPECT_THROW(Transfer(header(-1)), std::runtime_error);
}
TEST(FtxuiTransfer, EmptyHistory) {
    Transfer transfer(header(0)); EXPECT_TRUE(transfer.accept(done(0)));
}
TEST(FtxuiModel, RejectionRestoresDraftWithoutOverwritingNewText) {
    Model model; model.begin("original", "work-1"); model.draft = "new draft";
    model.apply({{"type", "request.failed"}, {"requestId", "work-1"}, {"error", "No credentials"}});
    EXPECT_EQ(model.draft, "original\nnew draft"); EXPECT_FALSE(model.busy); EXPECT_TRUE(model.entries.empty());
}
TEST(FtxuiModel, EchoConfirmsOptimisticMessage) {
    Model model; model.begin("hello", "work-1");
    Json message = {{"id", "m"}, {"role", "user"}, {"content", Json::array({{{"type", "text"}, {"text", "hello"}}})}};
    model.apply({{"type", "harness.message_arrived"}, {"messages", Json::array({message})}});
    ASSERT_EQ(model.entries.size(), 2); EXPECT_EQ(model.entries[0].id, "m");
    EXPECT_EQ(model.entries[1].id, "m/activity/0"); EXPECT_EQ(model.entries[1].kind, Entry::Kind::Activity);
    model.apply({{"type", "execution.failed"}, {"error", "Provider failed"}});
    EXPECT_TRUE(model.draft.empty()); EXPECT_FALSE(model.busy);
}
TEST(FtxuiModel, AutomaticCompactionDoesNotReleaseQueue) {
    Model model; model.busy = true; model.queued = "next";
    model.apply({{"type", "compaction.completed"}, {"automatic", true}});
    EXPECT_TRUE(model.busy); EXPECT_EQ(model.queued, "next");
    model.apply({{"type", "execution.cancelled"}});
    EXPECT_FALSE(model.busy); EXPECT_EQ(model.draft, "next"); EXPECT_TRUE(model.queued.empty());
}
TEST(FtxuiModel, PageSwitchIsAtomic) {
    Model model; model.entries.push_back(Entry{.id = "old"});
    auto start = header(); start["type"] = "session.attach_started";
    model.apply(start); ASSERT_EQ(model.entries[0].id, "old");
    model.apply(chunk("{\"id\":\"new\",\"role\":\"user\",\"content\":[]}"));
    ASSERT_EQ(model.entries[0].id, "old"); model.apply(done());
    ASSERT_EQ(model.entries.size(), 1); EXPECT_EQ(model.entries[0].id, "new");
}
TEST(FtxuiModel, DisconnectRestoresUnacceptedAndQueuedDrafts) {
    Model model; model.begin("first", "work-1"); model.queued = "second"; model.draft = "third";
    model.apply({{"type", "client.disconnected"}});
    EXPECT_FALSE(model.connected); EXPECT_FALSE(model.busy);
    EXPECT_NE(model.draft.find("first"), std::string::npos);
    EXPECT_NE(model.draft.find("second"), std::string::npos);
    EXPECT_NE(model.draft.find("third"), std::string::npos);
}
TEST(FtxuiTransfer, AcceptsNullableActivityIndex) {
    auto start = header(0); start["activityIndex"] = nullptr;
    Transfer transfer(start); EXPECT_EQ(transfer.activityIndex, -1);
    EXPECT_TRUE(transfer.accept(done(0)));
}
TEST(FtxuiModel, DoesNotDuplicateAssistantMessages) {
    Model model;
    Json message = {{"id", "assistant"}, {"role", "assistant"}, {"content", Json::array({{{"type", "text"}, {"text", "Answer"}}})}};
    model.append(message); model.append(message);
    ASSERT_EQ(model.entries.size(), 1);
}
TEST(FtxuiModel, ActivityAndDetailRemainSeparateLazyLoads) {
    Model model;
    model.append({{"id", "group"}, {"role", "activity"}, {"count", 1}, {"content", Json::array()}});
    ASSERT_TRUE(model.entries[0].folded); EXPECT_FALSE(model.entries[0].loaded);
    auto start = header(); start["type"] = "session.attach_started"; start["purpose"] = "activities"; start["groupId"] = "group";
    Json row = {{"kind", "tool"}, {"toolName", "bash"}, {"callId", "call"}, {"activityIndex", 0}};
    Json activity = {{"id", "group"}, {"role", "activity"}, {"content", Json::array()}, {"activities", Json::array({row})}};
    model.apply(start); model.apply(chunk(activity.dump())); model.apply(done());
    ASSERT_EQ(model.entries.size(), 2); EXPECT_TRUE(model.entries[0].loaded); EXPECT_FALSE(model.entries[1].loaded);
    start["purpose"] = "activity-detail"; start["activityIndex"] = 0;
    row["arguments"] = {{"command", "printf hello"}};
    row["result"] = {{"output", "hello"}, {"metadata", {{"refused_message_id", "refused"}, {"manually_approved", true}}}};
    activity["activities"] = Json::array({row});
    model.apply(start); model.apply(chunk(activity.dump())); model.apply(done());
    ASSERT_EQ(model.entries.size(), 2); EXPECT_TRUE(model.entries[1].loaded); EXPECT_TRUE(model.entries[1].approved);
    EXPECT_NE(model.entries[1].body.find("printf hello"), std::string::npos);
}
TEST(FtxuiModel, ImageOnlyDraftIsRestoredOnRejection) {
    Model model; model.begin("", "work-image");
    model.provisionalImages.push_back(Image{.id = "captured", .data = "pixels"});
    model.apply({{"type", "request.failed"}, {"requestId", "work-image"}, {"error", "Unsupported images"}});
    ASSERT_EQ(model.images.size(), 1); EXPECT_EQ(model.images[0].data, "pixels"); EXPECT_TRUE(model.provisionalImages.empty());
}
TEST(FtxuiAttachments, CanonicalBase64AndPathDetection) {
    EXPECT_EQ(base64(""), ""); EXPECT_EQ(base64("f"), "Zg=="); EXPECT_EQ(base64("fo"), "Zm8="); EXPECT_EQ(base64("foo"), "Zm9v");
    EXPECT_FALSE(imagePath("Explain the image in /tmp/example.png"));
}
TEST(FtxuiMarkdown, RendersFormattingWithoutMarkupAndPreservesLiteralCode) {
    using namespace ftxui;
    auto element = richText("# Title\n**bold** and `inline`\n```cpp\nint x = 1;\n```");
    auto screen = Screen::Create(Dimension::Fixed(80), Dimension::Fit(element));
    Render(screen, element);
    auto output = screen.ToString();
    EXPECT_EQ(output.find("**"), std::string::npos); EXPECT_EQ(output.find("```"), std::string::npos);
    EXPECT_NE(output.find("Title"), std::string::npos); EXPECT_NE(output.find("int"), std::string::npos);
}
TEST(FtxuiModel, BtwCompletionDoesNotEndPrincipalAndSurvivesPaging) {
    Model model; model.sessionId = "s"; model.busy = true;
    model.apply({{"type", "btw.accepted"}, {"branchId", "btw-1"}});
    auto start = header(); start["type"] = "btw.started"; start["branch"] = {{"id", "btw-1"}, {"anchor", "question"}, {"status", "completed"}};
    model.apply(start);
    auto part = chunk("{\"id\":\"answer\",\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"Side answer\"}]}");
    part["type"] = "btw.chunk"; model.apply(part);
    auto finish = done(); finish["type"] = "btw.finished"; model.apply(finish);
    EXPECT_TRUE(model.busy); ASSERT_EQ(model.branches.size(), 1); EXPECT_FALSE(model.branches.at("btw-1").entry.folded);
    start = header(0); start["type"] = "session.attach_started"; model.apply(start); model.apply(done(0));
    ASSERT_EQ(model.entries.size(), 1); EXPECT_EQ(model.entries[0].kind, Entry::Kind::Btw);
}
TEST(FtxuiModel, NewTurnPurgesDetailsButKeepsBackendActivityIds) {
    Model model;
    model.append({{"id", "u"}, {"role", "user"}, {"content", Json::array()}});
    model.append({{"id", "a"}, {"role", "assistant"}, {"content", Json::array({{{"type", "tool_call"}, {"callId", "call"}, {"toolName", "bash"}, {"arguments", {{"command", "ls"}}}}})}});
    ASSERT_EQ(model.entries.size(), 3);
    EXPECT_EQ(model.entries[2].group, "u/activity/0"); EXPECT_EQ(model.entries[2].activityIndex, 0);
    model.begin("next", "work-next");
    ASSERT_EQ(model.entries.size(), 3); EXPECT_EQ(model.entries[1].id, "u/activity/0");
    EXPECT_FALSE(model.entries[1].loaded); EXPECT_TRUE(model.entries[1].folded);
}
TEST(FtxuiModel, ProjectedMixedMessageDoesNotLoseItsSecondTextPart) {
    Model model;
    auto message = Json{{"id", "mixed"}, {"role", "assistant"}, {"content", Json::array({{{"type", "text"}, {"text", "before"}}})}};
    model.append(message, true);
    message["content"][0]["text"] = "after"; model.append(message, true);
    ASSERT_EQ(model.entries.size(), 2);
    EXPECT_EQ(model.entries[0].body, "before"); EXPECT_EQ(model.entries[1].body, "after");
    EXPECT_NE(model.entries[0].id, model.entries[1].id);
}
TEST(FtxuiMarkdown, CommonMarkReferencesEntitiesTablesAndNestedEmphasis) {
    using namespace ftxui;
    auto element = richText("***nested*** &amp; [reference][id]\n\n[id]: https://example.org\n\n| Name | Value |\n| --- | --- |\n| alpha | beta |\n\n- [x] complete\n- [ ] pending\n");
    auto screen = Screen::Create(Dimension::Fixed(80), Dimension::Fit(element)); Render(screen, element);
    const auto output = screen.ToString();
    EXPECT_EQ(output.find("***"), std::string::npos); EXPECT_EQ(output.find("&amp;"), std::string::npos);
    EXPECT_EQ(output.find("[reference]"), std::string::npos);
    for (const auto* word : {"nested", "reference", "alpha", "beta", "complete", "pending"}) EXPECT_NE(output.find(word), std::string::npos);
}
