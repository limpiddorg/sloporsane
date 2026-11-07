#include <daemon/session/session_history_view.hpp>
#include <agent-runtime/common/path_resolution.hpp>
#include <gtest/gtest.h>

namespace {
Message text(int index) {
    return {"m" + std::to_string(index), index % 2 ? MessageRole::ASSISTANT : MessageRole::USER,
        {TextContent{"text " + std::to_string(index)}}};
}
void tools(std::vector<Message>& history, int turn, int count) {
    for (int i = 0; i < count; ++i) {
        const auto id = std::to_string(turn) + "-" + std::to_string(i);
        history.push_back({"call-" + id, MessageRole::ASSISTANT,
            {ToolCallContent{id, "bash", nlohmann::json{{"command", "true"}}}}});
        history.push_back({"result-" + id, MessageRole::TOOL,
            {ToolResultContent{id, "ok", false}}});
    }
}
}

TEST(SessionHistoryViewTest, PagesVisibleMessagesInBothDirectionsWithStableAnchors) {
    std::vector<Message> history;
    for (int i = 0; i < 130; ++i) history.push_back(text(i));
    SessionHistoryView view;
    view.update(history);
    const auto size = SessionHistoryView::PAGE_SIZE;
    const auto step = SessionHistoryView::PAGE_STEP;
    const auto id = [](std::size_t index) { return "m" + std::to_string(index); };
    const auto latest = view.page();
    EXPECT_EQ(latest.first, id(130 - size));
    EXPECT_EQ(latest.last, "m129");
    EXPECT_TRUE(latest.hasOlder);
    EXPECT_FALSE(latest.hasNewer);
    const auto previous = view.page("before", latest.first);
    EXPECT_EQ(previous.first, id(130 - size - step));
    EXPECT_EQ(previous.last, id(129 - step));
    EXPECT_TRUE(previous.loadedGroups.empty());
    auto first = previous;
    while (first.hasOlder) first = view.page("before", first.first);
    EXPECT_EQ(first.first, "m0");
    EXPECT_EQ(first.last, id(size - 1));
    EXPECT_FALSE(first.hasOlder);
    const auto back = view.page("after", previous.last);
    EXPECT_EQ(back.first, latest.first);
    EXPECT_EQ(back.last, latest.last);
    EXPECT_EQ(back.loadedGroups, latest.loadedGroups);
    history.push_back(text(130));
    view.update(history);
    EXPECT_EQ(view.page("before", latest.first).first, id(130 - size - step));
    EXPECT_EQ(view.page("latest", "", true).first, id(131 - size));
    EXPECT_EQ(view.page("latest", "", true).last, "m130");
    EXPECT_EQ(view.page("after", first.last).first, id(step));
    EXPECT_EQ(view.page("after", first.last).last, id(step + size - 1));
    EXPECT_THROW(view.page("before", "unknown"), std::invalid_argument);
}

TEST(SessionHistoryViewTest, OnlyLastGroupIsPreloadedAndOlderGroupsRemainAvailableOnDemand) {
    std::vector<Message> history;
    history.push_back(text(0)); tools(history, 0, 1); history.push_back(text(1));
    history.push_back(text(2)); tools(history, 1, 5); history.push_back(text(3));
    history.push_back(text(4)); tools(history, 2, 2); history.push_back(text(5));
    SessionHistoryView view;
    view.update(history);
    const auto page = view.page();
    std::size_t loaded = 0;
    for (const auto index : page.records) {
        const auto record = view.record(history, index, page.loadedGroups.contains(index));
        if (record.contains("activities")) loaded += record["activities"].size();
    }
    EXPECT_EQ(loaded, 2);
    EXPECT_EQ(page.loadedGroups.size(), 1);
    const auto old = view.activities(history, "m2/activity/0");
    EXPECT_EQ(old["activities"].size(), 5);
    EXPECT_EQ(old["activities"][0]["toolName"], "bash");
    EXPECT_FALSE(old["activities"][0]["result"].contains("output"));
    EXPECT_EQ(view.activityDetail(history, "m2/activity/0", 0)["activities"][0]["result"]["output"], "ok");
    EXPECT_THROW(view.activities(history, "missing"), std::invalid_argument);
}

TEST(SessionHistoryViewTest, SearchPatternsAreAvailableBeforeLoadingDetails) {
    std::vector<Message> history{text(0)};
    history.push_back({"search", MessageRole::ASSISTANT,
        {ToolCallContent{"grep-call", "grep", nlohmann::json{{"path", "src"}, {"pattern", "TODO|FIXME"}}},
         ToolCallContent{"glob-call", "glob", nlohmann::json{{"path", "src"}, {"pattern", "**/*.cpp"}}}}});
    SessionHistoryView view;
    view.update(history);
    const auto items = view.activities(history, "m0/activity/0")["activities"];
    ASSERT_EQ(items.size(), 2);
    EXPECT_EQ(items[0]["arguments"], nlohmann::json({{"pattern", "TODO|FIXME"}}));
    EXPECT_EQ(items[1]["arguments"], nlohmann::json({{"pattern", "**/*.cpp"}}));
}

TEST(SessionHistoryViewTest, LastGroupIsPreloadedWholeEvenAboveTheFormerActivityBudget) {
    std::vector<Message> history{text(0)};
    tools(history, 0, 51);
    SessionHistoryView view;
    view.update(history);
    const auto page = view.page();
    ASSERT_EQ(page.loadedGroups.size(), 1);
    EXPECT_EQ(view.record(history, *page.loadedGroups.begin(), true)["activities"].size(), 51);
    EXPECT_EQ(view.activities(history, "m0/activity/0")["activities"].size(), 51);
    EXPECT_EQ(view.page().first, "m0");
    EXPECT_EQ(view.page().last, "m0");
    history.push_back(text(2)); // A new, still-empty current group replaces the preloaded one.
    view.update(history);
    const auto next = view.page();
    ASSERT_EQ(next.loadedGroups.size(), 1);
    const auto current = view.record(history, *next.loadedGroups.begin(), true);
    EXPECT_EQ(current["id"], "m2/activity/0");
    EXPECT_TRUE(current["activities"].empty());
    EXPECT_EQ(view.activities(history, "m0/activity/0")["activities"].size(), 51);
}

TEST(SessionHistoryViewTest, IncrementalResultsAndMixedMessagesDoNotDuplicateActivities) {
    std::vector<Message> history{text(0)};
    history.push_back({"mixed", MessageRole::ASSISTANT,
        {ReasoningContent{std::string("thinking")}, ToolCallContent{"c", "read", nlohmann::json{{"filePath", "/tmp/f"}}}, TextContent{"answer"}}});
    SessionHistoryView view;
    view.update(history);
    EXPECT_EQ(view.page().last, "mixed");
    auto group = view.activities(history, "m0/activity/0");
    EXPECT_EQ(group["count"], 2);
    EXPECT_FALSE(group["activities"][1].contains("result"));
    history.push_back({"result", MessageRole::TOOL, {ToolResultContent{"c", "contents", false}}});
    view.update(history);
    view.update(history);
    group = view.activities(history, "m0/activity/0");
    EXPECT_EQ(group["count"], 2);
    EXPECT_FALSE(group["activities"][1]["result"].contains("output"));
    EXPECT_EQ(view.activityDetail(history, "m0/activity/0", 1)["activities"][0]["result"]["output"], "contents");
    const auto page = view.page();
    const auto message = view.record(history, page.records.back(), false);
    EXPECT_EQ(message["content"].size(), 1);
    EXPECT_FALSE(message.dump().contains("providerData"));
}

TEST(SessionHistoryViewTest, EmptyAndTechnicalOnlyHistoriesAreSupported) {
    SessionHistoryView view;
    std::vector<Message> history;
    view.update(history);
    EXPECT_TRUE(view.page().records.empty());
    tools(history, 0, 1);
    view.update(history);
    EXPECT_EQ(view.page().records.size(), 1);
    EXPECT_TRUE(view.page().first.empty());
}

TEST(SessionHistoryViewTest, SkillScriptClassificationIsConservativeAndDisplayOnly) {
    const auto global = PathResolution::skillsDirectory() / "demo/scripts/run.py";
    EXPECT_EQ(SessionHistoryView::skillScriptName("bash", {{"command", "python3 '" + global.string() + "'"}}), "demo");
    EXPECT_FALSE(SessionHistoryView::skillScriptName("bash", {{"command", "echo '" + global.string() + "'"}}));
    for (const auto* command : {".sos/skills/demo/scripts/run.sh", "./.sos/skills/demo/scripts/run.sh hello",
        "python3 /workspace/.sos/skills/demo/scripts/nested/run.py --text=hello",
        "python .sos/skills/demo/scripts/run.py", "/bin/bash .sos/skills/demo/scripts/run.sh",
        "sh '/my workspace/.sos/skills/demo/scripts/run.sh'"}) {
        EXPECT_EQ(SessionHistoryView::skillScriptName("bash", {{"command", command}}), "demo") << command;
    }
    for (const auto* command : {"", "python3", "echo .sos/skills/demo/scripts/run.py",
        "cat .sos/skills/demo/scripts/run.py", "python3 -c '.sos/skills/demo/scripts/run.py'",
        "python3 .sos/skills/demo/scripts/run.py && echo done", "python3 .sos/skills/demo/scripts/run.py > out",
        "python3 .sos/skills/demo/scripts/../run.py", "python3 '.sos/skills/demo/scripts/run.py",
        "python3 skills/demo/scripts/run.py", "python3 .sos/skills/demo/SKILL.md"}) {
        EXPECT_FALSE(SessionHistoryView::skillScriptName("bash", {{"command", command}})) << command;
    }
    EXPECT_FALSE(SessionHistoryView::skillScriptName("read", {{"command", ".sos/skills/demo/scripts/run.sh"}}));
}

TEST(SessionHistoryViewTest, SkillScriptPathsAreRelativeToScriptsAndKeepSubdirectories) {
    for (const auto& root : {std::string(".sos/skills/demo/scripts/"),
        std::string("/my workspace/.sos/skills/demo/scripts/"),
        (PathResolution::skillsDirectory() / "demo/scripts").generic_string() + "/"}) {
        for (const auto* relative : {"run.py", "nested/my script.py"}) {
            const auto script = SessionHistoryView::skillScript("bash", {{"command", "python3 '" + root + relative + "' --text=hello"}});
            ASSERT_TRUE(script);
            EXPECT_EQ(script->name, "demo");
            EXPECT_EQ(script->path, relative);
        }
    }
}

TEST(SessionHistoryViewTest, ActivityRowsExcludePayloadsAndDetailsReturnOnlyTheRequestedItem) {
    using nlohmann::json;
    const std::string payload = "PRIVATE_PAYLOAD_" + std::string(100000, 'x');
    std::vector<Message> history{text(0)};
    history.push_back({"calls", MessageRole::ASSISTANT, {
        ReasoningContent{payload},
        ToolCallContent{"read", "read", json{{"filePath", "/tmp/file"}, {"unused", payload}}},
        ToolCallContent{"write", "write", json{{"filePath", "/tmp/file"}, {"content", payload}}},
        ToolCallContent{"bash", "bash", json{{"command", payload}}}}});
    for (const auto* call : {"read", "write", "bash"}) {
        ToolResultContent result{call, payload, false};
        result.metadata = {{"exit", 1}, {"diff", payload}, {"display", {{"text", payload}}},
            {"permissions", json::array({{{"id", "p"}, {"decision", "yes"}, {"unused", payload}}})}};
        result.attachments.push_back({"image/png", "data:image/png;base64," + payload});
        history.push_back({std::string("result-") + call, MessageRole::TOOL, {result}});
    }
    SessionHistoryView view;
    view.update(history);
    const auto rows = view.activities(history, "m0/activity/0");
    EXPECT_LT(rows.dump().size(), 2000);
    EXPECT_EQ(rows.dump().find("PRIVATE_PAYLOAD"), std::string::npos);
    EXPECT_FALSE(rows["activities"][0].contains("text"));
    EXPECT_EQ(rows["activities"][1]["arguments"], json({{"filePath", "/tmp/file"}}));
    EXPECT_EQ(rows["activities"][2]["arguments"], json({{"filePath", "/tmp/file"}}));
    EXPECT_EQ(rows["activities"][3]["result"]["metadata"]["exit"], 1);
    EXPECT_EQ(rows["activities"][1]["result"]["metadata"]["permissions"][0].size(), 2);
    const auto detail = view.activityDetail(history, "m0/activity/0", 2);
    ASSERT_EQ(detail["activities"].size(), 1);
    EXPECT_EQ(detail["activities"][0]["arguments"]["content"], payload);
    EXPECT_EQ(detail["activities"][0]["result"]["output"], payload);
    EXPECT_EQ(detail["activities"][0]["result"]["metadata"]["diff"], payload);
    EXPECT_EQ(detail["activities"][0]["result"]["attachments"].size(), 1);
    EXPECT_EQ(view.activityDetail(history, "m0/activity/0", 0)["activities"][0]["text"], payload);
    EXPECT_THROW(view.activityDetail(history, "m0/activity/0", 4), std::invalid_argument);
    EXPECT_THROW(view.activityDetail(history, "missing", 0), std::invalid_argument);
    history.push_back({"bad-read", MessageRole::ASSISTANT,
        {ToolCallContent{"bad", "read", json{{"filePath", "/missing"}}}}});
    history.push_back({"bad-result", MessageRole::TOOL, {ToolResultContent{"bad", "File not found", true}}});
    view.update(history);
    EXPECT_EQ(view.activities(history, "m0/activity/0")["activities"][4]["result"]["output"], "File not found");
}
