#include <daemon/permissions/permission_manager.hpp>
#include <agent-runtime/agent.hpp>
#include <agent-runtime/core/cancellation.hpp>
#include <agent-runtime/conversation/messages/message_json.hpp>
#include <agent-runtime/tools/fs/write.hpp>
#include <agent-runtime/tools/fs/read.hpp>
#include <agent-runtime/tools/fs/common.hpp>
#include <agent-runtime/tools/fs/glob.hpp>
#include <agent-runtime/tools/fs/grep.hpp>
#include <agent-runtime/core/ripgrep.hpp>
#include <agent-runtime/tools/common/bash.hpp>
#include <daemon/integration/integration.hpp>
#include <daemon/transport/server_socket.hpp>
#include <asio/co_spawn.hpp>
#include <asio/use_future.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <fstream>
#include <future>
#include <algorithm>
#include <cstdlib>

namespace {
using nlohmann::json;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

class PermissionTest : public testing::Test {
protected:
    fs::path directory;
    std::mutex mutex;
    std::condition_variable received;
    std::vector<PermissionEvent> events;
    std::unique_ptr<PermissionManager> policy;
    void SetUp() override {
        std::string pattern = "/tmp/sos-permission-XXXXXX";
        ASSERT_NE(mkdtemp(pattern.data()), nullptr);
        directory = fs::canonical(pattern);
        fs::create_directory(directory / "workspace");
        fs::create_directory(directory / "outside");
        policy = std::make_unique<PermissionManager>([this](PermissionEvent event) {
            std::lock_guard lock(mutex);
            events.push_back(std::move(event));
            received.notify_all();
        });
        policy->configure(directory / "workspace", ExecutionMode::SLOP);
    }
    void TearDown() override { policy->close(); std::error_code error; fs::remove_all(directory, error); }
    PermissionEvent nextRequest(std::size_t after = 0) {
        std::unique_lock lock(mutex);
        if (!received.wait_for(lock, 2s, [&] {
            return std::any_of(events.begin() + static_cast<std::ptrdiff_t>(after), events.end(),
                [](const auto& event) { return event.type == PermissionEventType::REQUESTED; });
        })) {
            lock.unlock();
            policy->close();
            throw std::runtime_error("Permission request did not arrive");
        }
        return *std::find_if(events.begin() + static_cast<std::ptrdiff_t>(after), events.end(),
            [](const auto& event) { return event.type == PermissionEventType::REQUESTED; });
    }
    ToolPermissionRequest edit() {
        return {"edit", {"file.txt"}, {"*"}, {{"filepath", (directory / "workspace/file.txt").string()}}, "call-1", "write"};
    }
    PermissionAnswer withoutPrompt(const ToolPermissionRequest& request) {
        int checks = 0;
        return policy->ask(request, [&] {
            if (++checks > 1) throw std::runtime_error("Unexpected permission prompt");
        });
    }
};

TEST_F(PermissionTest, OnceAlwaysAndModePriorityAreEnforcedPerConnection) {
    const auto request = edit();
    auto first = std::async(std::launch::async, [&] { return policy->ask(request, [] {}); });
    const auto prompt = nextRequest();
    EXPECT_EQ(first.wait_for(0s), std::future_status::timeout);
    EXPECT_THROW(policy->reply("wrong", PermissionDecision::YES), std::invalid_argument);
    policy->reply(prompt.id, PermissionDecision::YES);
    EXPECT_EQ(first.get().decision, PermissionDecision::YES);
    const auto count = events.size();
    auto second = std::async(std::launch::async, [&] { return policy->ask(request, [] {}); });
    policy->reply(nextRequest(count).id, PermissionDecision::ALWAYS);
    EXPECT_EQ(second.get().decision, PermissionDecision::ALWAYS);
    for (const auto* tool : {"write", "edit", "apply_patch"}) {
        auto sibling = request;
        sibling.toolName = tool;
        sibling.patterns = {"nested/another.txt"};
        EXPECT_EQ(withoutPrompt(sibling).decision, PermissionDecision::ALWAYS);
    }
    EXPECT_THROW(withoutPrompt({"external_directory", {}, {}, {{"parentDir", (directory / "outside").string()}}}), std::runtime_error);
    policy->setMode(ExecutionMode::SANE);
    for (const auto* tool : {"write", "edit", "apply_patch"}) {
        auto sibling = request;
        sibling.toolName = tool;
        EXPECT_THROW(withoutPrompt(sibling), std::runtime_error);
    }
    auto bash = ToolPermissionRequest{"bash", {"python script.py"}, {"python script.py"}, {}, "bash-1", "bash"};
    EXPECT_THROW(policy->ask(bash, [] {}), std::runtime_error);
    bash.readOnly = true;
    EXPECT_EQ(policy->ask(bash, [] {}).decision, PermissionDecision::ALLOW);
    PermissionManager fresh([](PermissionEvent) {});
    fresh.configure(directory / "workspace", ExecutionMode::SLOP);
    auto waiting = std::async(std::launch::async, [&] { return fresh.ask(request, [] {}); });
    EXPECT_EQ(waiting.wait_for(30ms), std::future_status::timeout);
    fresh.close();
    EXPECT_THROW(waiting.get(), core::Cancelled);
}

TEST_F(PermissionTest, BashAlwaysKeepsCommandScopesAndDoesNotApproveEdits) {
    ToolPermissionRequest command{"bash", {"python script.py"}, {"python script.py"}, {}, "bash-1", "bash"};
    auto waiting = std::async(std::launch::async, [&] { return policy->ask(command, [] {}); });
    policy->reply(nextRequest().id, PermissionDecision::ALWAYS);
    EXPECT_EQ(waiting.get().decision, PermissionDecision::ALWAYS);
    EXPECT_EQ(withoutPrompt(command).decision, PermissionDecision::ALWAYS);
    command.patterns = command.always = {"python other.py"};
    EXPECT_THROW(withoutPrompt(command), std::runtime_error);
    EXPECT_THROW(withoutPrompt(edit()), std::runtime_error);
}

TEST_F(PermissionTest, WorktreeHasNoExtraApprovalInSaneButReviewRejectsSwitches) {
    ToolPermissionRequest request{"worktree", {}, {}, {}, "worktree-call", "worktree"};
    EXPECT_EQ(withoutPrompt(request).decision, PermissionDecision::ALLOW);
    EXPECT_TRUE(events.empty());
    policy->setMode(ExecutionMode::SANE);
    EXPECT_EQ(withoutPrompt(request).decision, PermissionDecision::ALLOW);
    EXPECT_TRUE(events.empty());
    request.readOnly = true;
    EXPECT_EQ(withoutPrompt(request).decision, PermissionDecision::ALLOW);
    policy->setMode(ExecutionMode::SLOP);
    policy->setImplementationPhase(ImplementationPhase::REVIEW);
    request.readOnly = false;
    EXPECT_THROW(withoutPrompt(request), std::runtime_error);
    policy->setMode(ExecutionMode::SANE);
    EXPECT_THROW(withoutPrompt(request), std::runtime_error);
    request.readOnly = true;
    EXPECT_EQ(withoutPrompt(request).decision, PermissionDecision::ALLOW);
}

TEST_F(PermissionTest, WorkspaceHasNoExtraApprovalInSaneOrSlopButReviewRejectsMoves) {
    ToolPermissionRequest request{"workspace", {}, {}, {}, "workspace-call", "workspace"};
    for (const auto mode : {ExecutionMode::SANE, ExecutionMode::SLOP}) {
        policy->setMode(mode);
        policy->setImplementationPhase(ImplementationPhase::PLANNING);
        EXPECT_EQ(withoutPrompt(request).decision, PermissionDecision::ALLOW);
        EXPECT_TRUE(events.empty());
        policy->setImplementationPhase(ImplementationPhase::REVIEW);
        EXPECT_THROW(withoutPrompt(request), std::runtime_error);
    }
}

TEST_F(PermissionTest, LaunchWorkspaceAndDescendantsAreReadableWithoutApprovalIncludingCanonicalAliases) {
    const auto workspace = directory / "workspace";
    fs::create_directories(workspace / "nested/deeper");
    fs_tools::writeText(workspace / "nested/deeper/file.txt", "Workspace contents\n");
    fs::create_directory_symlink(workspace, directory / "alias");
    const auto ripgrep = core::ensureRipgrep(directory / "bin", [](const std::string&) -> std::string {
        throw std::runtime_error("Tests require rg on PATH; network download is disabled");
    });
    for (const auto mode : {ExecutionMode::SANE, ExecutionMode::SLOP}) {
        policy->setMode(mode);
        ToolContext context{directory / "alias", directory / "alias"};
        context.permissionRoot = policy->root();
        context.ask = [&](const ToolPermissionRequest& request) { withoutPrompt(request); };
        EXPECT_NO_THROW(ReadTool{}.execute({{"filePath", "."}}, context));
        EXPECT_NE(ReadTool{}.execute({{"filePath", "nested/deeper/file.txt"}}, context).output.find("Workspace contents"), std::string::npos);
        EXPECT_NE(GlobTool{ripgrep}.execute({{"pattern", "**/*.txt"}}, context).output.find("file.txt"), std::string::npos);
        EXPECT_NE(GrepTool{ripgrep}.execute({{"pattern", "Workspace"}}, context).output.find("file.txt"), std::string::npos);
        EXPECT_NE(BashTool{}.execute({{"command", "cat nested/deeper/file.txt"}, {"mainCommand", "cat"}}, context).output.find("Workspace contents"), std::string::npos);
        EXPECT_EQ(withoutPrompt({"external_directory", {}, {}, {{"parentDir", (workspace / "nested/deeper").string()}}}).decision,
            PermissionDecision::ALLOW);
    }
    EXPECT_TRUE(events.empty());
}

TEST_F(PermissionTest, ExternalDirectoryApprovalIsIndependentOfModeAndWriteApproval) {
    policy->setMode(ExecutionMode::SANE);
    ToolContext context{directory / "outside", directory / "outside"};
    context.permissionRoot = directory / "workspace";
    context.ask = [&](ToolPermissionRequest request) { request.toolName = "read"; policy->ask(request, [] {}); };
    { std::ofstream file(directory / "outside/file.txt"); file << "Outside contents"; }
    auto reading = std::async(std::launch::async, [&] { return ReadTool{}.execute({{"filePath", "file.txt"}}, context); });
    const auto prompt = nextRequest();
    EXPECT_EQ(prompt.request.permission, "external_directory");
    policy->reply(prompt.id, PermissionDecision::ALWAYS);
    EXPECT_NE(reading.get().output.find("Outside contents"), std::string::npos);
    EXPECT_NO_THROW(ReadTool{}.execute({{"filePath", "file.txt"}}, context));
    fs::create_directories(directory / "outside/nested/deeper");
    fs_tools::writeText(directory / "outside/nested/deeper/file.txt", "Nested contents");
    const auto countBeforeChildren = events.size();
    context.ask = [&](ToolPermissionRequest request) { request.toolName = "read"; withoutPrompt(request); };
    EXPECT_NE(ReadTool{}.execute({{"filePath", "nested/deeper/file.txt"}}, context).output.find("Nested contents"), std::string::npos);
    for (const auto* tool : {"read", "glob", "grep", "bash"}) {
        ToolPermissionRequest external{"external_directory", {}, {}, {{"parentDir", (directory / "outside/nested/deeper").string()}}};
        external.toolName = tool;
        external.readOnly = true;
        EXPECT_EQ(withoutPrompt(external).decision, PermissionDecision::ALWAYS);
    }
    EXPECT_EQ(events.size(), countBeforeChildren);
    // Neither a parent nor a sibling with a shared name prefix is implicitly granted.
    EXPECT_THROW(withoutPrompt({"external_directory", {}, {}, {{"parentDir", directory.string()}}}), std::runtime_error);
    fs::create_directory(directory / "outside-other");
    EXPECT_THROW(withoutPrompt({"external_directory", {}, {}, {{"parentDir", (directory / "outside-other").string()}}}), std::runtime_error);
    auto write = edit();
    write.metadata["filepath"] = (directory / "outside/file.txt").string();
    EXPECT_THROW(policy->ask(write, [] {}), std::runtime_error);
    policy->setMode(ExecutionMode::SLOP);
    for (const auto* tool : {"write", "edit", "apply_patch"}) {
        ToolPermissionRequest external{"external_directory", {}, {}, {{"parentDir", (directory / "outside/nested/deeper").string()}}};
        external.toolName = tool;
        EXPECT_EQ(withoutPrompt(external).decision, PermissionDecision::ALWAYS);
    }
    const auto count = events.size();
    auto writing = std::async(std::launch::async, [&] { return policy->ask(write, [] {}); });
    policy->reply(nextRequest(count).id, PermissionDecision::NO);
    EXPECT_EQ(writing.get().decision, PermissionDecision::NO);
}

TEST_F(PermissionTest, PendingRequestsAreDismissedOnCancellationAndModeChange) {
    std::atomic<bool> cancelled = false;
    auto waiting = std::async(std::launch::async, [&] {
        return policy->ask(edit(), [&] { if (cancelled) throw core::Cancelled(); });
    });
    nextRequest();
    cancelled = true;
    EXPECT_THROW(waiting.get(), core::Cancelled);
    EXPECT_EQ(events.back().type, PermissionEventType::DISMISSED);
    const auto count = events.size();
    auto next = std::async(std::launch::async, [&] { return policy->ask(edit(), [] {}); });
    nextRequest(count);
    policy->setMode(ExecutionMode::SANE);
    EXPECT_THROW(next.get(), std::runtime_error);
    EXPECT_EQ(events.back().type, PermissionEventType::DISMISSED);
}

TEST_F(PermissionTest, BashMutationsAreBlockedInSaneAndWaitForApprovalInSlop) {
    ToolContext context{directory / "workspace", directory / "workspace"};
    context.permissionRoot = policy->root();
    context.ask = [&](ToolPermissionRequest request) {
        request.callId = "bash-call";
        request.toolName = "bash";
        const auto answer = policy->ask(request, [] {});
        if (answer.decision == PermissionDecision::NO) throw core::Cancelled();
    };
    policy->setMode(ExecutionMode::SANE);
    EXPECT_THROW(BashTool{}.execute({{"command", "printf changed > blocked.txt"}, {"mainCommand", "printf"}}, context), std::runtime_error);
    EXPECT_FALSE(fs::exists(directory / "workspace/blocked.txt"));
    EXPECT_NE(BashTool{}.execute({{"command", "printf hello"}, {"mainCommand", "printf"}}, context).output.find("hello"), std::string::npos);
    policy->setMode(ExecutionMode::SLOP);
    auto executing = std::async(std::launch::async, [&] {
        return BashTool{}.execute({{"command", "printf allowed > allowed.txt"}, {"mainCommand", "printf"}}, context);
    });
    const auto prompt = nextRequest();
    EXPECT_FALSE(fs::exists(directory / "workspace/allowed.txt"));
    policy->reply(prompt.id, PermissionDecision::YES);
    EXPECT_FALSE(executing.get().metadata.at("timedOut").get<bool>());
    EXPECT_TRUE(fs::exists(directory / "workspace/allowed.txt"));
}

TEST_F(PermissionTest, SwitchingToSaneWhileBashAwaitsExternalAccessPreventsItsMutation) {
    ToolContext context{directory / "workspace", directory / "workspace"};
    context.permissionRoot = policy->root();
    context.ask = [&](ToolPermissionRequest request) {
        request.callId = "bash-external";
        request.toolName = "bash";
        policy->ask(request, [] {});
    };
    auto executing = std::async(std::launch::async, [&] {
        return BashTool{}.execute({{"command", "touch created.txt"}, {"mainCommand", "touch"}, {"workdir", (directory / "outside").string()}}, context);
    });
    const auto command = nextRequest();
    const auto count = events.size();
    policy->reply(command.id, PermissionDecision::YES);
    EXPECT_EQ(nextRequest(count).request.permission, "external_directory");
    policy->setMode(ExecutionMode::SANE);
    EXPECT_THROW(executing.get(), std::runtime_error);
    EXPECT_FALSE(fs::exists(directory / "outside/created.txt"));
}

class WriteProvider : public Provider {
public:
    int calls = 0;
    json lastInput;
    ProviderFormat getFormat() const override { return ProviderFormat::OPENAI_RESPONSES; }
    json generate(const json& input, const std::string&, const std::vector<ToolDefinition>&, const std::string&) override {
        lastInput = input;
        ++calls;
        if (calls % 2 == 1) return {{"id", "response_" + std::to_string(calls)}, {"status", "completed"}, {"output", json::array({
            {{"type", "function_call"}, {"id", "first"}, {"call_id", "first"}, {"name", "write"},
                {"arguments", json{{"filePath", "first.txt"}, {"content", "first"}}.dump()}},
            {{"type", "function_call"}, {"id", "second"}, {"call_id", "second"}, {"name", "write"},
                {"arguments", json{{"filePath", "second.txt"}, {"content", "second"}}.dump()}}
        })}};
        return {{"id", "done_" + std::to_string(calls)}, {"status", "completed"}, {"output", json::array({
            {{"type", "message"}, {"id", "answer_" + std::to_string(calls)}, {"role", "assistant"},
                {"content", json::array({{{"type", "output_text"}, {"text", "Done"}, {"annotations", json::array()}}})}}
        })}};
    }
};

TEST_F(PermissionTest, AutoRefusalDoesNotWriteOrCancelAndTheModelReceivesGuidance) {
    auto provider = std::make_unique<WriteProvider>();
    auto* observed = provider.get();
    ToolRegistry tools;
    tools.add(std::make_unique<WriteTool>());
    Agent agent(std::move(provider), "test-model", std::move(tools), ToolContext{directory / "workspace", directory / "workspace"});
    policy->setPermissionMode(PermissionMode::AUTO, [](const auto& request, const auto&) {
        return request.callId != "first" ? 0.95 : 0.02;
    });
    agent.setPermissionHandler([&](const auto& request, const auto& check) { return policy->ask(request, check); }, policy->root());
    std::vector<AgentEvent> agentEvents;
    agent.setEventHandler([&](AgentEvent event) { agentEvents.push_back(std::move(event)); });
    agent.append(Message{"user", MessageRole::USER, {TextContent{"Write files"}}});
    const auto running = agent.run();
    if (running.wait_for(2s) != std::future_status::ready) {
        agent.cancel();
        running.wait();
        FAIL() << "Auto refusal must not wait for a human";
    }
    running.get();
    ASSERT_EQ(events.size(), 2);
    EXPECT_EQ(events[0].type, PermissionEventType::RESOLVED);
    EXPECT_EQ(events[0].decision, PermissionDecision::AI_REFUSED);
    EXPECT_EQ(events[1].decision, PermissionDecision::AI_ACCEPTED);
    EXPECT_EQ(observed->calls, 2);
    ASSERT_FALSE(agentEvents.empty());
    EXPECT_EQ(agentEvents.back().type, AgentEventType::EXECUTION_COMPLETED);
    EXPECT_FALSE(fs::exists(directory / "workspace/first.txt"));
    EXPECT_TRUE(fs::exists(directory / "workspace/second.txt"));
    const auto messages = agent.getMessages();
    const auto& refused = std::get<ToolResultContent>(messages[2].content[0]);
    EXPECT_TRUE(refused.isError);
    EXPECT_FALSE(refused.metadata.contains("cancelled"));
    EXPECT_EQ(refused.metadata["permissions"][0]["decision"], "ai_refused");
    EXPECT_EQ(refused.metadata["permissions"][0]["confidence"], 0.02);
    EXPECT_EQ(events[0].confidence, 0.02);
    EXPECT_EQ(std::get<ToolResultContent>(messages[3].content[0]).metadata["permissions"][0]["decision"], "ai_accepted");
    EXPECT_NE(refused.output.get<std::string>().find("refused by the command-verification AI"), std::string::npos);
    EXPECT_EQ(refused.metadata["refused_message_id"], messages[2].id);
    EXPECT_EQ(refused.metadata["refused_action"]["arguments"], std::get<ToolCallContent>(messages[1].content[0]).arguments);
    EXPECT_NE(observed->lastInput.dump().find("click Approve"), std::string::npos);
    EXPECT_NE(observed->lastInput.dump().find("continue without this action"), std::string::npos);
}

TEST_F(PermissionTest, NoCancelsTheWholeRunSkipsRemainingToolsAndKeepsTheDecision) {
    auto provider = std::make_unique<WriteProvider>();
    auto* observed = provider.get();
    ToolRegistry tools;
    tools.add(std::make_unique<WriteTool>());
    Agent agent(std::move(provider), "test-model", std::move(tools), ToolContext{directory / "workspace", directory / "workspace"});
    agent.setPermissionHandler([&](const auto& request, const auto& check) { return policy->ask(request, check); }, policy->root());
    std::vector<AgentEvent> agentEvents;
    agent.setEventHandler([&](AgentEvent event) { agentEvents.push_back(std::move(event)); });
    agent.append(Message{"user", MessageRole::USER, {TextContent{"Write files"}}});
    const auto running = agent.run();
    const auto prompt = nextRequest();
    EXPECT_EQ(prompt.request.callId, "first");
    EXPECT_FALSE(fs::exists(directory / "workspace/first.txt"));
    policy->reply(prompt.id, PermissionDecision::NO);
    running.get();
    EXPECT_EQ(observed->calls, 1);
    EXPECT_EQ(agentEvents.back().type, AgentEventType::EXECUTION_CANCELLED);
    EXPECT_FALSE(fs::exists(directory / "workspace/first.txt"));
    EXPECT_FALSE(fs::exists(directory / "workspace/second.txt"));
    const auto messages = agent.getMessages();
    const auto first = std::get<ToolResultContent>(messages[2].content[0]);
    EXPECT_EQ(first.metadata["permissions"][0]["decision"], "no");
    EXPECT_EQ(first.metadata["started"], false);
    const auto restored = json(messages[2]).get<Message>();
    EXPECT_EQ(std::get<ToolResultContent>(restored.content[0]).metadata, first.metadata);
}

TEST_F(PermissionTest, IntegrationPersistsManualApprovalAndAppliesItOnlyOnNextMessage) {
    asio::io_context io;
    SessionRegistry registry(directory / "sessions", [](const SessionInfo& info) {
        ToolRegistry tools;
        tools.add(std::make_unique<WriteTool>());
        return std::make_unique<Agent>(std::make_unique<WriteProvider>(), info.model, std::move(tools),
            ToolContext{info.workspace, info.workspace});
    });
    SessionInfo info;
    info.workspace = policy->root();
    info.title = "Manual approvals";
    info.mode = ExecutionMode::SLOP;
    auto saved = registry.create(info);
    const auto id = saved->getInfo().id;
    const json args{{"filePath", "first.txt"}, {"content", "first"}};
    ToolResultContent refused{"old-call", "Refused", true};
    refused.metadata = {{"refused_message_id", "old-result"}, {"refused_action", {
        {"tool", "write"}, {"arguments", args}, {"workspace", info.workspace.string()},
        {"working_directory", info.workspace.string()}}},
        {"permissions", json::array({{{"decision", "ai_refused"}}})}};
    saved->record({AgentEventType::HARNESS_MESSAGE_ARRIVED, {
        {"old-user", MessageRole::USER, {TextContent{"Write"}}},
        {"old-assistant", MessageRole::ASSISTANT, {ToolCallContent{"old-call", "write", args}}},
        {"old-result", MessageRole::TOOL, {refused}}}, {}});
    saved.reset();
    ServerSocket server(io, directory / "manual.sock");
    server.listen();
    asio::local::stream_protocol::socket socket(io);
    socket.connect(asio::local::stream_protocol::endpoint((directory / "manual.sock").string()));
    ConnectionSocket client(std::move(socket));
    std::shared_ptr<Integration> integration;
    auto run = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        integration = std::make_shared<Integration>(io, co_await server.accept(), registry,
            [](const std::string&, const std::string&, core::CancellationCheck) { return "Unused"; });
        co_await integration->run();
    }, asio::use_future);
    auto exchange = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        co_await client.receive();
        co_await client.send({{"type", "integration.configure"}, {"mode", "slop"}, {"workspace", info.workspace.string()}});
        co_await client.receive();
        co_await client.send({{"type", "session.attach"}, {"sessionId", id}});
        while ((co_await client.receive())->at("type") != "session.attached") {}
        co_await client.send({{"type", "permission.manual"}, {"sessionId", "wrong"},
            {"refusedMessageId", "old-result"}, {"approved", true}});
        EXPECT_EQ((co_await client.receive())->at("type"), "request.failed");
        for (const bool approved : {true, false}) {
            co_await client.send({{"type", "permission.manual"}, {"sessionId", id},
                {"refusedMessageId", "old-result"}, {"approved", approved}});
            const auto reply = co_await client.receive();
            EXPECT_EQ(reply->at("type"), "permission.manual_updated");
            EXPECT_EQ(reply->at("states").at("old-result"), approved);
            if (approved) EXPECT_FALSE(fs::exists(info.workspace / "first.txt"));
            co_await client.send({{"type", "message.send"}, {"text", "Continue"}});
            int prompts = 0, manual = 0, reports = 0;
            for (;;) {
                const auto event = co_await client.receive();
                if (!event) throw std::runtime_error("Disconnected during manual approval test");
                if (event->at("type") == "permission.requested") {
                    ++prompts;
                    if (approved) EXPECT_EQ(event->at("callId"), "second");
                    co_await client.send({{"type", "permission.reply"}, {"permissionId", event->at("permissionId")}, {"decision", "yes"}});
                }
                if (event->at("type") == "harness.message_arrived") for (const auto& message : event->at("messages")) {
                    if (message.at("id").get<std::string>().starts_with("approval_report_")) ++reports;
                    if (message.at("role") == "tool") for (const auto& decision : message.at("content")[0].at("metadata").at("permissions"))
                        if (decision.at("decision") == "manually_approved") ++manual;
                }
                if (event->at("type") == "execution.completed") break;
            }
            EXPECT_EQ(prompts, approved ? 1 : 2);
            EXPECT_EQ(manual, approved ? 1 : 0);
            EXPECT_EQ(reports, 1);
        }
        co_await client.send({{"type", "integration.close"}});
    }, asio::use_future);
    io.run_for(3s);
    if (integration) integration->close();
    client.close();
    io.restart();
    io.run_for(1s);
    ASSERT_EQ(exchange.wait_for(0s), std::future_status::ready);
    exchange.get();
    run.get();
    integration.reset();
    auto restored = registry.load(id);
    EXPECT_FALSE(restored->manualApprovalStates()["old-result"].get<bool>());
    EXPECT_TRUE(restored->manualActions().empty());
}

TEST_F(PermissionTest, IntegrationApprovesExternalWritesAndResumesWithCurrentConnectionPolicy) {
    asio::io_context io;
    SessionRegistry registry(directory / "sessions", [](const SessionInfo& info) {
        ToolRegistry tools;
        tools.add(std::make_unique<WriteTool>());
        return std::make_unique<Agent>(std::make_unique<WriteProvider>(), info.model, std::move(tools),
            ToolContext{info.workspace, info.workspace});
    });
    SessionInfo info;
    info.workspace = directory / "outside";
    info.title = "Another workspace";
    info.mode = ExecutionMode::SLOP;
    auto saved = registry.create(info);
    const auto id = saved->getInfo().id;
    saved.reset();
    ServerSocket server(io, directory / "permission.sock");
    server.listen();
    asio::local::stream_protocol::socket socket(io);
    socket.connect(asio::local::stream_protocol::endpoint((directory / "permission.sock").string()));
    ConnectionSocket client(std::move(socket));
    std::shared_ptr<Integration> integration;
    auto run = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        integration = std::make_shared<Integration>(io, co_await server.accept(), registry,
            [](const std::string&, const std::string&, core::CancellationCheck) { return "Unused title"; });
        co_await integration->run();
    }, asio::use_future);
    auto exchange = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        co_await client.receive();
        co_await client.send({{"type", "integration.configure"}, {"mode", "sane"}, {"workspace", (directory / "workspace").string()}});
        EXPECT_EQ((co_await client.receive())->at("type"), "integration.configured");
        co_await client.send({{"type", "session.attach"}, {"sessionId", id}});
        const auto attached = co_await client.receive();
        EXPECT_EQ(attached->at("type"), "session.attach_started");
        EXPECT_EQ(attached->at("session").at("mode"), "slop");
        for (;;) {
            const auto frame = co_await client.receive();
            if (!frame) throw std::runtime_error("Connection ended during history transfer");
            if (frame->at("type") == "session.attached") break;
            EXPECT_EQ(frame->at("type"), "session.history_chunk");
        }
        for (const auto* mode : {"slop", "sane"}) {
            if (std::string(mode) == "sane") {
                co_await client.send({{"type", "mode.set"}, {"mode", "sane"}});
                EXPECT_EQ((co_await client.receive())->at("mode"), "sane");
            }
            co_await client.send({{"type", "message.send"}, {"text", "Write files"}});
            int requests = 0;
            int results = 0;
            for (;;) {
                const auto event = co_await client.receive();
                if (!event) throw std::runtime_error("Connection ended during permission test");
                if (event->at("type") == "permission.requested") {
                    ++requests;
                    EXPECT_EQ(std::string(mode), "slop");
                    EXPECT_EQ(event->at("permission"), requests == 1 ? "external_directory" : "edit");
                    EXPECT_FALSE(fs::exists(directory / "outside/first.txt"));
                    co_await client.send({{"type", "permission.reply"}, {"permissionId", event->at("permissionId")}, {"decision", "always"}});
                }
                if (event->at("type") == "harness.message_arrived") {
                    for (const auto& message : event->at("messages")) {
                        if (message.at("role") != "tool") continue;
                        ++results;
                        const auto& result = message.at("content")[0];
                        EXPECT_EQ(result.at("isError"), std::string(mode) == "sane");
                        if (std::string(mode) == "slop") EXPECT_EQ(result.at("metadata").at("permissions").size(), 2);
                    }
                }
                if (event->at("type") == "execution.completed") break;
            }
            EXPECT_EQ(results, 2);
            EXPECT_EQ(requests, std::string(mode) == "slop" ? 2 : 0);
        }
        co_await client.send({{"type", "integration.close"}});
    }, asio::use_future);
    io.run_for(3s);
    if (integration) integration->close();
    client.close();
    io.restart();
    io.run_for(1s);
    ASSERT_EQ(exchange.wait_for(0s), std::future_status::ready);
    ASSERT_EQ(run.wait_for(0s), std::future_status::ready);
    exchange.get();
    run.get();
    integration.reset();
    const auto restored = registry.load(id);
    EXPECT_EQ(restored->getInfo().mode, ExecutionMode::SANE);
    bool found = false;
    for (const auto& message : restored->getMessages()) {
        for (const auto& content : message.content) {
            if (const auto* result = std::get_if<ToolResultContent>(&content); result && result->metadata.contains("permissions")) {
                EXPECT_EQ(result->metadata["permissions"][0]["decision"], "always");
                found = true;
            }
        }
    }
    EXPECT_TRUE(found);
}
}
