#include <agent-runtime/tools/common/rerun_approved_action.hpp>
#include <agent-runtime/tools/common/bash.hpp>
#include <agent-runtime/tools/fs/write.hpp>
#include <agent-runtime/tools/fs/read.hpp>
#include <agent-runtime/conversation/messages/message_json.hpp>
#include <daemon/session/session.hpp>
#include <daemon/permissions/permission_manager.hpp>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
#include <gtest/gtest.h>
#include <fstream>

namespace {
using nlohmann::json;
namespace fs = std::filesystem;

struct ReplayState {
    int calls = 0;
    json arguments = {{"id", "replay-session/approval-refused"}};
    std::vector<json> inputs;
};

class ReplayProvider : public Provider {
public:
    explicit ReplayProvider(std::shared_ptr<ReplayState> state) : state(std::move(state)) {}
    ProviderFormat getFormat() const override { return ProviderFormat::OPENAI_RESPONSES; }
    json generate(const json& input, const std::string&, const std::vector<ToolDefinition>& tools, const std::string&) override {
        state->inputs.push_back(input);
        EXPECT_TRUE(std::any_of(tools.begin(), tools.end(), [](const auto& tool) { return tool.id == "rerun_approved_action"; }));
        const auto id = std::to_string(++state->calls);
        const auto output = state->calls % 2
            ? json::array({{{"type", "function_call"}, {"id", "fc" + id}, {"call_id", "call" + id},
                {"name", "rerun_approved_action"}, {"arguments", state->arguments.dump()}}})
            : json::array({{{"type", "message"}, {"role", "assistant"}, {"id", "message" + id},
                {"content", json::array({{{"type", "output_text"}, {"text", "Done"}, {"annotations", json::array()}}})}}});
        return {{"id", "response" + id}, {"status", "completed"}, {"output", output}};
    }
private:
    std::shared_ptr<ReplayState> state;
};

class ApprovedReplayTest : public testing::Test {
protected:
    fs::path directory;
    SessionInfo info;
    std::shared_ptr<ReplayState> state = std::make_shared<ReplayState>();
    std::unique_ptr<PermissionManager> policy;
    int verified = 0;
    std::vector<PermissionEvent> permissions;
    json original;

    void SetUp() override {
        std::string pattern = "/tmp/sos-replay-XXXXXX";
        ASSERT_NE(mkdtemp(pattern.data()), nullptr);
        directory = fs::canonical(pattern);
        info.id = "replay-session";
        info.workspace = directory;
        info.mode = ExecutionMode::SLOP;
        original = {{"tool", "bash"}, {"arguments", {{"command", "printf exact >> result.txt"}, {"mainCommand", "printf"}, {"timeout", 1000}}},
            {"working_directory", directory.string()}, {"workspace", directory.string()}};
        ToolResultContent refused{"old-call", "Refused", true};
        refused.metadata = {{"refused_action", original}, {"refused_message_id", "refused"},
            {"permissions", json::array({{{"decision", "ai_refused"}, {"confidence", 0.02}}})}};
        SessionJournal(directory).save({
            {"system", MessageRole::SYSTEM, {TextContent{"Instructions"}}},
            {"user", MessageRole::USER, {TextContent{"Please run this"}}},
            {"old", MessageRole::ASSISTANT, {ToolCallContent{"old-call", "bash", original.at("arguments")}}},
            {"refused", MessageRole::TOOL, {refused}}}, info);
        policy = std::make_unique<PermissionManager>([&](auto event) { permissions.push_back(std::move(event)); });
        policy->configure(directory, ExecutionMode::SLOP);
        policy->setPermissionMode(PermissionMode::AUTO, [&](const auto&, const auto&) { ++verified; return 0.0; });
    }
    void TearDown() override { fs::remove_all(directory); }

    std::unique_ptr<Session> load() {
        auto store = std::make_shared<SessionJournal>(directory);
        auto session = std::make_unique<Session>(*store->info(), directory, [state = state](const auto& info) {
            ToolRegistry tools;
            tools.add(std::make_unique<BashTool>());
            tools.add(std::make_unique<WriteTool>());
            tools.add(std::make_unique<ReadTool>());
            return std::make_unique<Agent>(std::make_unique<ReplayProvider>(state), info.model,
                std::move(tools), ToolContext{info.workspace, info.workspace});
        }, store);
        session->setPermissionHandler([&](const auto& request, const auto& check) { return policy->ask(request, check); }, directory);
        return session;
    }

    ToolResultContent run(Session& session, ExecutionMode mode = ExecutionMode::SLOP) {
        std::vector<AgentEvent> events;
        session.setEventHandler([&](auto event) { events.push_back(std::move(event)); });
        session.sendMessage("It's approved, continue", mode);
        asio::io_context io;
        auto finished = asio::co_spawn(io, session.finishExecution(), asio::use_future);
        io.run();
        finished.get();
        ToolResultContent result;
        for (const auto& event : events) {
            session.record(event);
            for (const auto& message : event.messages)
                for (const auto& content : message.content)
                    if (const auto* tool = std::get_if<ToolResultContent>(&content)) result = *tool;
        }
        session.setEventHandler({});
        return result;
    }
};

TEST_F(ApprovedReplayTest, ExactActionRunsRepeatedlyWithoutJevAndSurvivesResume) {
    auto session = load();
    session->setManualApproval("refused", true);
    EXPECT_FALSE(fs::exists(directory / "result.txt"));
    EXPECT_EQ(SessionJournal(directory).recovery().approvedReplays()[0].at("id"), "approval-refused");
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto result = run(*session);
        EXPECT_FALSE(result.isError) << result.output;
        EXPECT_EQ(result.metadata.at("approved_action").at("arguments"), original.at("arguments"));
        EXPECT_EQ(result.metadata.at("approved_action").at("tool"), "bash");
        EXPECT_EQ(result.metadata.at("permissions")[0].at("decision"), "manually_approved");
        EXPECT_EQ(result.metadata.at("permissions")[0].at("confidence"), 0.02);
        ASSERT_FALSE(permissions.empty());
        EXPECT_EQ(permissions.back().confidence, 0.02);
        EXPECT_EQ(json(permissions.back()).at("confidence"), 0.02);
        EXPECT_EQ(verified, 0);
        const auto prompt = state->inputs[attempt * 2].dump();
        EXPECT_NE(prompt.find("approval-refused"), std::string::npos);
        EXPECT_NE(prompt.find("rerun_approved_action"), std::string::npos);
        EXPECT_NE(prompt.find("printf exact"), std::string::npos);
        session.reset();
        session = load();
    }
    std::ifstream file(directory / "result.txt");
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(file), {}), "exactexact");
    auto& history = session->history();
    const auto page = history.page();
    bool replayShown = false, scoreShown = false;
    for (std::size_t i = 0; i < page.count; ++i) {
        const auto record = page.record(i);
        if (record.at("role") != "activity") continue;
        const auto group = history.activity(record.at("id"));
        for (const auto& item : group.at("activities")) {
            if (item.value("callId", "") == "old-call") {
                EXPECT_EQ(item.at("result").at("metadata").at("permissions")[0].at("confidence"), 0.02);
                scoreShown = true;
            }
            if (item.value("callId", "") == "call3") {
                EXPECT_EQ(item.at("toolName"), "bash");
                EXPECT_EQ(item.at("result").at("metadata").at("permissions")[0].at("confidence"), 0.02);
                const auto detail = history.detail(record.at("id"), item.at("activityIndex"));
                EXPECT_EQ(detail.at("activities")[0].at("arguments"), original.at("arguments"));
                EXPECT_EQ(detail.at("activities")[0].at("result").at("metadata").at("permissions")[0].at("confidence"), 0.02);
                replayShown = true;
            }
        }
    }
    EXPECT_TRUE(replayShown);
    EXPECT_TRUE(scoreShown);
}

TEST_F(ApprovedReplayTest, ZeroAndMissingConfidenceSurviveReplayWithoutInventingAScore) {
    for (const auto confidence : {std::optional<double>{0.0}, std::optional<double>{}}) {
        const auto id = confidence ? "zero" : "missing";
        auto action = original;
        action["arguments"]["timeout"] = confidence ? 1001 : 1002;
        auto permission = json{{"decision", "ai_refused"}};
        if (confidence) permission["confidence"] = *confidence;
        ToolResultContent refusal{id, "Refused", true};
        refusal.metadata = {{"refused_action", action}, {"refused_message_id", id},
            {"permissions", json::array({permission})}};
        SessionJournal(directory).save({
            {std::string("call-") + id, MessageRole::ASSISTANT, {ToolCallContent{id, "bash", action.at("arguments")}}},
            {id, MessageRole::TOOL, {refusal}}}, info);
        auto session = load();
        session->setManualApproval(id, true);
        state->arguments = {{"id", std::string("replay-session/approval-") + id}};
        const auto result = run(*session);
        ASSERT_FALSE(result.isError) << result.output;
        EXPECT_EQ(permissions.back().confidence, confidence);
        for (const auto& record : result.metadata.at("permissions")) {
            EXPECT_EQ(record.at("decision"), "manually_approved");
            EXPECT_EQ(record.contains("confidence"), confidence.has_value());
            if (confidence) EXPECT_EQ(record.at("confidence"), *confidence);
        }
    }
    EXPECT_EQ(verified, 0);
}

TEST_F(ApprovedReplayTest, ReadReplayRetainsOriginalScoreInLiveEventAndResumedHistory) {
    auto action = original;
    action["tool"] = "read";
    action["arguments"] = {{"filePath", directory.string()}, {"limit", 1}};
    ToolResultContent refusal{"read-old", "Refused", true};
    refusal.metadata = {{"refused_action", action}, {"refused_message_id", "read-refused"},
        {"permissions", json::array({{{"decision", "ai_refused"}, {"confidence", 0.17}}})}};
    SessionJournal(directory).save({
        {"read-call", MessageRole::ASSISTANT, {ToolCallContent{"read-old", "read", action.at("arguments")}}},
        {"read-refused", MessageRole::TOOL, {refusal}}}, info);
    auto session = load();
    session->setManualApproval("read-refused", true);
    state->arguments = {{"id", "replay-session/approval-read-refused"}};
    const auto result = run(*session);
    ASSERT_FALSE(result.isError) << result.output;
    EXPECT_EQ(permissions.back().confidence, 0.17);
    EXPECT_EQ(result.metadata.at("permissions")[0].at("confidence"), 0.17);
    session.reset();
    session = load();
    auto& history = session->history();
    const auto page = history.page();
    bool found = false;
    for (std::size_t i = 0; i < page.count; ++i) {
        const auto record = page.record(i);
        if (record.at("role") != "activity") continue;
        const auto group = history.activity(record.at("id"));
        for (const auto& item : group.at("activities")) {
            if (item.value("callId", "") != "call1") continue;
            EXPECT_EQ(item.at("toolName"), "read");
            EXPECT_EQ(item.at("arguments").at("filePath"), directory.string());
            EXPECT_EQ(item.at("result").at("metadata").at("permissions")[0].at("confidence"), 0.17);
            found = true;
        }
    }
    EXPECT_TRUE(found);
    EXPECT_EQ(verified, 0);
}

TEST_F(ApprovedReplayTest, UnknownRevokedAndOverriddenActionsDoNotExecute) {
    auto session = load();
    EXPECT_TRUE(run(*session).isError); // An unapproved recorded refusal is not a grant.
    session->setManualApproval("refused", true);
    state->arguments["command"] = "touch wrong";
    EXPECT_TRUE(run(*session).isError);
    state->arguments = {{"id", "approval-unknown"}};
    EXPECT_TRUE(run(*session).isError);
    state->arguments = {{"id", "another-session/approval-refused"}};
    EXPECT_TRUE(run(*session).isError);
    state->arguments = {{"id", "replay-session/approval-refused"}};
    session->setManualApproval("refused", false);
    EXPECT_THROW(SessionJournal(directory).recovery().approvedReplay("approval-refused"), std::invalid_argument);
    EXPECT_TRUE(run(*session).isError);
    EXPECT_FALSE(fs::exists(directory / "result.txt"));
    EXPECT_FALSE(fs::exists(directory / "wrong"));
    EXPECT_EQ(verified, 0);
}

TEST_F(ApprovedReplayTest, SaneStillRefusesAnApprovedModifyingCommand) {
    auto session = load();
    session->setManualApproval("refused", true);
    policy->setMode(ExecutionMode::SANE);
    const auto result = run(*session, ExecutionMode::SANE);
    EXPECT_TRUE(result.isError);
    EXPECT_NE(result.output.get<std::string>().find("Forbidden in Sane mode"), std::string::npos);
    EXPECT_FALSE(fs::exists(directory / "result.txt"));
    EXPECT_EQ(verified, 0);
}

TEST_F(ApprovedReplayTest, ReplayChecksContextCancellationAndRevocationAtEveryPermissionBoundary) {
    ToolRegistry tools;
    tools.add(std::make_unique<WriteTool>());
    RerunApprovedActionTool replay(tools);
    ApprovedToolAction action{"id", "write", {{"filePath", "file"}, {"content", "exact"}}, directory, directory};
    ToolContext context{directory, directory};
    int resolutions = 0;
    context.approvedAction = [&](const auto&) {
        if (++resolutions == 3) throw std::runtime_error("revoked");
        return action;
    };
    context.ask = [&](const auto& request) {
        EXPECT_TRUE(request.manuallyApproved);
        EXPECT_EQ(request.toolName, "write");
        EXPECT_EQ(request.metadata.at("arguments"), action.arguments);
    };
    EXPECT_THROW(replay.execute({{"id", "id"}}, context), std::runtime_error);
    EXPECT_FALSE(fs::exists(directory / "file"));
    action.workspace = "/";
    EXPECT_THROW(replay.execute({{"id", "id"}}, context), std::runtime_error);
    action.workspace = directory;
    action.directory = "/";
    EXPECT_THROW(replay.execute({{"id", "id"}}, context), std::runtime_error);
    context.checkCancelled = [] { throw core::Cancelled(); };
    EXPECT_THROW(replay.execute({{"id", "id"}}, context), core::Cancelled);
}
}
