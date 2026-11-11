#include <daemon/session/session_registry.hpp>
#include <daemon/session/history_chunks.hpp>
#include <daemon/session/conversation_recap.hpp>
#include <daemon/session/implementation_changes.hpp>
#include <daemon/session/implementation_worktree.hpp>
#include <daemon/session/agent_worktree.hpp>
#include <agent-runtime/tools/fs/common.hpp>
#include <daemon/integration/integration.hpp>
#include <daemon/transport/server_socket.hpp>
#include <daemon/transport/await_future.hpp>
#include <agent-runtime/conversation/messages/message_json.hpp>
#include <agent-runtime/tools/fs/write.hpp>
#include <agent-runtime/tools/fs/read.hpp>
#include <agent-runtime/tools/common/bash.hpp>
#include <agent-runtime/core/process.hpp>
#include <asio/co_spawn.hpp>
#include <asio/post.hpp>
#include <asio/use_future.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <fstream>
#include <cstdlib>
#include <thread>
#include <set>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>

namespace {
using nlohmann::json;
namespace fs = std::filesystem;

// Assemble chunked history into the reply shape used by session assertions.
asio::awaitable<std::optional<json>> receiveReply(ConnectionSocket& client) {
    auto first = co_await client.receive();
    if (!first || first->at("type") != "session.attach_started") co_return first;
    json messages = json::array();
    std::string serialized;
    std::size_t part = 0;
    for (;;) {
        auto frame = co_await client.receive();
        if (!frame) throw std::runtime_error("Incomplete history transfer");
        if (frame->at("type") == "integration.ping") {
            co_await client.send({{"type", "integration.pong"}, {"pingId", frame->at("pingId")}});
            continue;
        }
        EXPECT_EQ(frame->at("requestId"), first->at("requestId"));
        EXPECT_EQ(frame->at("sessionId"), first->at("sessionId"));
        if (frame->at("type") == "session.attached") {
            EXPECT_TRUE(serialized.empty());
            EXPECT_EQ(messages.size(), first->at("messageCount"));
            EXPECT_EQ(messages.size(), frame->at("messageCount"));
            (*frame)["session"] = first->at("session");
            (*frame)["page"] = first->at("page");
            (*frame)["groupId"] = first->at("groupId");
            (*frame)["purpose"] = first->at("purpose");
            (*frame)["activityIndex"] = first->value("activityIndex", json(nullptr));
            (*frame)["messages"] = std::move(messages);
            co_return frame;
        }
        EXPECT_EQ(frame->at("type"), "session.history_chunk");
        EXPECT_LE(frame->dump().size() + 1, HistoryChunks::MAX_FRAME_BYTES);
        EXPECT_EQ(frame->at("messageIndex"), messages.size());
        EXPECT_EQ(frame->at("partIndex"), part++);
        serialized += frame->at("data").get<std::string>();
        if (frame->at("last").get<bool>()) {
            messages.push_back(json::parse(serialized));
            serialized.clear();
            part = 0;
        }
    }
}

json reply(const std::string& id, const std::string& text) {
    return {{"id", id}, {"status", "completed"}, {"output", json::array({{
        {"type", "message"}, {"id", "msg_" + id}, {"role", "assistant"}, {"phase", "final_answer"},
        {"content", json::array({{{"type", "output_text"}, {"text", text}, {"annotations", json::array()}}})}
    }})}};
}

json implementationPlan() {
    return {{"steps", json::array({{{"title", "Testable foundation"}, {"objective", "Expose a usable status model"},
        {"substeps", json::array({{{"title", "Model"}, {"intention", "Represent status transitions"},
            {"changes", "Define the state model and transition rules"}},
            {{"title", "Exercise"}, {"intention", "Make transitions observable"}, {"changes", "Add focused transition tests"}}})},
        {"validation", {"Run the status-model tests"}}},
        {{"title", "Application wiring"}, {"objective", "Present the model in the application"},
        {"substeps", json::array({{{"title", "Connect"}, {"intention", "Expose the tested behavior"},
            {"changes", "Wire the model into the existing status view"}}})}, {"validation", {"Exercise the status view"}}}})}};
}

json submitImplementation(const json& plan, std::size_t call) {
    return {{"id", "plan-call-" + std::to_string(call)}, {"status", "completed"}, {"output", json::array({{
        {"type", "function_call"}, {"id", "item-" + std::to_string(call)}, {"call_id", "call-" + std::to_string(call)},
        {"name", "submit_implementation_plan"}, {"arguments", plan.dump()}}})}};
}

struct ProviderState {
    std::string sessionId;
    ImageSupport imageSupport = ImageSupport::UNKNOWN;
    std::uint64_t contextLimit = 4096;
    std::uint64_t usedContext = 120;
    std::function<json(const json&, std::size_t)> respond;
    bool fail = false;
    bool waitForCancellation = false;
    bool waitForSummaryCancellation = false;
    std::string responseText = "Hello";
    std::string model;
    std::string reasoning;
    std::size_t toolCount = 0;
    std::vector<std::string> toolNames;
    std::shared_future<void> release;
    std::promise<void> entered;
    std::vector<json> inputs;
};

class TestProvider : public Provider {
public:
    explicit TestProvider(std::shared_ptr<ProviderState> state) : state(std::move(state)) {}
    bool cancellationRequested() const { return cancellationCheck && cancellationCheck(); }
    void setSessionId(const std::string& id) override { state->sessionId = id; }
    ProviderFormat getFormat() const override { return ProviderFormat::OPENAI_RESPONSES; }
    std::optional<std::uint64_t> getContextLimit(const std::string&) override { return state->contextLimit; }
    ModelInfo getModelInfo(const std::string& model) override {
        return {model, state->contextLimit, {}, {}, {}, state->imageSupport};
    }
    std::optional<std::uint64_t> getUsedContext(const json& response) const override { return response.value("test_used", state->usedContext); }
    json generate(const json& input, const std::string& model, const std::vector<ToolDefinition>& tools,
        const std::string& reasoningEffort) override {
        state->inputs.push_back(input);
        state->model = model;
        state->reasoning = reasoningEffort;
        state->toolCount = tools.size();
        state->toolNames.clear();
        for (const auto& tool : tools) state->toolNames.push_back(tool.id);
        if (state->waitForCancellation || (state->waitForSummaryCancellation &&
            input.dump().find("You summarize a coding conversation") != std::string::npos)) {
            state->entered.set_value();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while ((!cancellationCheck || !cancellationCheck()) && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            core::checkCancellation(cancellationCheck);
            throw std::runtime_error("Test cancellation timeout");
        }
        if (state->release.valid() && state->inputs.size() == 1) { state->entered.set_value(); state->release.wait(); }
        if (state->fail) throw std::runtime_error("Provider failed");
        if (state->respond) return state->respond(input, state->inputs.size());
        return reply("response_" + std::to_string(state->inputs.size()), state->responseText);
    }
private:
    std::shared_ptr<ProviderState> state;
};

class SessionTest : public testing::Test {
protected:
    fs::path directory;
    std::shared_ptr<ProviderState> provider = std::make_shared<ProviderState>();
    int constructed = 0;
    fs::path implementationData, configurationDirectory;
    std::optional<std::string> previousData, previousConfig;
    void exerciseReviewRework(const std::string& pace, bool revisePlan = false, bool activeSuccessor = false);
    void prepareImplementationRepository() {
        if (const auto* value = std::getenv("XDG_DATA_HOME")) previousData = value;
        implementationData = fs::current_path() / "build-ai" / ("wire-worktree-" + directory.filename().string());
        fs::create_directories(implementationData);
        setenv("XDG_DATA_HOME", implementationData.c_str(), 1);
        std::ofstream(directory / ".gitignore") << "sessions/\n*.sock\n";
        for (const auto& args : std::vector<std::vector<std::string>>{{"init", "--template="},
            {"-c", "user.name=Test", "-c", "user.email=test@localhost", "add", "."},
            {"-c", "user.name=Test", "-c", "user.email=test@localhost", "-c", "commit.gpgSign=false", "commit", "-m", "Base"}}) {
            const auto result = core::runProcess("git", args, directory);
            ASSERT_EQ(result.exitCode, 0) << result.error;
        }
    }

    void SetUp() override {
        std::string pattern = "/tmp/sos-session-XXXXXX";
        ASSERT_NE(mkdtemp(pattern.data()), nullptr);
        directory = pattern;
        if (const auto* value = std::getenv("XDG_CONFIG_HOME")) previousConfig = value;
        configurationDirectory = directory.string() + "-config";
        fs::create_directories(configurationDirectory);
        setenv("XDG_CONFIG_HOME", configurationDirectory.c_str(), 1);
    }
    void TearDown() override {
        std::error_code error; fs::remove_all(directory, error);
        if (previousConfig) setenv("XDG_CONFIG_HOME", previousConfig->c_str(), 1); else unsetenv("XDG_CONFIG_HOME");
        fs::remove_all(configurationDirectory, error);
        if (!implementationData.empty()) {
            if (previousData) setenv("XDG_DATA_HOME", previousData->c_str(), 1); else unsetenv("XDG_DATA_HOME");
            fs::remove_all(implementationData, error);
        }
    }
    SessionAgentFactory factory() {
        return [this](const SessionInfo& info) {
            ++constructed;
            return std::make_unique<Agent>(std::make_unique<TestProvider>(provider), info.model,
                ToolRegistry{}, ToolContext{}, info.reasoning);
        };
    }
    SessionInfo info() { SessionInfo info; info.workspace = directory; info.title = "Test session"; return info; }

    Message navigate(Session& session, bool redo) {
        asio::io_context io;
        auto result = asio::co_spawn(io, session.navigate(redo), asio::use_future);
        io.run();
        return result.get();
    }

    void turn(Session& session, const std::string& text, std::vector<AgentEventType>& types,
        ExecutionMode mode = ExecutionMode::SANE, std::vector<Image> images = {}, bool saneRequest = false, bool implementationRequest = false,
        ImplementationPhase phase = ImplementationPhase::PLANNING) {
        asio::io_context io;
        auto queue = std::make_shared<AsyncQueue<AgentEvent>>(io.get_executor());
        session.setEventHandler([queue, executor = io.get_executor()](AgentEvent event) {
            asio::post(executor, [queue, event = std::move(event)]() mutable { queue->push(std::move(event)); });
        });
        auto completed = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
            for (;;) {
                auto event = co_await queue->receive();
                types.push_back(event.type);
                session.record(event);
                if (event.isTerminal()) {
                    co_await session.finishExecution();
                    break;
                }
            }
        }, asio::use_future);
        session.sendMessage(text, mode, std::chrono::steady_clock::now(), std::move(images), {}, saneRequest, implementationRequest, phase);
        io.run();
        completed.get();
        session.setEventHandler({});
    }

    void healthExchange(std::function<asio::awaitable<void>(ConnectionSocket&)> interact,
        SessionTitleGenerator title = {}, CompactionService::ProviderFactory compact = {},
        std::chrono::seconds timeout = std::chrono::seconds(2),
        std::chrono::milliseconds pongTimeout = std::chrono::milliseconds(100),
        std::shared_ptr<ProviderConnections> preferences = {}, Integration::BtwProviderFactory btw = {}, SessionAgentFactory customFactory = {}) {
        asio::io_context io;
        SessionRegistry registry(directory / "sessions", customFactory ? std::move(customFactory) : factory());
        ServerSocket server(io, directory / "health.sock");
        server.listen();
        asio::local::stream_protocol::socket socket(io);
        socket.connect(asio::local::stream_protocol::endpoint((directory / "health.sock").string()));
        ConnectionSocket client(std::move(socket));
        if (!title) title = [](const std::string&, const std::string&, core::CancellationCheck) { return "Health test"; };
        std::shared_ptr<Integration> integration;
        auto run = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
            integration = std::make_shared<Integration>(io, co_await server.accept(), registry, title,
                Integration::ReasoningEffortsLoader{}, IntegrationHealthSettings{
                    std::chrono::milliseconds(10), pongTimeout}, preferences, compact, btw);
            co_await integration->run();
        }, asio::use_future);
        auto interaction = asio::co_spawn(io, interact(client), asio::use_future);
        io.run_for(timeout);
        if (integration) integration->close();
        client.close();
        io.restart();
        io.run_for(std::chrono::seconds(1));
        ASSERT_EQ(interaction.wait_for(std::chrono::seconds(0)), std::future_status::ready);
        ASSERT_EQ(run.wait_for(std::chrono::seconds(0)), std::future_status::ready);
        interaction.get();
        run.get();
    }
};

asio::awaitable<json> receiveEvent(ConnectionSocket& client);

TEST_F(SessionTest, ProviderIdentityUsesPersistedSessionIdOnCreationAndResume) {
    SessionRegistry registry(directory / "sessions", factory());
    auto session = registry.create(info());
    const auto id = session->getInfo().id;
    EXPECT_EQ(provider->sessionId, id);
    std::vector<AgentEventType> events;
    turn(*session, "Hello", events, ExecutionMode::SLOP);
    session.reset();
    provider->sessionId.clear();
    session = registry.load(id);
    turn(*session, "Continue", events, ExecutionMode::SLOP);
    EXPECT_EQ(provider->sessionId, id);
    auto other = registry.create(info());
    EXPECT_NE(other->getInfo().id, id);
    EXPECT_EQ(provider->sessionId, other->getInfo().id);
}

TEST_F(SessionTest, WorktreeToolMovesLiveConversationWithoutConfirmationAndResumeKeepsSourceConfiguration) {
    std::ofstream(directory / "file") << "committed\n";
    std::ofstream(directory / "AGENTS.md") << "CHECKOUT_INSTRUCTIONS";
    prepareImplementationRepository();
    std::ofstream(directory / "file") << "source dirty\n";
    std::ofstream(directory / "AGENTS.md") << "SOURCE_INSTRUCTIONS";
    fs::create_directories(directory / ".sos/skills/source-skill");
    std::ofstream(directory / ".sos/skills/source-skill/SKILL.md") <<
        "---\nname: source-skill\ndescription: Source-only skill\n---\nINHERITED_SKILL_CONTENT\n";
    const auto agentFactory = [this](const SessionInfo& info) {
        ToolRegistry tools; tools.add(std::make_unique<WriteTool>());
        auto agent = std::make_unique<Agent>(std::make_unique<TestProvider>(provider), info.model,
            std::move(tools), ToolContext{info.executionDirectory(), info.executionDirectory()}, info.reasoning);
        agent->enableProjectContext(info.executionDirectory(), info.configurationDirectory());
        return agent;
    };
    provider->respond = [](const json& input, std::size_t call) {
        if (call == 1) {
            auto result = submitImplementation({}, call);
            result["output"][0]["name"] = "worktree";
            result["output"][0]["arguments"] = json{{"action", "create"}, {"name", "wire"}}.dump();
            auto write = result["output"][0]; write["id"] = "write-item"; write["call_id"] = "write-call";
            write["name"] = "write";
            write["arguments"] = json{{"filePath", "file"}, {"content", "target edit\n"}}.dump();
            result["output"].push_back(write);
            return result;
        }
        if (call == 2) {
            EXPECT_NE(input.dump().find("CHECKOUT_INSTRUCTIONS"), std::string::npos);
            auto result = submitImplementation({}, call);
            result["output"][0]["name"] = "skill";
            result["output"][0]["arguments"] = json{{"name", "source-skill"}, {"input", ""}}.dump();
            return result;
        }
        EXPECT_NE(input.dump().find("INHERITED_SKILL_CONTENT"), std::string::npos);
        return reply("done", "Worked in the new worktree");
    };
    std::string id; fs::path target;
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await receiveEvent(client);
        co_await client.send({{"type", "integration.configure"}, {"workspace", directory.string()}, {"mode", "slop"}});
        co_await receiveEvent(client);
        co_await client.send({{"type", "message.send"}, {"workspace", directory.string()}, {"text", "Create a worktree and work there"}});
        const auto created = co_await receiveEvent(client); id = created.at("session").at("id");
        unsigned confirmations = 0;
        for (;;) {
            const auto event = co_await receiveEvent(client);
            if (event.at("type") == "permission.requested") {
                ++confirmations;
                EXPECT_EQ(event.at("tool"), "write");
                co_await client.send({{"type", "permission.reply"}, {"permissionId", event.at("permissionId")}, {"decision", "yes"}});
            }
            if (event.at("type") == "session.updated") target = event.at("info").at("workspace").get<std::string>();
            if (event.at("type") == "execution.completed") break;
        }
        EXPECT_EQ(confirmations, 1);
        EXPECT_EQ(fs_tools::readText(target / "file"), "target edit\n");
        EXPECT_EQ(fs_tools::readText(directory / "file"), "source dirty\n");
        EXPECT_FALSE(fs::exists(target / ".sos"));
        co_await client.send({{"type", "integration.close"}});
    }, {}, {}, std::chrono::seconds(10), std::chrono::milliseconds(2000), {}, {}, agentFactory);
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await receiveEvent(client);
        // Resuming even from the worktree keeps the original source configuration.
        co_await client.send({{"type", "integration.configure"}, {"workspace", target.string()}, {"mode", "slop"}});
        co_await receiveEvent(client);
        co_await client.send({{"type", "session.attach"}, {"sessionId", id}});
        const auto restored = co_await receiveEvent(client);
        EXPECT_EQ(restored.at("session").at("workspace"), target.string());
        EXPECT_EQ(restored.at("session").at("configuration_workspace"), fs::canonical(directory).string());
        const auto& skills = restored.at("info").at("skills");
        EXPECT_NE(std::find_if(skills.begin(), skills.end(), [](const auto& skill) {
            return skill.at("name") == "source-skill";
        }), skills.end());
        co_await client.send({{"type", "session.undo"}, {"requestId", "undo"}});
        EXPECT_EQ((co_await receiveEvent(client)).at("type"), "session.attached");
        EXPECT_EQ(fs_tools::readText(target / "file"), "committed\n");
        EXPECT_EQ(fs_tools::readText(directory / "file"), "source dirty\n");
        co_await client.send({{"type", "session.redo"}, {"requestId", "redo"}});
        EXPECT_EQ((co_await receiveEvent(client)).at("type"), "session.attached");
        EXPECT_EQ(fs_tools::readText(target / "file"), "target edit\n");
        co_await client.send({{"type", "integration.close"}});
    }, {}, {}, std::chrono::seconds(10), std::chrono::milliseconds(2000), {}, {}, agentFactory);
}

TEST_F(SessionTest, WorktreeRequestsCreateSwitchAndKeepConversationAndSourceConfiguration) {
    std::ofstream(directory / "file") << "committed\n";
    prepareImplementationRepository();
    std::string expectedWorkspace;
    provider->respond = [&](const json& input, std::size_t call) {
        EXPECT_NE(input.dump().find(expectedWorkspace), std::string::npos);
        return reply("done-" + std::to_string(call), "Ready");
    };
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await receiveEvent(client);
        co_await client.send({{"type", "integration.configure"}, {"workspace", directory.string()}, {"mode", "sane"}});
        co_await receiveEvent(client);
        co_await client.send({{"type", "worktree.list"}, {"requestId", "list"}});
        EXPECT_EQ((co_await receiveEvent(client)).at("type"), "worktree.listed");
        co_await client.send({{"type", "worktree.create"}, {"name", "ui"}});
        EXPECT_EQ((co_await receiveEvent(client)).at("type"), "request.failed");
        co_await client.send({{"type", "mode.set"}, {"mode", "slop"}});
        co_await receiveEvent(client);
        co_await client.send({{"type", "worktree.create"}, {"name", "ui"}, {"requestId", "create"}});
        const auto created = co_await receiveEvent(client);
        EXPECT_EQ(created.at("type"), "worktree.switched") << created.dump();
        if (created.at("type") != "worktree.switched") co_return;
        const auto id = created.at("sessionId");
        const auto target = created.at("workspace");
        expectedWorkspace = target.get<std::string>();
        EXPECT_EQ(created.at("info").at("configuration_workspace"), fs::canonical(directory).string());
        co_await client.send({{"type", "message.send"}, {"workspace", target}, {"text", "Work here"}});
        for (;;) { const auto event = co_await receiveEvent(client); EXPECT_NE(event.at("type"), "request.failed") << event.dump(); if (event.at("type") == "execution.completed" || event.at("type") == "request.failed") break; }
        co_await client.send({{"type", "worktree.switch"}, {"path", directory.string()}});
        const auto switched = co_await receiveEvent(client);
        EXPECT_EQ(switched.at("type"), "worktree.switched");
        EXPECT_EQ(switched.at("sessionId"), id);
        EXPECT_EQ(switched.at("workspace"), fs::canonical(directory).string());
        expectedWorkspace = fs::canonical(directory).string();
        co_await client.send({{"type", "message.send"}, {"workspace", directory.string()}, {"text", "Continue here"}});
        for (;;) { const auto event = co_await receiveEvent(client); EXPECT_NE(event.at("type"), "request.failed") << event.dump(); if (event.at("type") == "execution.completed" || event.at("type") == "request.failed") break; }
        co_await client.send({{"type", "integration.close"}});
    }, {}, {}, std::chrono::seconds(15), std::chrono::milliseconds(2000));
}

TEST_F(SessionTest, NewSessionKeepsCurrentWorktreeAndDeletedSessionCannotResumeElsewhere) {
    prepareImplementationRepository();
    std::string savedId;
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await receiveEvent(client);
        co_await client.send({{"type", "integration.configure"}, {"workspace", directory.string()}, {"mode", "slop"}});
        co_await receiveEvent(client);
        co_await client.send({{"type", "worktree.create"}, {"name", "session-workspace"}});
        const auto created = co_await receiveEvent(client);
        EXPECT_EQ(created.at("type"), "worktree.switched");
        if (created.at("type") != "worktree.switched") co_return;
        savedId = created.at("sessionId");
        const auto target = created.at("workspace");
        co_await client.send({{"type", "session.new"}});
        const auto reset = co_await receiveEvent(client);
        EXPECT_EQ(reset.at("workspace"), target);
        EXPECT_EQ(reset.at("info").at("workspace"), target);
        co_await client.send({{"type", "mode.set"}, {"mode", "slop"}});
        co_await receiveEvent(client);
        co_await client.send({{"type", "worktree.delete"}, {"path", target}});
        EXPECT_EQ((co_await receiveEvent(client)).at("type"), "request.failed");
        co_await client.send({{"type", "worktree.switch"}, {"path", directory.string()}});
        EXPECT_EQ((co_await receiveEvent(client)).at("type"), "worktree.switched");
        co_await client.send({{"type", "worktree.delete"}, {"path", target}});
        const auto deleted = co_await receiveEvent(client);
        EXPECT_EQ(deleted.at("type"), "worktree.deleted") << deleted.dump();
        EXPECT_FALSE(fs::exists(target.get<std::string>()));
        co_await client.send({{"type", "session.attach"}, {"sessionId", savedId}});
        const auto failed = co_await receiveEvent(client);
        EXPECT_EQ(failed.at("type"), "request.failed") << failed.dump();
        EXPECT_NE(failed.at("error").get<std::string>().find("workspace is unavailable"), std::string::npos);
        co_await client.send({{"type", "integration.close"}});
    }, {}, {}, std::chrono::seconds(15), std::chrono::milliseconds(2000));
}

TEST_F(SessionTest, OpenedWorkspacesAndDeletionReservationsAreSharedAndReleased) {
    prepareImplementationRepository();
    const auto target = AgentWorktree(directory).create("other-session");
    SessionRegistry registry(directory / "sessions", factory());
    auto selected = info(); selected.workspace = target;
    auto other = registry.create(selected);
    EXPECT_THROW(registry.beginWorktreeDeletion(target), std::logic_error);
    const auto id = other->getInfo().id;
    other.reset();
    fs::path opened = target;
    registry.watchWorkspace(this, [&] { return opened; });
    EXPECT_THROW(registry.beginWorktreeDeletion(target), std::logic_error);
    opened = directory;
    registry.beginWorktreeDeletion(target);
    EXPECT_THROW(registry.load(id), std::logic_error);
    EXPECT_THROW(registry.create(selected), std::logic_error);
    EXPECT_THROW(registry.requireWorkspaceAvailable(target / "subdir"), std::logic_error);
    registry.endWorktreeDeletion(target);
    other = registry.load(id);
    EXPECT_EQ(other->getInfo().workspace, target);
    registry.unwatchWorkspace(this);
    other.reset();
    selected.workspace = directory;
    selected.executionWorkspace = target;
    auto executing = registry.create(selected);
    EXPECT_THROW(registry.beginWorktreeDeletion(target), std::logic_error);
    const auto executionId = executing->getInfo().id;
    executing.reset();
    registry.beginWorktreeDeletion(target);
    EXPECT_THROW(registry.load(executionId), std::logic_error);
    asio::io_context io;
    auto load = asio::co_spawn(io, registry.loadAsync(executionId, std::nullopt), asio::use_future);
    io.run();
    EXPECT_THROW(load.get(), std::logic_error);
    registry.endWorktreeDeletion(target);
}

TEST_F(SessionTest, FrozenArchiveRemainsReadableWithoutItsUserWorkspace) {
    SessionRegistry registry(directory / "sessions", factory());
    auto selected = info(); selected.workspace = directory / "removed-workspace";
    selected.frozen = true;
    fs::create_directories(selected.workspace);
    auto archive = registry.create(selected);
    const auto id = archive->getInfo().id;
    archive.reset();
    fs::remove_all(selected.workspace);
    EXPECT_TRUE(registry.load(id)->getInfo().frozen);
}

TEST_F(SessionTest, WorktreeListAndDeleteProtectOtherConnectedIntegrations) {
    prepareImplementationRepository();
    const auto target = AgentWorktree(directory).create("opened-elsewhere");
    asio::io_context io;
    SessionRegistry registry(directory / "sessions", factory());
    ServerSocket server(io, directory / "multi.sock"); server.listen();
    asio::local::stream_protocol::socket firstSocket(io), secondSocket(io);
    const auto endpoint = asio::local::stream_protocol::endpoint((directory / "multi.sock").string());
    firstSocket.connect(endpoint); secondSocket.connect(endpoint);
    ConnectionSocket first(std::move(firstSocket)), second(std::move(secondSocket));
    std::vector<std::shared_ptr<Integration>> integrations;
    auto accepting = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        for (int i = 0; i < 2; ++i) {
            auto integration = std::make_shared<Integration>(io, co_await server.accept(), registry, SessionTitleGenerator{});
            integrations.push_back(integration);
            asio::co_spawn(io, integration->run(), [integration](std::exception_ptr error) {
                if (error) std::rethrow_exception(error);
            });
        }
    }, asio::use_future);
    auto interaction = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        co_await receiveEvent(first); co_await receiveEvent(second);
        co_await first.send({{"type", "integration.configure"}, {"workspace", directory.string()}, {"mode", "slop"}});
        co_await receiveEvent(first);
        co_await second.send({{"type", "integration.configure"}, {"workspace", target.string()}, {"mode", "slop"}});
        co_await receiveEvent(second);
        co_await first.send({{"type", "worktree.list"}});
        const auto listed = co_await receiveEvent(first);
        const auto entries = listed.at("worktrees");
        const auto other = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) { return entry.at("path") == target.string(); });
        EXPECT_NE(other, entries.end());
        if (other != entries.end()) {
            EXPECT_TRUE(other->at("opened"));
            EXPECT_FALSE(other->at("deletable"));
        }
        co_await first.send({{"type", "worktree.delete"}, {"path", target.string()}});
        EXPECT_EQ((co_await receiveEvent(first)).at("type"), "request.failed");
        EXPECT_TRUE(fs::is_directory(target));
        co_await second.send({{"type", "worktree.switch"}, {"path", directory.string()}});
        EXPECT_EQ((co_await receiveEvent(second)).at("type"), "worktree.switched");
        co_await first.send({{"type", "worktree.delete"}, {"path", target.string()}});
        const auto deleted = co_await receiveEvent(first);
        EXPECT_EQ(deleted.at("type"), "worktree.deleted") << deleted.dump();
        EXPECT_FALSE(fs::exists(target));
        co_await first.send({{"type", "integration.close"}});
        co_await second.send({{"type", "integration.close"}});
    }, asio::use_future);
    io.run_for(std::chrono::seconds(15));
    for (const auto& integration : integrations) integration->close();
    first.close(); second.close(); io.restart(); io.run_for(std::chrono::seconds(1));
    ASSERT_EQ(interaction.wait_for(std::chrono::seconds(0)), std::future_status::ready);
    accepting.get(); interaction.get();
}

TEST_F(SessionTest, WorktreeToolCreatesListsAndSwitchesInSaneWithoutConfirmation) {
    prepareImplementationRepository();
    provider->respond = [this](const json&, std::size_t call) {
        if (call > 2) return reply("done", "Worktrees selected in Sane");
        auto result = submitImplementation({}, call);
        result["output"][0]["name"] = "worktree";
        if (call == 2) {
            result["output"][0]["arguments"] = json{{"action", "switch"}, {"path", directory.string()}}.dump();
            return result;
        }
        result["output"][0]["arguments"] = json{{"action", "create"}, {"name", "sane"}}.dump();
        auto list = result["output"][0]; list["id"] = "list-item"; list["call_id"] = "list-call";
        list["arguments"] = json{{"action", "list"}}.dump(); result["output"].push_back(list);
        return result;
    };
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await receiveEvent(client);
        co_await client.send({{"type", "integration.configure"}, {"workspace", directory.string()}, {"mode", "sane"}});
        co_await receiveEvent(client);
        co_await client.send({{"type", "message.send"}, {"workspace", directory.string()}, {"text", "Create a worktree, list, then switch back"}});
        bool created = false, switched = false, listed = false;
        unsigned workspaceUpdates = 0;
        for (;;) {
            const auto event = co_await receiveEvent(client);
            EXPECT_NE(event.at("type"), "permission.requested");
            if (event.at("type") == "session.updated") {
                const auto& info = event.at("info");
                EXPECT_EQ(info.at("configuration_workspace"), fs::canonical(directory).string());
                // Listing also publishes session.updated without changing the workspace.
                if (workspaceUpdates++ < 2) EXPECT_NE(info.at("workspace"), fs::canonical(directory).string());
                else EXPECT_EQ(info.at("workspace"), fs::canonical(directory).string());
            }
            if (event.contains("messages")) for (const auto& message : event.at("messages"))
                for (const auto& part : message.at("content")) {
                    if (part.value("type", "") != "tool_result") continue;
                    if (part.at("callId") == "call-1") created = !part.at("isError").get<bool>();
                    if (part.at("callId") == "call-2") switched = !part.at("isError").get<bool>();
                    if (part.at("callId") == "list-call") listed = !part.at("isError").get<bool>();
                }
            if (event.at("type") == "execution.completed") break;
        }
        EXPECT_TRUE(created); EXPECT_TRUE(listed); EXPECT_TRUE(switched);
        EXPECT_EQ(workspaceUpdates, 3);
        EXPECT_EQ(AgentWorktree(directory).list().size(), 2);
        co_await client.send({{"type", "integration.close"}});
    }, {}, {}, std::chrono::seconds(5), std::chrono::milliseconds(1000));
}

TEST_F(SessionTest, ActiveImplementBlocksWorkspaceSwitchAndKeepsSessionIdentity) {
    SessionRegistry registry(directory / "sessions", factory());
    auto selected = info(); selected.activeImplementationId = "active-implementation";
    auto session = registry.create(selected);
    const auto target = directory / "target"; fs::create_directories(target);
    asio::io_context io;
    auto result = asio::co_spawn(io, session->switchWorkspace(target, target), asio::use_future);
    io.run();
    EXPECT_THROW(result.get(), std::logic_error);
    EXPECT_EQ(session->getInfo().workspace, fs::canonical(directory));
    EXPECT_TRUE(session->getInfo().configurationWorkspace.empty());
}

TEST_F(SessionTest, WorkspaceToolMovesAcrossPlainFoldersRepositoriesAndWorktreesKeepingSourceConfiguration) {
    const auto parent = directory / "FEAT_multi";
    const auto first = parent / "first", second = parent / "second", selected = parent / "first-worktree";
    for (const auto& repo : {first, second}) {
        fs::create_directories(repo);
        for (const auto& args : std::vector<std::vector<std::string>>{{"init", "--template="},
            {"-c", "user.name=Test", "-c", "user.email=test@localhost", "-c", "commit.gpgSign=false", "commit", "--allow-empty", "-m", "Base"}}) {
            const auto result = core::runProcess("git", args, repo);
            ASSERT_EQ(result.exitCode, 0) << result.error;
        }
    }
    ASSERT_EQ(core::runProcess("git", {"-c", "core.hooksPath=/dev/null", "worktree", "add", "--detach", selected.string()}, first).exitCode, 0);
    std::ofstream(directory / "not-a-directory") << "file";
    fs::create_directories(directory / ".sos/skills/source-skill");
    std::ofstream(directory / ".sos/skills/source-skill/SKILL.md") <<
        "---\nname: source-skill\ndescription: Origin skill\n---\nORIGINAL_WORKSPACE_SKILL\n";
    fs::create_directories(second / ".sos/skills/source-skill");
    std::ofstream(second / ".sos/skills/source-skill/SKILL.md") <<
        "---\nname: source-skill\ndescription: Destination skill\n---\nWRONG_DESTINATION_SKILL\n";
    std::ofstream(second / "AGENTS.md") << "SECOND_REPO_INSTRUCTIONS\n";
    std::ofstream(second / "local.txt") << "SECOND_REPO_LOCAL_FILE\n";
    const auto agentFactory = [this](const SessionInfo& info) {
        ToolRegistry tools; tools.add(std::make_unique<ReadTool>());
        auto agent = std::make_unique<Agent>(std::make_unique<TestProvider>(provider), info.model,
            std::move(tools), ToolContext{info.executionDirectory(), info.executionDirectory()}, info.reasoning);
        agent->enableProjectContext(info.executionDirectory(), info.configurationDirectory());
        return agent;
    };
    const std::vector<std::pair<std::string, json>> calls{
        {"workspace", {{"path", "relative"}}},
        {"workspace", {{"path", (directory / "missing").string()}}},
        {"workspace", {{"path", (directory / "not-a-directory").string()}}},
        {"workspace", {{"path", parent.string()}}},
        {"workspace", {{"path", first.string()}}},
        {"worktree", {{"action", "switch"}, {"path", selected.string()}}},
        {"workspace", {{"path", second.string()}}},
        {"worktree", {{"action", "list"}}},
        {"read", {{"filePath", "local.txt"}}},
        {"skill", {{"name", "source-skill"}, {"input", ""}}},
        {"workspace", {{"path", parent.string()}}}};
    provider->respond = [calls](const json& input, std::size_t call) {
        if (call > calls.size()) {
            EXPECT_NE(input.dump().find("SECOND_REPO_INSTRUCTIONS"), std::string::npos);
            EXPECT_NE(input.dump().find("SECOND_REPO_LOCAL_FILE"), std::string::npos);
            EXPECT_NE(input.dump().find("ORIGINAL_WORKSPACE_SKILL"), std::string::npos);
            EXPECT_EQ(input.dump().find("WRONG_DESTINATION_SKILL"), std::string::npos);
            return reply("done-" + std::to_string(call), "Moved");
        }
        auto result = submitImplementation({}, call);
        result["output"][0]["name"] = calls[call - 1].first;
        result["output"][0]["arguments"] = calls[call - 1].second.dump();
        return result;
    };
    std::string id;
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await receiveEvent(client);
        co_await client.send({{"type", "integration.configure"}, {"workspace", directory.string()}, {"mode", "sane"}}); co_await receiveEvent(client);
        co_await client.send({{"type", "message.send"}, {"workspace", directory.string()}, {"text", "Visit the task folder, both repositories and the existing worktree, then return to the task folder"}});
        unsigned updates = 0, errors = 0, successes = 0;
        for (;;) {
            const auto event = co_await receiveEvent(client);
            EXPECT_NE(event.at("type"), "permission.requested");
            if (event.at("type") == "session.created") id = event.at("session").at("id");
            if (event.at("type") == "session.updated") {
                ++updates;
                EXPECT_EQ(event.at("info").at("configuration_workspace"), fs::canonical(directory).string());
            }
            if (event.contains("messages")) for (const auto& message : event.at("messages")) for (const auto& part : message.at("content")) {
                if (part.value("type", "") != "tool_result") continue;
                if (part.at("isError").get<bool>()) ++errors; else ++successes;
            }
            if (event.at("type") != "execution.completed") continue;
            EXPECT_EQ(event.at("info").at("workspace"), fs::canonical(parent).string());
            EXPECT_EQ(event.at("info").at("configuration_workspace"), fs::canonical(directory).string());
            break;
        }
        EXPECT_EQ(errors, 3); EXPECT_EQ(successes, 8); EXPECT_EQ(updates, 9);
        co_await client.send({{"type", "integration.close"}});
    }, {}, {}, std::chrono::seconds(60), std::chrono::milliseconds(5000), {}, {}, agentFactory);
    SessionRegistry registry(directory / "sessions", factory());
    const auto restored = registry.load(id);
    EXPECT_EQ(restored->getInfo().workspace, fs::canonical(parent));
    EXPECT_EQ(restored->getInfo().configurationDirectory(), fs::canonical(directory));
    EXPECT_FALSE(fs::exists(parent / ".git"));
}

TEST_F(SessionTest, TerminalOpeningsPersistAndDoNotChangeActivity) {
    std::string id;
    std::uint64_t updatedAt;
    {
        SessionRegistry registry(directory / "sessions", factory());
        auto session = registry.create(info());
        id = session->getInfo().id;
        updatedAt = session->getInfo().updatedAt;
        session->recordTerminalOpen("terminal-a", 10);
        session->recordTerminalOpen("terminal-b", 20);
        session->recordTerminalOpen("terminal-a", 30);
        session->recordTerminalOpen("");
        ASSERT_EQ(session->getInfo().terminalOpenings.size(), 2);
        EXPECT_EQ(session->getInfo().updatedAt, updatedAt);
    }
    SessionRegistry restarted(directory / "sessions", factory());
    auto loaded = restarted.load(id);
    ASSERT_EQ(loaded->getInfo().terminalOpenings.size(), 2);
    EXPECT_EQ(loaded->getInfo().terminalOpenings[0].terminalSessionId, "terminal-a");
    EXPECT_EQ(loaded->getInfo().terminalOpenings[0].lastOpenedAt, 30);
    EXPECT_EQ(loaded->getInfo().updatedAt, updatedAt);
    EXPECT_EQ(restarted.list()[0].terminalOpenings.size(), 2);
    auto incomplete = json(loaded->getInfo());
    incomplete.erase("terminalOpenings");
    EXPECT_THROW(incomplete.get<SessionInfo>(), json::out_of_range);
}

TEST_F(SessionTest, TerminalOpeningsEvictLeastRecentlyOpenedAtOneHundred) {
    SessionRegistry registry(directory / "sessions", factory());
    auto session = registry.create(info());
    for (int i = 1; i <= 100; ++i) session->recordTerminalOpen("terminal-" + std::to_string(i), i);
    session->recordTerminalOpen("terminal-1", 101);
    session->recordTerminalOpen("terminal-new", 102);
    const auto& openings = session->getInfo().terminalOpenings;
    ASSERT_EQ(openings.size(), 100);
    EXPECT_EQ(openings.front().terminalSessionId, "terminal-new");
    EXPECT_EQ(openings[1].terminalSessionId, "terminal-1");
    EXPECT_EQ(std::count_if(openings.begin(), openings.end(), [](const auto& item) {
        return item.terminalSessionId == "terminal-2";
    }), 0);
}

TEST_F(SessionTest, TerminalOpeningsIntegrationTracksCreationAndSuccessfulResumeOnly) {
    std::string id;
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await client.receive();
        co_await client.send({{"type", "integration.configure"}, {"workspace", directory.string()},
            {"mode", "sane"}, {"terminalSessionId", "terminal-a"}});
        EXPECT_EQ((co_await receiveEvent(client)).at("type"), "integration.configured");
        co_await client.send({{"type", "message.send"}, {"text", "Hello"}, {"workspace", directory.string()}});
        auto reply = co_await receiveEvent(client);
        id = reply.at("session").at("id");
        while (reply.at("type") != "execution.completed") reply = co_await receiveEvent(client);
        co_await client.send({{"type", "session.list"}});
        auto listing = co_await receiveEvent(client);
        EXPECT_GT(listing.at("sessions")[0].at("terminalLastOpenedAt").get<std::uint64_t>(), 0);
        EXPECT_FALSE(listing.at("sessions")[0].contains("terminalOpenings"));
        co_await client.send({{"type", "integration.close"}});
    });
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await client.receive();
        co_await client.send({{"type", "integration.configure"}, {"workspace", directory.string()},
            {"mode", "sane"}, {"terminalSessionId", "terminal-b"}});
        co_await receiveEvent(client);
        co_await client.send({{"type", "session.preview"}, {"sessionId", id}});
        co_await receiveEvent(client);
        co_await client.send({{"type", "session.list"}});
        EXPECT_FALSE((co_await receiveEvent(client)).at("sessions")[0].contains("terminalLastOpenedAt"));
        co_await client.send({{"type", "session.attach"}, {"sessionId", id}});
        EXPECT_EQ((co_await receiveEvent(client)).at("type"), "session.attached");
        co_await client.send({{"type", "session.list"}});
        EXPECT_GT((co_await receiveEvent(client)).at("sessions")[0].at("terminalLastOpenedAt").get<std::uint64_t>(), 0);
        co_await client.send({{"type", "session.attach"}, {"sessionId", std::string(32, 'f')}});
        EXPECT_EQ((co_await receiveEvent(client)).at("type"), "request.failed");
        co_await client.send({{"type", "integration.close"}});
    });
    SessionRegistry restarted(directory / "sessions", factory());
    ASSERT_EQ(restarted.list().size(), 1);
    EXPECT_EQ(restarted.list()[0].terminalOpenings.size(), 2);
}

TEST_F(SessionTest, TerminalOpeningsConfigurationIsOptionalAndBounded) {
    const auto oldClient = json{{"type", "integration.configure"}, {"workspace", directory.string()}, {"mode", "sane"}};
    EXPECT_TRUE(oldClient.get<IntegrationRequest>().terminalSessionId.empty());
    auto oversized = oldClient;
    oversized["terminalSessionId"] = std::string(257, 'x');
    EXPECT_THROW(oversized.get<IntegrationRequest>(), std::invalid_argument);
    auto invalid = oldClient;
    invalid["terminalSessionId"] = 7;
    EXPECT_THROW(invalid.get<IntegrationRequest>(), json::type_error);
}

TEST_F(SessionTest, ImplementationRoutingOpensSwitchesFreezesAndArchives) {
    std::string id, branchId;
    {
        SessionRegistry registry(directory / "sessions", factory());
        auto discussion = registry.create(info());
        id = discussion->getInfo().id;
        Message plan{"objective", MessageRole::ASSISTANT, {TextContent{"Build a status page."}}};
        plan.objectivePlan = true;
        Message request{"request", MessageRole::USER, {TextContent{"Sane Implementation request"}}};
        request.userMetadata = UserMessageMetadata{"sane", true};
        discussion->record({AgentEventType::HARNESS_MESSAGE_ARRIVED, {request, plan}, {}, {}});
    }
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await receiveEvent(client);
        co_await client.send({{"type", "integration.configure"}, {"workspace", directory.string()}, {"mode", "sane"}});
        co_await receiveEvent(client);
        co_await client.send({{"type", "session.attach"}, {"sessionId", id}});
        co_await receiveEvent(client);
        co_await client.send({{"type", "implementation.open"}, {"sessionId", id}});
        auto opened = co_await receiveEvent(client);
        EXPECT_EQ(opened.at("type"), "session.attached");
        branchId = opened.at("sessionId");
        EXPECT_NE(branchId, id);
        EXPECT_TRUE(opened.at("messages").empty());
        EXPECT_TRUE(opened.at("info").at("implementation_view"));
        EXPECT_EQ(opened.at("info").at("objective_plans").size(), 1);
        EXPECT_EQ(opened.at("info").at("objective_plans")[0].at("id"), "objective");
        co_await client.send({{"type", "session.history"}, {"sessionId", id}, {"direction", "latest"}});
        auto preview = co_await receiveEvent(client);
        EXPECT_EQ(preview.at("sessionId"), id);
        EXPECT_NE(preview.at("messages").dump().find("Build a status page."), std::string::npos);
        EXPECT_TRUE(preview.at("info").at("implementation_view"));
        co_await client.send({{"type", "session.history"}, {"sessionId", "unrelated"}, {"direction", "latest"}});
        EXPECT_EQ((co_await receiveEvent(client)).at("type"), "request.failed");
        co_await client.send({{"type", "mode.set"}, {"mode", "slop"}});
        EXPECT_EQ((co_await receiveEvent(client)).at("type"), "request.failed");
        co_await client.send({{"type", "message.send"}, {"text", "PRIVATE DETAIL"}, {"mode", "sane"}});
        for (;;) if ((co_await receiveEvent(client)).at("type") == "execution.completed") break;
        provider->waitForCancellation = true;
        co_await client.send({{"type", "message.send"}, {"text", "Keep generating"}, {"mode", "sane"}});
        for (;;) if ((co_await receiveEvent(client)).at("type") == "message.accepted") break;
        co_await client.send({{"type", "session.history"}, {"sessionId", id}, {"direction", "latest"}, {"requestId", "preview-busy"}});
        for (;;) {
            auto event = co_await receiveEvent(client);
            if (event.value("requestId", "") != "preview-busy") continue;
            EXPECT_EQ(event.at("type"), "session.attached");
            EXPECT_EQ(event.at("sessionId"), id);
            EXPECT_TRUE(event.at("info").at("implementation_view"));
            break;
        }
        co_await client.send({{"type", "execution.cancel"}});
        for (;;) if ((co_await receiveEvent(client)).at("type") == "execution.cancelled") break;
        provider->waitForCancellation = false;
        co_await client.send({{"type", "implementation.view"}, {"sessionId", id}});
        auto parent = co_await receiveEvent(client);
        EXPECT_EQ(parent.at("sessionId"), id);
        EXPECT_EQ(parent.at("messages").dump().find("PRIVATE DETAIL"), std::string::npos);
        EXPECT_EQ(parent.at("info").at("active_implementation_id"), branchId);
        co_await client.send({{"type", "implementation.view"}, {"sessionId", id}, {"branchId", branchId}});
        auto branch = co_await receiveEvent(client);
        EXPECT_NE(branch.at("messages").dump().find("PRIVATE DETAIL"), std::string::npos);
        EXPECT_EQ(branch.at("messages").dump().find("Build a status page."), std::string::npos);
        co_await client.send({{"type", "implementation.view"}, {"sessionId", id}});
        co_await receiveEvent(client);
        co_await client.send({{"type", "message.send"}, {"text", "Continue Discussion"}, {"mode", "sane"}});
        for (;;) if ((co_await receiveEvent(client)).at("type") == "execution.completed") break;
        co_await client.send({{"type", "implementation.view"}, {"sessionId", id}, {"branchId", branchId}});
        auto archive = co_await receiveEvent(client);
        EXPECT_TRUE(archive.at("info").at("implementation_archive"));
        EXPECT_TRUE(archive.at("info").at("active_implementation_id").get<std::string>().empty());
        co_await client.send({{"type", "message.send"}, {"text", "Forbidden"}, {"mode", "sane"}});
        EXPECT_EQ((co_await receiveEvent(client)).at("type"), "request.failed");
        co_await client.send({{"type", "integration.close"}});
    }, {}, {}, std::chrono::seconds(10));
}

TEST_F(SessionTest, ImplementationProgressPersistsWithoutCompletingOrAssigningDiffs) {
    const auto path = directory / "progress";
    fs::create_directories(path);
    ImplementationRun run(path);
    run.start(implementationPlan().get<ImplementationPlan>(), "step_by_step");
    const auto announce = [&](int index, const std::string& status, const std::string& explanation) {
        ToolResultContent result{"progress", "Announced"};
        TurnSnapshot snapshot; snapshot.id = 1; snapshot.complete = true;
        const auto id = ImplementationBoundaries(path).capture(0, 2, {static_cast<std::size_t>(index), status, explanation}, snapshot);
        result.metadata = {{"implementation_boundary", id}, {"implementation_progress", {{"substep", index}, {"status", status}, {"explanation", explanation}}}};
        run.recordActivity({Message{"progress", MessageRole::TOOL, {result}}});
    };
    announce(0, "started", "");
    run.recordActivity({Message{"intermediate", MessageRole::ASSISTANT, {TextContent{"Checking the model"}}}});
    announce(0, "completed", "- Provisional model");
    announce(1, "started", "");
    run.recordActivity({Message{"checks", MessageRole::ASSISTANT, {TextContent{"Running checks"}}}});
    auto view = ImplementationRun(path).view();
    const auto& first = view["steps"][0]["substeps"][0];
    EXPECT_TRUE(first["announced"]);
    EXPECT_FALSE(first["complete"]);
    EXPECT_TRUE(first["diffs"].empty());
    EXPECT_EQ(first["explanation"], "- Provisional model");
    EXPECT_EQ(first["activity"][1]["id"], "intermediate");
    EXPECT_EQ(view["steps"][0]["substeps"][1]["activity"][1]["id"], "checks");
    EXPECT_THROW(run.review(0, 0, true), std::logic_error);
    announce(0, "unnecessary", "Already covered by the existing model");
    auto skipped = ImplementationRun(path).view()["steps"][0]["substeps"][0];
    EXPECT_TRUE(skipped["unnecessary"]);
    EXPECT_FALSE(skipped["complete"]);
    EXPECT_EQ(skipped["explanation"], "Already covered by the existing model");
    announce(0, "started", "");
    EXPECT_FALSE(ImplementationRun(path).view()["steps"][0]["substeps"][0]["unnecessary"]);
}

TEST_F(SessionTest, ImplementationCompactionRecordsStayInTheirSubstepOrValidationTimeline) {
    const auto path = directory / "compaction-timeline";
    fs::create_directories(path);
    ImplementationRun run(path);
    run.start(implementationPlan().get<ImplementationPlan>(), "step_by_step");
    const Message started{"compaction-status-first", MessageRole::SYSTEM, {TextContent{"started"}}};
    Message checkpoint{"checkpoint", MessageRole::ASSISTANT, {TextContent{"Construction summary"}}};
    checkpoint.compaction = CompactionMetadata{{}, "sane"};
    run.recordActivity({started, checkpoint});
    auto value = ImplementationRun(path).view();
    EXPECT_EQ(value["steps"][0]["substeps"][0]["activity"].size(), 2);
    EXPECT_TRUE(value["steps"][0]["validation"]["activity"].empty());
    ToolResultContent validating{"validate", "Announced"};
    TurnSnapshot snapshot; snapshot.id = 1; snapshot.complete = true;
    ImplementationBoundaries evidence(path);
    evidence.capture(0, 2, {0, "unnecessary", "Covered"}, snapshot);
    evidence.capture(0, 2, {1, "unnecessary", "Covered"}, snapshot);
    const auto boundary = evidence.capture(0, 2, {std::nullopt, "validating", ""}, snapshot);
    validating.metadata = {{"implementation_boundary", boundary},
        {"implementation_progress", {{"substep", nullptr}, {"status", "validating"}}}};
    run.recordActivity({Message{"validating", MessageRole::TOOL, {validating}}, started, checkpoint});
    value = ImplementationRun(path).view();
    EXPECT_EQ(value["steps"][0]["substeps"][0]["activity"].size(), 2);
    EXPECT_EQ(value["steps"][0]["validation"]["activity"].size(), 3);
    EXPECT_EQ(value["steps"][0]["validation"]["activity"][1]["id"], started.id);
    EXPECT_TRUE(value["steps"][0]["validation"]["activity"][2].contains("compaction"));
}

TEST_F(SessionTest, ImplementationPerformanceAccumulatesExecutionTurnsAndPersists) {
    const auto path = directory / "performance";
    fs::create_directories(path);
    ImplementationRun run(path);
    run.start(implementationPlan().get<ImplementationPlan>(), "step_by_step");
    TurnSnapshot snapshot; snapshot.id = 1;
    run.begin(snapshot);
    EXPECT_TRUE(run.view().contains("active_duration_ms"));
    EXPECT_NE(run.instruction().find("Reserve those checks for Validating the step"), std::string::npos);
    Message first{"first", MessageRole::ASSISTANT, {TextContent{"Working"}}};
    first.elapsedMs = 5000;
    first.modelCall = ModelCallMetadata{"test", "none", 2000, 100};
    run.recordActivity({first});
    Message tool{"tool", MessageRole::TOOL, {ToolResultContent{"call", "Checked", false}}};
    tool.elapsedMs = 10000;
    run.recordActivity({tool});
    run.stop("Interrupted");
    EXPECT_FALSE(run.view().contains("active_duration_ms"));
    ImplementationRun restored(path);
    restored.resume();
    snapshot.id = 2;
    restored.begin(snapshot);
    EXPECT_GE(restored.view()["active_duration_ms"].get<std::uint64_t>(), 10000);
    first.id = "resumed";
    first.elapsedMs = 2000;
    first.modelCall = ModelCallMetadata{"test", "none", 1000, 50};
    restored.recordActivity({first});
    const auto step = ImplementationRun(path).view()["steps"][0];
    EXPECT_EQ(step["whole"]["duration_ms"], 12000);
    EXPECT_EQ(step["substeps"][0]["duration_ms"], 12000);
    EXPECT_EQ(step["whole"]["model_statistics"]["duration_ms"], 3000);
    EXPECT_EQ(step["whole"]["model_statistics"]["output_tokens"], 150);
    first.elapsedMs = 3000;
    first.modelCall = ModelCallMetadata{"test", "none", 1000, std::nullopt};
    restored.recordActivity({first});
    EXPECT_FALSE(ImplementationRun(path).view()["steps"][0]["whole"]["model_statistics"].contains("output_tokens"));
}

TEST_F(SessionTest, ImplementationValidationGuidanceKeepsChecksFocused) {
    const auto path = directory / "focused-validation";
    fs::create_directories(path);
    ImplementationRun run(path);
    run.start(implementationPlan().get<ImplementationPlan>(), "step_by_step");
    const auto instruction = run.instruction();
    EXPECT_NE(instruction.find("Keep validation proportional to this step"), std::string::npos);
    EXPECT_NE(instruction.find("harness activates the first substep before execution"), std::string::npos);
    EXPECT_NE(instruction.find("Do not build an ad-hoc test framework"), std::string::npos);
    EXPECT_NE(instruction.find("Keep completion concise"), std::string::npos);
    EXPECT_NE(instruction.find("Entering validation does not require new checks"), std::string::npos);
    EXPECT_NE(instruction.find("If no additional check is needed, explain why and complete the step without inventing checks"), std::string::npos);
    EXPECT_NE(instruction.find("Reuse successful earlier results"), std::string::npos);
    EXPECT_NE(instruction.find("Do not reread all files"), std::string::npos);
    EXPECT_NE(instruction.find("satisfy an explicit plan requirement"), std::string::npos);
    EXPECT_NE(instruction.find("The last step is not a fresh global audit"), std::string::npos);
    EXPECT_NE(instruction.find("unperformed required checks still block completion"), std::string::npos);
}

TEST_F(SessionTest, ImplementationAttachResumesActiveBranch) {
    std::string id, branchId;
    {
        SessionRegistry registry(directory / "sessions", factory());
        auto discussion = registry.create(info());
        id = discussion->getInfo().id;
        Message plan{"objective", MessageRole::ASSISTANT, {TextContent{"Objective"}}};
        plan.objectivePlan = true;
        Message request{"request", MessageRole::USER, {TextContent{"Sane Implementation request"}}};
        request.userMetadata = UserMessageMetadata{"sane", true};
        discussion->record({AgentEventType::HARNESS_MESSAGE_ARRIVED, {request, plan}, {}, {}});
        auto branch = registry.createImplementation(*discussion);
        branchId = branch->getInfo().id;
        ImplementationRun run(branch->storageDirectory());
        run.start(implementationPlan().get<ImplementationPlan>(), "step_by_step");
    }
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await receiveEvent(client);
        co_await client.send({{"type", "integration.configure"}, {"workspace", directory.string()}, {"mode", "sane"}});
        co_await receiveEvent(client);
        for (int repeat = 0; repeat < 2; ++repeat) {
            co_await client.send({{"type", "session.attach"}, {"sessionId", id}});
            auto resumed = co_await receiveEvent(client);
            EXPECT_EQ(resumed.at("sessionId"), branchId);
            EXPECT_EQ(resumed.at("session").at("discussion_id"), id);
            EXPECT_TRUE(resumed.at("messages").empty());
            EXPECT_TRUE(resumed.at("info").at("implementation_execution"));
            co_await client.send({{"type", "implementation.state"}, {"sessionId", branchId}});
            const auto restored = co_await receiveEvent(client);
            EXPECT_EQ(restored.at("type"), "implementation.updated");
            EXPECT_EQ(restored.at("implementation").at("plan"), implementationPlan());
        }
        co_await client.send({{"type", "integration.close"}});
    });
}

TEST_F(SessionTest, ImplementationPlanIsStructuredPersistedAndRegeneratesAfterDiscussion) {
    SessionRegistry registry(directory / "sessions", factory());
    auto discussion = registry.create(info());
    Message request{"request", MessageRole::USER, {TextContent{"Sane Implementation request"}}};
    request.userMetadata = UserMessageMetadata{"sane", true};
    Message objective{"objective", MessageRole::ASSISTANT, {TextContent{"Build a status page."}}};
    objective.objectivePlan = true;
    discussion->record({AgentEventType::HARNESS_MESSAGE_ARRIVED, {request, objective}, {}, {120, 32768}});
    auto branch = registry.createImplementation(*discussion);
    const auto id = branch->getInfo().id;
    provider->respond = [](const json&, std::size_t call) {
        return call == 1 || call == 4 ? submitImplementation(implementationPlan(), call) : reply("answer-" + std::to_string(call), "Submitted");
    };
    std::vector<AgentEventType> events;
    turn(*branch, "Implementation plan request", events, ExecutionMode::SANE, {}, false, true);
    ASSERT_TRUE(branch->implementationPlanReady());
    ASSERT_TRUE(branch->getMessages().back().implementationPlan);
    const auto rendered = std::get<TextContent>(branch->getMessages().back().content.front()).text;
    EXPECT_NE(rendered.find("Step 1: Testable foundation"), std::string::npos);
    EXPECT_NE(rendered.find("### 1.2 Exercise"), std::string::npos);
    EXPECT_NE(rendered.find("Validation"), std::string::npos);
    EXPECT_EQ(json(*branch->getMessages().back().implementationPlan), implementationPlan());
    EXPECT_NE(provider->inputs[0].dump().find("Continue the preceding conversation"), std::string::npos);
    EXPECT_NE(provider->inputs[0].dump().find("Build a status page."), std::string::npos);
    EXPECT_NE(provider->inputs[0].dump().find("if no additional check is needed, state why in validation"), std::string::npos);
    EXPECT_NE(provider->inputs[0].dump().find("Do not invent verification-only substeps, repeated test suites or a mandatory final audit"), std::string::npos);
    EXPECT_NE(provider->inputs[0].dump().find("Keep the plan proportional to the request"), std::string::npos);
    EXPECT_NE(provider->inputs[0].dump().find("not an exhaustive matrix of hypothetical edge cases"), std::string::npos);
    const auto page = branch->history().page("latest", "", false, branch->getInfo().inheritedMessages);
    bool found = false;
    for (std::size_t i = 0; i < page.count; ++i) if (page.record(i).contains("implementation_plan")) found = true;
    EXPECT_TRUE(found);
    turn(*branch, "Adjust the second step", events);
    EXPECT_FALSE(branch->implementationPlanReady());
    EXPECT_NE(std::find(provider->toolNames.begin(), provider->toolNames.end(), "submit_implementation_plan"), provider->toolNames.end());
    EXPECT_NE(provider->inputs.back().dump().find("Step 1: Testable foundation"), std::string::npos);
    turn(*branch, "Implementation plan request", events, ExecutionMode::SANE, {}, false, true);
    ASSERT_TRUE(branch->implementationPlanReady());
    branch.reset();
    branch = registry.load(id);
    EXPECT_TRUE(branch->implementationPlanReady());
    EXPECT_EQ(json(*branch->latestImplementationPlan()->implementationPlan), implementationPlan());
    EXPECT_EQ(json(discussion->getMessages()).dump().find("Testable foundation"), std::string::npos);
    EXPECT_THROW(discussion->sendMessage("Implementation plan request", ExecutionMode::SANE, std::chrono::steady_clock::now(), {}, {}, false, true), std::logic_error);
}

TEST_F(SessionTest, OrdinaryPlanningExchangePublishesStructuredImplementationPlan) {
    SessionRegistry registry(directory / "sessions", factory());
    auto discussion = registry.create(info());
    Message objective{"objective", MessageRole::ASSISTANT, {TextContent{"Build a status page."}}};
    objective.objectivePlan = true;
    discussion->record({AgentEventType::HARNESS_MESSAGE_ARRIVED, {objective}, {}, {}});
    auto branch = registry.createImplementation(*discussion);
    provider->respond = [](const json&, std::size_t call) {
        return call == 1 || call == 3 ? submitImplementation(implementationPlan(), call) : reply("done-" + std::to_string(call), "Submitted");
    };
    std::vector<AgentEventType> events;
    turn(*branch, "Prepare the plan now", events);
    EXPECT_NE(std::find(provider->toolNames.begin(), provider->toolNames.end(), "submit_implementation_plan"), provider->toolNames.end());
    ASSERT_TRUE(branch->implementationPlanReady());
    ASSERT_TRUE(branch->getMessages().back().implementationPlan);
    EXPECT_EQ(json(*branch->getMessages().back().implementationPlan), implementationPlan());
    EXPECT_NE(provider->inputs.front().dump().find("publish it in English with submit_implementation_plan"), std::string::npos);
    EXPECT_NE(provider->inputs.front().dump().find("even when the user writes in another language"), std::string::npos);
    const auto id = branch->getInfo().id;
    branch.reset();
    branch = registry.load(id);
    EXPECT_TRUE(branch->implementationPlanReady());
    turn(*branch, "Publish the updated plan", events);
    EXPECT_TRUE(branch->implementationPlanReady());
    turn(*branch, "Explain the plan", events, ExecutionMode::SANE, {}, false, false, ImplementationPhase::REVIEW);
    EXPECT_EQ(std::find(provider->toolNames.begin(), provider->toolNames.end(), "submit_implementation_plan"), provider->toolNames.end());
}

TEST_F(SessionTest, ImplementationPhaseAnnouncementsArePersistedAndResetAfterReview) {
    SessionRegistry registry(directory / "sessions", factory());
    auto discussion = registry.create(info());
    Message request{"request", MessageRole::USER, {TextContent{"Sane Implementation request"}}};
    request.userMetadata = UserMessageMetadata{"sane", true};
    Message objective{"objective", MessageRole::ASSISTANT, {TextContent{"Build a status page."}}};
    objective.objectivePlan = true;
    discussion->record({AgentEventType::HARNESS_MESSAGE_ARRIVED, {request, objective}, {}, {}});
    auto branch = registry.createImplementation(*discussion);
    const auto id = branch->getInfo().id;
    std::vector<AgentEventType> events;
    turn(*branch, "Build the assigned step", events, ExecutionMode::SANE, {}, false, false, ImplementationPhase::EXECUTION);
    EXPECT_NE(provider->inputs.back().dump().find("Current implementation phase: execution"), std::string::npos);
    EXPECT_EQ(branch->getInfo().mode, ExecutionMode::SANE);
    branch.reset();
    branch = registry.load(id);
    turn(*branch, "Explain the selected diff", events, ExecutionMode::SANE, {}, false, false, ImplementationPhase::REVIEW);
    EXPECT_NE(provider->inputs.back().dump().find("Current implementation phase: read-only review"), std::string::npos);
    turn(*branch, "Discuss the plan", events);
    const auto messages = branch->getMessages();
    std::vector<std::string> announcements;
    for (const auto& message : messages) if (message.id.starts_with("implementation_phase_"))
        announcements.push_back(std::get<TextContent>(message.content.front()).text);
    ASSERT_EQ(announcements.size(), 3);
    EXPECT_TRUE(announcements.back().starts_with("Current implementation phase: planning"));
    EXPECT_THROW(discussion->sendMessage("Execute", ExecutionMode::SANE, std::chrono::steady_clock::now(), {}, {},
        false, false, ImplementationPhase::EXECUTION), std::logic_error);
}

TEST_F(SessionTest, ImplementationPlanRequestReturnsFinalPlanOnTheWire) {
    std::string discussionId;
    {
        SessionRegistry registry(directory / "sessions", factory());
        auto discussion = registry.create(info());
        discussionId = discussion->getInfo().id;
        Message request{"request", MessageRole::USER, {TextContent{"Sane Implementation request"}}};
        request.userMetadata = UserMessageMetadata{"sane", true};
        Message objective{"objective", MessageRole::ASSISTANT, {TextContent{"Objective"}}};
        objective.objectivePlan = true;
        discussion->record({AgentEventType::HARNESS_MESSAGE_ARRIVED, {request, objective}, {}, {}});
        registry.createImplementation(*discussion);
    }
    provider->respond = [](const json&, std::size_t call) {
        return call == 1 ? submitImplementation(implementationPlan(), call) : reply("done", "Submitted");
    };
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await receiveEvent(client);
        co_await client.send({{"type", "integration.configure"}, {"workspace", directory.string()}, {"mode", "sane"}});
        co_await receiveEvent(client);
        co_await client.send({{"type", "session.attach"}, {"sessionId", discussionId}});
        co_await receiveEvent(client);
        co_await client.send({{"type", "message.send"}, {"text", "Implementation plan request"},
            {"implementation_request", true}, {"mode", "sane"}});
        for (;;) {
            const auto event = co_await receiveEvent(client);
            if (event.at("type") != "execution.completed") continue;
            EXPECT_TRUE(event.at("info").at("implementation_plan_ready"));
            EXPECT_TRUE(event.at("messages").is_array());
            EXPECT_EQ(event.at("messages").size(), 1);
            EXPECT_EQ(event.at("messages")[0].at("implementation_plan"), implementationPlan());
            break;
        }
        co_await client.send({{"type", "integration.close"}});
    }, {}, {}, std::chrono::seconds(5));
}

TEST_F(SessionTest, AutoPlanAcceptsFinalPlanAndStartsChosenPaceWithoutAnotherRequest) {
    prepareImplementationRepository();
    for (const std::string pace : {"step_by_step", "everything"}) {
        std::string branchId;
        {
            SessionRegistry registry(directory / "sessions", factory());
            auto discussion = registry.create(info());
            Message objective{"objective", MessageRole::ASSISTANT, {TextContent{"Objective"}}};
            objective.objectivePlan = true;
            discussion->record({AgentEventType::HARNESS_MESSAGE_ARRIVED, {objective}, {}, {}});
            branchId = registry.createImplementation(*discussion)->getInfo().id;
        }
        // Stop at checkout preparation so this test isolates automatic plan acceptance.
        fs::create_directories(implementationData / "sos/worktrees" / branchId);
        provider->respond = [](const json&, std::size_t call) {
            return call % 2 == 1 ? submitImplementation(implementationPlan(), call) : reply("done-" + std::to_string(call), "Submitted");
        };
        healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
            co_await receiveEvent(client);
            co_await client.send({{"type", "integration.configure"}, {"workspace", directory.string()}, {"mode", "sane"}});
            co_await receiveEvent(client);
            co_await client.send({{"type", "session.attach"}, {"sessionId", branchId}}); co_await receiveEvent(client);
            co_await client.send({{"type", "implementation.auto_plan"}, {"sessionId", branchId}, {"pace", pace}, {"requestId", "auto"}});
            bool planning = false, accepted = false;
            for (;;) {
                const auto event = co_await receiveEvent(client);
                EXPECT_NE(event.at("type"), "request.failed") << event.dump();
                if (event.at("type") == "request.failed") break;
                planning = planning || event.at("info").value("auto_plan_running", false);
                if (event.at("type") != "implementation.updated" || event.at("implementation").is_null()) continue;
                EXPECT_EQ(event.at("implementation").at("pace"), pace);
                EXPECT_FALSE(event.at("info").at("auto_plan_pending"));
                EXPECT_TRUE(event.at("info").at("implementation_plan_ready"));
                accepted = true;
                if (event.at("implementation").at("status") == "interrupted") break;
            }
            EXPECT_TRUE(planning); EXPECT_TRUE(accepted);
            co_await client.send({{"type", "integration.close"}});
        }, {}, {}, std::chrono::seconds(10), std::chrono::milliseconds(2000));
        SessionRegistry registry(directory / "sessions", factory());
        EXPECT_TRUE(registry.inspect(branchId).autoPlanPace.empty());
    }
}

TEST_F(SessionTest, AutoPlanWithoutFinalPlanStaysInterruptedAndPersistsRetryChoice) {
    prepareImplementationRepository();
    std::string branchId;
    {
        SessionRegistry registry(directory / "sessions", factory());
        auto discussion = registry.create(info());
        Message objective{"objective", MessageRole::ASSISTANT, {TextContent{"Objective"}}}; objective.objectivePlan = true;
        discussion->record({AgentEventType::HARNESS_MESSAGE_ARRIVED, {objective}, {}, {}});
        branchId = registry.createImplementation(*discussion)->getInfo().id;
    }
    provider->respond = [](const json&, std::size_t call) { return reply("done-" + std::to_string(call), "No finalized plan"); };
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await receiveEvent(client);
        co_await client.send({{"type", "integration.configure"}, {"workspace", directory.string()}, {"mode", "sane"}}); co_await receiveEvent(client);
        co_await client.send({{"type", "session.attach"}, {"sessionId", branchId}}); co_await receiveEvent(client);
        co_await client.send({{"type", "implementation.auto_plan"}, {"sessionId", branchId}, {"pace", "invalid"}});
        EXPECT_EQ((co_await receiveEvent(client)).at("type"), "request.failed");
        co_await client.send({{"type", "implementation.auto_plan"}, {"sessionId", branchId}, {"pace", "everything"}});
        for (;;) {
            const auto event = co_await receiveEvent(client);
            if (event.at("type") != "execution.completed") continue;
            EXPECT_TRUE(event.at("info").at("auto_plan_pending"));
            EXPECT_FALSE(event.at("info").at("auto_plan_running"));
            EXPECT_FALSE(event.at("info").at("implementation_execution"));
            EXPECT_FALSE(event.at("info").at("auto_plan_error").get<std::string>().empty());
            break;
        }
        co_await client.send({{"type", "integration.close"}});
    }, {}, {}, std::chrono::seconds(10), std::chrono::milliseconds(2000));
    SessionRegistry registry(directory / "sessions", factory());
    EXPECT_EQ(registry.inspect(branchId).autoPlanPace, "everything");
    EXPECT_FALSE(ImplementationRun(directory / "sessions" / branchId).exists());
    fs::create_directories(implementationData / "sos/worktrees" / branchId);
    provider->respond = [](const json&, std::size_t call) {
        return call == 2 ? submitImplementation(implementationPlan(), call) : reply("retry-done-" + std::to_string(call), "Submitted");
    };
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await receiveEvent(client);
        co_await client.send({{"type", "integration.configure"}, {"workspace", directory.string()}, {"mode", "sane"}}); co_await receiveEvent(client);
        co_await client.send({{"type", "session.attach"}, {"sessionId", branchId}});
        const auto restored = co_await receiveEvent(client);
        EXPECT_TRUE(restored.at("info").at("auto_plan_pending"));
        EXPECT_FALSE(restored.at("info").at("auto_plan_running"));
        co_await client.send({{"type", "implementation.auto_plan"}, {"sessionId", branchId}, {"pace", "everything"}});
        for (;;) {
            const auto event = co_await receiveEvent(client);
            if (event.at("type") == "implementation.updated" && !event.at("implementation").is_null()) {
                EXPECT_EQ(event.at("implementation").at("pace"), "everything");
                EXPECT_FALSE(event.at("info").at("auto_plan_pending"));
                if (event.at("implementation").at("status") == "interrupted") break;
            }
        }
        co_await client.send({{"type", "integration.close"}});
    }, {}, {}, std::chrono::seconds(10), std::chrono::milliseconds(2000));
}

TEST_F(SessionTest, AutoPlanCancellationNeverStartsConstruction) {
    prepareImplementationRepository();
    std::string branchId;
    {
        SessionRegistry registry(directory / "sessions", factory());
        auto discussion = registry.create(info());
        Message objective{"objective", MessageRole::ASSISTANT, {TextContent{"Objective"}}}; objective.objectivePlan = true;
        discussion->record({AgentEventType::HARNESS_MESSAGE_ARRIVED, {objective}, {}, {}});
        branchId = registry.createImplementation(*discussion)->getInfo().id;
    }
    provider->waitForCancellation = true;
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await receiveEvent(client);
        co_await client.send({{"type", "integration.configure"}, {"workspace", directory.string()}, {"mode", "sane"}}); co_await receiveEvent(client);
        co_await client.send({{"type", "session.attach"}, {"sessionId", branchId}}); co_await receiveEvent(client);
        co_await client.send({{"type", "implementation.auto_plan"}, {"sessionId", branchId}, {"pace", "step_by_step"}});
        for (;;) { if ((co_await receiveEvent(client)).at("type") == "message.accepted") break; }
        co_await client.send({{"type", "execution.cancel"}});
        for (;;) {
            const auto event = co_await receiveEvent(client);
            if (event.at("type") != "execution.cancelled") continue;
            EXPECT_TRUE(event.at("info").at("auto_plan_pending"));
            EXPECT_FALSE(event.at("info").at("auto_plan_running"));
            EXPECT_FALSE(event.at("info").at("implementation_execution"));
            break;
        }
        co_await client.send({{"type", "integration.close"}});
    }, {}, {}, std::chrono::seconds(10), std::chrono::milliseconds(2000));
    EXPECT_FALSE(ImplementationRun(directory / "sessions" / branchId).exists());
}

TEST_F(SessionTest, ImplementationPlanCancellationDoesNotFinalizeSubmittedCandidate) {
    SessionRegistry registry(directory / "sessions", factory());
    auto discussion = registry.create(info());
    Message request{"request", MessageRole::USER, {TextContent{"Sane Implementation request"}}};
    request.userMetadata = UserMessageMetadata{"sane", true};
    Message objective{"objective", MessageRole::ASSISTANT, {TextContent{"Objective"}}};
    objective.objectivePlan = true;
    discussion->record({AgentEventType::HARNESS_MESSAGE_ARRIVED, {request, objective}, {}, {}});
    auto branch = registry.createImplementation(*discussion);
    provider->respond = [](const json&, std::size_t call) -> json {
        if (call == 1) return submitImplementation(implementationPlan(), call);
        throw core::Cancelled{};
    };
    std::vector<AgentEventType> events;
    turn(*branch, "Implementation plan request", events, ExecutionMode::SANE, {}, false, true);
    EXPECT_EQ(events.back(), AgentEventType::EXECUTION_CANCELLED);
    EXPECT_FALSE(branch->implementationPlanReady());
    for (const auto& message : branch->getMessages()) EXPECT_FALSE(message.implementationPlan);
}

TEST_F(SessionTest, ImplementationPlanRejectsEmptyStepsSubstepsAndValidation) {
    const json empty{{"steps", json::array()}};
    EXPECT_THROW(empty.get<ImplementationPlan>(), std::invalid_argument);
    for (const auto* field : {"substeps", "validation"}) {
        auto invalid = implementationPlan();
        invalid["steps"][0][field] = json::array();
        EXPECT_THROW(invalid.get<ImplementationPlan>(), std::invalid_argument);
    }
    auto invalid = implementationPlan();
    invalid["steps"][0]["substeps"][0]["intention"] = "   ";
    EXPECT_THROW(invalid.get<ImplementationPlan>(), std::invalid_argument);
    EXPECT_EQ(json(implementationPlan().get<ImplementationPlan>()), implementationPlan());
}

TEST_F(SessionTest, ImplementationPreparationPersistsAndResumesAfterInterruption) {
    const auto path = directory / "preparing-run";
    fs::create_directory(path);
    ImplementationRun run(path);
    run.start(implementationPlan().get<ImplementationPlan>(), "step_by_step");
    EXPECT_TRUE(run.needsCheckout());
    EXPECT_TRUE(run.view().at("preparing"));
    EXPECT_TRUE(run.view().at("steps")[0].at("preparing"));
    EXPECT_FALSE(run.view().at("steps")[1].at("preparing"));
    EXPECT_FALSE(run.view().at("steps")[0].at("substeps")[0].at("started"));
    EXPECT_THROW(run.start(implementationPlan().get<ImplementationPlan>(), "step_by_step"), std::logic_error);
    ImplementationRun restored(path);
    restored.recover({});
    EXPECT_EQ(restored.status, "interrupted");
    EXPECT_FALSE(restored.view().at("preparing"));
    EXPECT_TRUE(restored.needsCheckout());
    restored.resume();
    EXPECT_TRUE(restored.view().at("preparing"));
    restored.checkoutPrepared();
    EXPECT_FALSE(ImplementationRun(path).needsCheckout());
    TurnSnapshot snapshot; snapshot.id = 1;
    restored.begin(snapshot);
    EXPECT_FALSE(restored.view().at("preparing"));
}

TEST_F(SessionTest, PreviousImplementationFormatsAreRejectedWithoutRewritingState) {
    const auto path = directory / "run";
    fs::create_directory(path);
    ImplementationRun(path).start(implementationPlan().get<ImplementationPlan>(), "step_by_step");
    SessionDatabase db(path / "implementation.sqlite");
    SessionDatabase::Statement read(db, "SELECT payload FROM implementation_state WHERE id=1");
    ASSERT_TRUE(read.step());
    const auto current = json::parse(read.text(0));
    for (int version = 1; version < 7; ++version) {
        auto previous = current;
        previous["version"] = version;
        const auto payload = previous.dump();
        SessionDatabase::Statement write(db, "UPDATE implementation_state SET payload=? WHERE id=1");
        write.bind(1, payload); write.step();
        EXPECT_THROW(ImplementationRun{path}, std::runtime_error);
        SessionDatabase::Statement unchanged(db, "SELECT payload FROM implementation_state WHERE id=1");
        ASSERT_TRUE(unchanged.step());
        EXPECT_EQ(unchanged.text(0), payload);
    }
}

TEST_F(SessionTest, CurrentImplementationStateRequiresStructuralFieldsWithoutRewritingIt) {
    const auto path = directory / "run";
    fs::create_directory(path);
    ImplementationRun(path).start(implementationPlan().get<ImplementationPlan>(), "step_by_step");
    SessionDatabase db(path / "implementation.sqlite");
    SessionDatabase::Statement read(db, "SELECT payload FROM implementation_state WHERE id=1");
    ASSERT_TRUE(read.step());
    const auto current = json::parse(read.text(0));
    const auto rejected = [&](const json& incomplete) {
        const auto payload = incomplete.dump();
        SessionDatabase::Statement write(db, "UPDATE implementation_state SET payload=? WHERE id=1");
        write.bind(1, payload); write.step();
        EXPECT_THROW(ImplementationRun{path}, json::out_of_range);
        SessionDatabase::Statement unchanged(db, "SELECT payload FROM implementation_state WHERE id=1");
        ASSERT_TRUE(unchanged.step());
        EXPECT_EQ(unchanged.text(0), payload);
    };
    for (const auto* field : {"review_branch", "checkout_pending", "active_turn", "boundary_cursor", "activity_substep",
        "revision", "proposed_rework", "accepted_rework", "revision_history", "revision_sources", "revision_archive",
        "revision_planning", "revision_branch", "plan_versions"}) {
        auto incomplete = current;
        incomplete.erase(field);
        rejected(incomplete);
    }
    for (const auto* field : {"validation", "generation", "identity", "display_revision", "progress_boundary", "conclusion", "review_branch"}) {
        auto incomplete = current;
        incomplete["steps"][0].erase(field);
        rejected(incomplete);
    }
    EXPECT_FALSE(current["steps"][0].contains("captured"));
}

TEST_F(SessionTest, CurrentSnapshotsRequireWorkspaceHistory) {
    TurnSnapshot snapshot;
    snapshot.workspaces = {{"/other", "before", "after"}};
    auto encoded = json(snapshot);
    EXPECT_EQ(json(encoded.get<TurnSnapshot>()), encoded);
    encoded.erase("workspaces");
    EXPECT_THROW(encoded.get<TurnSnapshot>(), json::out_of_range);
}

TEST_F(SessionTest, PreviousBoundaryStorageIsRejectedWithoutChangingSchemaOrEvidence) {
    const auto path = directory / "run";
    fs::create_directory(path);
    SessionDatabase db(path / "implementation.sqlite");
    db.execute("CREATE TABLE implementation_state (id INTEGER PRIMARY KEY, payload TEXT NOT NULL);"
        "INSERT INTO implementation_state VALUES(1,'{\"version\":6}');"
        "CREATE TABLE implementation_boundaries (id INTEGER PRIMARY KEY, step INTEGER NOT NULL, payload TEXT NOT NULL);"
        "INSERT INTO implementation_boundaries VALUES(1,0,'preserved evidence');");
    const auto schema = [&] {
        SessionDatabase::Statement read(db, "SELECT sql FROM sqlite_schema ORDER BY name");
        std::string result;
        while (read.step()) result += read.text(0) + "\n";
        return result;
    };
    const auto original = schema();
    EXPECT_THROW(ImplementationRun{path}, std::runtime_error);
    EXPECT_THROW(ImplementationBoundaries{path}, std::runtime_error);
    EXPECT_EQ(schema(), original);
    // Even a current state cannot silently upgrade an old boundary table.
    db.execute("UPDATE implementation_state SET payload='{\"version\":7}' WHERE id=1");
    EXPECT_THROW(ImplementationRun{path}, std::runtime_error);
    EXPECT_EQ(schema(), original);
    SessionDatabase::Statement evidence(db, "SELECT payload FROM implementation_boundaries WHERE id=1");
    ASSERT_TRUE(evidence.step());
    EXPECT_EQ(evidence.text(0), "preserved evidence");
}

TEST_F(SessionTest, RevisionArchivesRequireExplicitPlanAndReworkedStep) {
    ImplementationRevisionArchive archive{implementationPlan().get<ImplementationPlan>(),
        {ImplementationStepState{}}, "Repair", 1, 2, 0};
    const auto current = json(archive);
    EXPECT_EQ(json(current.get<ImplementationRevisionArchive>()), current);
    for (const auto* field : {"plan_number", "reworked_step"}) {
        auto incomplete = current;
        incomplete.erase(field);
        EXPECT_THROW(incomplete.get<ImplementationRevisionArchive>(), json::out_of_range);
    }
    auto invalid = current;
    invalid["plan_number"] = 0;
    EXPECT_THROW(invalid.get<ImplementationRevisionArchive>(), std::invalid_argument);
    invalid = current;
    invalid["reworked_step"] = 1;
    EXPECT_THROW(invalid.get<ImplementationRevisionArchive>(), std::invalid_argument);
}

TEST_F(SessionTest, ImplementationDiffRequiresContentsButNotHunkOrder) {
    const auto current = json(ImplementationDiff{"empty", "", "", ""});
    EXPECT_TRUE(current.get<ImplementationDiff>().hunkOrder.empty());
    for (const auto* field : {"before", "after"}) {
        auto incomplete = current;
        incomplete.erase(field);
        EXPECT_THROW(incomplete.get<ImplementationDiff>(), json::out_of_range);
    }
}

TEST_F(SessionTest, CheckoutRequiresExplicitPrivateRepositoryEvenWhenEmpty) {
    const auto path = directory / "checkout";
    fs::create_directory(path);
    ImplementationWorktree initial(path);
    SessionDatabase db(path / "worktree.sqlite");
    const json current{{"version", 1}, {"root", ""}, {"workspace", ""}, {"worktree", ""},
        {"branch", ""}, {"head", ""}, {"private_repository", ""}, {"ready", false}, {"closed", false}};
    const auto store = [&](const json& value) {
        SessionDatabase::Statement write(db, "INSERT OR REPLACE INTO checkout(id,payload) VALUES(1,?)");
        write.bind(1, value.dump()); write.step();
    };
    store(current);
    EXPECT_NO_THROW(ImplementationWorktree{path});
    auto incomplete = current;
    incomplete.erase("private_repository");
    store(incomplete);
    EXPECT_THROW(ImplementationWorktree{path}, json::out_of_range);
    SessionDatabase::Statement read(db, "SELECT payload FROM checkout WHERE id=1");
    ASSERT_TRUE(read.step());
    EXPECT_EQ(read.text(0), incomplete.dump());
}

TEST_F(SessionTest, ImplementationCompletionRejectsHunkAssignments) {
    const json assignment{{"explanation", "State"}, {"changes", json::array()}};
    EXPECT_THROW(assignment.get<ImplementationSubstepResult>(), std::invalid_argument);
    EXPECT_TRUE(json::object().get<ImplementationSubstepResult>().explanation.empty());
}

TEST_F(SessionTest, ImplementationCapturedOwnershipSurvivesAdjacentHunksAndTargetedRepair) {
    const auto diff = [](const std::string& before, const std::string& after) {
        return ImplementationDiff{"model.cpp", fs_tools::diff("model.cpp", before, after), before, after};
    };
    const std::string before = "base model\nbase wiring\n";
    const std::string first = "broken model\nbase wiring\n";
    const std::string constructed = "broken model\nworking wiring\n";
    const std::string corrected = "working model\nworking wiring\n";
    ImplementationBoundary model, wiring, repair;
    model.owner = 0; model.diffs = {diff(before, first)};
    wiring.owner = 1; wiring.diffs = {diff(first, constructed)};
    repair.owner = 0; repair.validating = true; repair.diffs = {diff(constructed, corrected)};
    const auto final = diff(before, corrected);
    ASSERT_EQ(implementationChangeEvidence({final})[0]["hunks"].size(), 1); // Must not collapse the two intentions.
    const auto parts = preserveImplementationOwnership({model, wiring, repair}, {final}, 2);
    ASSERT_EQ(parts[0].size(), 1); ASSERT_EQ(parts[1].size(), 1);
    EXPECT_EQ(parts[0][0].before, before);
    EXPECT_EQ(parts[0][0].after, "working model\nbase wiring\n");
    EXPECT_EQ(parts[1][0].before, parts[0][0].after);
    EXPECT_EQ(parts[1][0].after, corrected);
    EXPECT_EQ(parts[0][0].patch.find("broken"), std::string::npos);
    EXPECT_THROW(preserveImplementationOwnership({model, wiring}, {final}, 2), std::runtime_error);
}

TEST_F(SessionTest, ImplementationCapturedOwnershipPreservesDeletionNoNewlineAndMetadata) {
    const ImplementationDiff first{"tail", fs_tools::diff("tail", "old", "new"), "old", "new"};
    const ImplementationDiff repair{"tail", fs_tools::diff("tail", "new", "corrected"), "new", "corrected"};
    const ImplementationDiff final{"tail", fs_tools::diff("tail", "old", "corrected"), "old", "corrected"};
    ImplementationBoundary start, validation;
    start.owner = validation.owner = 0; start.diffs = {first}; validation.diffs = {repair};
    const auto result = preserveImplementationOwnership({start, validation}, {final}, 2);
    EXPECT_EQ(result[0][0].after, "corrected"); EXPECT_TRUE(result[1].empty());
    ImplementationBoundary deletion; deletion.owner = 1;
    const ImplementationDiff removed{"tail", fs_tools::diff("tail", "corrected", ""), "corrected", ""};
    deletion.diffs = {removed};
    const auto gone = preserveImplementationOwnership({start, validation, deletion},
        {{"tail", fs_tools::diff("tail", "old", ""), "old", ""}}, 2);
    EXPECT_EQ(gone[0][0].after, ""); // Original deletion keeps its construction owner.
    EXPECT_TRUE(gone[1].empty());
    ImplementationBoundary binary; binary.owner = 1;
    binary.diffs = {{"image", "Binary contents changed (preserved in snapshot blobs).\n", "", ""}};
    const auto metadata = preserveImplementationOwnership({binary}, binary.diffs, 2);
    EXPECT_TRUE(metadata[0].empty()); EXPECT_EQ(metadata[1][0].patch, binary.diffs[0].patch);
}

TEST_F(SessionTest, ImplementationNewFileRewritePreservesOrderAndReplacesItsExplanation) {
    const auto diff = [](const std::string& before, const std::string& after, const std::string& explanation) {
        return ImplementationDiff{"new.cpp", fs_tools::diff("new.cpp", before, after), before, after, {}, {explanation}};
    };
    ImplementationBoundary initial, corrected;
    initial.owner = corrected.owner = 0;
    initial.diffs = {diff("", "first", "Initial new file")};
    corrected.diffs = {diff("first", "corrected", "Complete corrected new file")};
    const auto original = preserveImplementationOwnership({initial}, initial.diffs, 1)[0][0];
    const auto result = preserveImplementationOwnership({initial, corrected}, {diff("", "corrected", "")}, 1)[0][0];
    EXPECT_EQ(result.hunkOrder, original.hunkOrder);
    EXPECT_EQ(result.hunkExplanations, (std::vector<std::string>{"Complete corrected new file"}));
    ImplementationBoundary removed;
    removed.owner = 0; removed.diffs = {diff("old\n", "", "Remove the obsolete file")};
    EXPECT_EQ(preserveImplementationOwnership({removed}, removed.diffs, 1)[0][0].hunkExplanations,
        (std::vector<std::string>{"Remove the obsolete file"}));
    ImplementationBoundary binary;
    binary.owner = 0; binary.diffs = {{"image", "Binary contents changed", "", "", {}, {"Update the image"}}};
    EXPECT_EQ(preserveImplementationOwnership({binary}, binary.diffs, 1)[0][0].hunkExplanations,
        (std::vector<std::string>{"Update the image"}));
}

namespace {
class ImplementationDiffOrderTest : public ::testing::Test {
protected:
    ImplementationDiffOrderTest() {
        for (unsigned i = 0; i < 90; ++i) original += "line " + std::to_string(i) + "\n";
    }
    void replace(const std::string& path, const std::string& before, const std::string& after, std::size_t owner = 0) {
        auto [entry, inserted] = current.try_emplace(path, original);
        auto updated = entry->second;
        const auto position = updated.find(before);
        ASSERT_NE(position, std::string::npos);
        updated.replace(position, before.size(), after);
        ImplementationBoundary boundary;
        boundary.owner = owner;
        boundary.diffs = {{path, fs_tools::diff(path, entry->second, updated), entry->second, updated}};
        evidence.push_back(std::move(boundary));
        entry->second = std::move(updated);
    }
    std::vector<ImplementationDiff> result(std::size_t owner = 0, std::size_t count = 1) const {
        std::vector<ImplementationDiff> final;
        for (const auto& [path, after] : current) if (after != original)
            final.push_back({path, fs_tools::diff(path, original, after), original, after});
        return preserveImplementationOwnership(evidence, final, count).at(owner);
    }
    ImplementationDiff file(const std::vector<ImplementationDiff>& diffs, const std::string& path) const {
        const auto found = std::find_if(diffs.begin(), diffs.end(), [&](const auto& diff) { return diff.path == path; });
        if (found == diffs.end()) throw std::runtime_error("Missing test contribution: " + path);
        return *found;
    }
    std::string original;
    std::map<std::string, std::string> current;
    std::vector<ImplementationBoundary> evidence;
};
}

TEST_F(ImplementationDiffOrderTest, FollowsMutationsAcrossFilesAndHunksRatherThanPathOrLineOrder) {
    replace("z.cpp", "line 70\n", "late first\n");
    replace("a.cpp", "line 40\n", "other second\n");
    replace("z.cpp", "line 5\n", "early third\n");
    const auto diffs = result();
    const auto z = file(diffs, "z.cpp"), a = file(diffs, "a.cpp");
    ASSERT_EQ(z.hunkOrder.size(), 2); ASSERT_EQ(a.hunkOrder.size(), 1);
    EXPECT_LT(z.hunkOrder[1], a.hunkOrder[0]);
    EXPECT_LT(a.hunkOrder[0], z.hunkOrder[0]);
    const auto restored = json(diffs).get<std::vector<ImplementationDiff>>();
    EXPECT_EQ(file(restored, "z.cpp").hunkOrder, z.hunkOrder);
    EXPECT_TRUE((json{{"path", "empty"}, {"patch", ""}, {"before", ""}, {"after", ""}}.get<ImplementationDiff>().hunkOrder.empty()));
}

TEST_F(ImplementationDiffOrderTest, ValidationRepairsKeepTheirPositionWhileIndependentHunksAppend) {
    replace("z.cpp", "line 70\n", "late first\n");
    replace("a.cpp", "line 40\n", "other second\n");
    replace("z.cpp", "line 5\n", "early third\n");
    const auto before = result();
    replace("z.cpp", "late first\n", "corrected first\n"); evidence.back().validating = true;
    replace("a.cpp", "line 10\n", "new validation change\n"); evidence.back().validating = true;
    const auto after = result();
    EXPECT_EQ(file(after, "z.cpp").hunkOrder, file(before, "z.cpp").hunkOrder);
    const auto a = file(after, "a.cpp");
    ASSERT_EQ(a.hunkOrder.size(), 2);
    EXPECT_EQ(a.hunkOrder[1], file(before, "a.cpp").hunkOrder[0]);
    EXPECT_GT(a.hunkOrder[0], file(after, "z.cpp").hunkOrder[0]);
}

TEST_F(ImplementationDiffOrderTest, NearbyRepairBridgesHunksAtTheOldestPosition) {
    replace("z.cpp", "line 30\n", "first\n");
    replace("a.cpp", "line 10\n", "second\n");
    replace("z.cpp", "line 40\n", "third\n");
    const auto before = result();
    const auto z = file(before, "z.cpp");
    ASSERT_EQ(z.hunkOrder.size(), 2);
    replace("z.cpp", "line 35\n", "validation bridge\n"); evidence.back().validating = true;
    const auto after = result();
    const auto merged = file(after, "z.cpp");
    ASSERT_EQ(merged.hunkOrder.size(), 1);
    EXPECT_EQ(merged.hunkOrder[0], z.hunkOrder[0]);
    EXPECT_LT(merged.hunkOrder[0], file(after, "a.cpp").hunkOrder[0]);
}

TEST_F(ImplementationDiffOrderTest, ExplanationsFollowHunksAndCorrectionsReplaceTheirWholeSynthesis) {
    replace("file.cpp", "line 5\n", "first change\n");
    evidence.back().diffs[0].hunkExplanations = {"First intention"};
    replace("file.cpp", "line 70\n", "second change\n");
    evidence.back().diffs[0].hunkExplanations = {"Second intention"};
    const auto before = file(result(), "file.cpp");
    EXPECT_EQ(before.hunkExplanations, (std::vector<std::string>{"First intention", "Second intention"}));
    replace("file.cpp", "first change\n", "corrected first change\n");
    evidence.back().diffs[0].hunkExplanations = {"Complete corrected first intention"};
    const auto corrected = file(result(), "file.cpp");
    EXPECT_EQ(corrected.hunkExplanations, (std::vector<std::string>{"Complete corrected first intention", "Second intention"}));
    EXPECT_EQ(json(corrected).get<ImplementationDiff>().hunkExplanations, corrected.hunkExplanations);
    EXPECT_EQ(before.hunkExplanations[0], "First intention");
}

TEST_F(ImplementationDiffOrderTest, MergedHunksJoinTheirExplanationsWithNewlines) {
    replace("file.cpp", "line 30\n", "first\n");
    evidence.back().diffs[0].hunkExplanations = {"First intention"};
    replace("file.cpp", "line 40\n", "second\n");
    evidence.back().diffs[0].hunkExplanations = {"Second intention"};
    replace("file.cpp", "line 35\n", "bridge\n");
    evidence.back().diffs[0].hunkExplanations = {"Bridge intention"};
    const auto merged = file(result(), "file.cpp");
    EXPECT_EQ(merged.hunkExplanations, (std::vector<std::string>{"First intention\nSecond intention\nBridge intention"}));
}

TEST_F(ImplementationDiffOrderTest, RepeatedExplanationsAndOtherSubstepsDoNotPolluteAHunk) {
    replace("file.cpp", "line 5\n", "first\n", 0);
    evidence.back().diffs[0].hunkExplanations = {"Rename the variable"};
    replace("file.cpp", "line 7\n", "second\n", 0);
    evidence.back().diffs[0].hunkExplanations = {"Rename the variable"};
    replace("file.cpp", "line 6\n", "wiring\n", 1);
    evidence.back().diffs[0].hunkExplanations = {"Wire the result"};
    EXPECT_EQ(file(result(0, 2), "file.cpp").hunkExplanations, (std::vector<std::string>{"Rename the variable"}));
    EXPECT_EQ(file(result(1, 2), "file.cpp").hunkExplanations, (std::vector<std::string>{"Wire the result"}));
}

TEST_F(ImplementationDiffOrderTest, RevertedZoneDisappearsAndRecreatedZoneGetsANewPosition) {
    replace("z.cpp", "line 70\n", "first\n");
    replace("z.cpp", "line 5\n", "second\n");
    replace("a.cpp", "line 40\n", "third\n");
    const auto before = file(result(), "z.cpp");
    replace("z.cpp", "first\n", "line 70\n");
    ASSERT_EQ(file(result(), "z.cpp").hunkOrder.size(), 1);
    replace("z.cpp", "line 70\n", "recreated\n");
    const auto after = result();
    const auto z = file(after, "z.cpp");
    ASSERT_EQ(z.hunkOrder.size(), 2);
    EXPECT_EQ(z.hunkOrder[0], before.hunkOrder[0]);
    EXPECT_GT(z.hunkOrder[1], file(after, "a.cpp").hunkOrder[0]);
}

TEST_F(ImplementationDiffOrderTest, EarlierSubstepInsertionsDoNotReorderLaterContributions) {
    replace("z.cpp", "line 0\n", "prefix\nline 0\n", 0);
    replace("z.cpp", "line 70\n", "later substep first\n", 1);
    replace("a.cpp", "line 40\n", "later substep second\n", 1);
    const auto before = result(1, 2);
    replace("z.cpp", "prefix\n", "longer\nprefix\n", 0);
    const auto after = result(1, 2);
    EXPECT_EQ(file(after, "z.cpp").hunkOrder, file(before, "z.cpp").hunkOrder);
    EXPECT_EQ(file(after, "a.cpp").hunkOrder, file(before, "a.cpp").hunkOrder);
    EXPECT_LT(file(after, "z.cpp").hunkOrder[0], file(after, "a.cpp").hunkOrder[0]);
}

TEST_F(ImplementationDiffOrderTest, DistinguishesDistantHunksInsideTextInsertedByAnEarlierSubstep) {
    std::string prefix;
    for (unsigned i = 0; i < 100; ++i) prefix += "inserted " + std::to_string(i) + "\n";
    replace("z.cpp", "line 0\n", prefix + "line 0\n", 0);
    replace("z.cpp", "inserted 70\n", "later substep first\n", 1);
    replace("a.cpp", "line 40\n", "later substep second\n", 1);
    replace("z.cpp", "inserted 5\n", "later substep third\n", 1);
    const auto diffs = result(1, 2);
    const auto z = file(diffs, "z.cpp"), a = file(diffs, "a.cpp");
    ASSERT_EQ(z.hunkOrder.size(), 2); ASSERT_EQ(a.hunkOrder.size(), 1);
    EXPECT_LT(z.hunkOrder[1], a.hunkOrder[0]);
    EXPECT_LT(a.hunkOrder[0], z.hunkOrder[0]);
}

TEST_F(SessionTest, ImplementationBoundariesCaptureBeforeNextToolAndRecoverValidationActivity) {
    const auto path = directory / "boundaries"; fs::create_directory(path);
    const auto target = directory / "model.txt";
    std::ofstream(target) << "base model\nbase wiring\n";
    TurnFiles files(path); files.begin(files.prepare(directory, directory));
    ImplementationRun run(path); run.start(implementationPlan().get<ImplementationPlan>(), "step_by_step");
    run.begin(files.boundary());
    const auto progress = [&](std::optional<std::size_t> index, const std::string& status, const std::string& explanation = "- Explained") {
        return ImplementationBoundaries(path).capture(0, 2, {index, status, explanation}, files.boundary(), 0, 0, {}, true);
    };
    progress(0, "started");
    files.mutate({target}, [&] { std::ofstream(target) << "broken model\nbase wiring\n"; });
    const auto transitioned = progress(0, "completed");
    const auto activated = ImplementationBoundaries(path).read(0).back();
    EXPECT_EQ(activated.id, transitioned);
    EXPECT_EQ(activated.progress.status, "started");
    EXPECT_EQ(activated.activeSubstep, 1);
    EXPECT_TRUE(activated.diffs.empty());
    EXPECT_FALSE(activated.owner);
    EXPECT_TRUE(ImplementationRun(path).view()["steps"][0]["substeps"][1]["started"]);
    EXPECT_NO_THROW(ImplementationBoundaries(path).requireActiveSubstep(0));
    files.mutate({target}, [&] { std::ofstream(target) << "broken model\nworking wiring\n"; });
    progress(1, "completed");
    EXPECT_TRUE(ImplementationBoundaries(path).read(0).back().validating);
    EXPECT_THROW(ImplementationBoundaries(path).requireActiveSubstep(0), std::logic_error);
    EXPECT_NO_THROW(ImplementationBoundaries(path).requireActiveSubstep(0, true));
    progress(0, "correcting");
    files.mutate({target}, [&] { std::ofstream(target) << "working model\nworking wiring\n"; });
    progress(0, "completed", "- Corrected the model; targeted checks passed.");
    EXPECT_FALSE(ImplementationBoundaries(path).read(0).back().activeSubstep);
    const auto finished = progress({}, "finished");
    // Boundary writes survive even if the Asio consumer has not received them yet.
    ImplementationRun recovered(path);
    EXPECT_TRUE(recovered.view()["steps"][0]["validation"]["started"]);
    ToolResultContent check{"check", "Tests passed"}; check.metadata = {{"implementation_boundary", finished}};
    recovered.recordActivity({Message{"checks", MessageRole::TOOL, {check}}});
    EXPECT_EQ(recovered.view()["steps"][0]["validation"]["activity"].back()["id"], "checks");
    files.finish();
    const auto snapshot = files.boundary();
    // A bogus old-style assignment must not steal the captured contribution.
    const ImplementationCompletion completion{"Checks passed", {{""}, {"Wiring"}}};
    recovered.finish(snapshot, completion, files.diffs(snapshot), true);
    const auto persisted = ImplementationRun(path).view();
    EXPECT_TRUE(persisted["steps"][0]["validation"]["complete"]);
    EXPECT_EQ(persisted["steps"][0]["substeps"][0]["explanation"], "- Corrected the model; targeted checks passed.");
    EXPECT_EQ(persisted["steps"][0]["substeps"][1]["explanation"], "Wiring");
    EXPECT_EQ(persisted["steps"][0]["substeps"][0]["diffs"][0]["after"], "working model\nbase wiring\n");
    EXPECT_EQ(persisted["steps"][0]["substeps"][1]["diffs"][0]["after"], "working model\nworking wiring\n");
}

TEST_F(SessionTest, ImplementationAutomaticTransitionsSkipObsoleteWorkWithoutCompletingTheStep) {
    const auto path = directory / "automatic-transitions"; fs::create_directory(path);
    TurnFiles files(path); files.begin(files.prepare(directory, directory));
    ImplementationBoundaries boundaries(path);
    const auto progress = [&](std::optional<std::size_t> index, const std::string& status) {
        return boundaries.capture(0, 3, {index, status, "Explained"}, files.boundary(), 0, 0, {}, true);
    };
    progress({}, "activate");
    const auto before = boundaries.read(0).size();
    EXPECT_EQ(boundaries.read(0).back().activeSubstep, 0);
    EXPECT_NO_THROW(boundaries.requireActiveSubstep(0));
    progress({}, "activate");
    EXPECT_EQ(boundaries.read(0).size(), before);
    EXPECT_THROW(progress(1, "completed"), std::logic_error);
    EXPECT_EQ(boundaries.read(0).size(), before);
    progress(0, "completed");
    EXPECT_EQ(boundaries.read(0).back().activeSubstep, 1);
    progress(1, "unnecessary");
    EXPECT_EQ(ImplementationBoundaries(path).read(0).back().activeSubstep, 2);
    progress(2, "completed");
    const auto evidence = ImplementationBoundaries(path).read(0);
    EXPECT_EQ(evidence.back().progress.status, "validating");
    EXPECT_FALSE(evidence.back().activeSubstep);
    EXPECT_TRUE(evidence.back().diffs.empty());
    EXPECT_NE(evidence.back().progress.status, "finished");
    EXPECT_THROW(boundaries.requireActiveSubstep(0), std::logic_error);
    files.finish();
}

TEST_F(SessionTest, ImplementationMutationChronologySurvivesPartialFailureAndRecovery) {
    const auto path = directory / "mutation-order"; fs::create_directory(path);
    const auto a = directory / "a.txt", z = directory / "z.txt";
    std::ofstream(a) << "base\n"; std::ofstream(z) << "base\n";
    TurnFiles files(path); files.begin(files.prepare(directory, directory));
    auto plan = implementationPlan().get<ImplementationPlan>();
    plan.steps.resize(1); plan.steps[0].substeps.resize(1);
    ImplementationRun run(path); run.start(plan, "step_by_step"); run.begin(files.boundary());
    ImplementationBoundaries boundaries(path);
    boundaries.capture(0, 1, {0, "started", ""}, files.boundary());
    const auto captured = [&](const TurnSnapshot& snapshot) {
        boundaries.capture(0, 1, {{}, "mutation", ""}, snapshot, 0, 0, {z.string(), a.string()});
    };
    files.mutate({z, a}, [&] { std::ofstream(z) << "first\n"; std::ofstream(a) << "second\n"; }, captured);
    auto evidence = boundaries.read(0);
    ASSERT_EQ(evidence.size(), 2);
    ASSERT_EQ(evidence.back().diffs.size(), 2);
    EXPECT_EQ(evidence.back().diffs[0].path, fs::canonical(z).string());
    EXPECT_EQ(evidence.back().diffs[1].path, fs::canonical(a).string());
    files.mutate({}, [] {}, captured);
    EXPECT_EQ(boundaries.read(0).size(), 2); // Checks without edits create no chronology entries.
    EXPECT_THROW(files.mutate({z}, [&] { std::ofstream(z) << "repaired\n"; throw std::runtime_error("partial tool"); }, captured), std::runtime_error);
    ASSERT_EQ(ImplementationBoundaries(path).read(0).size(), 3);
    boundaries.capture(0, 1, {0, "completed", "Built"}, files.boundary());
    boundaries.capture(0, 1, {{}, "validating", ""}, files.boundary());
    boundaries.capture(0, 1, {{}, "finished", "Checked"}, files.boundary());
    files.finish();
    const auto snapshot = files.boundary();
    ImplementationRun restored(path);
    restored.finish(snapshot, {"Checked", {{"Built"}}}, files.diffs(snapshot), true);
    const auto saved = ImplementationRun(path).view()["steps"][0]["substeps"][0]["diffs"].get<std::vector<ImplementationDiff>>();
    ASSERT_EQ(saved.size(), 2);
    const auto first = std::find_if(saved.begin(), saved.end(), [&](const auto& diff) { return diff.path == fs::canonical(z).string(); });
    const auto second = std::find_if(saved.begin(), saved.end(), [&](const auto& diff) { return diff.path == fs::canonical(a).string(); });
    ASSERT_NE(first, saved.end()); ASSERT_NE(second, saved.end());
    ASSERT_EQ(first->hunkOrder.size(), 1); ASSERT_EQ(second->hunkOrder.size(), 1);
    EXPECT_LT(first->hunkOrder[0], second->hunkOrder[0]);
    EXPECT_EQ(first->after, "repaired\n");
}

TEST_F(SessionTest, AcceptedReviewTransfersOnlyCurrentSegmentOnceAndSurvivesResume) {
    SessionRegistry registry(directory / "sessions", factory());
    auto session = registry.create(info());
    const auto id = session->getInfo().id;
    BtwConversation branch; branch.review = true;
    branch.messages = {{"user", MessageRole::USER, {TextContent{"FIRST_DISCUSSION"}}},
        {"answer", MessageRole::ASSISTANT, {TextContent{"FIRST_DECISION"}}}};
    branch = session->btwStore()->createBtw(branch);
    const auto before = session->messageCount();
    EXPECT_EQ(session->messageCount(), before); // Discussion alone is not construction.
    SessionDatabase fault(directory / "sessions" / id / "session.sqlite");
    fault.execute("CREATE TRIGGER fail_review_boundary BEFORE UPDATE ON btw BEGIN SELECT RAISE(ABORT,'injected boundary failure'); END");
    EXPECT_THROW(session->incorporateAcceptedReview(branch.id, "Accepted correction"), std::runtime_error);
    EXPECT_EQ(session->messageCount(), before);
    EXPECT_EQ(session->btwStore()->readBtw(branch.id).reviewSegmentStart, 0);
    fault.execute("DROP TRIGGER fail_review_boundary");
    session->incorporateAcceptedReview(branch.id, "Accepted correction");
    EXPECT_EQ(session->messageCount(), before + 1);
    session->incorporateAcceptedReview(branch.id, "Accepted correction");
    EXPECT_EQ(session->messageCount(), before + 1);
    branch = session->btwStore()->readBtw(branch.id);
    EXPECT_EQ(branch.reviewSegmentStart, branch.messages.size());
    EXPECT_TRUE(branch.activeContext.empty());
    EXPECT_TRUE(currentReviewMessages(branch).empty());
    branch.messages.push_back({"next", MessageRole::USER, {TextContent{"SECOND_DISCUSSION"}}});
    session->btwStore()->saveBtw(branch);
    session.reset(); session = registry.load(id);
    session->incorporateAcceptedReview(branch.id, "Second accepted correction");
    ASSERT_EQ(session->messageCount(), before + 2);
    const auto messages = session->getHistory();
    const auto last = json(messages.back()).dump();
    EXPECT_NE(last.find("SECOND_DISCUSSION"), std::string::npos);
    EXPECT_EQ(last.find("FIRST_DISCUSSION"), std::string::npos);
    EXPECT_NE(json(messages[messages.size() - 2]).dump().find("FIRST_DECISION"), std::string::npos);
    EXPECT_EQ(session->btwStore()->readBtw(branch.id).messages.size(), 5);
}

TEST_F(SessionTest, ImplementationReworkPreservesBaselineOwnershipAndRequiresFreshAcceptance) {
    const auto path = directory / "rework"; fs::create_directory(path);
    const auto target = directory / "rework.txt";
    std::ofstream(target) << "base model\nbase wiring\n";
    ImplementationRun run(path); run.start(implementationPlan().get<ImplementationPlan>(), "step_by_step");
    TurnFiles first(path); first.begin(first.prepare(directory, directory)); run.begin(first.boundary());
    ImplementationBoundaries boundaries(path);
    const auto report = [&](TurnSnapshot snapshot, std::optional<std::size_t> index, const std::string& status, std::int64_t after = 0) {
        return boundaries.capture(0, 2, {index, status, "Explained"}, snapshot, 0, after);
    };
    report(first.boundary(), 0, "started");
    first.mutate({target}, [&] { std::ofstream(target) << "old model\nbase wiring\n"; });
    report(first.boundary(), 0, "completed"); report(first.boundary(), 1, "started");
    first.mutate({target}, [&] { std::ofstream(target) << "old model\nworking wiring\n"; });
    report(first.boundary(), 1, "completed"); report(first.boundary(), {}, "validating"); report(first.boundary(), {}, "finished");
    first.finish();
    const ImplementationCompletion completion{"Built", {{"Model"}, {"Wiring"}}};
    run.finish(first.boundary(), completion, first.diffs(first.boundary()), true);
    ASSERT_TRUE(run.canRework(0)); EXPECT_FALSE(run.canRework(1));
    run.review(0, 0, true);
    const auto before = run.revisionVersion();
    run.proposeRework(0, before, "Replace the model implementation, keep the plan");
    EXPECT_THROW(run.acceptRework(before), std::logic_error);
    ImplementationRun restored(path);
    const auto version = restored.revisionVersion();
    restored.acceptRework(version);
    EXPECT_THROW(restored.acceptRework(version), std::logic_error);
    EXPECT_FALSE(restored.view()["steps"][0]["substeps"][0]["reviewed"]);
    EXPECT_FALSE(restored.view()["steps"][0]["whole"]["complete"]);
    EXPECT_FALSE(restored.view()["steps"][0]["validation"]["started"]);
    EXPECT_TRUE(restored.view()["steps"][0]["whole"]["explanation"].get<std::string>().empty());
    for (const auto& part : restored.view()["steps"][0]["substeps"]) {
        EXPECT_FALSE(part["started"]); EXPECT_FALSE(part["announced"]); EXPECT_FALSE(part["finalized"]);
    }
    ASSERT_EQ(restored.view()["steps"][0]["previous_versions"].size(), 1);
    ASSERT_EQ(restored.view()["steps"][0]["construction_history"].size(), 1);
    EXPECT_EQ(restored.view()["steps"][0]["construction_history"][0]["explanation"], "Built");
    EXPECT_EQ(restored.view()["steps"][0]["construction_history"][0]["substeps"][0]["explanation"], "Model");
    EXPECT_EQ(restored.view()["steps"][0]["previous_versions"][0]["title"], "Testable foundation");
    EXPECT_EQ(restored.view()["version_count"], 2);
    EXPECT_EQ(restored.view()["steps"][0]["correction_request"], "Replace the model implementation, keep the plan");
    const auto archived = restored.versions();
    ASSERT_EQ(archived.size(), 1);
    EXPECT_EQ(archived[0]["plan"], implementationPlan());
    EXPECT_EQ(archived[0]["steps"].size(), restored.view()["steps"].size());
    EXPECT_EQ(archived[0]["steps"][0]["whole"]["explanation"], "Built");
    EXPECT_TRUE(archived[0]["steps"][0]["whole"]["complete"]);
    EXPECT_FALSE(archived[0]["steps"][1]["whole"]["complete"]);
    EXPECT_EQ(archived[0]["version_request"], "");
    EXPECT_EQ(archived[0]["steps"][0]["correction_request"], "");
    EXPECT_EQ(archived[0]["accepted_plan_markdown"], implementationPlan().get<ImplementationPlan>().markdown());
    EXPECT_FALSE(archived[0]["steps"][0]["substeps"][0].contains("snapshot"));
    EXPECT_EQ(ImplementationRun(path).versions(), archived);
    EXPECT_NE(restored.instruction().find("Replace the model implementation"), std::string::npos);
    EXPECT_NE(restored.instruction().find("submit complete_implementation_step before ending normally"), std::string::npos);
    EXPECT_NE(restored.instruction().find("prose message, audit, progress report"), std::string::npos);
    EXPECT_NE(restored.instruction().find("genuine unresolved blockers"), std::string::npos);
    restored.stop("Interrupted before corrections");
    ImplementationRun resumed(path); resumed.resume();
    TurnFiles second(path); second.begin(second.prepare(directory, directory)); resumed.begin(second.boundary());
    const auto after = resumed.currentProgressBoundary();
    const auto started = report(resumed.substepCombined(second.boundary()), 0, "correcting", after);
    ToolResultContent progress{"progress", "Correcting"}; progress.metadata = {{"implementation_boundary", started}};
    resumed.recordActivity({Message{"new-pass", MessageRole::TOOL, {progress}}});
    EXPECT_TRUE(resumed.view()["steps"][0]["substeps"][0]["started"]);
    EXPECT_FALSE(resumed.view()["steps"][0]["validation"]["started"]);
    EXPECT_EQ(resumed.view()["work_substep"], 0);
    second.mutate({target}, [&] { std::ofstream(target) << "new model\nworking wiring\n"; });
    report(resumed.substepCombined(second.boundary()), 0, "completed", after);
    EXPECT_THROW(report(resumed.substepCombined(second.boundary()), {}, "validating", after), std::logic_error);
    report(resumed.substepCombined(second.boundary()), 1, "correcting", after);
    report(resumed.substepCombined(second.boundary()), 1, "completed", after);
    report(resumed.substepCombined(second.boundary()), {}, "validating", after);
    report(resumed.substepCombined(second.boundary()), {}, "finished", after);
    second.finish();
    resumed.finish(second.boundary(), completion, second.diffs(resumed.substepCombined(second.boundary())), true);
    const auto value = ImplementationRun(path).view();
    EXPECT_EQ(value["steps"][0]["substeps"][0]["diffs"][0]["before"], "base model\nbase wiring\n");
    EXPECT_EQ(value["steps"][0]["substeps"][0]["diffs"][0]["after"], "new model\nbase wiring\n");
    EXPECT_EQ(value["steps"][0]["substeps"][1]["diffs"][0]["after"], "new model\nworking wiring\n");
    EXPECT_EQ(value["review_step"], 0);
    resumed.proposeRework(0, resumed.revisionVersion(), "Another change");
    EXPECT_THROW(resumed.review(0, 2, true), std::logic_error);
    resumed.rejectRevision(resumed.revisionVersion());
    resumed.review(0, 2, true);
    EXPECT_FALSE(resumed.canRework(0));
    EXPECT_TRUE(resumed.view()["proposed_rework"].get<std::string>().empty());
    EXPECT_THROW(resumed.acceptRework(resumed.revisionVersion()), std::logic_error);
}

TEST_F(SessionTest, ImplementationVersionsKeepRemovedStepsAndResolveOnlyPersistedRepositoryBaselines) {
    const auto path = directory / "version-baselines";
    fs::create_directory(path);
    ImplementationRun(path).start(implementationPlan().get<ImplementationPlan>(), "everything");
    SessionDatabase database(path / "implementation.sqlite");
    json payload;
    {
        SessionDatabase::Statement read(database, "SELECT payload FROM implementation_state WHERE id=1");
        ASSERT_TRUE(read.step());
        payload = json::parse(read.text(0));
    }
    auto original = payload.at("steps").get<std::vector<ImplementationStepState>>();
    ASSERT_GT(original.size(), 1);
    TurnSnapshot baseline;
    baseline.root = directory.string(); baseline.before = "archived-tree";
    original.back().substeps.front().snapshot = baseline;
    original.back().whole.complete = true;
    original.back().substeps.front().activity = {Message{"old", MessageRole::ASSISTANT, {TextContent{"Historical activity"}}}};
    const ImplementationRevisionArchive archive{payload.at("plan").get<ImplementationPlan>(), original, "Remove successor", 2, 1, 0};
    payload["revision_archive"] = std::vector{archive};
    payload["steps"].erase(payload["steps"].size() - 1);
    payload["plan"]["steps"].erase(payload["plan"]["steps"].size() - 1);
    {
        SessionDatabase::Statement write(database, "UPDATE implementation_state SET payload=? WHERE id=1");
        write.bind(1, payload.dump()); write.step();
    }
    ImplementationRun restored(path);
    const auto versions = restored.versions();
    ASSERT_EQ(versions.size(), 1);
    EXPECT_EQ(versions[0]["steps"].size(), original.size());
    EXPECT_EQ(versions[0]["steps"].back()["substeps"][0]["activity"].size(), 1);
    EXPECT_EQ(versions[0]["steps"].back()["repository_tree"], "archived-tree");
    EXPECT_EQ(restored.repositoryBaseline(original.size() - 1, "archived-tree").root, baseline.root);
    EXPECT_THROW(restored.repositoryBaseline(original.size() - 1, "current-checkout"), std::invalid_argument);
    EXPECT_THROW(restored.repositoryBaseline(original.size(), "archived-tree"), std::invalid_argument);
    EXPECT_EQ(restored.view()["steps"].size(), original.size() - 1);
    EXPECT_EQ(restored.view()["steps"][0]["correction_request"], "");
}

TEST_F(SessionTest, ImplementationValidationRequiresDeclaredRepairTargetAndAllSubsteps) {
    const auto path = directory / "guard"; fs::create_directory(path);
    TurnSnapshot empty; empty.id = 1; empty.complete = true;
    ImplementationBoundaries boundaries(path);
    EXPECT_THROW(boundaries.requireActiveSubstep(0), std::logic_error);
    EXPECT_THROW(boundaries.capture(0, 2, {{}, "validating", ""}, empty), std::logic_error);
    boundaries.capture(0, 2, {0, "started", ""}, empty);
    EXPECT_NO_THROW(boundaries.requireActiveSubstep(0));
    boundaries.capture(0, 2, {0, "completed", "Built"}, empty);
    EXPECT_THROW(boundaries.capture(0, 2, {{}, "finished", ""}, empty), std::logic_error);
    boundaries.capture(0, 2, {1, "unnecessary", "Not needed"}, empty);
    boundaries.capture(0, 2, {{}, "validating", ""}, empty);
    EXPECT_NO_THROW(boundaries.requireActiveSubstep(0, true));
    EXPECT_THROW(boundaries.requireActiveSubstep(0), std::logic_error);
    EXPECT_THROW(boundaries.capture(0, 2, {0, "started", ""}, empty), std::logic_error);
    EXPECT_THROW(boundaries.capture(0, 2, {2, "correcting", ""}, empty), std::invalid_argument);
    boundaries.capture(0, 2, {0, "correcting", ""}, empty);
    EXPECT_NO_THROW(boundaries.requireActiveSubstep(0));
    boundaries.capture(0, 2, {0, "completed", "Fixed"}, empty);
    ImplementationProgress wrongCount{{}, "finished", "Done", ImplementationCompletion{"Done", {{"Only one"}}}};
    EXPECT_THROW(boundaries.capture(0, 2, wrongCount, empty), std::invalid_argument);
    boundaries.capture(0, 2, {{}, "finished", "Done"}, empty);
    EXPECT_THROW(boundaries.requireActiveSubstep(0, true), std::logic_error);
    EXPECT_THROW(boundaries.capture(0, 2, {0, "correcting", ""}, empty), std::logic_error);
}

TEST_F(SessionTest, ImplementationValidationCanRecoverUnownedChangesWithAnExplicitRepairTarget) {
    const auto path = directory / "unowned-validation"; fs::create_directory(path);
    const auto target = directory / "model.txt";
    std::ofstream(target) << "base\n";
    TurnFiles first(path); first.begin(first.prepare(directory, directory));
    ImplementationRun run(path); run.start(implementationPlan().get<ImplementationPlan>(), "step_by_step");
    run.begin(first.boundary());
    ImplementationBoundaries boundaries(path);
    boundaries.capture(0, 2, {0, "started", "Build model"}, first.boundary());
    first.mutate({target}, [&] { std::ofstream(target) << "built\n"; });
    boundaries.capture(0, 2, {0, "completed", "Built model"}, first.boundary());
    boundaries.capture(0, 2, {1, "unnecessary", "No wiring needed"}, first.boundary());
    boundaries.capture(0, 2, {{}, "validating", "Run checks"}, first.boundary());
    const auto saved = boundaries.read(0);
    // A validation command can leave changes before capture rejects its outcome.
    EXPECT_THROW(first.mutate({target}, [&] { std::ofstream(target) << "pending repair\n"; },
        [&](const auto& snapshot) { boundaries.capture(0, 2, {{}, "mutation", ""}, snapshot); }), std::logic_error);
    EXPECT_EQ(boundaries.read(0).size(), saved.size());
    EXPECT_THROW(boundaries.capture(0, 2, {{}, "finished", "Done"}, first.boundary()), std::logic_error);
    EXPECT_THROW(boundaries.capture(0, 2, {1, "started", "Wrong phase"}, first.boundary()), std::logic_error);
    EXPECT_THROW(boundaries.capture(0, 2, {2, "correcting", "Unknown target"}, first.boundary()), std::invalid_argument);
    EXPECT_EQ(boundaries.read(0).size(), saved.size());
    first.finish();
    run.finish(first.boundary(), {}, {}, false, "Validation interrupted");
    ImplementationRun resumed(path); resumed.resume();
    const auto baseline = resumed.baseline().value();
    TurnFiles second(path); second.begin(second.prepare(directory, directory));
    resumed.begin(second.boundary());
    const auto combined = [&] {
        auto snapshot = second.boundary(); snapshot.before = baseline.before;
        snapshot.edits.insert(snapshot.edits.begin(), baseline.edits.begin(), baseline.edits.end());
        return snapshot;
    };
    ASSERT_NO_THROW(boundaries.capture(0, 2, {1, "correcting", "Recover pending wiring repair"}, combined()));
    const auto recovered = ImplementationBoundaries(path).read(0);
    ASSERT_EQ(recovered.size(), saved.size() + 1);
    for (std::size_t index = 0; index < saved.size(); ++index)
        EXPECT_EQ(json(recovered[index]), json(saved[index]));
    const auto& repair = recovered.back();
    EXPECT_EQ(repair.owner, 1);
    EXPECT_EQ(repair.activeSubstep, 1);
    EXPECT_TRUE(repair.validating);
    ASSERT_EQ(repair.diffs.size(), 1);
    EXPECT_EQ(repair.diffs[0].before, "built\n");
    EXPECT_EQ(repair.diffs[0].after, "pending repair\n");
    EXPECT_NO_THROW(boundaries.requireActiveSubstep(0));
    second.mutate({target}, [&] { std::ofstream(target) << "fixed\n"; });
    boundaries.capture(0, 2, {1, "completed", "Repaired"}, combined());
    EXPECT_THROW(boundaries.requireActiveSubstep(0), std::logic_error);
    // A read-only check needs no repair target after recovery.
    EXPECT_NO_THROW(boundaries.requireActiveSubstep(0, true));
    EXPECT_NO_THROW(boundaries.capture(0, 2, {{}, "mutation", ""}, combined()));
    EXPECT_NO_THROW(boundaries.capture(0, 2, {{}, "finished", "Checks passed"}, combined()));
    second.finish();
}

TEST_F(SessionTest, ImplementationInterruptedRepairResumesWithUnclosedExternalEdits) {
    const auto path = directory / "resumed-boundaries"; fs::create_directory(path);
    const auto target = directory / "outside.txt";
    std::ofstream(target) << "base\n";
    TurnFiles first(path); first.begin(first.prepare(directory, directory));
    ImplementationRun run(path); run.start(implementationPlan().get<ImplementationPlan>(), "step_by_step");
    run.begin(first.boundary());
    ImplementationBoundaries boundaries(path);
    const auto capture = [&](std::optional<std::size_t> index, const std::string& status) {
        boundaries.capture(0, 2, {index, status, "Explained"}, first.boundary());
    };
    capture(0, "started");
    first.mutate({target}, [&] { std::ofstream(target) << "broken\n"; });
    capture(0, "completed"); capture(1, "unnecessary"); capture({}, "validating"); capture(0, "correcting");
    first.mutate({target}, [&] { std::ofstream(target) << "partial repair\n"; });
    first.finish();
    run.finish(first.boundary(), {}, {}, false, "Cancelled during repair");
    ImplementationRun resumed(path); resumed.resume();
    EXPECT_NE(resumed.instruction().find("\"validating\":true"), std::string::npos);
    const auto baseline = resumed.baseline().value();
    TurnFiles second(path); second.begin(second.prepare(directory, directory));
    resumed.begin(second.boundary());
    const auto combined = [&] {
        auto snapshot = second.boundary(); snapshot.before = baseline.before;
        snapshot.edits.insert(snapshot.edits.begin(), baseline.edits.begin(), baseline.edits.end());
        return snapshot;
    };
    second.mutate({target}, [&] { std::ofstream(target) << "corrected\n"; });
    boundaries.capture(0, 2, {0, "completed", "Repaired"}, combined());
    boundaries.capture(0, 2, {{}, "finished", "Passed"}, combined());
    second.finish();
    const auto latest = second.boundary();
    const auto whole = resumed.substepCombined(latest);
    resumed.finish(latest, {"Checked", {{"Model repaired"}, {"Unnecessary"}}}, second.diffs(whole), true);
    const auto final = ImplementationRun(path).view();
    EXPECT_EQ(final["steps"][0]["substeps"][0]["diffs"][0]["before"], "base\n");
    EXPECT_EQ(final["steps"][0]["substeps"][0]["diffs"][0]["after"], "corrected\n");
}

TEST_F(SessionTest, ImplementationRejectsFileEditsAfterSubmittedValidationBoundary) {
    const auto path = directory / "late-edit"; fs::create_directory(path);
    const auto target = directory / "late.bin";
    std::ofstream(target, std::ios::binary) << "original";
    TurnFiles files(path); files.begin(files.prepare(directory, directory));
    ImplementationRun run(path); run.start(implementationPlan().get<ImplementationPlan>(), "step_by_step");
    run.begin(files.boundary());
    ImplementationBoundaries boundaries(path);
    boundaries.capture(0, 2, {0, "started", ""}, files.boundary());
    files.mutate({target}, [&] { std::ofstream file(target, std::ios::binary); file.write("\0first", 6); });
    boundaries.capture(0, 2, {0, "completed", "Built"}, files.boundary());
    boundaries.capture(0, 2, {1, "unnecessary", "Not needed"}, files.boundary());
    boundaries.capture(0, 2, {{}, "validating", ""}, files.boundary());
    boundaries.capture(0, 2, {{}, "finished", "Passed"}, files.boundary());
    // Bypass the edit guard deliberately to exercise final snapshot verification.
    files.mutate({target}, [&] { std::ofstream file(target, std::ios::binary); file.write("\0late", 5); });
    files.finish(); const auto snapshot = files.boundary();
    EXPECT_THROW(run.finish(snapshot, {"Passed", {{"Model"}, {"Unnecessary"}}}, files.diffs(snapshot), true), std::runtime_error);
    EXPECT_FALSE(run.view()["steps"][0]["whole"]["complete"]);
}

TEST_F(SessionTest, ImplementationRecoversCrashBoundaryAndRejectsUnverifiedFileOutcome) {
    const auto path = directory / "crash"; fs::create_directory(path);
    TurnSnapshot started; started.id = 42; started.before = "original-tree";
    {
        ImplementationRun run(path);
        run.start(implementationPlan().get<ImplementationPlan>(), "everything");
        run.begin(started);
    }
    started.after = "partial-tree";
    ImplementationRun recovered(path);
    recovered.recover(started);
    EXPECT_EQ(recovered.status, "interrupted");
    recovered.resume();
    TurnSnapshot resumed; resumed.id = 43; resumed.before = "partial-tree"; resumed.after = "completed-tree"; resumed.complete = true;
    EXPECT_EQ(recovered.substepCombined(resumed).before, "original-tree");
    recovered.begin(resumed);
    resumed.edits.push_back({"/unverified", {}, {}, false});
    ImplementationRun uncertain(path);
    uncertain.recover(resumed);
    EXPECT_THROW(uncertain.resume(), std::runtime_error);
}

TEST_F(SessionTest, ImplementationCheckoutFailureIsVisibleAndCanResumeWithoutUserWorkspaceExecution) {
    prepareImplementationRepository();
    std::string branchId;
    {
        SessionRegistry registry(directory / "sessions", factory());
        auto discussion = registry.create(info());
        Message objective{"objective", MessageRole::ASSISTANT, {TextContent{"Objective"}}};
        objective.objectivePlan = true;
        discussion->record({AgentEventType::HARNESS_MESSAGE_ARRIVED, {objective}, {}, {}});
        auto branch = registry.createImplementation(*discussion);
        branchId = branch->getInfo().id;
        Message plan{"plan", MessageRole::ASSISTANT, {TextContent{"Plan"}}};
        plan.implementationPlan = implementationPlan().get<ImplementationPlan>();
        Message planning{"planning", MessageRole::USER, {TextContent{"Implementation plan request"}}};
        planning.userMetadata = UserMessageMetadata{"sane", false, true};
        branch->record({AgentEventType::HARNESS_MESSAGE_ARRIVED, {planning, plan}, {}, {}});
    }
    const auto collision = implementationData / "sos" / "worktrees" / branchId;
    fs::create_directories(collision);
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await receiveEvent(client);
        co_await client.send({{"type", "integration.configure"}, {"workspace", directory.string()}, {"mode", "sane"}});
        co_await receiveEvent(client);
        co_await client.send({{"type", "session.attach"}, {"sessionId", branchId}});
        co_await receiveEvent(client);
        co_await client.send({{"type", "implementation.start"}, {"sessionId", branchId}, {"pace", "step_by_step"}});
        const auto preparing = co_await receiveEvent(client);
        EXPECT_EQ(preparing.at("type"), "implementation.updated") << preparing.dump();
        EXPECT_TRUE(preparing.at("implementation").at("preparing"));
        const auto failed = co_await receiveEvent(client);
        EXPECT_EQ(failed.at("type"), "implementation.updated");
        EXPECT_EQ(failed.at("implementation").at("status"), "interrupted");
        EXPECT_FALSE(failed.at("implementation").at("preparing"));
        EXPECT_TRUE(failed.at("implementation").at("checkout_pending"));
        EXPECT_NE(failed.at("implementation").at("error").get<std::string>().find("already exists"), std::string::npos);
        EXPECT_TRUE(provider->inputs.empty());
        EXPECT_TRUE(fs::exists(collision));
        co_await client.send({{"type", "implementation.start"}, {"sessionId", branchId}, {"pace", "step_by_step"}});
        EXPECT_EQ((co_await receiveEvent(client)).at("type"), "request.failed");
        fs::remove(collision);
        co_await client.send({{"type", "implementation.resume"}, {"sessionId", branchId}});
        EXPECT_TRUE((co_await receiveEvent(client)).at("implementation").at("preparing"));
        co_await client.send({{"type", "execution.cancel"}, {"requestId", "cancel-preparation"}});
        for (;;) {
            const auto event = co_await receiveEvent(client);
            if (event.value("requestId", "") == "cancel-preparation") {
                EXPECT_EQ(event.at("type"), "execution.cancel_requested");
                break;
            }
        }
        EXPECT_TRUE(provider->inputs.empty());
        co_await client.send({{"type", "implementation.state"}, {"sessionId", branchId}});
        EXPECT_EQ((co_await receiveEvent(client)).at("implementation").at("status"), "interrupted");
        co_await client.send({{"type", "implementation.resume"}, {"sessionId", branchId}});
        EXPECT_TRUE((co_await receiveEvent(client)).at("implementation").at("preparing"));
        for (;;) {
            const auto event = co_await receiveEvent(client);
            if (event.at("type") == "implementation.updated" && event.at("implementation").at("status") == "interrupted") {
                EXPECT_FALSE(event.at("implementation").at("checkout_pending"));
                break;
            }
        }
        EXPECT_FALSE(provider->inputs.empty());
        EXPECT_TRUE(fs::is_directory(collision));
    }, {}, {}, std::chrono::seconds(15));
}

TEST_F(SessionTest, ImplementationExecutesWholeStepInOneTurnThenWaitsForValidationOnWire) {
    prepareImplementationRepository();
    fs::remove_all(directory / ".git"); // The wire flow also supports a plain source directory.
    std::string branchId, discussionId, repositoryTree;
    {
        SessionRegistry registry(directory / "sessions", factory());
        auto discussion = registry.create(info());
        discussionId = discussion->getInfo().id;
        Message request{"request", MessageRole::USER, {TextContent{"Sane Implementation request"}}};
        request.userMetadata = UserMessageMetadata{"sane", true};
        Message objective{"objective", MessageRole::ASSISTANT, {TextContent{"Objective"}}}; objective.objectivePlan = true;
        discussion->record({AgentEventType::HARNESS_MESSAGE_ARRIVED, {request, objective}, {}, {}});
        auto branch = registry.createImplementation(*discussion);
        branchId = branch->getInfo().id;
        Message plan{"plan", MessageRole::ASSISTANT, {TextContent{"Plan"}}};
        plan.implementationPlan = implementationPlan().get<ImplementationPlan>();
        Message planning{"planning", MessageRole::USER, {TextContent{"Implementation plan request"}}};
        planning.userMetadata = UserMessageMetadata{"sane", false, true};
        branch->record({AgentEventType::HARNESS_MESSAGE_ARRIVED, {planning, plan}, {}, {}});
    }
    provider->respond = [](const json&, std::size_t call) {
        auto result = submitImplementation({}, call);
        auto prototype = result["output"][0];
        result["output"] = json::array();
        const auto add = [&](const std::string& name, const json& arguments) {
            auto item = prototype;
            item["call_id"] = "call-" + std::to_string(call) + "-" + std::to_string(result["output"].size());
            item["id"] = item["call_id"]; item["name"] = name; item["arguments"] = arguments.dump();
            result["output"].push_back(item);
        };
        for (std::size_t part = 0; part < (call == 1 ? 2u : 1u); ++part) {
            add("report_implementation_progress", {{"substep", part}, {"status", "completed"}, {"explanation", "Checked"}});
        }
        ImplementationCompletion completion{"Step validated", {{""}, {""}}};
        if (call > 1) completion.substeps.resize(1);
        if (call > 1) completion.conclusion = "The entire implementation is ready. Both steps were checked.";
        add("complete_implementation_step", completion);
        return result;
    };
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await receiveEvent(client);
        co_await client.send({{"type", "integration.configure"}, {"workspace", directory.string()}, {"mode", "sane"}});
        co_await receiveEvent(client);
        co_await client.send({{"type", "session.attach"}, {"sessionId", branchId}});
        co_await receiveEvent(client);
        co_await client.send({{"type", "implementation.start"}, {"sessionId", branchId}, {"pace", "step_by_step"}});
        const auto preparing = co_await receiveEvent(client);
        EXPECT_EQ(preparing.at("type"), "implementation.updated");
        EXPECT_TRUE(preparing.at("info").at("implementation_execution"));
        EXPECT_TRUE(preparing.at("implementation").at("checkout_pending"));
        EXPECT_TRUE(preparing.at("implementation").at("steps")[0].at("preparing"));
        EXPECT_FALSE(preparing.at("implementation").at("steps")[0].at("whole").at("complete"));
        for (;;) {
            const auto event = co_await receiveEvent(client);
            if (event.at("type") == "implementation.updated" && event.at("implementation").at("status") == "waiting") {
                EXPECT_EQ(event.at("implementation").at("work_step"), 1);
                EXPECT_TRUE(event.at("implementation").at("steps")[0].at("whole").at("complete"));
                EXPECT_EQ(event.at("implementation").at("steps")[0].at("substeps")[0].at("explanation"), "Checked");
                EXPECT_EQ(provider->inputs.front().dump().find("Call inspect_implementation_changes to inspect"), std::string::npos);
                EXPECT_NE(provider->inputs.front().dump().find("harness automatically activates the first remaining substep"), std::string::npos);
                EXPECT_FALSE(event.at("implementation").at("steps")[0].contains("repository_files"));
                repositoryTree = event.at("implementation").at("steps")[0].at("repository_tree");
                break;
            }
        }
        EXPECT_EQ(provider->inputs.size(), 1);
        co_await client.send({{"type", "implementation.directory"}, {"requestId", "root-directory"},
            {"sessionId", branchId}, {"step", 0}, {"tree", repositoryTree}, {"path", ""}});
        for (;;) {
            const auto event = co_await receiveEvent(client);
            if (event.value("requestId", "") != "root-directory") continue;
            EXPECT_EQ(event.at("type"), "implementation.directory");
            EXPECT_EQ(event.at("tree"), repositoryTree);
            EXPECT_EQ(event.at("path"), "");
            for (const auto& entry : event.at("entries"))
                EXPECT_EQ(entry.at("name").get<std::string>().find('/'), std::string::npos);
            break;
        }
        EXPECT_EQ(provider->inputs.size(), 1);
        co_await client.send({{"type", "implementation.validate"}, {"requestId", "validate-before-preparation"},
            {"sessionId", branchId}, {"step", 0}, {"substep", 2}});
        for (;;) {
            const auto event = co_await receiveEvent(client);
            if (event.at("type") != "implementation.updated") continue;
            EXPECT_EQ(event.value("requestId", ""), "validate-before-preparation");
            const auto& confirmed = event.at("implementation");
            EXPECT_TRUE(confirmed.at("steps")[0].at("whole").at("reviewed"));
            EXPECT_EQ(confirmed.at("work_step"), 1);
            EXPECT_TRUE(confirmed.at("steps")[1].at("preparing"));
            EXPECT_FALSE(confirmed.at("steps")[1].at("substeps")[0].contains("snapshot"));
            EXPECT_EQ(provider->inputs.size(), 1);
            break;
        }
        for (;;) {
            const auto event = co_await receiveEvent(client);
            if (event.at("type") == "implementation.updated" && event.at("implementation").at("status") == "completed") {
                EXPECT_EQ(event.at("implementation").at("work_step"), 2);
                EXPECT_EQ(event.at("implementation").at("review_step"), 1);
                EXPECT_EQ(event.at("implementation").at("conclusion"), "The entire implementation is ready. Both steps were checked.");
                break;
            }
        }
        EXPECT_EQ(provider->inputs.size(), 2);
        // The plan conversation remains bounded before construction, including
        // after reconnecting to the frozen archive.
        co_await client.send({{"type", "session.history"}, {"sessionId", branchId}, {"direction", "latest"}});
        for (;;) {
            const auto event = co_await receiveEvent(client);
            if (event.at("type") != "session.attached") continue;
            EXPECT_EQ(event.at("page").at("last"), "plan");
            EXPECT_FALSE(event.at("page").at("hasNewer"));
            EXPECT_EQ(event.at("messages").back().at("id"), "plan");
            break;
        }
        // Closing must not freeze an implementation with outstanding validation.
        co_await client.send({{"type", "implementation.close"}, {"sessionId", discussionId}});
        for (;;) {
            const auto event = co_await receiveEvent(client);
            if (event.at("type") == "request.failed") break;
        }
        co_await client.send({{"type", "implementation.validate"}, {"sessionId", branchId}, {"step", 1}, {"substep", 1}});
        EXPECT_EQ((co_await receiveEvent(client)).at("type"), "implementation.updated");
        co_await client.send({{"type", "implementation.close"}, {"sessionId", discussionId}});
        const auto closed = co_await receiveEvent(client);
        EXPECT_EQ(closed.at("type"), "session.attached");
        EXPECT_EQ(closed.at("sessionId"), discussionId);
        EXPECT_TRUE(closed.at("info").at("active_implementation_id").get<std::string>().empty());
        EXPECT_FALSE(closed.at("info").at("implementation_view"));
        co_await client.send({{"type", "implementation.view"}, {"sessionId", discussionId}, {"branchId", branchId}});
        const auto archived = co_await receiveEvent(client);
        EXPECT_TRUE(archived.at("info").at("implementation_archive"));
        EXPECT_EQ(archived.at("messages").back().at("id"), "plan");
        EXPECT_FALSE(fs::exists(directory / ".git"));
        co_await client.send({{"type", "implementation.directory"}, {"requestId", "archive-directory"},
            {"sessionId", branchId}, {"step", 0}, {"tree", repositoryTree}, {"path", ""}});
        for (;;) {
            const auto event = co_await receiveEvent(client);
            if (event.value("requestId", "") != "archive-directory") continue;
            EXPECT_EQ(event.at("type"), "implementation.directory");
            EXPECT_TRUE(event.contains("entries"));
            break;
        }
        co_await client.send({{"type", "integration.close"}});
    }, {}, {}, std::chrono::seconds(30));
    const auto archived = ImplementationRun(directory / "sessions" / branchId).view();
    EXPECT_EQ(archived["conclusion"], "The entire implementation is ready. Both steps were checked.");
    EXPECT_EQ(archived["steps"][1]["substeps"][0]["explanation"], "Checked");
}

TEST_F(SessionTest, ImplementationEverythingWritesPreservesExactDiffsAndReviewCannotWrite) {
    prepareImplementationRepository();
    fs::create_directories(directory / ".sos/skills/review-guide");
    std::ofstream(directory / ".sos/skills/review-guide/SKILL.md") <<
        "---\nname: review-guide\ndescription: Explain a reviewed change.\n---\nREVIEW_SKILL_EVIDENCE $ARGUMENTS\n";
    auto writingFactory = [this](const SessionInfo& info) {
        ToolRegistry tools; tools.add(std::make_unique<WriteTool>());
        ToolContext context; context.directory = context.worktree = info.executionDirectory();
        return std::make_unique<Agent>(std::make_unique<TestProvider>(provider), info.model, std::move(tools), context, info.reasoning);
    };
    std::string branchId;
    std::ofstream(directory / "state.txt") << "original\n";
    {
        SessionRegistry registry(directory / "sessions", writingFactory);
        auto discussion = registry.create(info());
        Message request{"request", MessageRole::USER, {TextContent{"Sane Implementation request"}}};
        request.userMetadata = UserMessageMetadata{"sane", true};
        Message objective{"objective", MessageRole::ASSISTANT, {TextContent{"Objective"}}}; objective.objectivePlan = true;
        discussion->record({AgentEventType::HARNESS_MESSAGE_ARRIVED, {request, objective}, {}, {}});
        auto branch = registry.createImplementation(*discussion); branchId = branch->getInfo().id;
        Message planning{"planning", MessageRole::USER, {TextContent{"Implementation plan request"}}};
        planning.userMetadata = UserMessageMetadata{"sane", false, true};
        Message plan{"plan", MessageRole::ASSISTANT, {TextContent{"Plan"}}}; plan.implementationPlan = implementationPlan().get<ImplementationPlan>();
        branch->record({AgentEventType::HARNESS_MESSAGE_ARRIVED, {planning, plan}, {}, {}});
    }
    provider->respond = [this](const json& input, std::size_t call) {
        auto result = submitImplementation({}, call);
        const auto progress = [&](std::optional<std::size_t> substep, const std::string& status) {
            auto item = result["output"][0];
            item["name"] = "report_implementation_progress";
            item["id"] = item["call_id"] = "progress-" + std::to_string(call) + "-" + status + (substep ? std::to_string(*substep) : "");
            item["arguments"] = json{{"substep", substep ? json(*substep) : json(nullptr)}, {"status", status}, {"explanation", "- Checked the model"}}.dump();
            return item;
        };
        if (call == 1 || call == 2 || call == 5) {
            result["output"][0]["name"] = "write";
            result["output"][0]["arguments"] = json{{"filePath", "state.txt"},
                {"content", "increment-" + std::to_string(call == 5 ? 3 : call) + "\n"},
                {"explanation", "Current model increment " + std::to_string(call == 5 ? 3 : call)}}.dump();
            const auto write = result["output"][0];
            if (call == 1) result["output"] = json::array({progress(0, "started"), write});
            if (call == 2) result["output"] = json::array({progress(0, "completed"), progress(1, "started"), progress(1, "completed"),
                progress({}, "validating"), progress(0, "correcting"), write});
            if (call == 5) result["output"] = json::array({progress(0, "started"), write, progress(0, "completed"), progress({}, "validating")});
        } else if (call == 3 || call == 6) {
            result["output"][0]["name"] = "inspect_implementation_changes";
            result["output"][0]["arguments"] = "{}";
            if (call == 3) result["output"] = json::array({progress(0, "completed"), result["output"][0]});
        } else {
            EXPECT_NE(input.dump().find("hunks"), std::string::npos);
            result["output"][0]["name"] = "complete_implementation_step";
            ImplementationCompletion completion{"Checks passed after correction", {{"Final corrected model"}, {"Checks"}}};
            if (call == 7) completion.substeps.resize(1);
            result["output"][0]["arguments"] = json(completion).dump();
        }
        return result;
    };
    auto reviewProvider = std::make_shared<ProviderState>();
    reviewProvider->contextLimit = 32768;
    reviewProvider->respond = [this](const json& input, std::size_t call) {
        if (input.dump().find("Summarize this completed step review") != std::string::npos)
            return reply("review-summary", "PRECISE_REVIEW_SUMMARY: state.txt introduced the model; writes were refused; no correction was accepted.");
        if (input.dump().find("You summarize a coding conversation") != std::string::npos)
            return reply("compact-review", "REVIEW_COMPACTION_CHECKPOINT: model construction completed; review writes remain forbidden.");
        if (call != 1) return reply("review-done", "This change introduces the model. Review is read-only.");
        auto result = submitImplementation({}, call);
        result["test_used"] = 16380;
        result["id"] = "review-response-1";
        result["output"][0]["id"] = "review-call-1";
        result["output"][0]["call_id"] = "review-call-1";
        result["output"][0]["name"] = "write";
        result["output"][0]["arguments"] = json{{"filePath", (directory / "forbidden.txt").string()}, {"content", "forbidden"}}.dump();
        return result;
    };
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await receiveEvent(client);
        co_await client.send({{"type", "integration.configure"}, {"workspace", directory.string()}, {"mode", "sane"}}); co_await receiveEvent(client);
        co_await client.send({{"type", "session.attach"}, {"sessionId", branchId}}); co_await receiveEvent(client);
        co_await client.send({{"type", "implementation.start"}, {"sessionId", branchId}, {"pace", "everything"}});
        unsigned confirmations = 0;
        std::string pendingApproval;
        bool reviewFinished = false;
        for (;;) {
            const auto event = co_await receiveEvent(client);
            if (event.at("type") == "permission.requested") {
                ++confirmations;
                if (confirmations == 3) {
                    pendingApproval = event.at("permissionId");
                    co_await client.send({{"type", "implementation.review"}, {"sessionId", branchId}, {"step", 0}, {"substep", 0},
                        {"text", "/review-guide first change"}, {"skills", json::array({{{"name", "review-guide"}, {"input", "first change"}}})}});
                } else co_await client.send({{"type", "permission.reply"}, {"permissionId", event.at("permissionId")}, {"decision", "yes"}});
            }
            if (event.at("type") == "btw.started" && event.at("branch").at("status") == "completed") reviewFinished = true;
            if (reviewFinished && event.at("type") == "btw.finished" && !pendingApproval.empty()) {
                EXPECT_TRUE(fs::exists(directory / "state.txt"));
                std::ifstream original(directory / "state.txt");
                EXPECT_EQ(std::string(std::istreambuf_iterator<char>(original), {}), "original\n");
                EXPECT_FALSE(fs::exists(directory / "forbidden.txt"));
                co_await client.send({{"type", "permission.reply"}, {"permissionId", pendingApproval}, {"decision", "yes"}});
                pendingApproval.clear();
            }
            if (event.at("type") == "implementation.updated" && event.at("implementation").at("status") == "completed") {
                const auto& built = event.at("implementation");
                EXPECT_EQ(built.at("review_step"), 0);
                const auto patch = built.at("steps")[0].at("substeps")[0].at("diffs")[0].at("patch").get<std::string>();
                EXPECT_NE(patch.find("-original"), std::string::npos);
                EXPECT_NE(patch.find("+increment-2"), std::string::npos);
                EXPECT_EQ(built.at("steps")[0].at("substeps")[0].at("diffs")[0].at("hunk_explanations"),
                    json::array({"Current model increment 2"}));
                const auto whole = built.at("steps")[0].at("whole").at("diffs")[0].at("patch").get<std::string>();
                EXPECT_NE(whole.find("-original"), std::string::npos);
                EXPECT_NE(whole.find("+increment-2"), std::string::npos);
                EXPECT_TRUE(built.at("steps")[0].at("validation").at("complete"));
                bool validationWrite = false, validationResult = false;
                for (const auto& message : built.at("steps")[0].at("validation").at("activity"))
                    for (const auto& content : message.at("content")) {
                        validationWrite |= content.value("toolName", "") == "write";
                        validationResult |= content.value("callId", "") == "call-2" && content.at("type") == "tool_result";
                    }
                EXPECT_TRUE(validationWrite); EXPECT_TRUE(validationResult);
                break;
            }
        }
        EXPECT_EQ(confirmations, 3);
        EXPECT_TRUE(reviewFinished);
        const auto reviewCalls = reviewProvider->inputs.size();
        co_await client.send({{"type", "implementation.review"}, {"requestId", "invalid-review-skill"}, {"sessionId", branchId},
            {"step", 0}, {"substep", 0}, {"text", "/missing"}, {"skills", json::array({{{"name", "missing"}}})}});
        for (;;) {
            const auto invalidSkill = co_await receiveEvent(client);
            if (invalidSkill.value("requestId", "") != "invalid-review-skill") continue;
            EXPECT_EQ(invalidSkill.at("type"), "request.failed");
            break;
        }
        EXPECT_EQ(reviewProvider->inputs.size(), reviewCalls);
        co_await client.send({{"type", "implementation.review"}, {"requestId", "future-review"}, {"sessionId", branchId}, {"step", 1}, {"substep", 0}, {"text", "Explain future step"}});
        const auto future = co_await receiveEvent(client);
        EXPECT_EQ(future.at("type"), "request.failed");
        EXPECT_NE(future.at("error").get<std::string>().find("first unvalidated step"), std::string::npos);
        EXPECT_EQ(reviewProvider->inputs.size(), reviewCalls);
        co_await client.send({{"type", "implementation.review"}, {"sessionId", branchId}, {"step", 0}, {"substep", 0},
            {"path", "state.txt"}, {"hunk", 0}, {"text", "Explain this change"}});
        bool finished = false;
        bool sawReviewToolCall = false, sawReviewToolResult = false;
        std::string reviewMessage;
        for (;;) {
            const auto event = co_await receiveEvent(client);
            if (event.at("type") == "btw.started" && event.at("branch").at("status") == "completed") finished = true;
            if (finished && event.at("type") == "btw.chunk") {
                reviewMessage += event.at("data").get<std::string>();
                if (event.at("last").get<bool>()) {
                    const auto message = json::parse(reviewMessage);
                    for (const auto& part : message.at("content")) {
                        sawReviewToolCall |= part.at("type") == "tool_call";
                        sawReviewToolResult |= part.at("type") == "tool_result";
                    }
                    reviewMessage.clear();
                }
            }
            if (finished && event.at("type") == "btw.finished") break;
        }
        EXPECT_TRUE(sawReviewToolCall);
        EXPECT_TRUE(sawReviewToolResult);
        EXPECT_FALSE(fs::exists(directory / "forbidden.txt"));
        EXPECT_FALSE(reviewProvider->inputs.empty());
        EXPECT_NE(reviewProvider->inputs.front().dump().find("+increment-2"), std::string::npos);
        EXPECT_NE(reviewProvider->inputs.front().dump().find("Implementation plan request"), std::string::npos);
        EXPECT_NE(reviewProvider->inputs.front().dump().find("REVIEW_SKILL_EVIDENCE first change"), std::string::npos);
        EXPECT_NE(reviewProvider->inputs.front().dump().find("read-only"), std::string::npos);
        EXPECT_NE(reviewProvider->inputs.back().dump().find("Currently displayed hunk"), std::string::npos);
        EXPECT_EQ(reviewProvider->inputs.back().dump().find("increment-3"), std::string::npos);
        const auto firstReview = ImplementationRun(directory / "sessions" / branchId).stepReviewBranch(0);
        const auto savedReview = SessionJournal(directory / "sessions" / branchId).readBtw(firstReview);
        EXPECT_FALSE(savedReview.activeContext.empty());
        EXPECT_EQ(savedReview.contextMessageCount, savedReview.messages.size());
        EXPECT_EQ(std::count_if(savedReview.messages.begin(), savedReview.messages.end(), [](const auto& message) {
            return message.compaction.has_value();
        }), 1);
        EXPECT_NE(json(savedReview.messages).dump().find("REVIEW_COMPACTION_CHECKPOINT"), std::string::npos);
        std::ofstream(directory / "state.txt") << "concurrent user change\n";
        co_await client.send({{"type", "implementation.validate"}, {"requestId", "import-conflict"}, {"sessionId", branchId}, {"step", 0}, {"substep", 2}});
        const auto failed = co_await receiveEvent(client);
        EXPECT_EQ(failed.at("type"), "request.failed");
        EXPECT_EQ(failed.at("requestId"), "import-conflict");
        EXPECT_NE(failed.at("error").get<std::string>().find("remains unvalidated"), std::string::npos);
        EXPECT_FALSE(ImplementationRun(directory / "sessions" / branchId).view()["steps"][0]["whole"]["reviewed"]);
        std::ofstream(directory / "state.txt") << "original\n";
        co_await client.send({{"type", "implementation.validate"}, {"sessionId", branchId}, {"step", 0}, {"substep", 2}});
        EXPECT_EQ((co_await receiveEvent(client)).at("type"), "implementation.updated");
        // A construction context too large for the review's selected model
        // compacts before injecting the previous review's summary or answering.
        {
            ImplementationRun saved(directory / "sessions" / branchId);
            auto construction = *saved.reviewConstruction(1);
            construction.insert(construction.begin() + 1, Message{"oversized-construction", MessageRole::USER,
                {TextContent{"OLD_CONSTRUCTION_PAYLOAD " + std::string(90000, 'x')}}});
            saved.saveReviewConstruction(1, construction);
        }
        for (unsigned question = 0; question < 2; ++question) {
            co_await client.send({{"type", "implementation.review"}, {"sessionId", branchId}, {"step", 1}, {"substep", 0}, {"text", "Explain next step"}});
            bool done = false;
            for (;;) {
                const auto event = co_await receiveEvent(client);
                if (event.at("type") == "btw.started" && event.at("branch").at("status") == "completed") {
                    EXPECT_NE(event.at("branch").at("id"), firstReview);
                    EXPECT_EQ(event.at("branch").at("review_step"), 1);
                    done = true;
                }
                if (done && event.at("type") == "btw.finished") break;
            }
            EXPECT_NE(reviewProvider->inputs.back().dump().find("PRECISE_REVIEW_SUMMARY"), std::string::npos);
            EXPECT_EQ(reviewProvider->inputs.back().dump().find("REVIEW_SKILL_EVIDENCE first change"), std::string::npos);
        }
        {
            ImplementationRun saved(directory / "sessions" / branchId);
            const auto nextReview = SessionJournal(directory / "sessions" / branchId).readBtw(saved.stepReviewBranch(1));
            const auto firstResponse = std::find_if(nextReview.messages.begin(), nextReview.messages.end(), [](const auto& message) {
                return message.role == MessageRole::ASSISTANT;
            });
            EXPECT_NE(firstResponse, nextReview.messages.end());
            if (firstResponse != nextReview.messages.end()) EXPECT_TRUE(firstResponse->compaction);
            EXPECT_EQ(std::count_if(nextReview.messages.begin(), nextReview.messages.end(), [](const auto& message) {
                return message.compaction.has_value();
            }), 1);
            EXPECT_EQ(json(nextReview.activeContext).dump().find("OLD_CONSTRUCTION_PAYLOAD"), std::string::npos);
        }
        for (const auto& input : reviewProvider->inputs)
            if (input.dump().find("Summarize this completed step review") != std::string::npos)
                EXPECT_EQ(input.dump().find("REVIEW_COMPACTION_CHECKPOINT"), std::string::npos);
        EXPECT_EQ(std::count_if(reviewProvider->inputs.begin(), reviewProvider->inputs.end(), [](const auto& input) {
            return input.dump().find("Summarize this completed step review") != std::string::npos;
        }), 1);
        EXPECT_NE(SessionJournal(directory / "sessions" / branchId).readBtw(firstReview).reviewSummary.find("PRECISE_REVIEW_SUMMARY"), std::string::npos);
        {
            std::ifstream imported(directory / "state.txt");
            EXPECT_EQ(std::string(std::istreambuf_iterator<char>(imported), {}), "increment-2\n");
        }
        co_await client.send({{"type", "implementation.validate"}, {"sessionId", branchId}, {"step", 1}, {"substep", 1}});
        EXPECT_EQ((co_await receiveEvent(client)).at("type"), "implementation.updated");
        {
            std::ifstream imported(directory / "state.txt");
            EXPECT_EQ(std::string(std::istreambuf_iterator<char>(imported), {}), "increment-3\n");
        }
        co_await client.send({{"type", "integration.close"}});
    }, {}, {}, std::chrono::seconds(60), std::chrono::seconds(5), {},
        [reviewProvider](const ProviderSelection&) { return std::make_unique<TestProvider>(reviewProvider); }, writingFactory);
    ImplementationRun persisted(directory / "sessions" / branchId);
    EXPECT_EQ(persisted.status, "completed");
    EXPECT_FALSE(persisted.reviewBranch.empty());
}

void SessionTest::exerciseReviewRework(const std::string& pace, bool revisePlan, bool activeSuccessor) {
    prepareImplementationRepository();
    std::ofstream(directory / "state.txt") << "original\n";
    auto writingFactory = [this](const SessionInfo& info) {
        ToolRegistry tools; tools.add(std::make_unique<WriteTool>()); tools.add(std::make_unique<BashTool>());
        ToolContext context; context.directory = context.worktree = info.executionDirectory();
        return std::make_unique<Agent>(std::make_unique<TestProvider>(provider), info.model, std::move(tools), context, info.reasoning);
    };
    std::string branchId;
    auto accepted = implementationPlan().get<ImplementationPlan>();
    if (pace == "everything") accepted.steps.resize(activeSuccessor ? 2 : 1);
    if (activeSuccessor) accepted.steps[1].substeps = accepted.steps[0].substeps;
    {
        SessionRegistry registry(directory / "sessions", writingFactory);
        auto discussion = registry.create(info());
        Message request{"request", MessageRole::USER, {TextContent{"Sane Implementation request"}}};
        request.userMetadata = UserMessageMetadata{"sane", true};
        Message objective{"objective", MessageRole::ASSISTANT, {TextContent{"Objective"}}}; objective.objectivePlan = true;
        discussion->record({AgentEventType::HARNESS_MESSAGE_ARRIVED, {request, objective}, {}, {}});
        auto branch = registry.createImplementation(*discussion); branchId = branch->getInfo().id;
        Message planning{"planning", MessageRole::USER, {TextContent{"Implementation plan request"}}};
        planning.userMetadata = UserMessageMetadata{"sane", false, true};
        Message plan{"plan", MessageRole::ASSISTANT, {TextContent{"Plan"}}}; plan.implementationPlan = accepted;
        branch->record({AgentEventType::HARNESS_MESSAGE_ARRIVED, {planning, plan}, {}, {}});
    }
    provider->respond = [this, activeSuccessor](const json& input, std::size_t call) {
        const auto stage = activeSuccessor && call >= 5 ? (call - 5) % 2 : (call - 1) % 2;
        auto result = submitImplementation({}, call);
        const auto tool = [&](const std::string& name, const json& args, const std::string& suffix) {
            auto item = result["output"][0]; item["name"] = name;
            item["id"] = item["call_id"] = "rework-" + std::to_string(call) + suffix;
            item["arguments"] = args.dump(); return item;
        };
        const auto progress = [&](const json& substep, const std::string& status) {
            return tool("report_implementation_progress", {{"substep", substep}, {"status", status}, {"explanation", "Checked"}}, status + substep.dump());
        };
        if (stage == 0) {
            const bool partial = activeSuccessor && call == 3;
            if (partial) provider->waitForCancellation = true;
            if (call > 2 && !partial) {
                EXPECT_NE(input.dump().find("Use corrected implementation"), std::string::npos);
                EXPECT_NE(input.dump().find("REVIEW_DECISION_DETAIL"), std::string::npos);
            }
            result["output"] = json::array({progress(0, call == 1 || partial ? "started" : "correcting"),
                tool("write", {{"filePath", partial ? "successor.txt" : "state.txt"}, {"content", call == 1 || partial ? "built\n" : "corrected\n"},
                    {"explanation", call == 1 || partial ? "Initial model explanation" : "Complete corrected model explanation"}}, "write"), progress(0, "completed")});
            if (partial) return result;
            if (call == 1) result["output"].push_back(progress(1, "unnecessary"));
            if (call > 1) {
                result["output"].push_back(progress(1, "correcting"));
                result["output"].push_back(progress(1, "completed"));
            }
            result["output"].push_back(progress(nullptr, "validating"));
            result["output"].push_back(tool("bash", {{"command", "printf 'technical-checks-passed\\n'"}, {"mainCommand", "printf"}}, "checks"));
        } else {
            EXPECT_EQ(input.dump().find("Before editing, report started"), std::string::npos);
            EXPECT_NE(input.dump().find("technical-checks-passed"), std::string::npos);
            const ImplementationCompletion completion{"Checked", {{"Model"}, {"Not needed"}}};
            result["output"] = json::array({tool("complete_implementation_step", json(completion), "complete")});
        }
        return result;
    };
    auto reviewProvider = std::make_shared<ProviderState>();
    auto entered = activeSuccessor ? provider->entered.get_future().share() : std::shared_future<void>{};
    reviewProvider->respond = [this, revisePlan, accepted, entered](const json&, std::size_t call) {
        if (call != 1 && !(revisePlan && call == 3)) return reply("review-done-" + std::to_string(call), "The proposal requires your acceptance. REVIEW_DECISION_DETAIL");
        if (entered.valid()) entered.wait();
        auto result = submitImplementation({}, call);
        result["output"][0]["name"] = revisePlan ? "propose_implementation_revision" : "propose_step_rework";
        json proposal{{"request", "Use corrected implementation with uv run snake"}};
        if (revisePlan) {
            auto candidate = accepted; candidate.steps[0].substeps[1].title = "uv launch packaging";
            proposal["plan"] = candidate; proposal["retained_steps"] = {0}; proposal["substep_owners"] = {{0, 1}};
        }
        result["output"][0]["arguments"] = proposal.dump();
        return result;
    };
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await receiveEvent(client);
        co_await client.send({{"type", "integration.configure"}, {"workspace", directory.string()}, {"mode", "sane"}}); co_await receiveEvent(client);
        co_await client.send({{"type", "session.attach"}, {"sessionId", branchId}}); co_await receiveEvent(client);
        co_await client.send({{"type", "implementation.start"}, {"sessionId", branchId}, {"pace", pace}});
        for (;;) {
            const auto event = co_await receiveEvent(client);
            if (event.at("type") == "permission.requested")
                co_await client.send({{"type", "permission.reply"}, {"permissionId", event.at("permissionId")}, {"decision", "yes"}});
            if (event.at("type") == "implementation.updated" &&
                (activeSuccessor ? event.at("implementation").at("work_step") == 1 : event.at("implementation").at("status") == (pace == "everything" ? "completed" : "waiting"))) break;
        }
        co_await client.send({{"type", "implementation.review"}, {"sessionId", branchId}, {"step", 0}, {"substep", 0}, {"text", "Use corrected implementation"}});
        std::uint64_t revision = 0; bool finished = false;
        for (;;) {
            const auto event = co_await receiveEvent(client);
            if (event.at("type") == "permission.requested")
                co_await client.send({{"type", "permission.reply"}, {"permissionId", event.at("permissionId")}, {"decision", "yes"}});
            if (event.at("type") == "implementation.updated" && !event.at("implementation").at("proposed_rework").get<std::string>().empty()) {
                revision = event.at("implementation").at("revision").get<std::uint64_t>();
                EXPECT_EQ(!event.at("implementation").at("proposed_plan_markdown").get<std::string>().empty(), revisePlan);
            }
            if (event.at("type") == "btw.started" && event.at("branch").at("status") == "completed") finished = true;
            if (finished && event.at("type") == "btw.finished") break;
        }
        EXPECT_NE(revision, 0);
        if (activeSuccessor) {
            for (;;) {
                const auto event = co_await receiveEvent(client);
                if (event.at("type") == "implementation.updated" && event.at("implementation").at("status") == "interrupted") break;
            }
            provider->waitForCancellation = false;
        }
        if (!revision) throw std::runtime_error("No revision proposal was published");
        std::ifstream original(directory / "state.txt"); EXPECT_EQ(std::string(std::istreambuf_iterator<char>(original), {}), "original\n");
        co_await client.send({{"type", "implementation.rework"}, {"sessionId", branchId}, {"revision", revision - 1}});
        for (;;) {
            const auto event = co_await receiveEvent(client);
            if (event.at("type") == "request.failed") break;
        }
        if (revisePlan) {
            co_await client.send({{"type", "implementation.rework"}, {"sessionId", branchId}, {"revision", revision}});
            EXPECT_EQ((co_await receiveEvent(client)).at("type"), "request.failed");
            co_await client.send({{"type", "implementation.revision.open"}, {"sessionId", branchId}, {"revision", revision}});
            const auto opened = co_await receiveEvent(client);
            EXPECT_EQ(opened.at("implementation").at("status"), "revision_planning");
            EXPECT_EQ(opened.at("implementation").at("plan_versions").size(), 1);
            revision = opened.at("implementation").at("revision");
            const auto callsBeforePlanning = provider->inputs.size();
            co_await client.send({{"type", "implementation.revision.review"}, {"sessionId", branchId}, {"text", "Keep the packaging concise"}});
            bool planningFinished = false;
            for (;;) {
                const auto event = co_await receiveEvent(client);
                if (event.at("type") == "request.failed") throw std::runtime_error(event.dump());
                if (event.at("type") == "implementation.updated") revision = event.at("implementation").at("revision");
                if (event.at("type") == "btw.started" && event.at("branch").at("status") == "completed") {
                    EXPECT_TRUE(event.at("branch").at("revision_planning").get<bool>());
                    planningFinished = true;
                }
                if (planningFinished && event.at("type") == "btw.finished") break;
            }
            EXPECT_EQ(provider->inputs.size(), callsBeforePlanning);
            EXPECT_EQ(std::count(reviewProvider->toolNames.begin(), reviewProvider->toolNames.end(), "propose_step_rework"), 0);
            EXPECT_EQ(std::count(reviewProvider->toolNames.begin(), reviewProvider->toolNames.end(), "propose_implementation_revision"), 1);
        }
        co_await client.send({{"type", "implementation.rework"}, {"sessionId", branchId}, {"revision", revision}});
        for (;;) {
            const auto event = co_await receiveEvent(client);
            if (event.at("type") == "request.failed") throw std::runtime_error(event.dump());
            if (event.at("type") == "implementation.updated" && event.at("implementation").at("status") == "interrupted")
                throw std::runtime_error(event.at("implementation").at("error").get<std::string>());
            if (event.at("type") == "permission.requested")
                co_await client.send({{"type", "permission.reply"}, {"permissionId", event.at("permissionId")}, {"decision", "yes"}});
            if (event.at("type") == "implementation.updated" && event.at("implementation").at("status") == (pace == "everything" ? "completed" : "waiting")) {
                const auto& corrected = event.at("implementation");
                EXPECT_EQ(corrected.at("version_count"), 2);
                EXPECT_FALSE(corrected.at("steps")[0].at("whole").at("reviewed"));
                const auto patch = corrected.at("steps")[0].at("whole").at("diffs")[0].at("patch").get<std::string>();
                EXPECT_NE(patch.find("-original"), std::string::npos); EXPECT_NE(patch.find("+corrected"), std::string::npos);
                EXPECT_EQ(corrected.at("steps")[0].at("substeps")[0].at("diffs")[0].at("hunk_explanations"),
                    json::array({"Complete corrected model explanation"}));
                EXPECT_EQ(corrected.at("work_step"), activeSuccessor ? 2 : 1); break;
            }
        }
        co_await client.send({{"type", "implementation.history"}, {"sessionId", branchId}, {"all", true}, {"requestId", "versions"}});
        for (;;) {
            const auto event = co_await receiveEvent(client);
            if (event.value("requestId", "") != "versions") continue;
            EXPECT_EQ(event.at("type"), "implementation.history");
            const auto& versions = event.at("implementations");
            EXPECT_EQ(versions.size(), 1);
            EXPECT_TRUE(versions[0].at("steps")[0].at("whole").at("complete"));
            EXPECT_FALSE(versions[0].at("accepted_plan_markdown").get<std::string>().empty());
            EXPECT_EQ(versions[0].at("status"), "archived");
            EXPECT_FALSE(versions[0].at("steps")[0].at("substeps")[0].contains("snapshot"));
            EXPECT_EQ(versions[0].at("steps")[0].at("substeps")[0].at("diffs")[0].at("hunk_explanations"),
                json::array({"Initial model explanation"}));
            break;
        }
        std::ifstream untouched(directory / "state.txt"); EXPECT_EQ(std::string(std::istreambuf_iterator<char>(untouched), {}), "original\n");
        EXPECT_NE(reviewProvider->inputs.back().dump().find(revisePlan ? "candidate revised Implementation Plan" : "Do not tell the user to leave Implement"), std::string::npos);
        if (!revisePlan && !activeSuccessor) {
            const auto reviewId = ImplementationRun(directory / "sessions" / branchId).stepReviewBranch(0);
            auto stored = SessionJournal(directory / "sessions" / branchId).readBtw(reviewId);
            EXPECT_TRUE(currentReviewMessages(stored).empty());
            EXPECT_FALSE(stored.messages.empty());
            co_await client.send({{"type", "implementation.review"}, {"sessionId", branchId}, {"step", 0}, {"substep", 0},
                {"text", "AFTER_CORRECTION_QUESTION"}});
            bool answered = false;
            for (;;) {
                const auto event = co_await receiveEvent(client);
                if (event.at("type") == "request.failed") throw std::runtime_error(event.dump());
                if (event.at("type") == "btw.started" && event.at("branch").at("status") == "completed") answered = true;
                if (answered && event.at("type") == "btw.finished") break;
            }
            const auto input = reviewProvider->inputs.back().dump();
            const auto decision = input.find("REVIEW_DECISION_DETAIL");
            EXPECT_NE(decision, std::string::npos);
            if (decision != std::string::npos) EXPECT_EQ(input.find("REVIEW_DECISION_DETAIL", decision + 1), std::string::npos);
            EXPECT_NE(input.find("AFTER_CORRECTION_QUESTION"), std::string::npos);
            stored = SessionJournal(directory / "sessions" / branchId).readBtw(reviewId);
            const auto source = json(currentReviewMessages(stored)).dump();
            EXPECT_NE(source.find("AFTER_CORRECTION_QUESTION"), std::string::npos);
            EXPECT_EQ(source.find("Use corrected implementation"), std::string::npos);
        }
        co_await client.send({{"type", "integration.close"}});
    }, {}, {}, std::chrono::seconds(activeSuccessor ? 120 : 60), std::chrono::seconds(5), {},
        [reviewProvider](const ProviderSelection&) { return std::make_unique<TestProvider>(reviewProvider); }, writingFactory);
}

TEST_F(SessionTest, ImplementationReviewReworkStartsOnlyAfterAcceptanceAndKeepsUserWorkspaceUntouched) {
    exerciseReviewRework("step_by_step");
}
TEST_F(SessionTest, ImplementationEverythingLastStepSupportsUvReworkInReview) {
    exerciseReviewRework("everything");
}
TEST_F(SessionTest, ImplementationEverythingAcceptsRevisedPlanAndKeepsCurrentCodeBeforeCorrection) {
    exerciseReviewRework("everything", true);
}
TEST_F(SessionTest, ImplementationEverythingProposalPausesActiveSuccessorAndReconcilesItsPartialCode) {
    exerciseReviewRework("everything", false, true);
}

TEST_F(SessionTest, ImplementationPreservesProviderReasoningAndCompactsDuringPlanning) {
    provider->contextLimit = 32768;
    SessionRegistry registry(directory / "sessions", factory());
    auto discussion = registry.create(info());
    Message request{"request", MessageRole::USER, {TextContent{"Sane Implementation request"}}};
    request.userMetadata = UserMessageMetadata{"sane", true};
    Message objective{"objective", MessageRole::ASSISTANT, {
        ReasoningContent{std::nullopt, ProviderFormat::OPENAI_RESPONSES,
            {{"id", "reasoning"}, {"type", "reasoning"}, {"summary", json::array()}, {"encrypted_content", "preserved-opaque"}}},
        TextContent{"Build a status page."}}};
    objective.objectivePlan = true;
    discussion->record({AgentEventType::HARNESS_MESSAGE_ARRIVED, {request, objective}, {}, {120, 32768}});
    auto branch = registry.createImplementation(*discussion);
    EXPECT_EQ(branch->getInfo().context.lastTurnUsedContext, 120);
    EXPECT_EQ(branch->getInfo().providerContextStart, discussion->getInfo().providerContextStart);
    std::vector<AgentEventType> events;
    turn(*branch, "Discuss the approach", events);
    EXPECT_NE(provider->inputs[0].dump().find("preserved-opaque"), std::string::npos);
    provider->usedContext = 17000;
    turn(*branch, "Continue", events);
    provider->respond = [this](const json&, std::size_t call) {
        provider->usedContext = 120;
        return call == 4 ? submitImplementation(implementationPlan(), call) : reply("answer-" + std::to_string(call), "Keep the accepted status-page objective and implement it in steps.");
    };
    events.clear();
    turn(*branch, "Implementation plan request", events, ExecutionMode::SANE, {}, false, true);
    EXPECT_EQ(std::count(events.begin(), events.end(), AgentEventType::COMPACTION_COMPLETED), 1);
    EXPECT_TRUE(branch->implementationPlanReady());
    EXPECT_TRUE(branch->getInfo().providerContextStart > 0);
    const auto id = branch->getInfo().id;
    branch.reset();
    branch = registry.load(id);
    EXPECT_TRUE(branch->implementationPlanReady());
    EXPECT_NE(json(branch->activeConversation().getMessages()).dump().find("Step 1: Testable foundation"), std::string::npos);
    EXPECT_EQ(discussion->getInfo().providerContextStart, 0);
}

TEST_F(SessionTest, ImplementationKeepsCheckpointCreatedAfterObjective) {
    SessionRegistry registry(directory / "sessions", factory());
    auto discussion = registry.create(info());
    Message request{"request", MessageRole::USER, {TextContent{"Sane Implementation request"}}};
    request.userMetadata = UserMessageMetadata{"sane", true};
    Message objective{"objective", MessageRole::ASSISTANT, {TextContent{"Build a status page."}}};
    objective.objectivePlan = true;
    discussion->record({AgentEventType::HARNESS_MESSAGE_ARRIVED, {request, objective}, {}, {}});
    Message checkpoint{"checkpoint", MessageRole::ASSISTANT, {TextContent{"Current accepted status-page objective"}}};
    checkpoint.compaction = CompactionMetadata{{"objective"}, "sane"};
    discussion->record({AgentEventType::COMPACTION_COMPLETED, {checkpoint}, {}, {}});
    ASSERT_TRUE(discussion->objectivePlanReady());
    auto branch = registry.createImplementation(*discussion);
    EXPECT_EQ(branch->getInfo().objectiveId, objective.id);
    std::vector<AgentEventType> events;
    turn(*branch, "Plan the steps", events);
    EXPECT_NE(provider->inputs.back().dump().find("Current accepted status-page objective"), std::string::npos);
    EXPECT_EQ(provider->inputs.back().dump().find("Sane Implementation request"), std::string::npos);
    const auto page = branch->history().page("latest", "", false, branch->getInfo().inheritedMessages);
    EXPECT_FALSE(page.hasOlder);
    for (std::size_t i = 0; i < page.count; ++i) EXPECT_NE(page.record(i).at("id"), checkpoint.id);
}

TEST_F(SessionTest, ImplementationBranchesPersistIndependentContextAndResumeTarget) {
    SessionRegistry registry(directory / "sessions", factory());
    auto discussion = registry.create(info());
    Message request{"objective-request", MessageRole::USER, {TextContent{"Sane Implementation request"}}};
    request.userMetadata = UserMessageMetadata{"sane", true};
    Message plan{"accepted-objective", MessageRole::ASSISTANT, {TextContent{"Build a status page."}}};
    plan.objectivePlan = true;
    plan.objectiveStack = "- Textual";
    discussion->record({AgentEventType::HARNESS_MESSAGE_ARRIVED, {request, plan}, {}, {}});
    auto branch = registry.createImplementation(*discussion);
    const auto discussionId = discussion->getInfo().id, branchId = branch->getInfo().id;
    EXPECT_EQ(branch->getInfo().discussionId, discussionId);
    EXPECT_EQ(branch->getInfo().objectiveId, plan.id);
    EXPECT_EQ(registry.resumeTarget(*discussion), branchId);
    EXPECT_EQ(registry.list().size(), 1);
    EXPECT_THROW(registry.createImplementation(*discussion), std::logic_error);
    EXPECT_THROW(branch->sendMessage("Edit", ExecutionMode::SLOP), std::logic_error);
    std::vector<AgentEventType> events;
    turn(*branch, "PRIVATE IMPLEMENTATION DETAIL", events);
    EXPECT_NE(provider->inputs.back().dump().find("Build a status page."), std::string::npos);
    EXPECT_NE(provider->inputs.back().dump().find("PRIVATE IMPLEMENTATION DETAIL"), std::string::npos);
    const auto parentHistory = json(discussion->getMessages()).dump();
    EXPECT_EQ(parentHistory.find("PRIVATE IMPLEMENTATION DETAIL"), std::string::npos);
    branch.reset();
    discussion.reset();
    SessionRegistry reopened(directory / "sessions", factory());
    discussion = reopened.load(discussionId);
    EXPECT_EQ(reopened.resumeTarget(*discussion), branchId);
    branch = reopened.load(branchId);
    EXPECT_NE(json(branch->getMessages()).dump().find("PRIVATE IMPLEMENTATION DETAIL"), std::string::npos);
    reopened.freezeImplementation(*discussion, *branch);
    EXPECT_EQ(reopened.resumeTarget(*discussion), discussionId);
    EXPECT_THROW(branch->sendMessage("Another message", ExecutionMode::SANE), std::logic_error);
    EXPECT_THROW(navigate(*branch, false), std::logic_error);
    auto resumedHistory = discussion->getMessages();
    ASSERT_TRUE(resumedHistory.back().objectiveCompleted);
    resumedHistory.pop_back();
    EXPECT_EQ(json(resumedHistory).dump(), parentHistory);
    EXPECT_FALSE(discussion->objectivePlanReady());
    turn(*discussion, "Back in Discussion", events);
    EXPECT_EQ(provider->inputs.back().dump().find("PRIVATE IMPLEMENTATION DETAIL"), std::string::npos);
    EXPECT_NE(provider->inputs.back().dump().find("Build a status page."), std::string::npos);
    request.id = "second-request";
    plan.id = "second-objective";
    discussion->record({AgentEventType::HARNESS_MESSAGE_ARRIVED, {request, plan}, {}, {}});
    auto second = reopened.createImplementation(*discussion);
    EXPECT_NE(second->getInfo().id, branchId);
    EXPECT_EQ(second->getInfo().objectiveId, "second-objective");
    EXPECT_EQ(json(second->getMessages()).dump().find("PRIVATE IMPLEMENTATION DETAIL"), std::string::npos);
    EXPECT_EQ(discussion->getInfo().implementationIds.size(), 2);
    branch.reset();
    branch = reopened.load(branchId);
    EXPECT_TRUE(branch->getInfo().frozen);
    EXPECT_FALSE(branch->history().page().last.empty());
}

TEST_F(SessionTest, CurrentSessionMetadataRequiresStructuralFields) {
    const auto current = json(info());
    EXPECT_EQ(json(current.get<SessionInfo>()), current);
    for (const auto* field : {"discussion_id", "objective_id", "inherited_messages", "frozen", "implementation_ids",
        "active_implementation_id", "execution_workspace", "configuration_workspace", "reasoning", "mode", "provider",
        "provider_context_start", "terminalOpenings", "auto_plan_pace", "auto_plan_error"}) {
        auto incomplete = current;
        incomplete.erase(field);
        EXPECT_THROW(incomplete.get<SessionInfo>(), json::out_of_range) << field;
    }
}

TEST_F(SessionTest, ImplementationBranchRequiresObjectiveAndDoesNotInheritApprovals) {
    SessionRegistry registry(directory / "sessions", factory());
    auto discussion = registry.create(info());
    EXPECT_THROW(registry.createImplementation(*discussion), std::logic_error);
    Message request{"request", MessageRole::USER, {TextContent{"Sane Implementation request"}}};
    request.userMetadata = UserMessageMetadata{"sane", true};
    Message approval{"approval", MessageRole::SYSTEM, {TextContent{"approved"}}};
    approval.manualApproval = ManualApprovalChange{"refused", true};
    ToolResultContent refusal{"call-refused", "Refused", true};
    refusal.metadata = {{"refused_action", {{"tool", "bash"}, {"arguments", {{"command", "echo yes"}}}}},
        {"refused_message_id", "refused"}, {"permissions", json::array({{{"decision", "ai_refused"}}})}};
    Message refused{"refused", MessageRole::TOOL, {refusal}};
    Message plan{"plan", MessageRole::ASSISTANT, {TextContent{"Objective"}}};
    plan.objectivePlan = true;
    discussion->record({AgentEventType::HARNESS_MESSAGE_ARRIVED, {request, refused, approval, plan}, {}, {}});
    auto branch = registry.createImplementation(*discussion);
    EXPECT_EQ(branch->getInfo().inheritedMessages, 5); // Includes the pinned Objective Plan version.
    for (const auto& message : branch->getMessages()) EXPECT_FALSE(message.manualApproval);
    EXPECT_TRUE(branch->manualActions().empty());
    EXPECT_THROW(registry.createImplementation(*branch), std::logic_error);
    auto unrelated = registry.create(info());
    EXPECT_THROW(registry.freezeImplementation(*unrelated, *branch), std::logic_error);
}

TEST_F(SessionTest, SaneRequestSubmitsFinalPlanAndRestoresImplementCondition) {
    SessionRegistry registry(directory / "sessions", factory());
    auto session = registry.create(info());
    const auto id = session->getInfo().id;
    std::vector<AgentEventType> events;
    turn(*session, "Create a tiny status page", events);
    provider->respond = [](const json&, std::size_t call) {
        if (call == 2) return json{{"id", "plan-call"}, {"status", "completed"}, {"output", json::array({{
            {"type", "function_call"}, {"id", "item"}, {"call_id", "call"},
            {"name", "submit_objective_plan"}, {"arguments", json{{"plan", "Build the status page, then verify it."},
                {"stack", "- Textual: terminal UI"}}.dump()}
        }})}};
        return reply("after-plan-" + std::to_string(call), "Submitted.");
    };
    turn(*session, "Sane Implementation request", events, ExecutionMode::SANE, {}, true);
    ASSERT_TRUE(session->objectivePlanReady());
    const auto messages = session->getMessages();
    const auto objective = session->latestObjectivePlan();
    ASSERT_TRUE(objective);
    EXPECT_EQ(std::get<TextContent>(objective->content.front()).text, "Build the status page, then verify it.");
    ASSERT_TRUE(objective->objectiveStack);
    EXPECT_EQ(*objective->objectiveStack, "- Textual: terminal UI");
    const auto user = std::find_if(messages.begin(), messages.end(), [](const Message& message) {
        return message.userMetadata && message.userMetadata->saneRequest;
    });
    ASSERT_NE(user, messages.end());
    EXPECT_EQ(std::get<TextContent>(user->content.front()).text, "Sane Implementation request");
    const auto planningInput = provider->inputs[1].dump();
    EXPECT_NE(planningInput.find("submit_objective_plan"), std::string::npos);
    EXPECT_NE(planningInput.find("separate plan and stack fields"), std::string::npos);
    EXPECT_NE(planningInput.find("never ask the user to pick a package"), std::string::npos);
    EXPECT_NE(planningInput.find("substantially different product directions"), std::string::npos);
    session.reset();
    session = registry.load(id);
    EXPECT_TRUE(session->objectivePlanReady());
    const auto page = session->history().page();
    bool foundRequest = false, foundPlan = false;
    for (std::size_t i = 0; i < page.count; ++i) {
        const auto record = page.record(i);
        if (record.at("id") == user->id) {
            foundRequest = true;
            EXPECT_TRUE(record.at("metadata").at("sane_request"));
        }
        if (record.value("objective_plan", false)) {
            foundPlan = true;
            EXPECT_EQ(record.at("objective_stack"), "- Textual: terminal UI");
        }
    }
    EXPECT_TRUE(foundRequest);
    EXPECT_TRUE(foundPlan);
    turn(*session, "Let's adjust it", events);
    EXPECT_TRUE(session->objectivePlanReady());
}

TEST_F(SessionTest, ObjectiveDrawerMetadataArrivesOnCompletedTurnsOnly) {
    const auto selected = json{{"type", "implementation.open"}, {"sessionId", "discussion"},
        {"objectiveId", "historical-objective"}}.get<IntegrationRequest>();
    EXPECT_EQ(selected.objectiveId, "historical-objective");
    provider->respond = [](const json&, std::size_t call) {
        if (call == 2) {
            auto response = submitImplementation({{"plan", "Ready objective"}}, call);
            response["output"][0]["name"] = "submit_objective_plan";
            return response;
        }
        return reply("answer-" + std::to_string(call), "Final answer");
    };
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await receiveEvent(client);
        co_await client.send({{"type", "integration.configure"}, {"workspace", directory.string()}, {"mode", "sane"}});
        co_await receiveEvent(client);
        for (int turn = 0; turn < 3; ++turn) {
            co_await client.send({{"type", "message.send"}, {"text", "Discuss " + std::to_string(turn)},
                {"workspace", directory.string()}});
            for (;;) {
                const auto event = co_await receiveEvent(client);
                if (event.at("type") != "execution.completed") continue;
                const auto& snapshots = event.at("info").at("objective_plan_turns");
                EXPECT_EQ(snapshots.size(), turn);
                if (turn > 0 && !snapshots.empty()) {
                    EXPECT_EQ(snapshots.back().at("updated"), turn == 1);
                    EXPECT_EQ(snapshots.back().at("objective_id"),
                        event.at("info").at("objective_plans")[0].at("id"));
                    EXPECT_FALSE(snapshots.back().at("message_id").get<std::string>().empty());
                }
                break;
            }
        }
        co_await client.send({{"type", "integration.close"}});
    });
}

TEST_F(SessionTest, CompletedObjectiveStaysHistoricalAcrossResumeAndNewDiscussion) {
    SessionRegistry registry(directory / "sessions", factory());
    auto session = registry.create(info());
    const auto id = session->getInfo().id;
    Message user{"u1", MessageRole::USER, {TextContent{"Discuss"}}};
    user.userMetadata = UserMessageMetadata{"sane"};
    Message plan{"p1", MessageRole::ASSISTANT, {TextContent{"First objective"}}};
    plan.objectivePlan = true;
    session->record({AgentEventType::HARNESS_MESSAGE_ARRIVED, {user, plan}, {}, {}});
    auto branch = registry.createImplementation(*session);
    registry.freezeImplementation(*session, *branch);
    EXPECT_FALSE(session->objectivePlanReady());
    EXPECT_FALSE(session->latestObjectivePlan());
    ASSERT_EQ(session->objectivePlanTurns().size(), 1);
    EXPECT_EQ(session->objectivePlanTurns()[0].messageId, plan.id);
    session.reset();
    session = registry.load(id);
    EXPECT_FALSE(session->objectivePlanReady());
    user.id = "u2";
    Message answer{"a2", MessageRole::ASSISTANT, {TextContent{"An ordinary answer"}}};
    session->record({AgentEventType::HARNESS_MESSAGE_ARRIVED, {user, answer}, {}, {}});
    ASSERT_EQ(session->objectivePlanTurns().size(), 1);
    EXPECT_EQ(session->objectivePlanTurns()[0].objectiveId, "p1");
    EXPECT_THROW(registry.createImplementation(*session), std::logic_error);
    user.id = "u3";
    plan.id = "p2";
    session->record({AgentEventType::HARNESS_MESSAGE_ARRIVED, {user, plan}, {}, {}});
    EXPECT_TRUE(session->objectivePlanReady());
    ASSERT_EQ(session->objectivePlanTurns().size(), 2);
    EXPECT_EQ(session->objectivePlanTurns().back().objectiveId, "p2");
    EXPECT_TRUE(session->objectivePlanTurns().back().updated);
    EXPECT_EQ(session->objectivePlans().size(), 2);
    auto second = registry.createImplementation(*session);
    registry.freezeImplementation(*session, *second);
    EXPECT_FALSE(session->objectivePlanReady());
    ASSERT_EQ(session->objectivePlanTurns().size(), 2);
}

TEST_F(SessionTest, SlopTurnsDoNotRepeatAnObjectiveAndReturningToSaneKeepsIt) {
    SessionRegistry registry(directory / "sessions", factory());
    auto session = registry.create(info());
    Message user{"u1", MessageRole::USER, {TextContent{"Discuss"}}};
    user.userMetadata = UserMessageMetadata{"sane"};
    Message plan{"p1", MessageRole::ASSISTANT, {TextContent{"Current objective"}}};
    plan.objectivePlan = true;
    session->record({AgentEventType::HARNESS_MESSAGE_ARRIVED, {user, plan}, {}, {}});
    user.id = "u2";
    user.userMetadata->mode = "slop";
    Message answer{"a2", MessageRole::ASSISTANT, {TextContent{"Slop answer"}}};
    session->record({AgentEventType::HARNESS_MESSAGE_ARRIVED, {user, answer}, {}, {}});
    ASSERT_EQ(session->objectivePlanTurns().size(), 1);
    EXPECT_TRUE(session->objectivePlanReady());
    user.id = "u3";
    user.userMetadata->mode = "sane";
    answer.id = "a3";
    session->record({AgentEventType::HARNESS_MESSAGE_ARRIVED, {user, answer}, {}, {}});
    ASSERT_EQ(session->objectivePlanTurns().size(), 2);
    EXPECT_EQ(session->objectivePlanTurns().back().messageId, "a3");
    EXPECT_EQ(session->objectivePlanTurns().back().objectiveId, "p1");
    EXPECT_FALSE(session->objectivePlanTurns().back().updated);
}

TEST_F(SessionTest, ObjectiveDrawersKeepTurnSnapshotsAfterPaginationAndResume) {
    std::string id;
    {
        SessionRegistry registry(directory / "sessions", factory());
        auto session = registry.create(info());
        id = session->getInfo().id;
        std::vector<Message> history;
        for (int i = 0; i < 30; ++i) {
            const auto suffix = std::to_string(i);
            history.push_back(Message{"user-" + suffix, MessageRole::USER, {TextContent{"Discuss"}}});
            history.push_back(Message{"intermediate-" + suffix, MessageRole::ASSISTANT, {TextContent{"Thinking"}}});
            if (i == 1 || i == 10) {
                Message plan{"plan-" + suffix, MessageRole::ASSISTANT, {TextContent{"Objective " + suffix}}};
                plan.objectivePlan = true;
                history.push_back(plan);
            }
            history.push_back(Message{"answer-" + suffix, MessageRole::ASSISTANT, {TextContent{"Final answer"}}});
        }
        session->record({AgentEventType::HARNESS_MESSAGE_ARRIVED, history, {}, {}});
        const auto snapshots = session->objectivePlanTurns();
        ASSERT_EQ(snapshots.size(), 29);
        EXPECT_EQ(snapshots[0].messageId, "answer-1");
        EXPECT_EQ(snapshots[0].objectiveId, "plan-1");
        EXPECT_TRUE(snapshots[0].updated);
        EXPECT_EQ(snapshots[1].objectiveId, "plan-1");
        EXPECT_FALSE(snapshots[1].updated);
        EXPECT_EQ(snapshots[9].objectiveId, "plan-10");
        EXPECT_TRUE(snapshots[9].updated);
        EXPECT_EQ(snapshots.back().messageId, "answer-29");
        EXPECT_FALSE(snapshots.back().updated);
    }
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await receiveEvent(client);
        co_await client.send({{"type", "integration.configure"}, {"workspace", directory.string()}, {"mode", "sane"}});
        co_await receiveEvent(client);
        co_await client.send({{"type", "session.attach"}, {"sessionId", id}});
        const auto attached = co_await receiveEvent(client);
        EXPECT_EQ(attached.at("type"), "session.attached") << attached.dump();
        if (attached.at("type") != "session.attached") co_return;
        EXPECT_TRUE(attached.at("page").at("hasOlder"));
        const auto& snapshots = attached.at("info").at("objective_plan_turns");
        EXPECT_EQ(snapshots.size(), 29);
        if (snapshots.size() != 29) co_return;
        EXPECT_EQ(snapshots[0], (json{{"message_id", "answer-1"}, {"objective_id", "plan-1"}, {"updated", true}}));
        EXPECT_EQ(snapshots[1].at("updated"), false);
        EXPECT_EQ(snapshots[9].at("objective_id"), "plan-10");
        EXPECT_EQ(attached.at("info").at("objective_plans").size(), 2);
        co_await client.send({{"type", "integration.close"}});
    });
}

TEST_F(SessionTest, LiveObjectiveVersionsPatchAtomicallyAndSelectedVersionStartsImplementation) {
    SessionRegistry registry(directory / "sessions", factory());
    auto session = registry.create(info());
    const auto id = session->getInfo().id;
    provider->respond = [](const json&, std::size_t call) {
        if (call > 4) return reply("answer-" + std::to_string(call), "Updated when needed.");
        json arguments;
        std::string name = "patch_objective_plan";
        if (call == 1) {
            name = "submit_objective_plan";
            arguments = {{"plan", "Build a status page. Verify transitions."}, {"stack", "- Textual"}};
        } else if (call == 2) arguments = {{"updates", json::array({
            {{"field", "plan"}, {"old_str", "status page"}, {"new_str", "broken page"}},
            {{"field", "plan"}, {"old_str", "missing target"}, {"new_str", "invalid"}}})}};
        else if (call == 3) arguments = {{"updates", json::array({
            {{"field", "plan"}, {"old_str", "status page"}, {"new_str", "health page"}}})}};
        else arguments = {{"updates", json::array({
            {{"field", "stack"}, {"old_str", "Textual"}, {"new_str", "Existing UI"}}})}};
        auto response = submitImplementation(arguments, call);
        response["output"][0]["name"] = name;
        return response;
    };
    std::vector<AgentEventType> events;
    turn(*session, "Design a ready-to-use status page", events);
    const auto plans = session->objectivePlans();
    ASSERT_EQ(plans.size(), 3);
    EXPECT_EQ(std::get<TextContent>(plans[0].content[0]).text, "Build a status page. Verify transitions.");
    EXPECT_EQ(std::get<TextContent>(plans[1].content[0]).text, "Build a health page. Verify transitions.");
    EXPECT_EQ(plans[2].objectiveStack, "- Existing UI");
    EXPECT_NE(provider->inputs.front().dump().find("patch_objective_plan"), std::string::npos);
    EXPECT_NE(provider->inputs.front().dump().find("call the plan tool in this same turn"), std::string::npos);
    EXPECT_NE(provider->inputs.front().dump().find("A chat-only feature list does not publish an Objective Plan"), std::string::npos);
    EXPECT_NE(provider->inputs.front().dump().find("publication neither accepts it nor starts implementation"), std::string::npos);
    EXPECT_NE(provider->inputs.front().dump().find("Discuss missing decisions first"), std::string::npos);
    turn(*session, "Explain the tradeoff without changing the plan", events);
    EXPECT_EQ(session->objectivePlans().size(), 3);
    session.reset();
    session = registry.load(id);
    EXPECT_EQ(session->objectivePlans().size(), 3);
    EXPECT_THROW(registry.createImplementation(*session, "foreign-plan"), std::logic_error);
    auto branch = registry.createImplementation(*session, plans[0].id);
    EXPECT_EQ(branch->getInfo().objectiveId, plans[0].id);
    const auto inherited = branch->getMessages();
    const auto pinned = inherited[inherited.size() - 2];
    ASSERT_EQ(pinned.role, MessageRole::SYSTEM);
    const auto text = std::get<TextContent>(pinned.content[0]).text;
    EXPECT_NE(text.find("Build a status page."), std::string::npos);
    EXPECT_EQ(text.find("Build a health page."), std::string::npos);
}

TEST_F(SessionTest, ObjectiveVersionSurvivesFailedTurnAndSlopHasNoPlanningTools) {
    SessionRegistry registry(directory / "sessions", factory());
    auto session = registry.create(info());
    const auto id = session->getInfo().id;
    provider->respond = [](const json&, std::size_t call) -> json {
        if (call != 1) throw std::runtime_error("Provider failed after publishing the plan");
        auto response = submitImplementation({{"plan", "Ready objective"}}, call);
        response["output"][0]["name"] = "submit_objective_plan";
        return response;
    };
    std::vector<AgentEventType> events;
    turn(*session, "Discuss the objective", events);
    ASSERT_EQ(session->objectivePlans().size(), 1);
    session.reset();
    session = registry.load(id);
    ASSERT_EQ(session->objectivePlans().size(), 1);
    EXPECT_EQ(std::get<TextContent>(session->latestObjectivePlan()->content[0]).text, "Ready objective");
    EXPECT_TRUE(session->objectivePlanReady());
    provider->respond = {};
    turn(*session, "A Slop turn", events, ExecutionMode::SLOP);
    for (const auto& tool : provider->toolNames) {
        EXPECT_NE(tool, "submit_objective_plan");
        EXPECT_NE(tool, "patch_objective_plan");
    }
}

TEST_F(SessionTest, SaneRequestWithoutTargetHasNoImplementAction) {
    SessionRegistry registry(directory / "sessions", factory());
    auto session = registry.create(info());
    provider->responseText = "There is no implementation target yet.";
    std::vector<AgentEventType> events;
    turn(*session, "Sane Implementation request", events, ExecutionMode::SANE, {}, true);
    EXPECT_FALSE(session->objectivePlanReady());
    const auto history = session->getMessages();
    EXPECT_TRUE(history[1].userMetadata->saneRequest);
    EXPECT_FALSE(history.back().objectivePlan);
}

TEST_F(SessionTest, QuestionChoiceFlowsThroughToolBeforeFinalPlan) {
    provider->respond = [](const json&, std::size_t call) {
        if (call <= 2) return json{{"id", "choice-call-" + std::to_string(call)}, {"status", "completed"},
            {"output", json::array({{{"type", "function_call"}, {"id", "item-" + std::to_string(call)},
                {"call_id", "call-" + std::to_string(call)},
                {"name", call == 1 ? "ask_choice" : "submit_objective_plan"},
                {"arguments", call == 1 ? json{{"question", "Which?"}, {"options", {"A", "B"}}}.dump()
                                        : json{{"plan", "Use choice B."}}.dump()}}})}};
        return reply("choice-done", "Submitted.");
    };
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await receiveEvent(client);
        co_await client.send({{"type", "integration.configure"}, {"workspace", directory.string()}, {"mode", "sane"}});
        EXPECT_EQ((co_await receiveEvent(client)).at("type"), "integration.configured");
        co_await client.send({{"type", "message.send"}, {"requestId", "send"}, {"workspace", directory.string()},
            {"text", "Sane Implementation request"}, {"mode", "sane"}, {"sane_request", true}});
        bool question = false, completed = false, published = false;
        while (!completed) {
            const auto event = co_await receiveEvent(client);
            if (event.contains("info") && !event.at("info").at("objective_plans").empty()) {
                EXPECT_TRUE(question);
                if (event.at("type") != "execution.completed") published = true;
                EXPECT_EQ(event.at("info").at("objective_plans")[0].at("content")[0].at("text"), "Use choice B.");
            }
            if (event.at("type") == "question.requested") {
                question = true;
                EXPECT_EQ(event.at("question"), "Which?");
                co_await client.send({{"type", "question.reply"}, {"questionId", event.at("questionId")},
                    {"choice", 1}, {"requestId", "answer"}});
            }
            if (event.at("type") == "execution.completed") {
                completed = true;
                EXPECT_TRUE(event.at("info").at("objective_plan_ready"));
                EXPECT_TRUE(published); // Published live, before the terminal event.
            }
        }
        EXPECT_TRUE(question);
        co_await client.send({{"type", "integration.close"}});
    }, {}, {}, std::chrono::seconds(5), std::chrono::milliseconds(500));
}

asio::awaitable<json> receiveEvent(ConnectionSocket& client) {
    for (;;) {
        const auto event = co_await receiveReply(client);
        if (!event) throw std::runtime_error("Unexpected disconnect");
        if (event->at("type") != "integration.ping") co_return *event;
        co_await client.send({{"type", "integration.pong"}, {"pingId", event->at("pingId")}});
    }
}

TEST_F(SessionTest, UndoRedoRemovesWholeTurnsPersistsAndDoesNotCallProvider) {
    SessionRegistry registry(directory / "sessions", factory());
    auto session = registry.create(info());
    const auto id = session->getInfo().id;
    std::vector<AgentEventType> events;
    turn(*session, "first", events);
    const auto first = session->getMessages();
    turn(*session, "second", events, ExecutionMode::SLOP);
    const auto second = session->getMessages();
    const auto calls = provider->inputs.size();
    const auto draft = navigate(*session, false);
    EXPECT_EQ(std::get<TextContent>(draft.content[0]).text, "second");
    EXPECT_EQ(json(session->getMessages()), json(first));
    EXPECT_EQ(session->getInfo().mode, ExecutionMode::SANE);
    session.reset();
    session = registry.load(id);
    navigate(*session, true);
    EXPECT_EQ(json(session->getMessages()), json(second));
    EXPECT_EQ(session->getInfo().mode, ExecutionMode::SLOP);
    EXPECT_EQ(provider->inputs.size(), calls);
    navigate(*session, false);
    navigate(*session, false);
    EXPECT_EQ(session->messageCount(), 1);
    navigate(*session, true);
    EXPECT_EQ(json(session->getMessages()), json(first));
    turn(*session, "replacement", events);
    EXPECT_THROW(navigate(*session, true), std::runtime_error);
    EXPECT_EQ(provider->inputs.back().dump().find("second"), std::string::npos);
}

TEST_F(SessionTest, FailedAndCancelledTurnsRemainUndoable) {
    SessionRegistry registry(directory / "sessions", factory());
    auto session = registry.create(info());
    std::vector<AgentEventType> events;
    provider->fail = true;
    turn(*session, "failing request", events);
    EXPECT_EQ(events.back(), AgentEventType::EXECUTION_FAILED);
    navigate(*session, false);
    EXPECT_EQ(session->messageCount(), 1);
    navigate(*session, true);
    EXPECT_EQ(session->lastUserMessage(), "failing request");
    provider->fail = false;
    provider->respond = [](const auto&, auto) -> json { throw core::Cancelled{}; };
    turn(*session, "cancelled request", events);
    EXPECT_EQ(events.back(), AgentEventType::EXECUTION_CANCELLED);
    navigate(*session, false);
    EXPECT_EQ(session->lastUserMessage(), "failing request");
}

TEST_F(SessionTest, UndoAfterProviderFailureRestoresActualToolWrites) {
    std::ofstream(directory / "edited") << "before";
    provider->respond = [](const auto&, std::size_t call) -> json {
        if (call == 1) return {{"id", "write"}, {"status", "completed"}, {"output", json::array({{
            {"type", "function_call"}, {"id", "edit-call"}, {"call_id", "edit-call"}, {"name", "write"},
            {"arguments", R"({"filePath":"edited","content":"after"})"}
        }})}};
        throw std::runtime_error("Provider failed after write");
    };
    SessionRegistry registry(directory / "sessions", [this](const SessionInfo& info) {
        ToolRegistry tools;
        tools.add(std::make_unique<WriteTool>());
        return std::make_unique<Agent>(std::make_unique<TestProvider>(provider), info.model, std::move(tools),
            ToolContext{info.workspace, info.workspace, [](const auto&) {}});
    });
    auto session = registry.create(info());
    std::vector<AgentEventType> events;
    turn(*session, "edit then fail", events, ExecutionMode::SLOP);
    const auto contents = [&] { std::ifstream file(directory / "edited"); return std::string(std::istreambuf_iterator<char>(file), {}); };
    EXPECT_EQ(contents(), "after");
    EXPECT_EQ(events.back(), AgentEventType::EXECUTION_FAILED);
    navigate(*session, false);
    EXPECT_EQ(contents(), "before");
    EXPECT_EQ(session->messageCount(), 1);
    navigate(*session, true);
    EXPECT_EQ(contents(), "after");
    EXPECT_EQ(provider->inputs.size(), 2);
}

TEST_F(SessionTest, UndoRestoresImagesAndRemovesCompactionOfTheUndoneTurn) {
    SessionRegistry registry(directory / "sessions", factory());
    auto session = registry.create(info());
    std::vector<AgentEventType> events;
    turn(*session, "Keep this", events);
    const auto kept = session->getMessages();
    const auto image = validateImage({"", "image/png", "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a6a0AAAAASUVORK5CYII=", 0, 0, "pixel.png", "clipboard"});
    turn(*session, "Remove this image turn", events, ExecutionMode::SANE, {image});
    TestProvider summarizer(std::make_shared<ProviderState>());
    session->applyCompaction(CompactionService{}.compact(session->activeConversation(), summarizer, session->getInfo(), "compact-undo"));
    const auto compacted = session->getMessages();
    const auto draft = navigate(*session, false);
    ASSERT_EQ(draft.images.size(), 1);
    EXPECT_EQ(draft.images[0].data, image.data);
    EXPECT_FALSE(session->hasStoredImages());
    EXPECT_EQ(json(session->getMessages()), json(kept));
    EXPECT_EQ(session->activeConversation().getMessages().size(), kept.size());
    navigate(*session, true);
    EXPECT_EQ(json(session->getMessages()), json(compacted));
    const auto active = session->activeConversation().getMessages();
    EXPECT_TRUE(std::any_of(active.begin(), active.end(), [](const auto& message) { return message.compaction.has_value(); }));
}

TEST_F(SessionTest, UndoRedoProtocolStreamsHistoryAndDraftWithoutGenerating) {
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        const auto next = [&]() -> asio::awaitable<json> {
            for (;;) {
                auto response = co_await receiveReply(client);
                if (!response) throw std::runtime_error("Disconnected during undo/redo");
                if (response->at("type") != "integration.ping") co_return *response;
                co_await client.send({{"type", "integration.pong"}, {"pingId", response->at("pingId")}});
            }
        };
        co_await client.receive();
        co_await client.send({{"type", "integration.configure"}, {"workspace", directory.string()}, {"mode", "sane"}});
        co_await client.receive();
        co_await client.send({{"type", "message.send"}, {"workspace", directory.string()}, {"text", "original"}});
        while ((co_await client.receive())->at("type") != "execution.completed") {}
        const auto calls = provider->inputs.size();
        co_await client.send({{"type", "session.undo"}, {"requestId", "undo"}});
        const auto undone = co_await next();
        EXPECT_EQ(undone.at("purpose"), "navigation");
        EXPECT_EQ(undone.at("messages")[0]["content"][0]["text"], "original");
        co_await client.send({{"type", "session.redo"}, {"requestId", "redo"}});
        const auto redone = co_await next();
        EXPECT_EQ(redone.at("purpose"), "navigation");
        EXPECT_GT(redone.at("messages").size(), undone.at("messages").size());
        EXPECT_EQ(provider->inputs.size(), calls);
        client.close();
    }, {}, {}, std::chrono::seconds(5), std::chrono::seconds(3));
}

TEST_F(SessionTest, ViewImageRestoresPixelsAfterCompactionAndAnotherResume) {
    provider->imageSupport = ImageSupport::SUPPORTED;
    const auto image = validateImage({"", "image/png", "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a6a0AAAAASUVORK5CYII=", 0, 0, "pixel.png", "clipboard"});
    SessionRegistry registry(directory / "sessions", factory());
    auto session = registry.create(info());
    const auto id = session->getInfo().id;
    std::vector<AgentEventType> events;
    turn(*session, "", events, ExecutionMode::SANE, {image});
    auto summaryState = std::make_shared<ProviderState>();
    TestProvider summarizer(summaryState);
    session->applyCompaction(CompactionService{}.compact(session->activeConversation(), summarizer,
        session->getInfo(), "checkpoint", {}, 0));
    EXPECT_FALSE(session->hasActiveImages());
    EXPECT_EQ(summaryState->inputs[0].dump().find(image.data), std::string::npos);
    session.reset();
    session = registry.load(id);
    provider->respond = [&](const json& input, std::size_t call) {
        if (call == 2) {
            EXPECT_EQ(input.dump().find(image.data), std::string::npos);
            return json{{"id", "view"}, {"status", "completed"}, {"output", json::array({{
                {"type", "function_call"}, {"call_id", "view-call"}, {"name", "view_image"},
                {"arguments", json{{"image_id", image.id}}.dump()}}})}};
        }
        EXPECT_NE(input.dump().find(image.data), std::string::npos);
        return reply("seen", "Image inspected");
    };
    turn(*session, "Inspect the saved image", events);
    EXPECT_TRUE(session->hasActiveImages());
    EXPECT_EQ(provider->inputs.size(), 3);
    session.reset();
    session = registry.load(id);
    EXPECT_NE(session->activeConversation().getProviderFormat().dump().find(image.data), std::string::npos);
}

TEST_F(SessionTest, BtwRunsBesidePrincipalSurvivesItsCancellationAndContinuesAfterResume) {
    provider->waitForCancellation = true;
    auto side = std::make_shared<ProviderState>();
    std::promise<void> release;
    auto released = release.get_future().share();
    side->respond = [released](const json&, std::size_t call) {
        if (call == 1) EXPECT_EQ(released.wait_for(std::chrono::seconds(3)), std::future_status::ready);
        return reply("btw-answer-" + std::to_string(call), "Side answer");
    };
    const auto factory = [side](const ProviderSelection&) { return std::make_unique<TestProvider>(side); };
    std::string sessionId;
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await receiveEvent(client);
        co_await client.send({{"type", "message.send"}, {"text", "Principal task"}, {"workspace", directory.string()}});
        sessionId = (co_await receiveEvent(client)).at("session").at("id");
        co_await client.send({{"type", "btw.send"}, {"sessionId", sessionId}, {"text", "Explain the approach"}, {"requestId", "side"}});
        for (;;) {
            const auto event = co_await receiveEvent(client);
            if (event.at("type") == "btw.accepted") {
                EXPECT_EQ(event.at("branchId"), "btw-1");
                break;
            }
        }
        co_await client.send({{"type", "execution.cancel"}});
        while ((co_await receiveEvent(client)).at("type") != "execution.cancelled") {}
        release.set_value();
        for (;;) {
            const auto event = co_await receiveEvent(client);
            if (event.at("type") == "btw.started" && event.at("branch").at("status") == "completed") break;
        }
        co_await client.send({{"type", "btw.send"}, {"sessionId", sessionId}, {"branchId", "btw-1"},
            {"text", "A follow-up"}, {"requestId", "continue"}});
        for (;;) {
            const auto event = co_await receiveEvent(client);
            if (event.at("type") == "btw.started" && event.at("branch").at("status") == "completed") break;
        }
        co_await client.send({{"type", "integration.close"}});
    }, {}, {}, std::chrono::seconds(5), std::chrono::milliseconds(500), {}, factory);
    ASSERT_EQ(side->inputs.size(), 2);
    EXPECT_NE(side->inputs[1].dump().find("Principal task"), std::string::npos);
    EXPECT_NE(side->inputs[1].dump().find("Explain the approach"), std::string::npos);
    EXPECT_NE(side->inputs[1].dump().find("Side answer"), std::string::npos);
    EXPECT_EQ(side->inputs[1].dump().find("previous run was interrupted"), std::string::npos);
    {
        SessionJournal journal(directory / "sessions" / sessionId);
        const auto branch = journal.readBtw("btw-1");
        EXPECT_EQ(branch.messages.size(), 4);
        EXPECT_EQ(branch.status, "completed");
        EXPECT_EQ(journal.readAll().size(), 3); // No BTW messages in the principal context.
    }
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await receiveEvent(client);
        co_await client.send({{"type", "integration.configure"}, {"workspace", directory.string()}, {"mode", "sane"}});
        EXPECT_EQ((co_await receiveEvent(client)).at("type"), "integration.configured");
        co_await client.send({{"type", "session.attach"}, {"sessionId", sessionId}});
        EXPECT_EQ((co_await receiveEvent(client)).at("type"), "session.attached");
        co_await client.send({{"type", "btw.list"}, {"sessionId", sessionId}});
        bool found = false;
        for (;;) {
            const auto event = co_await receiveEvent(client);
            if (event.at("type") == "btw.started") {
                found = true;
                EXPECT_EQ(event.at("branch").at("id"), "btw-1");
                EXPECT_EQ(event.at("messageCount"), 4);
            }
            if (event.at("type") == "btw.listed") break;
        }
        EXPECT_TRUE(found);
        co_await client.send({{"type", "btw.send"}, {"sessionId", sessionId}, {"branchId", "btw-1"}, {"text", "After restart"}});
        for (;;) {
            const auto event = co_await receiveEvent(client);
            if (event.at("type") == "btw.started" && event.at("branch").at("status") == "completed") break;
        }
        co_await client.send({{"type", "integration.close"}});
    }, {}, {}, std::chrono::seconds(5), std::chrono::milliseconds(500), {}, factory);
    EXPECT_EQ(side->inputs.size(), 3);
}

TEST_F(SessionTest, IdleBtwAfterResumeFreezesContextAndKeepsItsStandalonePlacement) {
    std::string sessionId, anchor, pageAnchor;
    std::size_t principalCount;
    {
        SessionRegistry registry(directory / "sessions", factory());
        auto session = registry.create(info());
        EXPECT_THROW(session->btwSnapshot(), std::logic_error);
        std::vector<AgentEventType> events;
        turn(*session, "Completed principal request", events);
        sessionId = session->getInfo().id;
        principalCount = session->messageCount();
        anchor = session->btwStore()->historyIndex().lastRecordId();
        pageAnchor = session->history().page().last;
        EXPECT_FALSE(session->btwSnapshot().empty());
    }
    const auto principalCalls = provider->inputs.size();
    auto side = std::make_shared<ProviderState>();
    const auto factory = [side](const ProviderSelection&) { return std::make_unique<TestProvider>(side); };
    for (int connection = 0; connection < 2; ++connection) {
        healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
            co_await receiveEvent(client);
            co_await client.send({{"type", "btw.send"}, {"sessionId", ""}, {"text", "No principal yet"}, {"requestId", "empty"}});
            EXPECT_EQ((co_await receiveEvent(client)).at("type"), "request.failed");
            co_await client.send({{"type", "integration.configure"}, {"workspace", directory.string()}, {"mode", "sane"}});
            EXPECT_EQ((co_await receiveEvent(client)).at("type"), "integration.configured");
            co_await client.send({{"type", "session.attach"}, {"sessionId", sessionId}});
            EXPECT_EQ((co_await receiveEvent(client)).at("type"), "session.attached");
            co_await client.send({{"type", "btw.send"}, {"sessionId", sessionId},
                {"branchId", connection == 0 ? "" : "btw-1"}, {"text", connection == 0 ? "Idle question" : "Follow-up"}});
            for (;;) {
                const auto event = co_await receiveEvent(client);
                if (event.at("type") != "btw.started") continue;
                const auto& branch = event.at("branch");
                EXPECT_EQ(branch.at("standalone"), true);
                EXPECT_EQ(branch.at("anchor"), anchor);
                EXPECT_EQ(branch.at("pageAnchor"), pageAnchor);
                if (branch.at("status") == "completed") break;
            }
            co_await client.send({{"type", "integration.close"}});
        }, {}, {}, std::chrono::seconds(5), std::chrono::milliseconds(500), {}, factory);
    }
    ASSERT_EQ(side->inputs.size(), 2);
    EXPECT_EQ(provider->inputs.size(), principalCalls); // Reconstructing context must not run the principal.
    for (const auto& input : side->inputs) {
        EXPECT_NE(input.dump().find("Completed principal request"), std::string::npos);
        EXPECT_EQ(input.dump().find("was being processed"), std::string::npos);
    }
    EXPECT_NE(side->inputs[1].dump().find("Idle question"), std::string::npos);
    SessionJournal saved(directory / "sessions" / sessionId);
    EXPECT_EQ(saved.count(), principalCount);
    EXPECT_TRUE(saved.readBtw("btw-1").standalone);
}

TEST_F(SessionTest, DisconnectInterruptsAndPersistsAllParallelBtwBranches) {
    provider->waitForCancellation = true;
    std::vector<std::shared_ptr<ProviderState>> branches;
    const auto factory = [&](const ProviderSelection&) {
        auto state = std::make_shared<ProviderState>();
        state->waitForCancellation = true;
        branches.push_back(state);
        return std::make_unique<TestProvider>(state);
    };
    std::string sessionId;
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await receiveEvent(client);
        co_await client.send({{"type", "message.send"}, {"text", "Principal"}, {"workspace", directory.string()}});
        sessionId = (co_await receiveEvent(client)).at("session").at("id");
        for (int i = 1; i <= 2; ++i) {
            co_await client.send({{"type", "btw.send"}, {"sessionId", sessionId}, {"text", "Independent question"}});
            for (;;) {
                const auto event = co_await receiveEvent(client);
                if (event.at("type") == "btw.accepted") {
                    EXPECT_EQ(event.at("branchId"), "btw-" + std::to_string(i));
                    break;
                }
            }
        }
        client.close();
    }, {}, {}, std::chrono::seconds(5), std::chrono::milliseconds(500), {}, factory);
    ASSERT_EQ(branches.size(), 2);
    SessionJournal journal(directory / "sessions" / sessionId);
    for (const auto& branch : journal.listBtw()) {
        EXPECT_EQ(branch.status, "interrupted");
        ASSERT_GE(branch.messages.size(), 2);
        EXPECT_EQ(branch.messages.front().role, MessageRole::USER);
        EXPECT_EQ(branch.messages.back().role, MessageRole::SYSTEM);
    }
}

TEST_F(SessionTest, RetryEventsReachTheClientWithoutDuplicatingPersistedMessages) {
    provider->respond = [](const json&, std::size_t call) {
        if (call == 1) throw RetryableGenerationError("OpenAI connection interrupted");
        return reply("recovered", "Recovered");
    };
    std::string id;
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await receiveEvent(client);
        co_await client.send({{"type", "message.send"}, {"text", "Hello"}, {"workspace", directory.string()}});
        const auto created = co_await receiveEvent(client);
        id = created.at("session").at("id");
        std::vector<json> retries;
        for (;;) {
            const auto event = co_await receiveEvent(client);
            if (event.at("type") == "execution.retry") {
                retries.push_back(event.at("retry"));
                EXPECT_TRUE(event.at("messages").empty());
            }
            if (event.at("type") == "execution.completed") break;
        }
        EXPECT_EQ(retries, (std::vector<json>{
            {{"attempt", 1}, {"limit", 10}, {"seconds", 0}, {"reason", "Connection problem"}, {"error", "OpenAI connection interrupted"}},
            {{"attempt", 0}, {"limit", 10}, {"seconds", 0}, {"reason", ""}, {"error", ""}}}));
        co_await client.send({{"type", "integration.close"}});
    });
    SessionRegistry restored(directory / "sessions", factory());
    const auto messages = restored.load(id)->getMessages();
    EXPECT_EQ(messages.size(), 3); // Initial instructions, accepted user, recovered answer.
    ASSERT_EQ(provider->inputs.size(), 2);
    EXPECT_EQ(provider->inputs[0], provider->inputs[1]);
}

TEST_F(SessionTest, SkillInvocationValidatesBeforeGenerationAndPersistsExactLoadedContent) {
    const auto skillFile = directory / ".sos/skills/check-api/SKILL.md";
    fs::create_directories(skillFile.parent_path());
    std::ofstream(skillFile) << "---\nname: check-api\ndescription: Check routes\narguments: [directory]\nargument-hint: "
        "'[directory]'\nargument-descriptions:\n  directory: Routes directory.\n"
        "context: fork\nallowed-tools: Bash(*)\n---\nORIGINAL_SKILL_BODY\n";
    std::string id;
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await receiveEvent(client);
        co_await client.send({{"type", "integration.configure"}, {"workspace", directory.string()}, {"mode", "sane"}});
        const auto configured = co_await receiveEvent(client);
        EXPECT_EQ(configured.at("info").at("skills").size(), 1);
        EXPECT_EQ(configured.at("info").at("skills")[0].at("scripts"), json::array());
        EXPECT_EQ(configured.at("info").at("skills")[0].at("arguments"), json::array({"directory"}));
        EXPECT_EQ(configured.at("info").at("skills")[0].at("argument-hint"), "[directory]");
        EXPECT_EQ(configured.at("info").at("skills")[0].at("argument-descriptions"),
            (json{{"directory", "Routes directory."}}));
        EXPECT_FALSE(configured.at("info").at("skills")[0].contains("context"));
        EXPECT_FALSE(configured.at("info").at("skills")[0].contains("allowed-tools"));
        co_await client.send({{"type", "message.send"}, {"text", "//check-api"},
            {"workspace", directory.string()},
            {"skill", {{"name", "missing"}, {"input", ""}}}});
        const auto invalid = co_await receiveEvent(client);
        EXPECT_EQ(invalid.at("type"), "request.failed");
        EXPECT_TRUE(provider->inputs.empty());
        co_await client.send({{"type", "message.send"}, {"text", "//check-api directory=src/http"},
            {"workspace", directory.string()}, {"skill", {{"name", "check-api"}, {"input", "src/http"}}}});
        const auto created = co_await receiveEvent(client);
        id = created.at("session").at("id");
        for (;;) {
            const auto event = co_await receiveEvent(client);
            if (event.at("type") == "execution.completed") break;
        }
        co_await client.send({{"type", "integration.close"}});
    });
    ASSERT_EQ(provider->inputs.size(), 1);
    EXPECT_NE(provider->inputs[0].dump().find("ORIGINAL_SKILL_BODY"), std::string::npos);
    EXPECT_EQ(provider->inputs[0].dump().find("//check-api"), std::string::npos);
    EXPECT_NE(provider->inputs[0].dump().find("Call the skill"), std::string::npos);
    EXPECT_EQ(provider->inputs[0].dump().find("The user explicitly invoked this skill"), std::string::npos);
    std::size_t skillCalls = 0, skillResults = 0;
    std::string callId;
    for (const auto& item : provider->inputs[0]) {
        if (item.value("type", "") == "function_call" && item.value("name", "") == "skill") {
            ++skillCalls;
            callId = item.at("call_id");
            EXPECT_EQ(json::parse(item.at("arguments").get<std::string>())["input"], "src/http");
        }
        if (item.value("type", "") == "function_call_output" && item.value("call_id", "") == callId) ++skillResults;
    }
    EXPECT_EQ(skillCalls, 1);
    EXPECT_EQ(skillResults, 1);
    std::ofstream(skillFile) << "CHANGED_ON_DISK";
    SessionRegistry restored(directory / "sessions", factory());
    const auto session = restored.load(id);
    EXPECT_NE(session->activeConversation().getProviderFormat().dump().find("ORIGINAL_SKILL_BODY"), std::string::npos);
    EXPECT_EQ(session->activeConversation().getProviderFormat().dump().find("CHANGED_ON_DISK"), std::string::npos);
    EXPECT_EQ(restored.lastUserMessage(id), "//check-api directory=src/http");
}

TEST_F(SessionTest, FreeFormSkillInputReachesProviderAndSurvivesResume) {
    const auto skillFile = directory / ".sos/skills/caveman/SKILL.md";
    fs::create_directories(skillFile.parent_path());
    std::ofstream(skillFile) << "---\nname: caveman\ndescription: Brief replies.\n---\nRespond briefly.\n";
    const std::string input = "status \"unfinished\n$(touch marker)";
    const json invocation = {{"name", "caveman"}, {"input", input}};
    std::string id;
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await receiveEvent(client);
        co_await client.send({{"type", "message.send"}, {"text", "/caveman " + input},
            {"workspace", directory.string()}, {"skill", invocation}});
        id = (co_await receiveEvent(client)).at("session").at("id");
        for (;;) {
            const auto event = co_await receiveEvent(client);
            if (event.at("type") == "execution.completed") break;
        }
        co_await client.send({{"type", "integration.close"}});
    });
    ASSERT_EQ(provider->inputs.size(), 1);
    bool foundCall = false, foundResult = false;
    for (const auto& item : provider->inputs[0]) {
        if (item.value("type", "") == "function_call" && item.value("name", "") == "skill") {
            foundCall = true;
            EXPECT_EQ(json::parse(item.at("arguments").get<std::string>()), invocation);
        }
        if (item.value("type", "") == "function_call_output") {
            const auto output = item.at("output").get<std::string>();
            foundResult = output.find("Input (JSON string data):\n" + json(input).dump()) != std::string::npos;
        }
    }
    EXPECT_TRUE(foundCall);
    EXPECT_TRUE(foundResult);
    EXPECT_FALSE(fs::exists(directory / "marker"));
    SessionRegistry restored(directory / "sessions", factory());
    const auto session = restored.load(id);
    const auto restoredMessages = session->activeConversation().getProviderFormat();
    for (std::size_t i = 0; i < provider->inputs[0].size(); ++i)
        EXPECT_EQ(restoredMessages.at(i), provider->inputs[0].at(i));
    EXPECT_EQ(restored.lastUserMessage(id), "/caveman " + input);
}

TEST_F(SessionTest, SkillWithPositionalNamesAcceptsMissingInputWithoutRequiredValidation) {
    const auto path = directory / ".sos/skills/greet/SKILL.md";
    fs::create_directories(path.parent_path());
    std::ofstream(path) << "---\nname: greet\ndescription: Test.\narguments: [name]\n"
        "argument-hint: '[name]'\n---\nHello <$name>; index <$0>\n";
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await receiveEvent(client);
        co_await client.send({{"type", "message.send"}, {"text", "/greet"},
            {"workspace", directory.string()}, {"skill", {{"name", "greet"}}}});
        const auto created = co_await receiveEvent(client);
        EXPECT_EQ(created.at("type"), "session.created");
        if (created.at("type") != "session.created") co_return;
        while ((co_await receiveEvent(client)).at("type") != "execution.completed") {}
        co_await client.send({{"type", "integration.close"}});
    });
    ASSERT_EQ(provider->inputs.size(), 1);
    EXPECT_NE(provider->inputs[0].dump().find("Hello <>; index <$0>"), std::string::npos);
    for (const auto& item : provider->inputs[0])
        if (item.value("type", "") == "function_call" && item.value("name", "") == "skill")
            EXPECT_EQ(json::parse(item.at("arguments").get<std::string>()),
                (json{{"name", "greet"}, {"input", ""}}));
}

TEST_F(SessionTest, StackedSkillsExpandInOrderAndPersistSeparateCallResults) {
    for (const std::string name : {"brief", "review"}) {
        const auto path = directory / (".sos/skills/" + name + "/SKILL.md");
        fs::create_directories(path.parent_path());
        std::ofstream(path) << "---\nname: " << name << "\ndescription: Test.\n"
            << "arguments: [first, second]\nargument-hint: '[first] [second]'\n---\n"
            << name << ": $first / $ARGUMENTS[1]; raw=$ARGUMENTS\n";
    }
    const std::string input = "\"hello world\" second";
    const auto invocations = json::array({
        {{"name", "brief"}, {"input", input}},
        {{"name", "review"}, {"input", input}},
        {{"name", "brief"}, {"input", input}}});
    const std::string text = "/brief /review /brief " + input;
    std::string id;
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await receiveEvent(client);
        co_await client.send({{"type", "message.send"}, {"text", text},
            {"workspace", directory.string()}, {"skills", invocations}});
        id = (co_await receiveEvent(client)).at("session").at("id");
        for (;;) {
            const auto event = co_await receiveEvent(client);
            if (event.at("type") == "execution.completed") break;
        }
        co_await client.send({{"type", "integration.close"}});
    });
    ASSERT_EQ(provider->inputs.size(), 1);
    std::set<std::string> callIds;
    std::vector<std::string> resultIds;
    std::size_t callIndex = 0;
    for (const auto& item : provider->inputs[0]) {
        if (item.value("type", "") == "function_call" && item.value("name", "") == "skill") {
            ASSERT_LT(callIndex, invocations.size());
            EXPECT_EQ(json::parse(item.at("arguments").get<std::string>()), invocations[callIndex++]);
            EXPECT_TRUE(callIds.insert(item.at("call_id")).second);
        }
        if (item.value("type", "") == "function_call_output") {
            const auto name = invocations[resultIds.size()].at("name").get<std::string>();
            EXPECT_NE(item.at("output").get<std::string>().find(name + ": hello world / second; raw=" + input), std::string::npos);
            resultIds.push_back(item.at("call_id"));
        }
    }
    EXPECT_EQ(callIndex, 3);
    EXPECT_EQ(resultIds.size(), 3);
    EXPECT_EQ(callIds, std::set<std::string>(resultIds.begin(), resultIds.end()));
    std::ofstream(directory / ".sos/skills/brief/SKILL.md") << "Changed after loading";
    SessionRegistry restored(directory / "sessions", factory());
    const auto session = restored.load(id);
    const auto restoredInput = session->activeConversation().getProviderFormat();
    std::size_t displayedSkills = 0;
    for (const auto& message : session->getMessages())
        for (const auto& content : message.content)
            if (const auto* result = std::get_if<ToolResultContent>(&content)) {
                EXPECT_EQ(result->metadata.at("argument_names"), json::array({"first", "second"}));
                EXPECT_EQ(result->metadata.at("argument_values"), json::array({"hello world", "second"}));
                ++displayedSkills;
            }
    EXPECT_EQ(displayedSkills, 3);
    for (std::size_t i = 0; i < provider->inputs[0].size(); ++i)
        EXPECT_EQ(restoredInput.at(i), provider->inputs[0].at(i));
    EXPECT_EQ(restored.lastUserMessage(id), text);
    std::vector<std::string> provenance;
    for (const auto& message : session->getMessages())
        if (message.userSkill) provenance.push_back(*message.userSkill);
    EXPECT_EQ(provenance, (std::vector<std::string>{"brief", "review", "brief"}));
}

TEST_F(SessionTest, InvalidStackedSkillRejectsWholeMessageBeforeGeneration) {
    const auto path = directory / ".sos/skills/brief/SKILL.md";
    fs::create_directories(path.parent_path());
    std::ofstream(path) << "---\nname: brief\ndescription: Test.\n---\nBrief: $0\n";
    const auto required = directory / ".sos/skills/required/SKILL.md";
    fs::create_directories(required.parent_path());
    std::ofstream(required) << "---\nname: required\ndescription: Test.\narguments:\n  value:\n"
        "    description: Value.\n    required: true\n---\n$0\n";
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await receiveEvent(client);
        for (const auto& second : {json{{"name", "missing"}},
                json{{"name", "required"}, {"input", "not enough"}},
                json{{"name", "brief"}, {"input", "\"unfinished"}}}) {
            co_await client.send({{"type", "message.send"}, {"text", "/brief /required"}, {"workspace", directory.string()},
                {"skills", json::array({{{"name", "brief"}}, second})}});
            const auto event = co_await receiveEvent(client);
            EXPECT_EQ(event.at("type"), "request.failed");
            EXPECT_TRUE(provider->inputs.empty());
        }
        co_await client.send({{"type", "integration.close"}});
    });
}

TEST(SkillRequestTest, StackedInvocationsAreBoundedAndCannotBypassPlanningGuards) {
    const json invocation = {{"name", "brief"}};
    json request = {{"type", "message.send"}, {"text", "/brief"}, {"skills", json::array({invocation})}};
    EXPECT_EQ(request.get<IntegrationRequest>().skills.size(), 1);
    request["skill"] = invocation;
    EXPECT_THROW(request.get<IntegrationRequest>(), std::invalid_argument);
    request.erase("skill");
    request["skills"] = json::array();
    EXPECT_THROW(request.get<IntegrationRequest>(), std::invalid_argument);
    for (std::size_t i = 0; i < MAX_STACKED_SKILLS; ++i) request["skills"].push_back(invocation);
    EXPECT_EQ(request.get<IntegrationRequest>().skills.size(), MAX_STACKED_SKILLS);
    request["skills"].push_back(invocation);
    EXPECT_THROW(request.get<IntegrationRequest>(), std::invalid_argument);
    request["skills"] = json::array({invocation});
    request["text"] = "Sane Implementation request";
    request["sane_request"] = true;
    EXPECT_THROW(request.get<IntegrationRequest>(), std::invalid_argument);
    request.erase("sane_request");
    request["text"] = "Implementation plan request";
    request["implementation_request"] = true;
    EXPECT_THROW(request.get<IntegrationRequest>(), std::invalid_argument);
}

TEST(SkillRequestTest, ReviewSkillsUseRawInvocationsAndRejectUnboundedChains) {
    json request = {{"type", "implementation.review"}, {"sessionId", "branch"}, {"step", 0}, {"substep", 0},
        {"text", "/brief explain"}, {"skills", json::array({{{"name", "brief"}, {"input", "explain"}}})}};
    const auto parsed = request.get<IntegrationRequest>();
    ASSERT_EQ(parsed.skills.size(), 1);
    EXPECT_EQ(parsed.skills.front().input, "explain");
    request["skills"] = json::array();
    EXPECT_THROW(request.get<IntegrationRequest>(), std::invalid_argument);
    for (std::size_t i = 0; i <= MAX_STACKED_SKILLS; ++i) request["skills"].push_back({{"name", "brief"}});
    EXPECT_THROW(request.get<IntegrationRequest>(), std::invalid_argument);
}

TEST_F(SessionTest, CancellationStopsRetryCountdownAndKeepsTheUserMessage) {
    provider->respond = [](const json&, std::size_t) -> json { throw RetryableGenerationError("offline"); };
    std::string id;
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await receiveEvent(client);
        co_await client.send({{"type", "message.send"}, {"text", "Hello"}, {"workspace", directory.string()}});
        id = (co_await receiveEvent(client)).at("session").at("id");
        for (;;) {
            const auto event = co_await receiveEvent(client);
            if (event.at("type") == "execution.retry" && event.at("retry").at("seconds") == 2)
                co_await client.send({{"type", "execution.cancel"}});
            if (event.at("type") == "execution.cancelled") break;
        }
        co_await client.send({{"type", "integration.close"}});
    });
    EXPECT_EQ(provider->inputs.size(), 2);
    SessionRegistry restored(directory / "sessions", factory());
    const auto messages = restored.load(id)->getMessages();
    ASSERT_EQ(messages.size(), 3);
    EXPECT_EQ(messages[1].role, MessageRole::USER);
    EXPECT_EQ(messages[2].role, MessageRole::SYSTEM);
}

TEST_F(SessionTest, ImageInputIsCheckedBeforeTitleAndSurvivesResumeWithoutPixelHistoryFrames) {
    auto auth = std::make_shared<AuthStore>(directory / "auth.json");
    auth->set("openai", ProviderCredential{.key = "fake-key"});
    auto preferences = std::make_shared<ProviderConnections>(auth, directory / "config.json");
    preferences->select({"openai", "o3-mini", "medium"});
    provider->imageSupport = ImageSupport::SUPPORTED;
    const auto image = validateImage({"", "image/png", "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a6a0AAAAASUVORK5CYII=", 0, 0, "pixel.png", "clipboard"});
    int titles = 0;
    std::string id;
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await receiveEvent(client);
        for (const auto& model : {"o3-mini", "gpt-unknown"}) {
            co_await client.send({{"type", "message.send"}, {"text", ""}, {"images", std::vector<Image>{image}},
                {"workspace", directory.string()}, {"model", model}});
            auto refused = co_await receiveEvent(client);
            EXPECT_EQ(refused["code"], "image_input");
            EXPECT_EQ(titles, 0);
            EXPECT_TRUE(provider->inputs.empty());
        }
        co_await client.send({{"type", "message.send"}, {"text", ""}, {"images", std::vector<Image>{image}},
            {"workspace", directory.string()}, {"model", "gpt-4.1"}});
        const auto created = co_await receiveEvent(client);
        if (created["type"] != "session.created") throw std::runtime_error(created.dump());
        id = created["session"]["id"].get<std::string>();
        for (;;) {
            auto event = co_await receiveEvent(client);
            EXPECT_EQ(event.dump().find(image.data), std::string::npos);
            if (event["type"] == "execution.completed") break;
        }
        EXPECT_EQ(provider->inputs.size(), 1);
        EXPECT_NE(provider->inputs.at(0).dump().find(image.data), std::string::npos);
        EXPECT_EQ(titles, 1);
        co_await client.send({{"type", "message.send"}, {"text", "Continue"}, {"model", "o3-mini"}});
        EXPECT_EQ((co_await receiveEvent(client))["code"], "image_input");
        EXPECT_EQ(provider->inputs.size(), 1);
        co_await client.send({{"type", "session.new"}});
        co_await receiveEvent(client);
        co_await client.send({{"type", "session.attach"}, {"sessionId", id}});
        auto attached = co_await receiveEvent(client);
        EXPECT_NE(attached.dump().find(image.id), std::string::npos);
        EXPECT_EQ(attached.dump().find(image.data), std::string::npos);
        co_await client.send({{"type", "integration.close"}});
    }, [&](const std::string&, const std::string&, core::CancellationCheck) { ++titles; return "Image discussion"; },
        {}, std::chrono::seconds(5), std::chrono::milliseconds(500), preferences);
    SessionRegistry registry(directory / "sessions", factory());
    const auto restored = registry.load(id);
    EXPECT_EQ(restored->activeConversation().getMessages()[1].images[0].data, image.data);
}

TEST_F(SessionTest, ResumeStreamsLargeHistoryAndAnOversizedMessageWithoutDisconnecting) {
    SessionRegistry registry(directory / "sessions", factory());
    auto saved = registry.create(info());
    const auto id = saved->getInfo().id;
    const std::string large(17 * 1024 * 1024, 'x');
    saved->record({AgentEventType::HARNESS_MESSAGE_ARRIVED,
        {Message{"large", MessageRole::ASSISTANT, {TextContent{large}}}}, {}, {}});
    saved.reset();
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await receiveEvent(client);
        co_await client.send({{"type", "integration.configure"}, {"workspace", directory.string()}, {"mode", "sane"}});
        co_await receiveEvent(client);
        co_await client.send({{"type", "session.attach"}, {"sessionId", id}, {"requestId", "large-resume"}});
        const auto restored = co_await receiveEvent(client);
        EXPECT_EQ(restored.at("type"), "session.attached");
        EXPECT_EQ(restored.at("messages").size(), 1);
        EXPECT_EQ(restored.at("messages")[0].at("content")[0].at("text"), large);
        co_await client.send({{"type", "session.list"}, {"requestId", "still-alive"}});
        EXPECT_EQ((co_await receiveEvent(client)).at("requestId"), "still-alive");
        co_await client.send({{"type", "integration.close"}});
    // The debug client parses a 17 MiB message synchronously on the daemon's test io_context.
    }, {}, {}, std::chrono::seconds(20), std::chrono::seconds(5));
}

TEST_F(SessionTest, DisconnectDuringHistoryTransferLeavesPersistedSessionResumable) {
    SessionRegistry registry(directory / "sessions", factory());
    auto saved = registry.create(info());
    const auto id = saved->getInfo().id;
    saved->record({AgentEventType::HARNESS_MESSAGE_ARRIVED,
        {Message{"large", MessageRole::ASSISTANT, {TextContent{std::string(2 * 1024 * 1024, 'x')}}}}, {}, {}});
    saved.reset();
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await receiveEvent(client);
        co_await client.send({{"type", "integration.configure"}, {"workspace", directory.string()}, {"mode", "sane"}});
        co_await receiveEvent(client);
        co_await client.send({{"type", "session.attach"}, {"sessionId", id}});
        EXPECT_EQ((co_await client.receive())->at("type"), "session.attach_started");
        EXPECT_EQ((co_await client.receive())->at("type"), "session.history_chunk");
        client.close();
    });
    EXPECT_EQ(registry.load(id)->getMessages().size(), 2);
}

TEST_F(SessionTest, HistoryWindowPagesAndLoadsOnlyTheRequestedActivityGroup) {
    SessionRegistry registry(directory / "sessions", factory());
    auto saved = registry.create(info());
    const auto id = saved->getInfo().id;
    std::vector<Message> history;
    const std::string largeOutput(300 * 1024, 'x');
    for (int i = 0; i < 120; ++i) {
        const auto suffix = std::to_string(i);
        history.push_back({"u" + suffix, MessageRole::USER, {TextContent{"Question " + suffix}}});
        history.push_back({"c" + suffix, MessageRole::ASSISTANT,
            {ToolCallContent{"call" + suffix, "bash", json{{"command", "true"}}}}});
        history.push_back({"r" + suffix, MessageRole::TOOL,
            {ToolResultContent{"call" + suffix, i == 119 ? largeOutput : "result " + suffix, false}}});
        history.push_back({"a" + suffix, MessageRole::ASSISTANT, {TextContent{"Answer " + suffix}}});
    }
    saved->record({AgentEventType::HARNESS_MESSAGE_ARRIVED, history, {}, {}});
    saved.reset();
    const auto start = 240 - SessionHistoryView::PAGE_SIZE;
    const auto previousStart = start - SessionHistoryView::PAGE_STEP;
    const auto messageId = [](std::size_t index) { return std::string(index % 2 ? "a" : "u") + std::to_string(index / 2); };
    const auto groupTurn = (previousStart + 1) / 2;
    const auto groupId = "u" + std::to_string(groupTurn) + "/activity/0";
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await receiveEvent(client);
        co_await client.send({{"type", "integration.configure"}, {"workspace", directory.string()}, {"mode", "sane"}});
        co_await receiveEvent(client);
        co_await client.send({{"type", "session.attach"}, {"sessionId", id}, {"requestId", "attach"}});
        const auto recent = co_await receiveEvent(client);
        EXPECT_EQ(recent["page"]["first"], messageId(start));
        EXPECT_EQ(recent["page"]["last"], "a119");
        EXPECT_EQ(recent["messages"].size(), SessionHistoryView::PAGE_SIZE * 3 / 2);
        EXPECT_FALSE(recent["messages"][1].contains("activities"));
        EXPECT_FALSE(recent["messages"][recent["messages"].size() - 2]["activities"][0]["result"].contains("output"));
        EXPECT_EQ(std::count_if(recent["messages"].begin(), recent["messages"].end(),
            [](const auto& record) { return record.contains("activities"); }), 1);
        co_await client.send({{"type", "session.history"}, {"sessionId", id}, {"direction", "before"}, {"anchor", recent["page"]["first"]}});
        const auto previous = co_await receiveEvent(client);
        EXPECT_EQ(previous["page"]["first"], messageId(previousStart));
        EXPECT_EQ(previous["page"]["last"], messageId(previousStart + SessionHistoryView::PAGE_SIZE - 1));
        EXPECT_FALSE(previous.dump().contains("\"output\""));
        co_await client.send({{"type", "session.activities"}, {"sessionId", id}, {"groupId", groupId}});
        const auto group = co_await receiveEvent(client);
        EXPECT_EQ(group["messages"].size(), 1);
        EXPECT_FALSE(group["messages"][0]["activities"][0]["result"].contains("output"));
        co_await client.send({{"type", "session.activity_detail"}, {"sessionId", id}, {"groupId", groupId}, {"activityIndex", 0}});
        const auto detail = co_await receiveEvent(client);
        EXPECT_EQ(detail["purpose"], "activity-detail");
        EXPECT_EQ(detail["activityIndex"], 0);
        EXPECT_EQ(detail["messages"][0]["activities"][0]["result"]["output"], "result " + std::to_string(groupTurn));
        co_await client.send({{"type", "session.activity_detail"}, {"sessionId", id}, {"groupId", "u119/activity/0"}, {"activityIndex", 0}});
        EXPECT_EQ((co_await receiveEvent(client))["messages"][0]["activities"][0]["result"]["output"], largeOutput);
        co_await client.send({{"type", "session.activity_detail"}, {"sessionId", id}, {"groupId", groupId}, {"activityIndex", 9}});
        EXPECT_EQ((co_await receiveEvent(client))["type"], "request.failed");
        co_await client.send({{"type", "session.history"}, {"sessionId", id}, {"direction", "after"}, {"anchor", previous["page"]["last"]}});
        EXPECT_EQ((co_await receiveEvent(client))["page"], recent["page"]);
        co_await client.send({{"type", "session.activities"}, {"sessionId", id}, {"groupId", "missing"}});
        EXPECT_EQ((co_await receiveEvent(client))["type"], "request.failed");
        co_await client.send({{"type", "integration.close"}});
    });
    EXPECT_EQ(registry.load(id)->getHistory().size(), history.size() + 1);
}

TEST_F(SessionTest, AutomaticCompactionDuringToolLoopPersistsCheckpointAndContinuesWithToolPair) {
    provider->contextLimit = 32768;
    provider->respond = [](const json& input, std::size_t call) {
        if (call == 2) return json{{"id", "tool_response"}, {"status", "completed"}, {"test_used", 16380},
            {"output", json::array({{{"type", "function_call"}, {"id", "tool_item"}, {"call_id", "call"},
                {"name", "unknown_tool"}, {"arguments", "{}"}}})}};
        if (call == 3) {
            EXPECT_NE(input.dump().find("You summarize a coding conversation"), std::string::npos);
            EXPECT_EQ(input.dump().find("Second request"), std::string::npos);
            return reply("summary", "Earlier work summary");
        }
        return reply("answer_" + std::to_string(call), "Done");
    };
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await receiveEvent(client);
        co_await client.send({{"type", "message.send"}, {"text", "First request"}, {"workspace", directory.string()}});
        const auto created = co_await receiveEvent(client);
        const auto id = created.at("session").at("id");
        while ((co_await receiveEvent(client)).at("type") != "execution.completed") {}
        co_await client.send({{"type", "message.send"}, {"text", "Second request"}});
        std::vector<std::string> types;
        for (;;) {
            const auto event = co_await receiveEvent(client);
            const auto type = event.at("type").get<std::string>();
            types.push_back(type);
            if (type == "compaction.completed") {
                EXPECT_TRUE(event.at("automatic").get<bool>());
                const auto retained = event.at("messages")[0].at("compaction").at("retained_message_ids");
                EXPECT_EQ(retained.size(), 3); // user + assistant tool call + tool result
            }
            if (type.starts_with("execution.")) { EXPECT_EQ(type, "execution.completed"); break; }
        }
        EXPECT_EQ(std::count(types.begin(), types.end(), "compaction.started"), 1);
        EXPECT_EQ(std::count(types.begin(), types.end(), "compaction.completed"), 1);
        EXPECT_EQ(provider->inputs.size(), 4);
        const auto resumed = provider->inputs.back().dump();
        EXPECT_NE(resumed.find("Earlier work summary"), std::string::npos);
        EXPECT_NE(resumed.find("Second request"), std::string::npos);
        EXPECT_NE(resumed.find("function_call_output"), std::string::npos);
        EXPECT_NE(resumed.find("unknown_tool"), std::string::npos);
        EXPECT_EQ(resumed.find("First request"), std::string::npos);
        co_await client.send({{"type", "session.new"}});
        co_await receiveEvent(client);
        co_await client.send({{"type", "session.attach"}, {"sessionId", id}});
        const auto attached = co_await receiveEvent(client);
        EXPECT_NE(attached.dump().find("First request"), std::string::npos);
        EXPECT_NE(attached.dump().find("Earlier work summary"), std::string::npos);
        co_await client.send({{"type", "message.send"}, {"text", "After resume"}});
        while ((co_await receiveEvent(client)).at("type") != "execution.completed") {}
        EXPECT_NE(provider->inputs.back().dump().find("Earlier work summary"), std::string::npos);
        EXPECT_EQ(provider->inputs.back().dump().find("First request"), std::string::npos);
        client.close();
    });
}

TEST_F(SessionTest, AutomaticCompactionBeforeUserRequestUsesMeasuredUsageAndKeepsPendingRequest) {
    provider->contextLimit = 32768;
    provider->usedContext = 16000; // Below the threshold until the new user message is counted.
    provider->respond = [](const json&, std::size_t call) { return reply("answer_" + std::to_string(call), "Short answer"); };
    SessionRegistry registry(directory, factory());
    auto session = registry.create(info());
    std::vector<AgentEventType> events;
    turn(*session, "Earlier request", events);
    events.clear();
    turn(*session, "Pending request " + std::string(4000, 'x'), events);
    EXPECT_EQ(std::count(events.begin(), events.end(), AgentEventType::COMPACTION_COMPLETED), 1);
    ASSERT_EQ(provider->inputs.size(), 3);
    EXPECT_EQ(provider->inputs[1].dump().find("Pending request"), std::string::npos);
    EXPECT_NE(provider->inputs[2].dump().find("Pending request"), std::string::npos);
    EXPECT_EQ(provider->inputs[2].dump().find("Earlier request"), std::string::npos);
}

TEST_F(SessionTest, FailedAutomaticCompactionStopsWithoutActivatingCheckpointOrRetryLoop) {
    provider->contextLimit = 32768;
    provider->usedContext = 17000;
    provider->respond = [](const json&, std::size_t call) {
        if (call == 2) throw std::runtime_error("Summary failed");
        return reply("answer", "Short answer");
    };
    SessionRegistry registry(directory, factory());
    auto session = registry.create(info());
    std::vector<AgentEventType> events;
    turn(*session, "Earlier request", events);
    events.clear();
    turn(*session, "Pending request", events);
    EXPECT_EQ(events.back(), AgentEventType::EXECUTION_FAILED);
    EXPECT_EQ(std::count(events.begin(), events.end(), AgentEventType::COMPACTION_FAILED), 1);
    EXPECT_EQ(provider->inputs.size(), 2);
    for (const auto& message : session->getMessages()) EXPECT_FALSE(message.compaction);
    EXPECT_NE(json(session->activeConversation().getMessages()).dump().find("Pending request"), std::string::npos);
}

TEST_F(SessionTest, AutomaticCompactionCancellationAndDisconnectPreserveOriginalHistory) {
    for (const bool disconnect : {false, true}) {
        provider = std::make_shared<ProviderState>();
        provider->contextLimit = 32768;
        provider->usedContext = 17000;
        provider->waitForSummaryCancellation = true;
        auto entered = provider->entered.get_future();
        healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
            co_await receiveEvent(client);
            co_await client.send({{"type", "message.send"}, {"text", "Earlier request"}, {"workspace", directory.string()}});
            while ((co_await receiveEvent(client)).at("type") != "execution.completed") {}
            co_await client.send({{"type", "message.send"}, {"text", "Pending request"}});
            while ((co_await receiveEvent(client)).at("type") != "compaction.started") {}
            co_await awaitFuture(entered);
            if (disconnect) { client.close(); co_return; }
            co_await client.send({{"type", "execution.cancel"}});
            bool cancelled = false;
            for (;;) {
                const auto event = co_await receiveEvent(client);
                if (event.at("type") == "compaction.cancelled") cancelled = true;
                if (event.at("type") == "execution.cancelled") break;
            }
            EXPECT_TRUE(cancelled);
            client.close();
        });
        EXPECT_EQ(provider->inputs.size(), 2);
    }
    SessionRegistry registry(directory / "sessions", factory());
    for (const auto& saved : registry.list()) {
        auto session = registry.load(saved.id);
        const auto messages = session->getMessages();
        for (const auto& message : messages) EXPECT_FALSE(message.compaction);
        EXPECT_NE(json(messages).dump().find("Pending request"), std::string::npos);
    }
}

TEST_F(SessionTest, AutomaticCompactionReducesOversizedSummaryAndResumesWithPreservedHistory) {
    provider->contextLimit = 32768;
    provider->usedContext = 17000;
    provider->respond = [](const json&, std::size_t call) {
        return reply("answer_" + std::to_string(call), call == 2 ? std::string(80000, 'x') : "Short answer");
    };
    SessionRegistry registry(directory, factory());
    auto session = registry.create(info());
    std::vector<AgentEventType> events;
    turn(*session, "Earlier request", events);
    const json before = session->getMessages();
    events.clear();
    turn(*session, "Pending request", events);
    EXPECT_EQ(events.back(), AgentEventType::EXECUTION_COMPLETED);
    EXPECT_EQ(std::count(events.begin(), events.end(), AgentEventType::COMPACTION_COMPLETED), 1);
    EXPECT_EQ(std::count(events.begin(), events.end(), AgentEventType::COMPACTION_FAILED), 0);
    ASSERT_EQ(provider->inputs.size(), 4); // First turn, summary, reduction, resumed turn.
    EXPECT_NE(provider->inputs[2].dump().find("Rewrite it much more concisely"), std::string::npos);
    EXPECT_NE(provider->inputs[2].dump().find("MAXIMUM 2000 tokens"), std::string::npos);
    EXPECT_EQ(provider->inputs[2].dump().find("6552 UTF-8 bytes"), std::string::npos);
    EXPECT_NE(provider->inputs.back().dump().find("Pending request"), std::string::npos);
    const auto history = session->getMessages();
    ASSERT_GE(history.size(), before.size());
    for (std::size_t i = 0; i < before.size(); ++i) EXPECT_EQ(json(history[i]), before[i]);
    ASSERT_EQ(std::count_if(history.begin(), history.end(), [](const auto& message) { return message.compaction.has_value(); }), 1);
    const auto checkpoint = std::find_if(history.begin(), history.end(), [](const auto& message) { return message.compaction.has_value(); });
    EXPECT_EQ(std::get<TextContent>(checkpoint->content.front()).text, "Short answer\n");
    EXPECT_LE(std::get<TextContent>(checkpoint->content.front()).text.size(), 4096);
}

TEST_F(SessionTest, AutomaticCompactionRejectsOversizedRetainedTurnWithoutActivatingCheckpoint) {
    provider->contextLimit = 32768;
    provider->usedContext = 17000;
    SessionRegistry registry(directory, factory());
    auto session = registry.create(info());
    std::vector<AgentEventType> events;
    turn(*session, "Earlier request", events);
    const json before = session->getMessages();
    events.clear();
    const auto pending = "Pending request " + std::string(80000, 'x');
    turn(*session, pending, events);
    EXPECT_EQ(events.back(), AgentEventType::EXECUTION_FAILED);
    EXPECT_EQ(std::count(events.begin(), events.end(), AgentEventType::COMPACTION_STARTED), 1);
    EXPECT_EQ(std::count(events.begin(), events.end(), AgentEventType::COMPACTION_FAILED), 1);
    EXPECT_EQ(std::count(events.begin(), events.end(), AgentEventType::COMPACTION_COMPLETED), 0);
    EXPECT_EQ(provider->inputs.size(), 2);
    const auto history = session->getMessages();
    ASSERT_GE(history.size(), before.size());
    for (std::size_t i = 0; i < before.size(); ++i) EXPECT_EQ(json(history[i]), before[i]);
    for (const auto& message : history) EXPECT_FALSE(message.compaction);
    EXPECT_NE(json(session->activeConversation().getMessages()).dump().find("Earlier request"), std::string::npos);
    EXPECT_NE(json(session->activeConversation().getMessages()).dump().find(pending), std::string::npos);
}

TEST_F(SessionTest, FailedAutomaticCheckpointSaveReleasesWorkerWithoutReplacingContext) {
    provider->contextLimit = 32768;
    provider->usedContext = 17000;
    SessionRegistry registry(directory, factory());
    auto session = registry.create(info());
    std::vector<AgentEventType> events;
    turn(*session, "Earlier request", events);
    asio::io_context io;
    auto queue = std::make_shared<AsyncQueue<AgentEvent>>(io.get_executor());
    session->setEventHandler([queue, executor = io.get_executor()](AgentEvent event) {
        asio::post(executor, [queue, event = std::move(event)]() mutable { queue->push(std::move(event)); });
    });
    auto completed = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        for (;;) {
            const auto event = co_await queue->receive();
            if (event.type == AgentEventType::COMPACTION_COMPLETED) {
                const auto journal = directory / session->getInfo().id / "session.sqlite";
                SessionDatabase fault(journal);
                fault.execute("CREATE TRIGGER fail_checkpoint BEFORE INSERT ON messages BEGIN SELECT RAISE(ABORT,'injected checkpoint failure'); END");
                EXPECT_THROW(session->record(event), std::exception);
                fault.execute("DROP TRIGGER fail_checkpoint");
            } else session->record(event);
            if (event.isTerminal()) {
                EXPECT_EQ(event.type, AgentEventType::EXECUTION_FAILED);
                co_await session->finishExecution();
                break;
            }
        }
    }, asio::use_future);
    session->sendMessage("Pending request", ExecutionMode::SANE);
    io.run();
    completed.get();
    session->setEventHandler({});
    EXPECT_EQ(provider->inputs.size(), 2);
    for (const auto& message : session->getMessages()) EXPECT_FALSE(message.compaction);
    EXPECT_NE(json(session->activeConversation().getMessages()).dump().find("Earlier request"), std::string::npos);
}

TEST_F(SessionTest, HistoricalReviewIgnoresCompactionAndConstructionAfterItsBoundary) {
    SessionRegistry registry(directory, factory());
    auto session = registry.create(info());
    std::vector<AgentEventType> events;
    turn(*session, "STEP_ONE_REQUEST", events);
    const auto end = session->btwStore()->count();
    turn(*session, "FUTURE_STEP_REQUEST", events);
    provider->responseText = "FUTURE_STEP_SUMMARY";
    TestProvider summarizer(provider);
    session->applyCompaction(CompactionService{}.compact(session->activeConversation(), summarizer, info(), "future-checkpoint", {}, 0));
    const auto historical = json(session->reviewSnapshot(end)).dump();
    EXPECT_NE(historical.find("STEP_ONE_REQUEST"), std::string::npos);
    EXPECT_EQ(historical.find("FUTURE_STEP_REQUEST"), std::string::npos);
    EXPECT_EQ(historical.find("FUTURE_STEP_SUMMARY"), std::string::npos);
    EXPECT_NE(json(session->reviewSnapshot()).dump().find("FUTURE_STEP_SUMMARY"), std::string::npos);
}

TEST_F(SessionTest, RecapGenerationIsIsolatedToolFreeAndUsesSelectedModelAndReasoning) {
    Conversation source(ProviderFormat::OPENAI_RESPONSES);
    source.append(Message{"sys", MessageRole::SYSTEM, {TextContent{"PRIVATE HARNESS INSTRUCTIONS"}}});
    source.append(Message{"user", MessageRole::USER, {TextContent{"Corrige le problème"}}});
    source.append(Message{"tool", MessageRole::TOOL, {ToolResultContent{"call", "RAW TOOL CONTENT", false}}});
    source.append(Message{"assistant", MessageRole::ASSISTANT, {TextContent{"Fixed, tests pending."}}});
    provider->responseText = "The issue is fixed.\nRun the tests next.";
    TestProvider generator(provider);
    auto selected = info();
    selected.model = "chosen-model";
    selected.reasoning = "high";
    const auto recap = generateConversationRecap(source, generator, selected, "recap-1");
    EXPECT_TRUE(recap.recap);
    EXPECT_EQ(provider->model, selected.model);
    EXPECT_EQ(provider->reasoning, selected.reasoning);
    EXPECT_EQ(provider->toolCount, 0);
    EXPECT_EQ(provider->inputs[0].dump().find("PRIVATE HARNESS"), std::string::npos);
    EXPECT_EQ(provider->inputs[0].dump().find("RAW TOOL CONTENT"), std::string::npos);
    EXPECT_NE(provider->inputs[0].dump().find("Fixed, tests pending."), std::string::npos);
    const auto prompt = provider->inputs[0][0].at("content").get<std::string>();
    EXPECT_NE(prompt.find("HUMAN"), std::string::npos);
    EXPECT_NE(prompt.find("45 words"), std::string::npos);
    EXPECT_NE(prompt.find("Write in English, even when the user writes in another language"), std::string::npos);
    EXPECT_NE(provider->inputs[0].dump().find("Corrige le problème"), std::string::npos);
    EXPECT_NE(prompt.find("Omit test results"), std::string::npos);
    EXPECT_TRUE(json(recap).get<Message>().recap);
    const auto original = source.getProviderFormat();
    source.append(recap);
    EXPECT_EQ(source.getProviderFormat(), original);
    EXPECT_EQ(source.getMessages().size(), 4);
    provider->responseText = " \n ";
    EXPECT_THROW(generateConversationRecap(source, generator, selected, "empty"), std::runtime_error);
}

TEST_F(SessionTest, RecapsPersistAtMultipleHistoryPositionsButNeverReachAgentOrCompaction) {
    SessionRegistry registry(directory, factory());
    auto session = registry.create(info());
    std::vector<AgentEventType> events;
    turn(*session, "First request", events);
    const auto original = session->activeConversation().getProviderFormat();
    Message recap{"recap-1", MessageRole::ASSISTANT, {TextContent{"RECAP USER ONLY"}}};
    recap.recap = true;
    session->saveRecap(recap);
    EXPECT_EQ(session->activeConversation().getProviderFormat(), original);
    turn(*session, "Next request", events);
    recap.id = "recap-2";
    session->saveRecap(recap);
    const auto id = session->getInfo().id;
    const auto before = session->activeConversation().getProviderFormat();
    session.reset();
    session = registry.load(id);
    EXPECT_EQ(session->activeConversation().getProviderFormat(), before);
    EXPECT_EQ(before.dump().find("RECAP USER ONLY"), std::string::npos);
    const auto page = session->history().page();
    std::vector<std::string> recapIds;
    for (std::size_t i = 0; i < page.count; ++i) {
        const auto record = page.record(i);
        if (record.value("recap", false)) recapIds.push_back(record.at("id"));
    }
    EXPECT_EQ(recapIds, (std::vector<std::string>{"recap-1", "recap-2"}));
    auto summaryState = std::make_shared<ProviderState>();
    TestProvider summarizer(summaryState);
    session->applyCompaction(CompactionService{}.compact(session->activeConversation(), summarizer, session->getInfo(), "checkpoint"));
    EXPECT_EQ(summaryState->inputs.back().dump().find("RECAP USER ONLY"), std::string::npos);
    const auto count = session->messageCount();
    EXPECT_THROW(session->saveRecap(recap), std::exception); // Duplicate ID rolls back.
    EXPECT_EQ(session->messageCount(), count);
    recap.id = "recap-after-checkpoint";
    session->saveRecap(recap);
    session.reset();
    session = registry.load(id);
    EXPECT_EQ(session->activeConversation().getProviderFormat().dump().find("RECAP USER ONLY"), std::string::npos);
    EXPECT_TRUE(session->getHistory()[3].recap);
}

TEST_F(SessionTest, IntegrationRecapIsPersistedAndDoesNotContinueTheAgent) {
    auto summary = std::make_shared<ProviderState>();
    summary->responseText = "UNIQUE USER RECAP";
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await receiveEvent(client);
        co_await client.send({{"type", "session.recap"}, {"requestId", "empty"}});
        EXPECT_EQ((co_await receiveEvent(client)).at("type"), "request.failed");
        co_await client.send({{"type", "message.send"}, {"text", "Original request"}, {"workspace", directory.string()}});
        const auto created = co_await receiveEvent(client);
        const auto id = created.at("session").at("id");
        while ((co_await receiveEvent(client)).at("type") != "execution.completed") {}
        co_await client.send({{"type", "session.recap"}, {"requestId", "recap"}});
        EXPECT_EQ((co_await receiveEvent(client)).at("type"), "recap.started");
        const auto completed = co_await receiveEvent(client);
        EXPECT_EQ(completed.at("type"), "recap.completed");
        EXPECT_TRUE(completed.at("messages")[0].at("recap").get<bool>());
        EXPECT_TRUE(completed.at("info").at("last_history_is_recap").get<bool>());
        EXPECT_EQ(provider->inputs.size(), 1);
        EXPECT_EQ(summary->toolCount, 0);
        co_await client.send({{"type", "session.new"}});
        co_await receiveEvent(client);
        co_await client.send({{"type", "session.attach"}, {"sessionId", id}});
        const auto attached = co_await receiveEvent(client);
        EXPECT_NE(attached.at("messages").dump().find("UNIQUE USER RECAP"), std::string::npos);
        EXPECT_TRUE(attached.at("info").at("last_history_is_recap").get<bool>());
        // Explicit /recap remains allowed even immediately after a saved recap.
        co_await client.send({{"type", "session.recap"}, {"requestId", "manual-again"}});
        EXPECT_EQ((co_await receiveEvent(client)).at("type"), "recap.started");
        const auto second = co_await receiveEvent(client);
        EXPECT_EQ(second.at("type"), "recap.completed");
        EXPECT_NE(second.at("messages")[0].at("id"), completed.at("messages")[0].at("id"));
        EXPECT_TRUE(second.at("info").at("last_history_is_recap").get<bool>());
        co_await client.send({{"type", "message.send"}, {"text", "Next request"}});
        while ((co_await receiveEvent(client)).at("type") != "execution.completed") {}
        EXPECT_EQ(provider->inputs.back().dump().find("UNIQUE USER RECAP"), std::string::npos);
        co_await client.send({{"type", "session.list"}});
        EXPECT_FALSE((co_await receiveEvent(client)).at("info").at("last_history_is_recap").get<bool>());
        co_await client.send({{"type", "integration.close"}});
    }, {}, [summary](const SessionInfo&) { return std::make_unique<TestProvider>(summary); });
}

TEST_F(SessionTest, RecapFailureCancellationAndDisconnectLeaveNoHistoryEntry) {
    auto summary = std::make_shared<ProviderState>();
    summary->fail = true;
    std::string id;
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await receiveEvent(client);
        co_await client.send({{"type", "message.send"}, {"text", "Keep this"}, {"workspace", directory.string()}});
        id = (co_await receiveEvent(client)).at("session").at("id").get<std::string>();
        while ((co_await receiveEvent(client)).at("type") != "execution.completed") {}
        co_await client.send({{"type", "session.recap"}});
        EXPECT_EQ((co_await receiveEvent(client)).at("type"), "recap.started");
        EXPECT_EQ((co_await receiveEvent(client)).at("type"), "request.failed");
        summary->fail = false;
        summary->waitForCancellation = true;
        co_await client.send({{"type", "session.recap"}});
        EXPECT_EQ((co_await receiveEvent(client)).at("type"), "recap.started");
        co_await client.send({{"type", "session.recap"}, {"requestId", "duplicate"}});
        co_await client.send({{"type", "execution.cancel"}});
        EXPECT_EQ((co_await receiveEvent(client)).at("type"), "recap.cancelled");
        EXPECT_EQ((co_await receiveEvent(client)).at("type"), "request.failed");
        EXPECT_EQ((co_await receiveEvent(client)).at("type"), "execution.cancel_requested");
        co_await client.send({{"type", "session.recap"}});
        EXPECT_EQ((co_await receiveEvent(client)).at("type"), "recap.started");
        client.close();
    }, {}, [summary](const SessionInfo&) { return std::make_unique<TestProvider>(summary); });
    SessionRegistry registry(directory / "sessions", factory());
    const auto session = registry.load(id);
    EXPECT_EQ(session->messageCount(), 3);
    for (const auto& message : session->getHistory()) EXPECT_FALSE(message.recap);
}

TEST_F(SessionTest, IntegrationCompactsWithoutRunningToolsOrContinuingAndRetainsHistoryForAttach) {
    auto summary = std::make_shared<ProviderState>();
    summary->responseText = "Summary checkpoint";
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await receiveEvent(client);
        co_await client.send({{"type", "session.compact"}, {"requestId", "empty"}});
        EXPECT_EQ((co_await receiveEvent(client)).at("type"), "request.failed");
        co_await client.send({{"type", "message.send"}, {"text", "Original request"}, {"workspace", directory.string()}});
        const auto created = co_await receiveEvent(client);
        const auto id = created.at("session").at("id");
        while ((co_await receiveEvent(client)).at("type") != "execution.completed") {}
        co_await client.send({{"type", "session.compact"}, {"requestId", "compact"}});
        EXPECT_EQ((co_await receiveEvent(client)).at("type"), "compaction.started");
        const auto progress = co_await receiveEvent(client);
        EXPECT_EQ(progress.at("type"), "compaction.progress");
        EXPECT_EQ(progress.at("attempt"), 1);
        EXPECT_EQ(progress.at("limit"), CompactionService::MAX_SUMMARY_ATTEMPTS);
        EXPECT_EQ(progress.at("requestId"), "compact");
        EXPECT_EQ(progress.at("sessionId"), id);
        auto completed = co_await receiveEvent(client);
        std::size_t attempt = 1;
        while (completed.at("type") == "compaction.progress") {
            EXPECT_EQ(completed.at("attempt"), ++attempt);
            EXPECT_EQ(completed.at("requestId"), "compact");
            completed = co_await receiveEvent(client);
        }
        EXPECT_EQ(completed.at("type"), "compaction.completed");
        EXPECT_TRUE(completed.at("messages")[0].contains("compaction"));
        EXPECT_TRUE(completed.at("info").at("last_turn_used_context").is_null());
        EXPECT_EQ(provider->inputs.size(), 1); // The discussion agent did not continue autonomously.
        EXPECT_EQ(summary->toolCount, 0);
        co_await client.send({{"type", "session.new"}});
        co_await receiveEvent(client);
        co_await client.send({{"type", "session.attach"}, {"sessionId", id}});
        const auto attached = co_await receiveEvent(client);
        EXPECT_EQ(attached.at("messages").size(), 4); // Initial system, original user/assistant, checkpoint.
        EXPECT_NE(attached.at("messages").dump().find("Original request"), std::string::npos);
        co_await client.send({{"type", "message.send"}, {"text", "Next request"}});
        while ((co_await receiveEvent(client)).at("type") != "execution.completed") {}
        EXPECT_EQ(provider->inputs.back().dump().find("Original request"), std::string::npos);
        EXPECT_NE(provider->inputs.back().dump().find("Summary checkpoint"), std::string::npos);
        co_await client.send({{"type", "integration.close"}});
    }, {}, [summary](const SessionInfo&) { return std::make_unique<TestProvider>(summary); });
}

TEST_F(SessionTest, IntegrationCompactionFailureAndCancellationKeepTheOriginalContext) {
    auto summary = std::make_shared<ProviderState>();
    summary->fail = true;
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await receiveEvent(client);
        co_await client.send({{"type", "message.send"}, {"text", "Preserve original"}, {"workspace", directory.string()}});
        while ((co_await receiveEvent(client)).at("type") != "execution.completed") {}
        co_await client.send({{"type", "session.compact"}});
        EXPECT_EQ((co_await receiveEvent(client)).at("type"), "compaction.started");
        EXPECT_EQ((co_await receiveEvent(client)).at("type"), "compaction.progress");
        EXPECT_EQ((co_await receiveEvent(client)).at("type"), "request.failed");
        summary->fail = false;
        summary->waitForCancellation = true;
        const auto ping = co_await client.receive();
        EXPECT_EQ(ping->at("type"), "integration.ping");
        co_await client.send({{"type", "session.compact"}});
        EXPECT_EQ((co_await receiveEvent(client)).at("type"), "compaction.started");
        // The reader can still acknowledge a heartbeat while the summary worker is running.
        EXPECT_EQ((co_await receiveEvent(client)).at("type"), "compaction.progress");
        co_await client.send({{"type", "integration.pong"}, {"pingId", ping->at("pingId")}});
        co_await client.send({{"type", "message.send"}, {"text", "Must not queue work"}, {"requestId", "busy"}});
        co_await client.send({{"type", "execution.cancel"}});
        EXPECT_EQ((co_await receiveEvent(client)).at("type"), "compaction.cancelled");
        EXPECT_EQ((co_await receiveEvent(client)).at("type"), "request.failed");
        EXPECT_EQ((co_await receiveEvent(client)).at("type"), "execution.cancel_requested");
        co_await client.send({{"type", "message.send"}, {"text", "Continue normally"}});
        while ((co_await receiveEvent(client)).at("type") != "execution.completed") {}
        EXPECT_NE(provider->inputs.back().dump().find("Preserve original"), std::string::npos);
        EXPECT_EQ(provider->inputs.back().dump().find("Must not queue work"), std::string::npos);
        co_await client.send({{"type", "integration.close"}});
    }, {}, [summary](const SessionInfo&) { return std::make_unique<TestProvider>(summary); });
}

TEST_F(SessionTest, DisconnectCancelsCompactionAndLeavesTheSessionResumable) {
    auto summary = std::make_shared<ProviderState>();
    summary->waitForCancellation = true;
    std::string id;
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await receiveEvent(client);
        co_await client.send({{"type", "message.send"}, {"text", "Original context"}, {"workspace", directory.string()}});
        const auto created = co_await receiveEvent(client);
        id = created.at("session").at("id").get<std::string>();
        while ((co_await receiveEvent(client)).at("type") != "execution.completed") {}
        co_await client.send({{"type", "session.compact"}});
        EXPECT_EQ((co_await receiveEvent(client)).at("type"), "compaction.started");
        client.close();
    }, {}, [summary](const SessionInfo&) { return std::make_unique<TestProvider>(summary); });
    SessionRegistry registry(directory / "sessions", factory());
    const auto restored = registry.load(id);
    EXPECT_EQ(restored->getMessages().size(), 3);
    EXPECT_EQ(json(restored->activeConversation().getMessages()), json(restored->getMessages()));
}

TEST_F(SessionTest, HeartbeatsAcceptMatchingPongsAndExplicitGoodbyeClosesTheConnection) {
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        EXPECT_EQ((co_await client.receive())->at("type"), "daemon.ready");
        std::string previous;
        for (int i = 0; i < 3; ++i) {
            const auto ping = co_await client.receive();
            EXPECT_EQ(ping->at("type"), "integration.ping");
            const auto id = ping->at("pingId").get<std::string>();
            EXPECT_NE(id, previous);
            previous = id;
            co_await client.send({{"type", "integration.pong"}, {"pingId", id}});
        }
        co_await client.send({{"type", "integration.close"}});
        EXPECT_FALSE(co_await client.receive()); // Server closes even though the client has not closed its socket.
    });
    EXPECT_EQ(constructed, 0);
}

TEST_F(SessionTest, WrongPongAndOtherRequestsDoNotPreventHeartbeatTimeout) {
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await client.receive();
        const auto ping = co_await client.receive();
        EXPECT_EQ(ping->at("type"), "integration.ping");
        co_await client.send({{"type", "integration.pong"}, {"pingId", "wrong"}});
        co_await client.send({{"type", "session.list"}});
        EXPECT_EQ((co_await client.receive())->at("type"), "session.listed");
        EXPECT_FALSE(co_await client.receive());
    });
}

TEST_F(SessionTest, HeartbeatTimeoutCancelsAndPersistsTheActiveRun) {
    provider->waitForCancellation = true;
    std::string id;
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await client.receive();
        co_await client.send({{"type", "message.send"}, {"text", "Work"}, {"workspace", directory.string()}});
        const auto created = co_await client.receive();
        id = created->at("session").at("id").get<std::string>();
        while (co_await client.receive()) {} // Deliberately ignore pings.
    });
    SessionRegistry registry(directory / "sessions", factory());
    auto restored = registry.load(id);
    EXPECT_EQ(restored->getMessages().back().role, MessageRole::SYSTEM);
    EXPECT_NE(std::get<TextContent>(restored->getMessages().back().content[0]).text.find("interrupted"), std::string::npos);
}

TEST_F(SessionTest, PongIsReadWhileTheEventConsumerWaitsForTheTitle) {
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await client.receive();
        const auto ping = co_await client.receive();
        co_await client.send({{"type", "message.send"}, {"text", "Hello"}, {"workspace", directory.string()}});
        co_await client.send({{"type", "integration.pong"}, {"pingId", ping->at("pingId")}});
        bool created = false;
        for (;;) {
            const auto event = co_await client.receive();
            if (!event) throw std::runtime_error("Healthy integration closed while waiting for its title");
            if (event->at("type") == "integration.ping")
                co_await client.send({{"type", "integration.pong"}, {"pingId", event->at("pingId")}});
            if (event->at("type") == "session.created") created = true;
            if (event->at("type") == "execution.completed") break;
        }
        EXPECT_TRUE(created);
        co_await client.send({{"type", "integration.close"}});
    }, [](const std::string&, const std::string&, core::CancellationCheck) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        return "Slow title";
    });
}

TEST_F(SessionTest, SavesIncrementallyListsWithoutAgentsAndReloads) {
    SessionRegistry registry(directory / "sessions", factory());
    auto initialInfo = info();
    initialInfo.reasoning = "high";
    initialInfo.provider = "openai";
    auto session = registry.create(initialInfo);
    const auto id = session->getInfo().id;
    ASSERT_EQ(registry.list().size(), 1);
    EXPECT_EQ(constructed, 1);
    EXPECT_THROW(registry.load(id), std::logic_error);
    std::vector<AgentEventType> events;
    turn(*session, "Hi", events);
    EXPECT_EQ(provider->reasoning, "high");
    EXPECT_EQ(events, (std::vector<AgentEventType>{AgentEventType::HARNESS_MESSAGE_ARRIVED,
        AgentEventType::PROVIDER_MESSAGE_ARRIVED, AgentEventType::EXECUTION_COMPLETED}));
    const auto journal = directory / "sessions" / id / "session.sqlite";
    const auto before = SessionJournal(journal.parent_path()).sizeBytes();
    session->save();
    EXPECT_EQ(SessionJournal(journal.parent_path()).sizeBytes(), before);
    const json original = session->getMessages();
    session.reset();
    session = registry.load(id);
    EXPECT_EQ(json(session->getMessages()), original);
    ASSERT_TRUE(session->getMessages().back().metadata);
    EXPECT_EQ(session->getMessages().back().metadata->model, initialInfo.model);
    EXPECT_EQ(session->getMessages().back().metadata->reasoning, "high");
    EXPECT_EQ(session->getInfo().context.lastTurnUsedContext, 120);
    EXPECT_EQ(session->getInfo().context.totalContext, 4096);
    EXPECT_EQ(session->getInfo().reasoning, "high");
    EXPECT_EQ(session->getInfo().provider, "openai");
    EXPECT_EQ(json(session->getInfo())["last_turn_used_context"], 120);
    EXPECT_EQ(json(session->getInfo())["total_context"], 4096);
    turn(*session, "Again", events);
    ASSERT_EQ(provider->inputs.size(), 2);
    EXPECT_EQ(provider->inputs[1].size(), 4); // system, user, assistant, next user
    EXPECT_GT(SessionJournal(journal.parent_path()).sizeBytes(), before);
    EXPECT_EQ(session->getMessages().size(), 5);
}

TEST_F(SessionTest, McpSetupGuideReachesTheModelAndSurvivesSessionReload) {
    SessionRegistry registry(directory / "sessions", factory());
    auto session = registry.create(info());
    const auto initial = std::get<TextContent>(session->getMessages().at(0).content.at(0)).text;
    const auto start = initial.find("## MCP setup\n");
    ASSERT_NE(start, std::string::npos);
    const auto end = initial.find("\n## ", start + 1);
    ASSERT_NE(end, std::string::npos);
    const auto guide = initial.substr(start, end - start);
    EXPECT_LT(guide.size(), 1600); // Setup help, not the full MCP documentation.
    for (const auto* required : {"`/mcps`", "stdio", "Streamable HTTP", "Connect",
        "approval", "Enable", "Connect", "Never ask for secrets in chat", ".sos/mcps.json", "read-only"})
        EXPECT_NE(guide.find(required), std::string::npos) << required;
    const auto id = session->getInfo().id;
    session.reset();
    session = registry.load(id);
    EXPECT_EQ(std::get<TextContent>(session->getMessages().at(0).content.at(0)).text, initial);
    std::vector<AgentEventType> events;
    turn(*session, "How do I connect an MCP server?", events);
    ASSERT_FALSE(provider->inputs.empty());
    EXPECT_NE(provider->inputs.back().at(0).at("content").get<std::string>().find(guide), std::string::npos);
}

TEST_F(SessionTest, AnnouncesInitialModeAndPersistsOnlyChangesOnUserMessages) {
    SessionRegistry registry(directory / "sessions", factory());
    auto initial = info();
    initial.mode = ExecutionMode::SLOP;
    auto session = registry.create(initial);
    const auto initialMessages = json(session->getMessages());
    ASSERT_EQ(initialMessages.size(), 1);
    EXPECT_NE(std::get<TextContent>(session->getMessages()[0].content[0]).text.find("Current execution mode: slop."), std::string::npos);
    const auto id = session->getInfo().id;
    session.reset();
    session = registry.load(id);
    EXPECT_EQ(session->getInfo().mode, ExecutionMode::SLOP);
    EXPECT_EQ(json(session->getInfo()).at("mode"), "slop");
    std::vector<AgentEventType> events;
    turn(*session, "First", events, ExecutionMode::SLOP);
    ASSERT_EQ(provider->inputs[0].size(), 2); // Initial system prompt, user; no redundant announcement.
    EXPECT_THROW(session->sendMessage("  ", ExecutionMode::SANE), std::invalid_argument);
    EXPECT_EQ(session->getInfo().mode, ExecutionMode::SLOP);
    EXPECT_EQ(session->getMessages().size(), 3);
    turn(*session, "Read only now", events, ExecutionMode::SANE);
    ASSERT_EQ(provider->inputs[1].size(), 5);
    EXPECT_EQ(provider->inputs[1][3]["role"], "system");
    EXPECT_NE(provider->inputs[1][3]["content"].get<std::string>().find("Current execution mode: sane."), std::string::npos);
    EXPECT_EQ(provider->inputs[1][4]["role"], "user");
    EXPECT_EQ(json(session->getMessages())[0], initialMessages[0]);
    const json history = session->getMessages();
    EXPECT_EQ(history[1]["metadata"], (json{{"mode", "slop"}}));
    EXPECT_EQ(history[4]["metadata"], (json{{"mode", "sane"}}));
    EXPECT_FALSE(provider->inputs[0][1].contains("metadata"));
    EXPECT_FALSE(provider->inputs[1][4].contains("metadata"));
    session.reset();
    session = registry.load(id);
    EXPECT_EQ(session->getInfo().mode, ExecutionMode::SANE);
    EXPECT_EQ(json(session->getMessages()), history);
    EXPECT_EQ(session->getMessages()[1].userMetadata->mode, "slop");
    EXPECT_EQ(session->getMessages()[4].userMetadata->mode, "sane");
    turn(*session, "Still read only", events);
    EXPECT_EQ(provider->inputs.back().size(), history.size() + 1);
    turn(*session, "Implement now", events, ExecutionMode::SLOP);
    const auto& changed = provider->inputs.back();
    EXPECT_EQ(changed[changed.size() - 2]["role"], "system");
    EXPECT_NE(changed[changed.size() - 2]["content"].get<std::string>().find("Current execution mode: slop."), std::string::npos);
    // Every earlier provider input remains an identical prefix, including across reload.
    for (std::size_t i = 1; i < provider->inputs.size(); ++i)
        for (std::size_t j = 0; j < provider->inputs[i - 1].size(); ++j)
            EXPECT_EQ(provider->inputs[i][j], provider->inputs[i - 1][j]);
    auto incomplete = json(session->getInfo());
    incomplete.erase("mode");
    EXPECT_THROW(incomplete.get<SessionInfo>(), json::out_of_range);
}

TEST_F(SessionTest, CompactionPreservesFullHistoryAndRestoresOnlyActiveContextAcrossRepeatedCompactions) {
    SessionRegistry registry(directory / "sessions", factory());
    auto session = registry.create(info());
    const auto id = session->getInfo().id;
    std::vector<AgentEventType> events;
    turn(*session, "Old request", events);
    turn(*session, "Recent request", events, ExecutionMode::SLOP);
    const json before = session->getMessages();
    auto summaryState = std::make_shared<ProviderState>();
    summaryState->responseText = "## Goal\n- Summary one";
    TestProvider summarizer(summaryState);
    auto result = CompactionService{}.compact(session->activeConversation(), summarizer, session->getInfo(), "compact_1");
    session->applyCompaction(std::move(result));
    auto full = session->getMessages();
    ASSERT_EQ(full.size(), before.size() + 1);
    for (std::size_t i = 0; i < before.size(); ++i) EXPECT_EQ(json(full[i]), before[i]);
    EXPECT_EQ(registry.lastUserMessage(id), "Recent request");
    EXPECT_FALSE(session->getInfo().context.lastTurnUsedContext);
    const json active = session->activeConversation().getMessages();
    EXPECT_EQ(active.dump().find("Old request"), std::string::npos);
    EXPECT_NE(active.dump().find("Recent request"), std::string::npos);
    session.reset();
    session = registry.load(id);
    EXPECT_EQ(json(session->getMessages()), json(full));
    EXPECT_EQ(json(session->activeConversation().getMessages()), active);
    turn(*session, "After compaction", events, ExecutionMode::SLOP);
    EXPECT_EQ(provider->inputs.back().dump().find("Old request"), std::string::npos);
    EXPECT_NE(provider->inputs.back().dump().find("Summary one"), std::string::npos);
    EXPECT_NE(provider->inputs.back().dump().find("Recent request"), std::string::npos);
    summaryState->responseText = "## Goal\n- Summary two";
    const auto secondSummaryStart = summaryState->inputs.size();
    auto second = CompactionService{}.compact(session->activeConversation(), summarizer, session->getInfo(), "compact_2");
    session->applyCompaction(std::move(second));
    full = session->getMessages();
    const json secondActive = session->activeConversation().getMessages();
    EXPECT_EQ(secondActive.dump().find("Summary one"), std::string::npos);
    EXPECT_NE(json(full).dump().find("Summary one"), std::string::npos);
    // The prior summary must enter this synthesis, not necessarily its last
    // request after chunking and recursive merging of intermediate summaries.
    EXPECT_TRUE(std::any_of(summaryState->inputs.begin() + secondSummaryStart, summaryState->inputs.end(), [](const auto& input) {
        return input.dump().find("Summary one") != std::string::npos;
    }));
    session->applySelection({"openrouter", "changed-model", "high"});
    session.reset();
    session = registry.load(id);
    EXPECT_EQ(json(session->getMessages()), json(full));
    EXPECT_EQ(json(session->activeConversation().getMessages()), secondActive);
    turn(*session, "Continue after resume", events);
    EXPECT_NE(provider->inputs.back().dump().find("Summary two"), std::string::npos);
    EXPECT_EQ(provider->inputs.back().dump().find("Old request"), std::string::npos);
    EXPECT_EQ(provider->inputs.back().dump().find("Recent request"), std::string::npos);
    EXPECT_EQ(provider->model, "changed-model");
    std::set<std::string> ids;
    for (const auto& message : session->getMessages()) EXPECT_TRUE(ids.insert(message.id).second);
}

TEST_F(SessionTest, FailedCompactionSaveKeepsHistoryAndActiveConversation) {
    SessionRegistry registry(directory / "sessions", factory());
    auto session = registry.create(info());
    std::vector<AgentEventType> events;
    turn(*session, "Keep this", events);
    const json history = session->getMessages();
    const json active = session->activeConversation().getMessages();
    TestProvider summarizer(std::make_shared<ProviderState>());
    auto result = CompactionService{}.compact(session->activeConversation(), summarizer, session->getInfo(), "compact");
    const auto journal = directory / "sessions" / session->getInfo().id / "session.sqlite";
    SessionDatabase fault(journal);
    fault.execute("CREATE TRIGGER fail_checkpoint BEFORE INSERT ON messages BEGIN SELECT RAISE(ABORT,'injected checkpoint failure'); END");
    EXPECT_THROW(session->applyCompaction(std::move(result)), std::exception);
    fault.execute("DROP TRIGGER fail_checkpoint");
    EXPECT_EQ(json(session->getMessages()), history);
    EXPECT_EQ(json(session->activeConversation().getMessages()), active);
}

TEST_F(SessionTest, RuntimeSwitchesDoNotChangeSessionUntilAMessageStarts) {
    std::string id;
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        const auto receive = [&]() -> asio::awaitable<json> {
            for (;;) {
                const auto event = co_await client.receive();
                if (!event) throw std::runtime_error("Unexpected disconnect");
                if (event->at("type") != "integration.ping") co_return *event;
                co_await client.send({{"type", "integration.pong"}, {"pingId", event->at("pingId")}});
            }
        };
        co_await receive();
        const auto send = [&](const char* mode) -> asio::awaitable<void> {
            co_await client.send({{"type", "message.send"}, {"text", "Hello"}, {"workspace", directory.string()}, {"mode", mode}});
            const auto accepted = co_await receive();
            id = accepted.at("session").at("id").get<std::string>();
            EXPECT_EQ(accepted.at("session").at("mode"), mode);
            while ((co_await receive()).at("type") != "execution.completed") {}
        };
        const auto switchMode = [&](const char* mode) -> asio::awaitable<void> {
            co_await client.send({{"type", "mode.set"}, {"mode", mode}});
            EXPECT_EQ((co_await receive()).at("mode"), mode);
        };
        co_await send("sane");
        EXPECT_NE(provider->inputs[0][0]["content"].get<std::string>().find("Current execution mode: sane."), std::string::npos);
        const auto journal = directory / "sessions" / id / "session.sqlite";
        const auto size = SessionJournal(journal.parent_path()).sizeBytes();
        co_await switchMode("slop");
        co_await client.send({{"type", "session.list"}});
        EXPECT_EQ((co_await receive()).at("sessions")[0].at("mode"), "sane");
        EXPECT_EQ(SessionJournal(journal.parent_path()).sizeBytes(), size);
        co_await switchMode("sane");
        co_await send("sane");
        EXPECT_EQ(provider->inputs[1].size(), 4); // No announcement for the round trip.
        co_await switchMode("slop");
        co_await send("slop");
        EXPECT_EQ(provider->inputs[2].size(), 7); // system notification, then user.
        EXPECT_EQ(provider->inputs[2][5]["role"], "system");
        co_await send("slop");
        EXPECT_EQ(provider->inputs[3].size(), 9);
        co_await send("sane"); // Explicit message mode overrides the runtime selection.
        EXPECT_EQ(provider->inputs[4].size(), 12);
        EXPECT_NE(provider->inputs[4][10]["content"].get<std::string>().find("Current execution mode: sane."), std::string::npos);
        co_await switchMode("slop"); // Leaving without sending must not persist this selection.
        co_await client.send({{"type", "integration.close"}});
    });
    SessionRegistry registry(directory / "sessions", factory());
    auto restored = registry.load(id);
    EXPECT_EQ(restored->getInfo().mode, ExecutionMode::SANE);
    EXPECT_EQ(restored->getMessages().size(), 13);
}

TEST_F(SessionTest, ResumeRestoresConversationPreferencesAndNewUsesLastExplicitSelection) {
    provider->contextLimit = 262144;
    SessionRegistry registry(directory / "sessions", factory());
    auto original = info();
    original.model = "gpt-5.4";
    original.reasoning = "low";
    original.provider = "openai";
    original.contextLimit = 65536;
    auto old = registry.create(original);
    std::vector<AgentEventType> events;
    turn(*old, "Old question", events);
    const auto id = old->getInfo().id;
    const auto originalMessages = json(old->getMessages());
    const auto originalHistory = old->getMessages();
    const auto historicalGeneration = std::find_if(originalHistory.begin(), originalHistory.end(), [](const auto& message) {
        return message.metadata.has_value();
    });
    ASSERT_NE(historicalGeneration, originalHistory.end());
    const auto originalGeneration = json(*historicalGeneration);
    old.reset();
    auto auth = std::make_shared<AuthStore>(directory / "auth.json");
    auth->set("openai", ProviderCredential{.key = "fake-key"});
    auto preferences = std::make_shared<ProviderConnections>(auth, directory / "config.json");
    preferences->select({"openai", "gpt-4.1", "high", 131072});

    asio::io_context io;
    ServerSocket server(io, directory / "preferences.sock");
    server.listen();
    asio::local::stream_protocol::socket socket(io);
    socket.connect(asio::local::stream_protocol::endpoint((directory / "preferences.sock").string()));
    ConnectionSocket client(std::move(socket));
    auto run = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        auto integration = std::make_shared<Integration>(io, co_await server.accept(), registry,
            SessionTitleGenerator{}, [](const std::string&) { return std::vector<std::string>{"low", "xhigh"}; }, IntegrationHealthSettings{}, preferences);
        co_await integration->run();
    }, asio::use_future);
    auto interaction = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        const auto ready = co_await client.receive();
        EXPECT_EQ(ready->at("info").at("model"), "gpt-4.1");
        EXPECT_EQ(ready->at("info").at("reasoning"), "high");
        co_await client.send({{"type", "integration.configure"}, {"workspace", directory.string()}, {"mode", "sane"}});
        co_await client.receive();
        co_await client.send({{"type", "session.attach"}, {"sessionId", id}});
        const auto attached = co_await receiveReply(client);
        EXPECT_EQ(attached->at("info").at("model"), "gpt-5.4");
        EXPECT_EQ(attached->at("info").at("reasoning"), "low");
        EXPECT_EQ(attached->at("info").at("context_limit"), 65536);
        EXPECT_EQ(preferences->selection().model, "gpt-4.1");
        EXPECT_EQ(preferences->selection().reasoning, "high");
        EXPECT_EQ(preferences->selection().contextLimit, 131072);
        co_await client.send({{"type", "reasoning.next"}});
        EXPECT_EQ((co_await client.receive())->at("info").at("reasoning"), "medium");
        co_await client.send({{"type", "session.new"}});
        const auto fresh = co_await client.receive();
        EXPECT_EQ(fresh->at("info").at("model"), "gpt-5.4");
        EXPECT_EQ(fresh->at("info").at("reasoning"), "medium");
        EXPECT_EQ(fresh->at("info").at("context_limit"), 65536);
        co_await client.send({{"type", "session.attach"}, {"sessionId", id}});
        EXPECT_EQ((co_await receiveReply(client))->at("info").at("reasoning"), "medium");
        co_await client.send({{"type", "model.select"}, {"provider", "openai"}, {"model", "gpt-4.1"}});
        const auto selected = co_await client.receive();
        EXPECT_EQ(selected->at("type"), "model.selected");
        EXPECT_EQ(selected->at("info").at("model"), "gpt-4.1");
        EXPECT_EQ(selected->at("info").at("reasoning"), "medium");
        co_await client.send({{"type", "model.select"}, {"provider", "openrouter"}, {"model", "vendor/model"}});
        EXPECT_EQ((co_await client.receive())->at("type"), "request.failed");
        co_await client.send({{"type", "model.select"}, {"provider", "openai"}, {"model", "gpt-5.4"}});
        EXPECT_EQ((co_await client.receive())->at("type"), "model.selected");
        co_await client.send({{"type", "message.send"}, {"text", "Continue"}});
        EXPECT_EQ((co_await client.receive())->at("type"), "execution.preparing");
        const auto accepted = co_await client.receive();
        EXPECT_EQ(accepted->at("type"), "message.accepted");
        EXPECT_EQ(accepted->at("session").at("id"), id);
        for (;;) {
            const auto event = co_await client.receive();
            if (event->at("type") == "execution.completed") {
                EXPECT_EQ(event->at("metadata").at("model"), "gpt-5.4");
                EXPECT_EQ(event->at("metadata").at("reasoning"), "medium");
                break;
            }
        }
        client.close();
    }, asio::use_future);
    io.run_for(std::chrono::seconds(3));
    client.close();
    io.restart();
    io.run_for(std::chrono::seconds(1));
    ASSERT_EQ(interaction.wait_for(std::chrono::seconds(0)), std::future_status::ready);
    interaction.get();
    ASSERT_EQ(run.wait_for(std::chrono::seconds(0)), std::future_status::ready);
    run.get();
    ProviderConnections restarted(auth, directory / "config.json");
    EXPECT_EQ(restarted.selection().model, "gpt-5.4");
    EXPECT_EQ(restarted.selection().reasoning, "medium");
    const auto restored = registry.load(id);
    const auto restoredMessages = restored->getMessages();
    const auto historical = std::find_if(restoredMessages.begin(), restoredMessages.end(), [&](const auto& message) {
        return message.id == originalGeneration.at("id").get<std::string>();
    });
    ASSERT_NE(historical, restoredMessages.end());
    EXPECT_EQ(json(*historical), originalGeneration);
    const auto latestGeneration = std::find_if(restoredMessages.rbegin(), restoredMessages.rend(), [](const auto& message) {
        return message.metadata.has_value();
    });
    ASSERT_NE(latestGeneration, restoredMessages.rend());
    EXPECT_EQ(latestGeneration->metadata->model, "gpt-5.4");
    EXPECT_GE(restored->getInfo().providerContextStart, originalMessages.size());
    EXPECT_EQ(provider->inputs.back().dump().find("msg_response_1"), std::string::npos);
}

TEST_F(SessionTest, FailureStillSavesUserAndAllowsNextRun) {
    SessionRegistry registry(directory / "sessions", factory());
    auto session = registry.create(info());
    provider->fail = true;
    std::vector<AgentEventType> events;
    turn(*session, "Hi", events);
    EXPECT_EQ(events.back(), AgentEventType::EXECUTION_FAILED);
    EXPECT_FALSE(session->isExecuting());
    EXPECT_EQ(session->getMessages().size(), 2);
    provider->fail = false;
    turn(*session, "Retry", events);
    EXPECT_EQ(events.back(), AgentEventType::EXECUTION_COMPLETED);
}

TEST_F(SessionTest, ContextMeasurementsRemainOptionalButGenerationSettingsAreRequired) {
    json encoded = info();
    encoded.erase("last_turn_used_context");
    encoded.erase("total_context");
    const auto restored = encoded.get<SessionInfo>();
    EXPECT_FALSE(restored.context.lastTurnUsedContext);
    EXPECT_FALSE(restored.context.totalContext);
    EXPECT_EQ(restored.reasoning, info().reasoning);
    EXPECT_EQ(restored.provider, info().provider);
    for (const auto* field : {"reasoning", "provider"}) {
        auto incomplete = encoded;
        incomplete.erase(field);
        EXPECT_THROW(incomplete.get<SessionInfo>(), json::out_of_range);
    }
    EXPECT_THROW((json{{"last_turn_used_context", -1}}.get<ContextUsage>()), std::invalid_argument);
}

TEST_F(SessionTest, MissingInfoCacheIsRecoveredAndInvalidIdsAreRejected) {
    SessionRegistry registry(directory / "sessions", factory());
    auto session = registry.create(info());
    const auto id = session->getInfo().id;
    session.reset();
    const auto cache = directory / "sessions" / id / "info.json";
    fs::remove(cache);
    EXPECT_EQ(registry.list().size(), 1);
    EXPECT_TRUE(fs::exists(cache));
    fs::remove(cache);
    session = registry.load(id);
    EXPECT_TRUE(fs::exists(cache));
    EXPECT_EQ(session->messageCount(), 1);
    EXPECT_THROW(registry.load("../outside"), std::invalid_argument);
    EXPECT_THROW(registry.load(std::string(32, '0')), std::runtime_error);
}

TEST_F(SessionTest, InfoIsACacheAndIsNotRewrittenForEveryToolResult) {
    SessionRegistry registry(directory / "sessions", factory());
    auto session = registry.create(info());
    const auto folder = directory / "sessions" / session->getInfo().id;
    const auto cache = folder / "info.json";
    const auto initialStamp = fs::last_write_time(cache);
    session->record({AgentEventType::PROVIDER_MESSAGE_ARRIVED,
        {{"call", MessageRole::ASSISTANT, {ToolCallContent{"call", "bash", {{"command", "echo hi"}}}}}}, {}, {}});
    session->record({AgentEventType::HARNESS_MESSAGE_ARRIVED,
        {{"result", MessageRole::TOOL, {ToolResultContent{"call", "hi", false}}}}, {}, {}});
    EXPECT_EQ(fs::last_write_time(cache), initialStamp);
    // A failed cache publication must not turn a committed write into a failure.
    fs::create_directory(folder / "info.json.tmp");
    EXPECT_NO_THROW(session->save());
    EXPECT_EQ(SessionJournal(folder).count(), 3);
    fs::remove(folder / "info.json.tmp");
    // A valid but stale cache is also ignored by resume, not just malformed JSON.
    auto stale = json(session->getInfo());
    stale["model"] = "stale-model";
    { std::ofstream stream(cache); stream << stale.dump(); }
    const auto id = session->getInfo().id;
    const auto model = session->getInfo().model;
    session.reset();
    session = registry.load(id);
    EXPECT_EQ(session->getInfo().model, model);
    EXPECT_EQ(session->getMessages().size(), 3);
    std::ifstream stream(cache);
    EXPECT_EQ(json::parse(stream)["model"], model);
}

TEST_F(SessionTest, SerializesUserModeAndAcceptsLegacyMessagesWithoutIt) {
    Message message{"user", MessageRole::USER, {TextContent{"Hello"}}};
    const json legacy = message;
    EXPECT_FALSE(legacy.get<Message>().userMetadata);
    auto nullable = legacy;
    nullable["metadata"] = nullptr;
    EXPECT_FALSE(nullable.get<Message>().userMetadata);
    for (const auto* mode : {"sane", "slop"}) {
        message.userMetadata = UserMessageMetadata{mode};
        const json serialized = message;
        EXPECT_EQ(serialized.at("metadata"), (json{{"mode", mode}}));
        EXPECT_EQ(json(serialized.get<Message>()), serialized);
        EXPECT_FALSE(serialized.get<Message>().metadata);
    }
    auto invalid = json(message);
    invalid["metadata"]["mode"] = "unknown";
    EXPECT_THROW(invalid.get<Message>(), std::invalid_argument);
    message.userMetadata->mode = "unknown";
    EXPECT_THROW(json{message}, std::invalid_argument);
}

TEST_F(SessionTest, SerializesEveryContentWithoutLosingProviderMetadata) {
    Message message{"message", MessageRole::ASSISTANT, {
        TextContent{"text", ProviderFormat::OPENAI_RESPONSES, {{"phase", "commentary"}, {"item_id", "msg"}}},
        ReasoningContent{std::nullopt, ProviderFormat::OPENAI_RESPONSES, {{"encrypted_content", "opaque"}}},
        ToolCallContent{"call", "read", {{"filePath", "file"}}, ProviderFormat::OPENAI_RESPONSES, {{"id", "fc"}}},
        ToolResultContent{"call", "output", true, {{"image/png", "data:image/png;base64,AAA="}},
            {{"diff", "-old\n+new\n"}, {"filepath", "/test.cpp"}}}
    }};
    const json serialized = message;
    EXPECT_EQ(json(serialized.get<Message>()), serialized);
    auto legacy = serialized;
    legacy["content"][3].erase("metadata");
    EXPECT_TRUE(std::get<ToolResultContent>(legacy.get<Message>().content[3]).metadata.empty());
    auto invalid = serialized;
    invalid["content"][0]["type"] = "unknown";
    EXPECT_THROW(invalid.get<Message>(), std::invalid_argument);
}

TEST_F(SessionTest, WorkerCompletesBeforeAsioConsumesNotifications) {
    SessionRegistry registry(directory / "sessions", factory());
    auto session = registry.create(info());
    const auto journal = directory / "sessions" / session->getInfo().id / "session.sqlite";
    const auto initialSize = SessionJournal(journal.parent_path()).sizeBytes();
    asio::io_context io;
    auto queue = std::make_shared<AsyncQueue<AgentEvent>>(io.get_executor());
    std::promise<void> notified;
    auto notification = notified.get_future();
    session->setEventHandler([queue, executor = io.get_executor(), &notified](AgentEvent event) {
        const bool terminal = event.isTerminal();
        asio::post(executor, [queue, event = std::move(event)]() mutable { queue->push(std::move(event)); });
        if (terminal) notified.set_value();
    });
    session->sendMessage("Hi", ExecutionMode::SANE);
    EXPECT_EQ(notification.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(SessionJournal(journal.parent_path()).sizeBytes(), initialSize); // The consumer has not run at all yet.
    auto consume = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        for (;;) {
            auto event = co_await queue->receive();
            session->record(event);
            if (event.isTerminal()) { co_await session->finishExecution(); break; }
        }
    }, asio::use_future);
    io.run();
    consume.get();
    EXPECT_GT(SessionJournal(journal.parent_path()).sizeBytes(), initialSize);
    EXPECT_EQ(session->getMessages().size(), 3);
    session->setEventHandler({});
}

TEST_F(SessionTest, PersistsToolDisplayMetadata) {
    SessionRegistry registry(directory / "sessions", factory());
    auto session = registry.create(info());
    const auto id = session->getInfo().id;
    const json metadata{{"diff", "-old\n+new\n"}, {"filepath", "source.cpp"}};
    session->record({AgentEventType::HARNESS_MESSAGE_ARRIVED, {
        Message{"call", MessageRole::ASSISTANT, {ToolCallContent{"call_1", "edit", {{"filePath", "source.cpp"}}}}},
        Message{"result", MessageRole::TOOL, {ToolResultContent{"call_1", "Edit applied successfully.", false, {}, metadata}}}
    }, {}});
    session.reset();
    session = registry.load(id);
    EXPECT_EQ(std::get<ToolResultContent>(session->getMessages().back().content[0]).metadata, metadata);
}

TEST_F(SessionTest, PreviewReadsLatestUserRequestWithoutLoadingOrChangingHistory) {
    SessionRegistry registry(directory / "sessions", factory());
    auto session = registry.create(info());
    const auto id = session->getInfo().id;
    EXPECT_TRUE(registry.lastUserMessage(id).empty());
    std::vector<AgentEventType> events;
    turn(*session, "First request", events);
    turn(*session, "Last request\nwith details", events);
    const auto journal = directory / "sessions" / id / "session.sqlite";
    const auto bytes = SessionJournal(journal.parent_path()).sizeBytes();
    const auto agents = constructed;
    EXPECT_EQ(registry.lastUserMessage(id), "Last request\nwith details");
    EXPECT_EQ(SessionJournal(journal.parent_path()).sizeBytes(), bytes);
    EXPECT_EQ(constructed, agents);
    EXPECT_THROW(registry.lastUserMessage("../other"), std::invalid_argument);
}

TEST_F(SessionTest, CyclesReasoningBeforeCreationAndPersistsSelectionForFollowingTurns) {
    asio::io_context io;
    SessionRegistry registry(directory / "sessions", factory());
    ServerSocket server(io, directory / "server.sock");
    server.listen();
    asio::local::stream_protocol::socket socket(io);
    socket.connect(asio::local::stream_protocol::endpoint((directory / "server.sock").string()));
    ConnectionSocket client(std::move(socket));
    std::shared_ptr<Integration> integration;
    std::string id;
    std::string selected = daemon_config::DEFAULT_REASONING_EFFORT;
    bool unavailable = false;
    auto run = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        integration = std::make_shared<Integration>(io, co_await server.accept(), registry,
            [](const std::string&, const std::string&, core::CancellationCheck) { return "Reasoning selection"; },
            [&](const std::string& model) {
                EXPECT_EQ(model, daemon_config::DEFAULT_MODEL);
                return unavailable ? std::vector<std::string>{} : std::vector<std::string>{"none", "low", "high"};
            });
        co_await integration->run();
    }, asio::use_future);
    auto interaction = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        co_await client.receive();
        for (int step = 0; step < 3; ++step) {
            co_await client.send({{"type", "reasoning.next"}, {"requestId", "cycle"}});
            const auto updated = co_await client.receive();
            selected = selected == "none" ? "low" : selected == "low" ? "high" : "none";
            EXPECT_EQ(updated->at("type"), "reasoning.updated");
            EXPECT_EQ(updated->at("requestId"), "cycle");
            EXPECT_EQ(updated->at("info").at("reasoning"), selected);
            EXPECT_TRUE(registry.list().empty());
        }
        co_await client.send({{"type", "message.send"}, {"text", "First"}, {"workspace", directory.string()}});
        const auto created = co_await client.receive();
        EXPECT_EQ(created->at("session").at("reasoning"), selected);
        id = created->at("session").at("id").get<std::string>();
        for (;;) {
            const auto event = co_await client.receive();
            if (event->at("type") == "execution.completed") {
                EXPECT_EQ(event->at("metadata").at("reasoning"), selected);
                break;
            }
        }
        co_await client.send({{"type", "reasoning.next"}});
        const auto updated = co_await client.receive();
        selected = selected == "none" ? "low" : selected == "low" ? "high" : "none";
        EXPECT_EQ(updated->at("info").at("reasoning"), selected);
        unavailable = true;
        co_await client.send({{"type", "reasoning.next"}});
        const auto failed = co_await client.receive();
        EXPECT_EQ(failed->at("type"), "request.failed");
        EXPECT_EQ(failed->at("info").at("reasoning"), selected);
        co_await client.send({{"type", "message.send"}, {"text", "Next"}});
        for (;;) {
            const auto event = co_await client.receive();
            if (event->at("type") == "execution.completed") {
                EXPECT_EQ(event->at("metadata").at("reasoning"), selected);
                break;
            }
        }
        client.close();
    }, asio::use_future);
    io.run_for(std::chrono::seconds(3));
    client.close();
    io.restart();
    io.run_for(std::chrono::seconds(1));
    ASSERT_EQ(interaction.wait_for(std::chrono::seconds(0)), std::future_status::ready);
    interaction.get();
    ASSERT_EQ(run.wait_for(std::chrono::seconds(0)), std::future_status::ready);
    run.get();
    integration.reset();
    auto restored = registry.load(id);
    EXPECT_EQ(restored->getInfo().reasoning, selected);
    EXPECT_EQ(restored->getMessages().back().metadata->reasoning, selected);
}

TEST_F(SessionTest, DisconnectCancelsAndDrainsBeforeSessionIsReleased) {
    std::promise<void> release;
    provider->release = release.get_future().share();
    auto entered = provider->entered.get_future();
    asio::io_context io;
    TestProvider* activeProvider = nullptr;
    SessionRegistry registry(directory / "sessions", [&](const SessionInfo& info) {
        auto instance = std::make_unique<TestProvider>(provider);
        activeProvider = instance.get();
        return std::make_unique<Agent>(std::move(instance), info.model,
            ToolRegistry{}, ToolContext{}, info.reasoning);
    });
    ServerSocket server(io, directory / "server.sock");
    server.listen();
    asio::local::stream_protocol::socket socket(io);
    socket.connect(asio::local::stream_protocol::endpoint((directory / "server.sock").string()));
    ConnectionSocket client(std::move(socket));
    std::shared_ptr<Integration> integration;
    std::string id;
    auto run = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        integration = std::make_shared<Integration>(io, co_await server.accept(), registry,
            [](const std::string&, const std::string&, core::CancellationCheck) { return "Workspace greeting"; });
        co_await integration->run();
    }, asio::use_future);
    auto interaction = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        co_await client.receive();
        co_await client.send({{"type", "message.send"}, {"text", "Hello"}, {"workspace", directory.string()}});
        const auto created = co_await client.receive();
        id = created->at("session").at("id").get<std::string>();
        // Exercise disconnect during generation, rather than racing the worker's first cancellation check.
        asio::steady_timer timer(io);
        while (entered.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            timer.expires_after(std::chrono::milliseconds(1));
            co_await timer.async_wait(asio::use_awaitable);
        }
        client.close();
    }, asio::use_future);
    // Preparation and disconnect detection both need the event loop to keep running.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        io.run_for(std::chrono::milliseconds(5));
        if (entered.wait_for(std::chrono::seconds(0)) == std::future_status::ready &&
            activeProvider->cancellationRequested()) break;
    }
    const bool started = entered.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    EXPECT_TRUE(started);
    if (started) EXPECT_TRUE(activeProvider->cancellationRequested());
    EXPECT_EQ(run.wait_for(std::chrono::seconds(0)), std::future_status::timeout);
    if (!id.empty()) EXPECT_THROW(registry.load(id), std::logic_error);
    release.set_value();
    io.restart();
    io.run_for(std::chrono::seconds(2));
    ASSERT_EQ(run.wait_for(std::chrono::seconds(0)), std::future_status::ready);
    run.get();
    interaction.get();
    integration.reset();
    auto loaded = registry.load(id);
    EXPECT_EQ(loaded->getMessages().size(), 4); // Late response and interruption note are both saved.
    EXPECT_EQ(loaded->getMessages().back().role, MessageRole::SYSTEM);
}

TEST_F(SessionTest, InterruptedCallsAreNotAutomaticallyReexecutedOnLoad) {
    SessionRegistry registry(directory / "sessions", factory());
    auto session = registry.create(info());
    const auto id = session->getInfo().id;
    session->record({AgentEventType::PROVIDER_MESSAGE_ARRIVED,
        {Message{"pending", MessageRole::ASSISTANT, {ToolCallContent{"call_1", "write", {{"filePath", "file"}}}}}}, {}});
    session.reset();
    session = registry.load(id);
    const auto messages = session->getMessages();
    ASSERT_EQ(messages.size(), 3);
    EXPECT_TRUE(std::get<ToolResultContent>(messages.back().content[0]).isError);
    EXPECT_TRUE(provider->inputs.empty());
}

TEST_F(SessionTest, CancelDuringDeferredPreparationKeepsHistoryAndAllowsRetry) {
    std::promise<void> entered, release;
    auto enteredFuture = entered.get_future();
    const auto released = release.get_future().share();
    std::atomic<bool> pauseOnce = false;
    const auto normalFactory = factory();
    SessionRegistry registry(directory / "sessions", [&](const SessionInfo& selected) {
        if (pauseOnce.exchange(false)) {
            entered.set_value();
            if (released.wait_for(std::chrono::seconds(2)) != std::future_status::ready)
                throw std::runtime_error("Preparation blocked the request reader");
        }
        return normalFactory(selected);
    });
    auto saved = registry.create(info());
    const auto id = saved->getInfo().id;
    saved.reset();
    pauseOnce = true;
    asio::io_context io;
    ServerSocket server(io, directory / "preparation.sock");
    server.listen();
    asio::local::stream_protocol::socket socket(io);
    socket.connect(asio::local::stream_protocol::endpoint((directory / "preparation.sock").string()));
    ConnectionSocket client(std::move(socket));
    auto run = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        auto integration = std::make_shared<Integration>(io, co_await server.accept(), registry, SessionTitleGenerator{});
        co_await integration->run();
    }, asio::use_future);
    auto interaction = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        co_await client.receive();
        co_await client.send({{"type", "integration.configure"}, {"workspace", directory.string()}, {"mode", "sane"}});
        co_await client.receive();
        co_await client.send({{"type", "session.attach"}, {"sessionId", id}});
        EXPECT_EQ((co_await receiveReply(client))->at("type"), "session.attached");
        co_await client.send({{"type", "message.send"}, {"text", "Do not persist this"}});
        EXPECT_EQ((co_await client.receive())->at("type"), "execution.preparing");
        co_await awaitFuture(enteredFuture);
        co_await client.send({{"type", "execution.cancel"}});
        asio::steady_timer timer(io, std::chrono::milliseconds(10));
        co_await timer.async_wait(asio::use_awaitable);
        release.set_value();
        EXPECT_EQ((co_await client.receive())->at("type"), "execution.cancelled");
        EXPECT_EQ((co_await client.receive())->at("type"), "execution.cancel_requested");
        co_await client.send({{"type", "message.send"}, {"text", "Continue"}});
        EXPECT_EQ((co_await client.receive())->at("type"), "execution.preparing");
        EXPECT_EQ((co_await client.receive())->at("type"), "message.accepted");
        while ((co_await client.receive())->at("type") != "execution.completed") {}
        co_await client.send({{"type", "integration.close"}});
    }, asio::use_future);
    io.run_for(std::chrono::seconds(4));
    client.close();
    io.restart(); io.run_for(std::chrono::seconds(1));
    interaction.get(); run.get();
    const auto restored = registry.load(id);
    EXPECT_EQ(restored->messageCount(), 3);
    EXPECT_EQ(json(restored->getMessages()).dump().find("Do not persist this"), std::string::npos);
}

TEST_F(SessionTest, IntegrationCreatesLazilyDrainsEventsAndCanReattach) {
    asio::io_context io;
    SessionRegistry registry(directory / "sessions", factory());
    ServerSocket server(io, directory / "server.sock");
    server.listen();
    std::string id;
    auto titleState = std::make_shared<ProviderState>();
    titleState->responseText = "Workspace greeting";
    const auto exchange = [&](bool attach) {
        asio::local::stream_protocol::socket socket(io);
        socket.connect(asio::local::stream_protocol::endpoint((directory / "server.sock").string()));
        ConnectionSocket client(std::move(socket));
        std::shared_ptr<Integration> integration;
        auto run = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
            integration = std::make_shared<Integration>(io, co_await server.accept(), registry,
                [titleState](const std::string& model, const std::string& firstMessage, core::CancellationCheck) {
                    TestProvider provider(titleState);
                    return generateSessionTitle(provider, model, firstMessage);
                });
            co_await integration->run();
        }, asio::use_future);
        auto clientRun = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
            const auto ready = co_await client.receive();
            EXPECT_EQ(ready->at("type"), "daemon.ready");
            EXPECT_EQ(ready->at("info").at("reasoning"), daemon_config::DEFAULT_REASONING_EFFORT);
            EXPECT_TRUE(ready->at("info").at("last_turn_used_context").is_null());
            if (!attach) {
                EXPECT_TRUE(registry.list().empty());
                co_await client.send({{"type", "message.send"}, {"text", "Hello"}, {"workspace", directory.string()}});
                const auto created = co_await client.receive();
                EXPECT_EQ(created->at("type"), "session.created");
                EXPECT_EQ(created->at("session").at("title"), "Workspace greeting");
                EXPECT_TRUE(created->at("info").at("last_turn_used_context").is_null()); // Title call excluded.
                EXPECT_EQ(titleState->inputs.size(), 1);
                EXPECT_EQ(titleState->model, daemon_config::DEFAULT_MODEL);
                id = created->at("session").at("id").get<std::string>();
                std::vector<std::string> types;
                for (;;) {
                    const auto event = co_await client.receive();
                    types.push_back(event->at("type").get<std::string>());
                    EXPECT_EQ(event->at("info").at("model"), daemon_config::DEFAULT_MODEL);
                    if (types.back() != "harness.message_arrived") {
                        EXPECT_EQ(event->at("info").at("last_turn_used_context"), 120);
                        EXPECT_EQ(event->at("info").at("total_context"), 4096);
                    }
                    if (types.back() == "provider.message_arrived")
                        EXPECT_TRUE(event->at("messages")[0].at("metadata").at("duration_ms").is_number_unsigned());
                    if (types.back() == "execution.completed") {
                        EXPECT_EQ(event->at("metadata").at("model"), daemon_config::DEFAULT_MODEL);
                        EXPECT_TRUE(event->at("metadata").contains("duration_ms"));
                    }
                    if (types.back() == "execution.completed") break;
                }
                EXPECT_EQ(types, (std::vector<std::string>{"harness.message_arrived", "provider.message_arrived", "execution.completed"}));
            } else {
                co_await client.send({{"type", "integration.configure"}, {"workspace", directory.string()}, {"mode", "sane"}});
                EXPECT_EQ((co_await client.receive())->at("type"), "integration.configured");
                co_await client.send({{"type", "session.list"}});
                const auto listed = co_await client.receive();
                EXPECT_EQ(listed->at("sessions").size(), 1);
                EXPECT_EQ(listed->at("sessions")[0].at("title"), "Workspace greeting");
                const auto agentsBeforeAttach = constructed;
                co_await client.send({{"type", "session.attach"}, {"sessionId", id}});
                const auto attached = co_await receiveReply(client);
                EXPECT_EQ(attached->at("type"), "session.attached");
                EXPECT_EQ(attached->at("messages").size(), 3);
                EXPECT_EQ(attached->at("session").at("title"), "Workspace greeting");
                EXPECT_EQ(attached->at("info").at("last_turn_used_context"), 120);
                EXPECT_EQ(attached->at("info").at("total_context"), 4096);
                EXPECT_EQ(titleState->inputs.size(), 1); // Loading never regenerates a title.
                EXPECT_EQ(constructed, agentsBeforeAttach); // Nor does it instantiate a discussion agent.
                co_await client.send({{"type", "session.preview"}, {"sessionId", id}});
                const auto preview = co_await client.receive();
                EXPECT_EQ(preview->at("type"), "session.previewed");
                EXPECT_EQ(preview->at("text"), "Hello");
                co_await client.send({{"type", "session.attach"}, {"sessionId", id}});
                EXPECT_EQ((co_await receiveReply(client))->at("type"), "session.attached");
                co_await client.send({{"type", "session.attach"}, {"sessionId", "invalid"}});
                EXPECT_EQ((co_await client.receive())->at("type"), "request.failed");
                auto otherInfo = info();
                otherInfo.title = "Another session";
                auto other = registry.create(otherInfo);
                const auto otherId = other->getInfo().id;
                co_await client.send({{"type", "session.attach"}, {"sessionId", otherId}});
                EXPECT_EQ((co_await client.receive())->at("type"), "request.failed"); // Still owned elsewhere.
                other.reset();
                co_await client.send({{"type", "session.attach"}, {"sessionId", otherId}});
                const auto switched = co_await receiveReply(client);
                EXPECT_EQ(switched->at("session").at("title"), "Another session");
                co_await client.send({{"type", "session.attach"}, {"sessionId", id}});
                EXPECT_EQ((co_await receiveReply(client))->at("session").at("id"), id);
                co_await client.send({{"type", "message.send"}, {"text", "Follow-up"}});
                EXPECT_EQ((co_await client.receive())->at("type"), "execution.preparing");
                const auto accepted = co_await client.receive();
                EXPECT_EQ(accepted->at("type"), "message.accepted");
                EXPECT_EQ(accepted->at("session").at("title"), "Workspace greeting");
                for (;;) {
                    const auto event = co_await client.receive();
                    EXPECT_EQ(event->at("info").at("last_turn_used_context"), 120);
                    if (event->at("type") == "execution.completed") break;
                }
                EXPECT_EQ(titleState->inputs.size(), 1);
                const auto savedCount = registry.list().size();
                const auto agents = constructed;
                co_await client.send({{"type", "session.new"}, {"requestId", "new"}});
                const auto reset = co_await client.receive();
                EXPECT_EQ(reset->at("type"), "session.reset");
                EXPECT_EQ(reset->at("requestId"), "new");
                EXPECT_EQ(reset->at("info").at("reasoning"), daemon_config::DEFAULT_REASONING_EFFORT);
                EXPECT_TRUE(reset->at("info").at("last_turn_used_context").is_null());
                EXPECT_EQ(registry.list().size(), savedCount);
                EXPECT_EQ(constructed, agents);
                EXPECT_EQ(registry.lastUserMessage(id), "Follow-up");
                co_await client.send({{"type", "session.new"}}); // Also valid without an attached session.
                EXPECT_EQ((co_await client.receive())->at("type"), "session.reset");
                co_await client.send({{"type", "message.send"}, {"text", "Fresh start"}, {"workspace", directory.string()}});
                const auto fresh = co_await client.receive();
                EXPECT_EQ(fresh->at("type"), "session.created");
                EXPECT_NE(fresh->at("session").at("id"), id);
                while ((co_await client.receive())->at("type") != "execution.completed") {}
                EXPECT_EQ(registry.list().size(), savedCount + 1);
                EXPECT_EQ(titleState->inputs.size(), 2);
            }
            client.close();
        }, asio::use_future);
        io.run_for(std::chrono::seconds(3));
        if (integration) integration->close();
        client.close();
        io.restart();
        io.run_for(std::chrono::seconds(1));
        ASSERT_EQ(clientRun.wait_for(std::chrono::seconds(0)), std::future_status::ready);
        ASSERT_EQ(run.wait_for(std::chrono::seconds(0)), std::future_status::ready);
        clientRun.get();
        run.get();
        integration.reset();
        io.restart();
    };
    exchange(false);
    exchange(true);
}

TEST_F(SessionTest, GeneratesIsolatedShortTitleAndRejectsEmptyResponses) {
    TestProvider titleProvider(provider);
    provider->responseText = " \"Create\n a pleasant and fast discussion interface\" \n";
    EXPECT_EQ(generateSessionTitle(titleProvider, "title-model", "Améliore cette interface"),
        "Create a pleasant and fast discussion");
    EXPECT_EQ(provider->model, "title-model");
    EXPECT_EQ(provider->toolCount, 0);
    ASSERT_EQ(provider->inputs.size(), 1);
    ASSERT_EQ(provider->inputs[0].size(), 2);
    EXPECT_EQ(provider->inputs[0][1]["content"], "Améliore cette interface");
    EXPECT_NE(provider->inputs[0][0].at("content").get<std::string>().find(
        "Write in English, even when the user writes in another language"), std::string::npos);
    provider->responseText = "Discussion";
    EXPECT_EQ(generateSessionTitle(titleProvider, "title-model", "Hello"), "Discussion");
    provider->responseText = " \n\t";
    EXPECT_THROW(generateSessionTitle(titleProvider, "title-model", "Hello"), std::runtime_error);
    provider->fail = true;
    EXPECT_THROW(generateSessionTitle(titleProvider, "title-model", "Hello"), std::runtime_error);
}

TEST_F(SessionTest, TitleFailureCreatesNoSessionAndCanBeRetried) {
    asio::io_context io;
    SessionRegistry registry(directory / "sessions", factory());
    ServerSocket server(io, directory / "server.sock");
    server.listen();
    asio::local::stream_protocol::socket socket(io);
    socket.connect(asio::local::stream_protocol::endpoint((directory / "server.sock").string()));
    ConnectionSocket client(std::move(socket));
    int titleCalls = 0;
    std::shared_ptr<Integration> integration;
    auto run = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        integration = std::make_shared<Integration>(io, co_await server.accept(), registry,
            [&](const std::string&, const std::string&, core::CancellationCheck) {
                if (++titleCalls == 1) throw std::runtime_error("Title provider failed");
                return "Short title";
            });
        co_await integration->run();
    }, asio::use_future);
    auto interaction = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        co_await client.receive();
        co_await client.send({{"type", "message.send"}, {"requestId", "first"},
            {"text", "Hello"}, {"workspace", directory.string()}});
        const auto failed = co_await client.receive();
        EXPECT_EQ(failed->at("type"), "request.failed");
        EXPECT_EQ(failed->at("requestId"), "first");
        EXPECT_EQ(failed->at("error"), "Title provider failed");
        EXPECT_TRUE(registry.list().empty());
        EXPECT_EQ(constructed, 0);
        co_await client.send({{"type", "message.send"}, {"text", "Hello"}, {"workspace", directory.string()}});
        const auto created = co_await client.receive();
        EXPECT_EQ(created->at("type"), "session.created");
        EXPECT_EQ(created->at("session").at("title"), "Short title");
        while ((co_await client.receive())->at("type") != "execution.completed") {}
        client.close();
    }, asio::use_future);
    io.run_for(std::chrono::seconds(3));
    if (integration) integration->close();
    client.close();
    io.restart();
    io.run_for(std::chrono::seconds(1));
    ASSERT_EQ(run.wait_for(std::chrono::seconds(0)), std::future_status::ready);
    ASSERT_EQ(interaction.wait_for(std::chrono::seconds(0)), std::future_status::ready);
    run.get();
    interaction.get();
    EXPECT_EQ(titleCalls, 2);
}

TEST_F(SessionTest, PendingTitleDoesNotBlockAsioOrCreateSessionAfterDisconnect) {
    asio::io_context io;
    SessionRegistry registry(directory / "sessions", factory());
    ServerSocket server(io, directory / "server.sock");
    server.listen();
    asio::local::stream_protocol::socket socket(io);
    socket.connect(asio::local::stream_protocol::endpoint((directory / "server.sock").string()));
    ConnectionSocket client(std::move(socket));
    std::promise<void> release, entered;
    auto released = release.get_future().share();
    auto started = entered.get_future();
    std::shared_ptr<Integration> integration;
    auto run = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        integration = std::make_shared<Integration>(io, co_await server.accept(), registry,
            [&](const std::string&, const std::string&, core::CancellationCheck) {
                entered.set_value();
                released.wait();
                return "Delayed title";
            });
        co_await integration->run();
    }, asio::use_future);
    auto interaction = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        co_await client.receive();
        co_await client.send({{"type", "message.send"}, {"text", "Hello"}, {"workspace", directory.string()}});
    }, asio::use_future);
    io.run_for(std::chrono::milliseconds(50));
    EXPECT_EQ(started.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_TRUE(registry.list().empty());
    EXPECT_EQ(constructed, 0);
    client.close();
    io.restart();
    io.run_for(std::chrono::milliseconds(20));
    EXPECT_EQ(run.wait_for(std::chrono::seconds(0)), std::future_status::timeout);
    release.set_value();
    io.restart();
    io.run_for(std::chrono::seconds(2));
    ASSERT_EQ(run.wait_for(std::chrono::seconds(0)), std::future_status::ready);
    run.get();
    interaction.get();
    EXPECT_TRUE(registry.list().empty());
    EXPECT_EQ(constructed, 0);
}

TEST_F(SessionTest, ModelChangeDuringTurnAppliesOnlyAfterItsLastGeneration) {
    SessionRegistry registry(directory / "sessions", factory());
    auto session = registry.create(info());
    const auto id = session->getInfo().id;
    const auto oldModel = session->getInfo().model;
    auto entered = provider->entered.get_future();
    std::promise<void> release;
    provider->release = release.get_future().share();
    provider->respond = [](const json&, std::size_t count) {
        if (count > 1) return reply("done_" + std::to_string(count), "Done");
        return json{{"id", "tools"}, {"status", "completed"}, {"output", json::array({{
            {"type", "function_call"}, {"id", "tool"}, {"call_id", "call"}, {"name", "unknown"}, {"arguments", "{}"}}})}};
    };
    auto replacement = std::make_shared<ProviderState>();
    asio::io_context io;
    auto queue = std::make_shared<AsyncQueue<AgentEvent>>(io.get_executor());
    session->setEventHandler([queue, executor = io.get_executor()](AgentEvent event) {
        asio::post(executor, [queue, event = std::move(event)]() mutable { queue->push(std::move(event)); });
    });
    auto run = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        session->sendMessage("One user turn", ExecutionMode::SANE);
        co_await awaitFuture(entered);
        session->applySelection({"openai", "new-model", "low"}, std::make_unique<TestProvider>(replacement));
        EXPECT_EQ(session->executionGeneration().model, oldModel);
        release.set_value();
        for (;;) {
            const auto event = co_await queue->receive();
            session->record(event);
            if (event.isTerminal()) {
                EXPECT_EQ(event.type, AgentEventType::EXECUTION_COMPLETED) << event.error;
                co_await session->finishExecution();
                break;
            }
        }
    }, asio::use_future);
    io.run();
    run.get();
    EXPECT_EQ(provider->inputs.size(), 2);
    EXPECT_EQ(replacement->inputs.size(), 0);
    EXPECT_EQ(provider->reasoning, session->getExecutionReasoning());
    std::vector<AgentEventType> events;
    turn(*session, "Next turn", events);
    EXPECT_EQ(replacement->inputs.size(), 1);
    session.reset();
    auto restored = registry.load(id);
    EXPECT_EQ(restored->getInfo().model, "new-model");
    EXPECT_EQ(restored->getInfo().provider, "openai");
    EXPECT_EQ(restored->getInfo().reasoning, "low");
    EXPECT_EQ(restored->getInfo().providerContextStart, restored->messageCount() - 2);
    std::vector<std::string> models;
    for (const auto& message : restored->getMessages()) if (message.modelCall) models.push_back(message.modelCall->model);
    EXPECT_EQ(models, (std::vector<std::string>{oldModel, oldModel, "new-model"}));
    EXPECT_EQ(restored->getMessages().back().metadata->model, "new-model");
}

TEST_F(SessionTest, ReasoningChangeDuringTurnWaitsForNextUserMessage) {
    SessionRegistry registry(directory / "sessions", factory());
    auto session = registry.create(info());
    const auto original = session->getInfo().reasoning;
    auto entered = provider->entered.get_future();
    std::promise<void> release;
    provider->release = release.get_future().share();
    provider->respond = [](const json&, std::size_t count) {
        if (count > 1) return reply("done_" + std::to_string(count), "Done");
        return json{{"id", "tools"}, {"status", "completed"}, {"output", json::array({{
            {"type", "function_call"}, {"id", "tool"}, {"call_id", "call"}, {"name", "unknown"}, {"arguments", "{}"}}})}};
    };
    asio::io_context io;
    auto queue = std::make_shared<AsyncQueue<AgentEvent>>(io.get_executor());
    session->setEventHandler([queue, executor = io.get_executor()](AgentEvent event) {
        asio::post(executor, [queue, event = std::move(event)]() mutable { queue->push(std::move(event)); });
    });
    auto run = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        session->sendMessage("First turn", ExecutionMode::SANE);
        co_await awaitFuture(entered);
        session->setReasoningEffort("high");
        EXPECT_EQ(session->getInfo().reasoning, original);
        release.set_value();
        for (;;) {
            auto event = co_await queue->receive();
            session->record(event);
            if (event.isTerminal()) {
                EXPECT_EQ(event.type, AgentEventType::EXECUTION_COMPLETED) << event.error;
                co_await session->finishExecution();
                break;
            }
        }
    }, asio::use_future);
    io.run();
    run.get();
    EXPECT_EQ(provider->inputs.size(), 2);
    EXPECT_EQ(provider->reasoning, original);
    EXPECT_EQ(session->getInfo().reasoning, "high");
    std::vector<AgentEventType> events;
    turn(*session, "Second turn", events);
    EXPECT_EQ(provider->reasoning, "high");
}

TEST_F(SessionTest, ModeSwitchDoesNotRestrictToolsFromAnActiveSlopTurn) {
    auto entered = provider->entered.get_future();
    std::promise<void> release;
    provider->release = release.get_future().share();
    provider->respond = [](const json&, std::size_t count) {
        if (count > 1) return reply("done_" + std::to_string(count), "Done");
        return json{{"id", "tools"}, {"status", "completed"}, {"output", json::array({{
            {"type", "function_call"}, {"id", "tool"}, {"call_id", "call"}, {"name", "write"}, {"arguments", "{}"}}})}};
    };
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await receiveEvent(client);
        co_await client.send({{"type", "integration.configure"}, {"workspace", directory.string()}, {"mode", "slop"}});
        co_await receiveEvent(client);
        co_await client.send({{"type", "message.send"}, {"text", "First"}, {"workspace", directory.string()}, {"mode", "slop"}});
        const auto accepted = co_await receiveEvent(client);
        EXPECT_EQ(accepted.at("session").at("mode"), "slop");
        co_await awaitFuture(entered);
        co_await client.send({{"type", "mode.set"}, {"mode", "sane"}});
        json switched;
        do { switched = co_await receiveEvent(client); } while (switched.at("type") != "mode.updated");
        EXPECT_EQ(switched.at("mode"), "sane");
        release.set_value();
        bool checked = false;
        for (;;) {
            const auto event = co_await receiveEvent(client);
            if (event.at("type") == "harness.message_arrived") {
                for (const auto& message : event.at("messages")) {
                    if (message.at("role") != "tool") continue;
                    checked = true;
                    EXPECT_EQ(message.at("content")[0].at("isError"), true);
                    EXPECT_EQ(message.at("content")[0].at("output").get<std::string>().find("Forbidden in Sane"), std::string::npos);
                }
            }
            if (event.at("type") == "execution.completed") break;
        }
        EXPECT_TRUE(checked);
        co_await client.send({{"type", "message.send"}, {"text", "Second"}, {"workspace", directory.string()}, {"mode", "sane"}});
        json next;
        do { next = co_await receiveEvent(client); } while (next.at("type") != "message.accepted");
        EXPECT_EQ(next.at("session").at("mode"), "sane");
        while ((co_await receiveEvent(client)).at("type") != "execution.completed") {}
        co_await client.send({{"type", "integration.close"}});
    }, {}, {}, std::chrono::seconds(8), std::chrono::seconds(2));
}

TEST_F(SessionTest, NewAndAttachDrainTheCancelledPrincipalBeforeSwitching) {
    for (const auto type : {"session.new", "session.attach"}) {
        provider = std::make_shared<ProviderState>();
        provider->waitForCancellation = true;
        auto entered = provider->entered.get_future();
        std::string target, original;
        {
            SessionRegistry registry(directory / "sessions", factory());
            target = registry.create(info())->getInfo().id;
        }
        healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
            co_await receiveEvent(client);
            co_await client.send({{"type", "integration.configure"}, {"workspace", directory.string()}, {"mode", "sane"}});
            co_await receiveEvent(client);
            co_await client.send({{"type", "message.send"}, {"text", "Interrupt this"}, {"workspace", directory.string()}});
            original = (co_await receiveEvent(client)).at("session").at("id");
            co_await awaitFuture(entered);
            co_await client.send({{"type", type}, {"sessionId", target}, {"requestId", "switch"}});
            bool cancelled = false;
            for (;;) {
                const auto event = co_await receiveEvent(client);
                if (event.at("type") == "execution.cancelled") {
                    cancelled = true;
                    EXPECT_EQ(event.at("sessionId"), original);
                }
                if (event.value("requestId", "") == "switch") {
                    EXPECT_TRUE(cancelled);
                    EXPECT_EQ(event.at("type"), std::string(type) == "session.new" ? "session.reset" : "session.attached");
                    if (std::string(type) == "session.attach") EXPECT_EQ(event.at("sessionId"), target);
                    break;
                }
            }
            co_await client.send({{"type", "integration.close"}});
        }, {}, {}, std::chrono::seconds(5), std::chrono::milliseconds(500));
        SessionJournal journal(directory / "sessions" / original);
        EXPECT_NE(json(journal.readAll()).dump().find("previous run was interrupted"), std::string::npos);
    }
}

TEST_F(SessionTest, SettingsAndProviderConnectionAreAvailableWhilePrincipalRuns) {
    provider->waitForCancellation = true;
    auto entered = provider->entered.get_future();
    auto auth = std::make_shared<AuthStore>(directory / "auth.json");
    auth->set("openai", ProviderCredential{.key = "fake-key"});
    auto preferences = std::make_shared<ProviderConnections>(auth, directory / "config.json");
    preferences->select({"openai", "gpt-5.4", "high"});
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        co_await receiveEvent(client);
        co_await client.send({{"type", "message.send"}, {"text", "Work"}, {"workspace", directory.string()}});
        co_await receiveEvent(client);
        co_await awaitFuture(entered);
        for (const auto& request : std::vector<json>{
            {{"type", "permissions.select"}, {"mode", "danger"}, {"requestId", "permissions"}},
            {{"type", "model.select"}, {"provider", "openai"}, {"model", "gpt-5.2"}, {"requestId", "model"}},
            {{"type", "provider.list"}, {"requestId", "providers"}}}) {
            co_await client.send(request);
            for (;;) {
                const auto event = co_await receiveEvent(client);
                if (event.value("requestId", "") != request.at("requestId").get<std::string>()) continue;
                EXPECT_NE(event.at("type"), "request.failed");
                EXPECT_EQ(event.at("info").at("execution_model"), "gpt-5.4");
                EXPECT_EQ(event.at("info").at("execution_reasoning"), "high");
                if (request.at("type") == "model.select") EXPECT_EQ(event.at("info").at("model"), "gpt-5.2");
                break;
            }
        }
        co_await client.send({{"type", "provider.select"}, {"provider", "openai"}, {"model", "gpt-5.4"}, {"requestId", "connect"}});
        bool cancelled = false;
        for (;;) {
            const auto event = co_await receiveEvent(client);
            if (event.at("type") == "execution.cancelled") cancelled = true;
            if (event.value("requestId", "") == "connect") {
                EXPECT_TRUE(cancelled);
                EXPECT_EQ(event.at("type"), "provider.selected");
                EXPECT_TRUE(event.at("info").at("execution_model").is_null());
                break;
            }
        }
        co_await client.send({{"type", "integration.close"}});
    }, {}, {}, std::chrono::seconds(5), std::chrono::milliseconds(500), preferences);
    EXPECT_EQ(preferences->permissionMode(), PermissionMode::DANGER);
}

TEST_F(SessionTest, ConfidenceSelectionAcknowledgesAndPersistsTheActualThreshold) {
    auto auth = std::make_shared<AuthStore>(directory / "auth.json");
    auto preferences = std::make_shared<ProviderConnections>(auth, directory / "config.json");
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        EXPECT_EQ((co_await receiveEvent(client)).at("info").at("permission_confidence"), 80);
        co_await client.send({{"type", "permissions.select"}, {"mode", "auto"}, {"confidence", 0}});
        const auto selected = co_await receiveEvent(client);
        EXPECT_EQ(selected.at("type"), "permissions.selected");
        EXPECT_EQ(selected.at("confidence"), 0);
        EXPECT_EQ(selected.at("info").at("permission_confidence"), 0);
        EXPECT_FALSE(selected.at("info").at("has_permission_key"));
        co_await client.send({{"type", "permissions.select"}, {"mode", "auto"}, {"confidence", 100}});
        const auto failed = co_await receiveEvent(client);
        EXPECT_EQ(failed.at("type"), "request.failed");
        EXPECT_EQ(failed.at("info").at("permission_confidence"), 0);
        co_await client.send({{"type", "permissions.get"}});
        EXPECT_EQ((co_await receiveEvent(client)).at("confidence"), 0);
        co_await client.send({{"type", "integration.close"}});
    }, {}, {}, std::chrono::seconds(5), std::chrono::milliseconds(500), preferences);
    preferences->savePermissionKey("saved-verifier-key");
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        EXPECT_EQ((co_await receiveEvent(client)).at("info").at("permission_confidence"), 0);
        co_await client.send({{"type", "permissions.select"}, {"mode", "auto"}, {"confidence", 95}});
        const auto selected = co_await receiveEvent(client);
        EXPECT_EQ(selected.at("type"), "permissions.selected");
        EXPECT_EQ(selected.at("confidence"), 95);
        EXPECT_EQ(selected.at("info").at("permission_confidence"), 95);
        co_await client.send({{"type", "session.new"}});
        EXPECT_EQ((co_await receiveEvent(client)).at("info").at("permission_confidence"), 95);
        co_await client.send({{"type", "integration.close"}});
    }, {}, {}, std::chrono::seconds(5), std::chrono::milliseconds(500), preferences);
    ProviderConnections restored(auth, directory / "config.json");
    EXPECT_EQ(restored.permissionConfidence(), 95);
}

TEST_F(SessionTest, AICheckModelSelectionIsVersionedPersistentAndNeverReturnsTheKey) {
    auto auth = std::make_shared<AuthStore>(directory / "auth.json");
    auto preferences = std::make_shared<ProviderConnections>(auth, directory / "config.json");
    const json model{{"protocol", "openai_decisions"}, {"endpoint", "https://api.openai.com/v1/decisions"},
        {"model", "custom-decision-model"}};
    healthExchange([&](ConnectionSocket& client) -> asio::awaitable<void> {
        const auto ready = co_await receiveEvent(client);
        EXPECT_EQ(ready.at("info").at("permission_check_version"), 1);
        co_await client.send({{"type", "permissions.select"}, {"mode", "auto"}, {"confidence", 91},
            {"check_version", 1}, {"model_config", model}, {"key", "test-decision-secret"}});
        const auto selected = co_await receiveEvent(client);
        EXPECT_EQ(selected.at("type"), "permissions.selected");
        EXPECT_EQ(selected.at("model_config").at("model"), "custom-decision-model");
        EXPECT_TRUE(selected.at("model_config").at("has_key"));
        EXPECT_EQ(selected.at("info").at("permission_model").at("endpoint"), model.at("endpoint"));
        EXPECT_EQ(selected.dump().find("test-decision-secret"), std::string::npos);
        co_await client.send({{"type", "permissions.get"}});
        const auto current = co_await receiveEvent(client);
        EXPECT_EQ(current.at("confidence"), 91);
        EXPECT_EQ(current.at("check_version"), 1);
        EXPECT_EQ(current.dump().find("test-decision-secret"), std::string::npos);
        // Omitting the version must not silently apply only the threshold.
        co_await client.send({{"type", "permissions.select"}, {"mode", "auto"}, {"confidence", 20}, {"model_config", model}});
        EXPECT_EQ((co_await receiveEvent(client)).at("type"), "request.failed");
        auto changed = model;
        changed["endpoint"] = "https://other.example/v1/decisions";
        co_await client.send({{"type", "permissions.select"}, {"mode", "auto"}, {"confidence", 30},
            {"check_version", 1}, {"model_config", changed}});
        const auto rejected = co_await receiveEvent(client);
        EXPECT_EQ(rejected.at("type"), "request.failed");
        EXPECT_EQ(rejected.dump().find("test-decision-secret"), std::string::npos);
        EXPECT_EQ(preferences->permissionConfidence(), 91);
        EXPECT_EQ(preferences->permissionModel().endpoint, model.at("endpoint").get<std::string>());
        co_await client.send({{"type", "integration.close"}});
    }, {}, {}, std::chrono::seconds(5), std::chrono::milliseconds(500), preferences);
    ProviderConnections restored(auth, directory / "config.json");
    EXPECT_EQ(restored.permissionMode(), PermissionMode::AUTO);
    EXPECT_EQ(restored.permissionConfidence(), 91);
    EXPECT_EQ(restored.permissionModel().model, "custom-decision-model");
    EXPECT_TRUE(restored.hasPermissionKey());
}

TEST_F(SessionTest, CancelRequestInterruptsProviderAndNextMessageResumesSameSession) {
    provider->waitForCancellation = true;
    auto entered = provider->entered.get_future();
    asio::io_context io;
    SessionRegistry registry(directory / "sessions", factory());
    ServerSocket server(io, directory / "cancel.sock");
    server.listen();
    asio::local::stream_protocol::socket socket(io);
    socket.connect(asio::local::stream_protocol::endpoint((directory / "cancel.sock").string()));
    ConnectionSocket client(std::move(socket));
    std::shared_ptr<Integration> integration;
    auto run = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        integration = std::make_shared<Integration>(io, co_await server.accept(), registry,
            [](const std::string&, const std::string&, core::CancellationCheck) { return "Cancellation test"; });
        co_await integration->run();
    }, asio::use_future);
    auto interaction = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        co_await client.receive();
        co_await client.send({{"type", "message.send"}, {"text", "First"}, {"workspace", directory.string()}});
        const auto created = co_await client.receive();
        const auto id = created->at("session").at("id");
        asio::steady_timer timer(io);
        while (entered.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            timer.expires_after(std::chrono::milliseconds(5));
            co_await timer.async_wait(asio::use_awaitable);
        }
        const auto journal = directory / "sessions" / id.get<std::string>() / "session.sqlite";
        const auto journalSize = SessionJournal(journal.parent_path()).sizeBytes();
        co_await client.send({{"type", "mode.set"}, {"mode", "slop"}});
        for (;;) {
            const auto event = co_await client.receive();
            if (event->at("type") == "mode.updated") {
                EXPECT_EQ(event->at("mode"), "slop");
                break;
            }
        }
        EXPECT_EQ(registry.list()[0].mode, ExecutionMode::SANE);
        EXPECT_EQ(SessionJournal(journal.parent_path()).sizeBytes(), journalSize); // No mode message injected into the active turn.
        co_await client.send({{"type", "execution.cancel"}, {"requestId", "stop"}});
        bool terminal = false, acknowledged = false, interruptionSaved = false;
        while (!terminal || !acknowledged) {
            const auto event = co_await client.receive();
            if (event->at("type") == "execution.cancel_requested") acknowledged = true;
            if (event->at("type") == "harness.message_arrived" &&
                event->at("messages")[0].at("role") == "system") interruptionSaved = true;
            if (event->at("type") == "execution.cancelled") terminal = true;
        }
        EXPECT_TRUE(interruptionSaved);
        EXPECT_EQ(registry.list().size(), 1);
        provider->waitForCancellation = false;
        co_await client.send({{"type", "message.send"}, {"text", "Continue"}});
        const auto accepted = co_await client.receive();
        EXPECT_EQ(accepted->at("type"), "message.accepted");
        EXPECT_EQ(accepted->at("session").at("id"), id);
        EXPECT_EQ(accepted->at("session").at("mode"), "slop");
        while ((co_await client.receive())->at("type") != "execution.completed") {}
        EXPECT_EQ(provider->inputs.size(), 2);
        EXPECT_NE(provider->inputs.back().dump().find("previous run was interrupted"), std::string::npos);
        const auto& resumedInput = provider->inputs.back();
        EXPECT_EQ(resumedInput[resumedInput.size() - 2]["role"], "system");
        EXPECT_NE(resumedInput[resumedInput.size() - 2]["content"].get<std::string>().find("Current execution mode: slop."), std::string::npos);
        co_await client.send({{"type", "execution.cancel"}, {"requestId", "idle"}});
        const auto idle = co_await client.receive();
        EXPECT_EQ(idle->at("type"), "execution.cancel_ignored");
        client.close();
    }, asio::use_future);
    io.run_for(std::chrono::seconds(5));
    if (integration) integration->close();
    client.close();
    io.restart();
    io.run_for(std::chrono::seconds(2));
    ASSERT_EQ(interaction.wait_for(std::chrono::seconds(0)), std::future_status::ready);
    interaction.get();
    run.get();
}

TEST_F(SessionTest, CancelDuringTitleCreatesNoSessionAndAllowsRetry) {
    asio::io_context io;
    SessionRegistry registry(directory / "sessions", factory());
    ServerSocket server(io, directory / "cancel-title.sock");
    server.listen();
    asio::local::stream_protocol::socket socket(io);
    socket.connect(asio::local::stream_protocol::endpoint((directory / "cancel-title.sock").string()));
    ConnectionSocket client(std::move(socket));
    std::promise<void> entered;
    auto started = entered.get_future();
    int titles = 0;
    std::shared_ptr<Integration> integration;
    auto run = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        integration = std::make_shared<Integration>(io, co_await server.accept(), registry,
            [&](const std::string&, const std::string&, core::CancellationCheck cancelled) {
                if (++titles == 1) {
                    entered.set_value();
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                    while (!cancelled() && std::chrono::steady_clock::now() < deadline)
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    core::checkCancellation(cancelled);
                }
                return "New title";
            });
        co_await integration->run();
    }, asio::use_future);
    auto interaction = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        co_await client.receive();
        co_await client.send({{"type", "message.send"}, {"text", "First"}, {"workspace", directory.string()}});
        asio::steady_timer timer(io);
        while (started.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            timer.expires_after(std::chrono::milliseconds(5));
            co_await timer.async_wait(asio::use_awaitable);
        }
        co_await client.send({{"type", "execution.cancel"}, {"requestId", "stop"}});
        bool terminal = false, acknowledged = false;
        while (!terminal || !acknowledged) {
            const auto event = co_await client.receive();
            if (event->at("type") == "execution.cancel_requested") acknowledged = true;
            if (event->at("type") == "execution.cancelled") terminal = true;
        }
        EXPECT_TRUE(registry.list().empty());
        EXPECT_EQ(constructed, 0);
        co_await client.send({{"type", "message.send"}, {"text", "Retry"}, {"workspace", directory.string()}});
        EXPECT_EQ((co_await client.receive())->at("type"), "session.created");
        while ((co_await client.receive())->at("type") != "execution.completed") {}
        EXPECT_EQ(titles, 2);
        EXPECT_EQ(constructed, 1);
        client.close();
    }, asio::use_future);
    io.run_for(std::chrono::seconds(5));
    if (integration) integration->close();
    client.close();
    io.restart();
    io.run_for(std::chrono::seconds(2));
    ASSERT_EQ(interaction.wait_for(std::chrono::seconds(0)), std::future_status::ready);
    interaction.get();
    run.get();
}
}
