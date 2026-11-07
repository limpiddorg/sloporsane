#include <daemon/mcp/manager.hpp>
#include <daemon/mcp/connections.hpp>
#include <daemon/mcp/management.hpp>
#include <daemon/integration/integration.hpp>
#include <daemon/mcp/json_file.hpp>
#include <daemon/mcp/tool_adapter.hpp>
#include <daemon/permissions/permission_manager.hpp>
#include <gtest/gtest.h>
#include <future>
#include <thread>
#include <cstdlib>

namespace {
using Json = nlohmann::json;
namespace fs = std::filesystem;
struct PeerState {
    std::atomic<int> created{0}, calls{0}, waiting{0};
    std::atomic<bool> blockInitialize{false}, releaseInitialize{false};
    bool readOnly = true;
    bool rpcError = false;
    bool notifyTools = false;
    Json response = {{"content", Json::array({{{"type", "text"}, {"text", "result"}}})}};
};
class Peer : public mcp::Transport {
public:
    explicit Peer(std::shared_ptr<PeerState> state) : state(std::move(state)) { ++this->state->created; }
    void send(const Json& message, mcp::Deadline, core::CancellationCheck) override { pending = message; }
    Json receive(mcp::Deadline deadline, core::CancellationCheck cancelled) override {
        const auto method = pending.at("method").get<std::string>();
        if (method == "initialize" && state->blockInitialize) {
            ++state->waiting;
            while (!state->releaseInitialize) {
                core::checkCancellation(cancelled);
                if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("timeout");
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        Json result;
        if (method == "initialize") result = {{"protocolVersion", "2025-11-25"},
            {"capabilities", {{"tools", Json::object()}}}, {"serverInfo", {{"name", "fake"}, {"version", "1"}}}};
        else if (method == "tools/list") result = {{"tools", Json::array({{{"name", "read"},
            {"inputSchema", {{"type", "object"}}}, {"annotations", {{"readOnlyHint", state->readOnly}}}}})}};
        else {
            if (state->notifyTools) {
                state->notifyTools = false;
                state->readOnly = false;
                return {{"jsonrpc", "2.0"}, {"method", "notifications/tools/list_changed"}};
            }
            ++state->calls;
            if (state->rpcError) return {{"jsonrpc", "2.0"}, {"id", pending.at("id")},
                {"error", {{"code", -32602}, {"message", "rejected"}}}};
            if (pending["params"]["arguments"].value("hang", false)) {
                ++state->waiting;
                for (;;) {
                    core::checkCancellation(cancelled);
                    if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("timeout");
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            }
            result = state->response;
        }
        return {{"jsonrpc", "2.0"}, {"id", pending.at("id")}, {"result", result}};
    }
    void close() noexcept override {}
private:
    std::shared_ptr<PeerState> state;
    Json pending;
};

class McpManagerTest : public testing::Test {
protected:
    fs::path root, workspace;
    std::map<std::string, std::optional<std::string>> environment;
    std::shared_ptr<PeerState> peer = std::make_shared<PeerState>();
    std::shared_ptr<mcp::Manager> manager;
    void SetUp() override {
        root = fs::temp_directory_path() / ("sos-mcp-manager-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        workspace = root / "workspace";
        fs::create_directories(workspace);
        for (const auto* key : {"XDG_CONFIG_HOME", "XDG_DATA_HOME"}) {
            const auto* previous = std::getenv(key);
            environment[key] = previous ? std::optional<std::string>(previous) : std::nullopt;
            setenv(key, (root / key).c_str(), 1);
        }
        manager = std::make_unique<mcp::Manager>(workspace, [state = peer](const auto&, const auto&) {
            return std::make_unique<Peer>(state);
        });
    }
    void TearDown() override {
        manager.reset();
        for (const auto& [key, value] : environment)
            if (value) setenv(key.c_str(), value->c_str(), 1); else unsetenv(key.c_str());
        fs::remove_all(root);
    }
    mcp::Server server(const std::string& name = "docs") {
        mcp::Server result{name, mcp::Scope::WORKSPACE, mcp::Local{{"fake-server"}, {}, {}}};
        result.requestTimeoutMs = 2000;
        return result;
    }
    mcp::ServerStatus status(const std::string& name = "docs") {
        for (const auto& item : manager->list().servers) if (item.server.name == name) return item;
        throw std::runtime_error("Missing test server");
    }
    void connect(const std::string& name = "docs") {
        manager->save(server(name));
        manager->enable(name, true);
        manager->trust(name, status(name).identity, true);
        manager->connect(name);
    }
};

TEST_F(McpManagerTest, DisabledServerCanConnectButCannotExposeOrExecuteTools) {
    manager->save(server());
    EXPECT_EQ(status().state, mcp::ConnectionState::NEEDS_TRUST);
    EXPECT_THROW(manager->connect("docs"), std::runtime_error);
    EXPECT_EQ(peer->created, 0);
    manager->trust("docs", status().identity, true);
    manager->prepare();
    EXPECT_EQ(peer->created, 0);
    manager->connect("docs");
    const auto connected = status();
    EXPECT_EQ(connected.state, mcp::ConnectionState::CONNECTED);
    EXPECT_FALSE(connected.server.enabled);
    EXPECT_TRUE(mcp::makeTools(manager).empty());
    ASSERT_FALSE(connected.tools.empty());
    EXPECT_THROW(manager->call("docs", connected.identity, connected.tools.front(), Json::object()), std::runtime_error);
    manager->enable("docs", true);
    ASSERT_EQ(mcp::makeTools(manager).size(), 1);
    EXPECT_NO_THROW(manager->call("docs", connected.identity, connected.tools.front(), Json::object()));
    manager->enable("docs", false);
    manager->prepare();
    EXPECT_EQ(status().state, mcp::ConnectionState::CONNECTED);
    EXPECT_EQ(status().identity, connected.identity);
    EXPECT_TRUE(mcp::makeTools(manager).empty());
    EXPECT_THROW(manager->call("docs", connected.identity, connected.tools.front(), Json::object()), std::runtime_error);
    manager->enable("docs", true);
    EXPECT_EQ(peer->created, 1);
    EXPECT_EQ(peer->calls, 1);
    EXPECT_EQ(mcp::makeTools(manager).size(), 1);
}

TEST_F(McpManagerTest, FileEnableChangesPreserveConnectionAndRejectAlreadyBoundTools) {
    auto configured = server();
    configured.enabled = true;
    manager->save(configured);
    manager->trust("docs", status().identity, true);
    manager->prepare(); // No explicit global choice yet; file defaults still apply.
    auto bound = mcp::makeTools(manager);
    auto value = status().server;
    value.enabled = false;
    mcp::ConfigStore(workspace).save(value);
    EXPECT_EQ(status().state, mcp::ConnectionState::CONNECTED);
    ToolContext context{workspace, workspace};
    context.ask = [](const auto&) {};
    EXPECT_THROW(bound.front()->execute(Json::object(), context), std::runtime_error);
    EXPECT_EQ(peer->calls, 0);
    value.enabled = true;
    mcp::ConfigStore(workspace).save(value);
    EXPECT_NO_THROW(bound.front()->execute(Json::object(), context));
    EXPECT_EQ(peer->created, 1);
}

TEST_F(McpManagerTest, DisableDoesNotWaitForOrCancelAnAlreadyRunningCall) {
    connect();
    const auto binding = status();
    auto pending = std::async(std::launch::async, [&] {
        return manager->call("docs", binding.identity, binding.tools.front(), {{"hang", true}});
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (peer->waiting == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    auto disable = std::async(std::launch::async, [&] { manager->enable("docs", false); });
    EXPECT_EQ(disable.wait_for(std::chrono::milliseconds(200)), std::future_status::ready);
    EXPECT_EQ(pending.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
    EXPECT_EQ(status().state, mcp::ConnectionState::CONNECTED);
    EXPECT_TRUE(mcp::makeTools(manager).empty());
    manager->disconnect("docs");
    EXPECT_THROW(pending.get(), core::Cancelled);
    disable.get();
}

TEST_F(McpManagerTest, ImportedPublicServerRequiresTrustButNotCredentials) {
    mcp::JsonFile(workspace / ".sos/mcps.json").update([](Json& json) {
        json = {{"mcpServers", {{"docs", {{"url", "https://example.test/mcp"}}}}}};
    });
    EXPECT_EQ(status().state, mcp::ConnectionState::NEEDS_TRUST);
    manager->prepare();
    EXPECT_EQ(peer->created, 0);
    manager->trust("docs", status().identity, true);
    manager->connect("docs");
    EXPECT_EQ(status().state, mcp::ConnectionState::CONNECTED);
    EXPECT_EQ(peer->created, 1);
}

TEST_F(McpManagerTest, ConnectionStateRetainsSafeTransportDiagnosticsButNotArbitraryErrors) {
    bool safe = true;
    manager = std::make_shared<mcp::Manager>(workspace, [&](const auto&, const auto&) -> std::unique_ptr<mcp::Transport> {
        if (safe) throw mcp::TransportError("Unexpected MCP HTTP status 503");
        throw std::runtime_error("private server data");
    });
    manager->save(server());
    manager->trust("docs", status().identity, true);
    EXPECT_THROW(manager->connect("docs"), mcp::TransportError);
    EXPECT_EQ(status().error, "Unexpected MCP HTTP status 503");
    safe = false;
    EXPECT_THROW(manager->connect("docs"), std::runtime_error);
    EXPECT_EQ(status().error.find("private server data"), std::string::npos);
}

TEST_F(McpManagerTest, ChangedConfigurationCannotReuseApprovalCredentialsOrAnOldToolBinding) {
    connect();
    const auto old = status();
    auto replacement = server();
    replacement.enabled = true;
    std::get<mcp::Local>(replacement.connection).command = {"another-server"};
    manager->save(replacement);
    EXPECT_EQ(status().state, mcp::ConnectionState::NEEDS_TRUST);
    EXPECT_THROW(manager->trust("docs", old.identity, true), std::runtime_error);
    EXPECT_THROW(manager->authenticate("docs", old.identity, mcp::Credentials{}), std::runtime_error);
    EXPECT_THROW(manager->call("docs", old.identity, old.tools.front(), Json::object()), std::runtime_error);
    EXPECT_EQ(peer->created, 1);
    manager->trust("docs", status().identity, true);
    manager->connect("docs");
    const auto current = status();
    auto changedTool = current.tools.front();
    changedTool.readOnly = false;
    EXPECT_THROW(manager->call("docs", current.identity, changedTool, Json::object()), std::runtime_error);
    EXPECT_EQ(peer->calls, 0);
}

TEST_F(McpManagerTest, DisconnectCancelsOneServerWithoutBlockingAnotherOrReplaying) {
    connect("a");
    connect("b");
    const auto a = status("a"), b = status("b");
    auto pending = std::async(std::launch::async, [&] {
        return manager->call("a", a.identity, a.tools.front(), {{"hang", true}});
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (peer->waiting == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ASSERT_EQ(peer->waiting, 1);
    EXPECT_NO_THROW(manager->call("b", b.identity, b.tools.front(), Json::object()));
    manager->disconnect("a");
    EXPECT_THROW(pending.get(), core::Cancelled);
    EXPECT_EQ(status("a").state, mcp::ConnectionState::DISCONNECTED);
    EXPECT_EQ(status("b").state, mcp::ConnectionState::CONNECTED);
    EXPECT_EQ(peer->calls, 2);
    manager->connect("a");
    EXPECT_EQ(peer->created, 3);
    EXPECT_EQ(peer->calls, 2);
}

TEST_F(McpManagerTest, MissingCredentialsAreReportedWithoutStartingTheTransport) {
    auto value = server();
    value.connection = mcp::Remote{"https://example.test/mcp", mcp::Authentication::TOKEN};
    value.enabled = true;
    manager->save(value);
    manager->trust("docs", status().identity, true);
    EXPECT_THROW(manager->connect("docs"), std::runtime_error);
    EXPECT_EQ(status().state, mcp::ConnectionState::NEEDS_AUTH);
    EXPECT_EQ(peer->created, 0);
    mcp::Credentials credentials;
    credentials.accessToken = "fixture-token";
    manager->authenticate("docs", status().identity, credentials);
    manager->connect("docs");
    EXPECT_EQ(status().state, mcp::ConnectionState::CONNECTED);
    EXPECT_EQ(peer->created, 1);
    manager->save(value, credentials);
    EXPECT_EQ(status().state, mcp::ConnectionState::DISCONNECTED);
    manager->connect("docs");
    EXPECT_EQ(peer->created, 2);
    manager->trust("docs", status().identity, false);
    EXPECT_EQ(status().state, mcp::ConnectionState::NEEDS_TRUST);
    EXPECT_THROW(manager->connect("docs"), std::runtime_error);
    manager->remove("docs");
    EXPECT_TRUE(manager->list().servers.empty());
}

TEST_F(McpManagerTest, RejectedCredentialsHaveAnAuthenticationStateRatherThanGenericFailure) {
    connect();
    manager.reset();
    manager = std::make_unique<mcp::Manager>(workspace, [](const auto&, const auto&) -> std::unique_ptr<mcp::Transport> {
        throw mcp::AuthenticationRequired();
    });
    EXPECT_THROW(manager->connect("docs"), mcp::AuthenticationRequired);
    EXPECT_EQ(status().state, mcp::ConnectionState::NEEDS_AUTH);
    EXPECT_TRUE(status().tools.empty());
}

TEST_F(McpManagerTest, OAuthExpiryZeroMeansNonExpiringButPastExpiryRequiresAuthentication) {
    auto value = server();
    value.connection = mcp::Remote{"https://example.test/mcp", mcp::Authentication::OAUTH};
    value.enabled = true;
    manager->save(value);
    manager->trust("docs", status().identity, true);
    mcp::Credentials credentials;
    credentials.accessToken = "fixture-token";
    manager->authenticate("docs", status().identity, credentials);
    EXPECT_NO_THROW(manager->connect("docs"));
    credentials.expiresAt = 1;
    manager->authenticate("docs", status().identity, credentials);
    EXPECT_THROW(manager->connect("docs"), mcp::AuthenticationRequired);
    EXPECT_EQ(status().state, mcp::ConnectionState::NEEDS_AUTH);
    EXPECT_EQ(peer->created, 1);
}

TEST_F(McpManagerTest, AdapterChecksPermissionsBeforeCallingOrStartingACheckpoint) {
    connect();
    auto tools = mcp::makeTools(manager);
    ASSERT_EQ(tools.size(), 1);
    ToolContext context{workspace, workspace};
    bool checkpoint = false;
    context.mutateFiles = [&](const auto& paths, const auto& action) {
        EXPECT_TRUE(paths.empty());
        checkpoint = true;
        action();
    };
    context.ask = [&](const ToolPermissionRequest& request) {
        EXPECT_EQ(request.permission, "mcp");
        EXPECT_TRUE(request.readOnly);
        EXPECT_EQ(request.metadata.at("identity"), status().identity);
        EXPECT_FALSE(checkpoint);
        throw std::runtime_error("refused");
    };
    EXPECT_THROW(tools.front()->execute(Json::object(), context), std::runtime_error);
    EXPECT_EQ(peer->calls, 0);
    EXPECT_FALSE(checkpoint);
    context.ask = [](const auto&) {};
    const auto result = tools.front()->execute(Json::object(), context);
    EXPECT_EQ(result.output, "result\n");
    EXPECT_TRUE(checkpoint);
    EXPECT_EQ(peer->calls, 1);
}

TEST_F(McpManagerTest, SaneRejectsMutatingMcpEvenWithDangerOrManualApproval) {
    peer->readOnly = false;
    connect();
    auto tools = mcp::makeTools(manager);
    PermissionManager permissions([](const auto&) {});
    permissions.configure(workspace, ExecutionMode::SANE);
    permissions.setPermissionMode(PermissionMode::DANGER);
    ToolContext context{workspace, workspace};
    context.ask = [&](ToolPermissionRequest request) {
        request.manuallyApproved = true;
        permissions.ask(request, [] {});
    };
    EXPECT_THROW(tools.front()->execute(Json::object(), context), std::runtime_error);
    EXPECT_EQ(peer->calls, 0);
    permissions.setMode(ExecutionMode::SLOP);
    EXPECT_NO_THROW(tools.front()->execute(Json::object(), context));
    EXPECT_EQ(peer->calls, 1);
}

TEST_F(McpManagerTest, ReadOnlyDeclarationStillRequiresPermissionAndBindingsChangeWithDefinitions) {
    connect();
    auto tools = mcp::makeTools(manager);
    PermissionManager permissions([](const auto&) {});
    permissions.configure(workspace, ExecutionMode::SANE);
    permissions.setPermissionMode(PermissionMode::AUTO, [](const auto&, const auto&) { return 0.0; });
    ToolContext context{workspace, workspace};
    context.ask = [&](const auto& request) {
        const auto answer = permissions.ask(request, [] {});
        if (answer.decision == PermissionDecision::AI_REFUSED) throw std::runtime_error(answer.reason);
    };
    EXPECT_THROW(tools.front()->execute(Json::object(), context), std::runtime_error);
    EXPECT_EQ(peer->calls, 0);
    permissions.setPermissionMode(PermissionMode::DANGER);
    EXPECT_NO_THROW(tools.front()->execute(Json::object(), context));
    const auto previousId = tools.front()->getDefinition().id;
    EXPECT_LE(previousId.size(), 64);
    manager->disconnect("docs");
    peer->readOnly = false;
    manager->connect("docs");
    auto changed = mcp::makeTools(manager);
    EXPECT_NE(previousId, changed.front()->getDefinition().id);
    context.ask = [](const auto&) {};
    EXPECT_THROW(tools.front()->execute(Json::object(), context), std::runtime_error);
    EXPECT_EQ(peer->calls, 1);
}

TEST_F(McpManagerTest, AdapterRevalidatesAfterPermissionAndPropagatesCancellation) {
    connect();
    auto tools = mcp::makeTools(manager);
    ToolContext context{workspace, workspace};
    context.ask = [&](const auto&) { manager->trust("docs", status().identity, false); };
    EXPECT_THROW(tools.front()->execute(Json::object(), context), std::runtime_error);
    EXPECT_EQ(peer->calls, 0);
    manager->trust("docs", status().identity, true);
    manager->connect("docs");
    bool cancelled = false;
    context.ask = [&](const auto&) { cancelled = true; };
    context.checkCancelled = [&] { if (cancelled) throw core::Cancelled(); };
    EXPECT_THROW(tools.front()->execute(Json::object(), context), core::Cancelled);
    EXPECT_EQ(peer->calls, 0);
}

TEST_F(McpManagerTest, AdapterConvertsStructuredOutputAndReportsErrorsWithoutReplaying) {
    connect();
    auto tools = mcp::makeTools(manager);
    ToolContext context{workspace, workspace};
    context.ask = [](const auto&) {};
    peer->response = {{"structuredContent", {{"answer", 42}}}};
    EXPECT_EQ(tools.front()->execute(Json::object(), context).output, "{\"answer\":42}\n");
    peer->response = {{"content", Json::array({{{"type", "text"}, {"text", "tool failed"}}})}, {"isError", true}};
    bool checkpointCompleted = false;
    context.mutateFiles = [&](const auto&, const auto& action) {
        try { action(); }
        catch (...) { checkpointCompleted = true; throw; }
    };
    EXPECT_THROW(tools.front()->execute(Json::object(), context), std::runtime_error);
    EXPECT_TRUE(checkpointCompleted);
    EXPECT_EQ(peer->calls, 2);
    EXPECT_EQ(status().state, mcp::ConnectionState::CONNECTED);
    peer->response = {{"content", Json::array({{{"type", "image"}, {"data", "invalid"}, {"mimeType", "image/png"}}})}};
    EXPECT_THROW(tools.front()->execute(Json::object(), context), std::exception);
    peer->response = {{"content", Json::array({{{"type", "text"}, {"text", std::string(60000, 'x')}}})}};
    const auto large = tools.front()->execute(Json::object(), context);
    EXPECT_TRUE(large.metadata.at("truncated").get<bool>());
    EXPECT_TRUE(fs::exists(large.metadata.at("outputPath").get<std::string>()));
}

TEST_F(McpManagerTest, RemoteResultsKeepImagesSeparateAndNeverFetchResourceLinks) {
    auto remote = server();
    remote.connection = mcp::Remote{"https://example.test/mcp", mcp::Authentication::NONE};
    remote.enabled = true;
    manager->save(remote);
    manager->trust("docs", status().identity, true);
    manager->connect("docs");
    auto tools = mcp::makeTools(manager);
    ToolContext context{workspace, workspace};
    context.ask = [](const auto&) {};
    context.mutateFiles = [](const auto&, const auto&) { FAIL() << "Remote effects cannot be snapshotted"; };
    peer->response = {{"content", Json::array({
        {{"type", "image"}, {"mimeType", "image/png"}, {"data",
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a6a0AAAAASUVORK5CYII="}},
        {{"type", "resource_link"}, {"uri", "https://example.test/private"}},
        {{"type", "resource"}, {"resource", {{"uri", "file:///secret"}, {"text", "embedded text"}}}},
        {{"type", "audio"}, {"data", "opaque"}, {"mimeType", "audio/wav"}}
    })}};
    const auto result = tools.front()->execute(Json::object(), context);
    ASSERT_EQ(result.images.size(), 1);
    EXPECT_EQ(result.images.front().width, 1);
    EXPECT_EQ(result.output.find("iVBOR"), std::string::npos);
    EXPECT_NE(result.output.find("not fetched"), std::string::npos);
    EXPECT_NE(result.output.find("embedded text"), std::string::npos);
    EXPECT_NE(result.output.find("Unsupported"), std::string::npos);
    EXPECT_EQ(peer->calls, 1);
}

TEST_F(McpManagerTest, AutomaticPreparationRequiresEnablementAndExactTrust) {
    manager->save(server());
    manager->prepare();
    EXPECT_EQ(peer->created, 0);
    manager->enable("docs", true);
    manager->prepare();
    EXPECT_EQ(peer->created, 0);
    manager->trust("docs", status().identity, true);
    manager->prepare();
    manager->prepare();
    EXPECT_EQ(peer->created, 1);
    EXPECT_EQ(status().state, mcp::ConnectionState::CONNECTED);
    auto changed = server();
    changed.enabled = true;
    std::get<mcp::Local>(changed.connection).command = {"different"};
    manager->save(changed);
    manager->prepare();
    EXPECT_EQ(peer->created, 1);
    EXPECT_EQ(status().state, mcp::ConnectionState::NEEDS_TRUST);
}

TEST_F(McpManagerTest, RpcRejectionDoesNotDestroyTheConnectionOrItsCatalogue) {
    connect();
    const auto binding = status();
    peer->rpcError = true;
    EXPECT_THROW(manager->call("docs", binding.identity, binding.tools.front(), Json::object()), mcp::RpcError);
    EXPECT_EQ(status().state, mcp::ConnectionState::CONNECTED);
    EXPECT_EQ(status().tools, binding.tools);
    peer->rpcError = false;
    EXPECT_NO_THROW(manager->call("docs", binding.identity, binding.tools.front(), Json::object()));
    EXPECT_EQ(peer->created, 1);
}

TEST_F(McpManagerTest, DisconnectDoesNotDependOnTheCredentialFileRemainingReadable) {
    connect();
    mcp::JsonFile(root / "XDG_DATA_HOME/sos/mcps/auth.json").update([](Json& json) { json["version"] = 999; });
    EXPECT_NO_THROW(manager->disconnect("docs"));
    EXPECT_EQ(status().state, mcp::ConnectionState::DISCONNECTED);
    EXPECT_TRUE(status().tools.empty());
}

TEST_F(McpManagerTest, RetiredConnectionCannotBeResurrectedByALateAuthenticationFailure) {
    auto value = server();
    value.enabled = true;
    const auto identity = mcp::ConfigStore(workspace).identity(value);
    mcp::AuthStore auth;
    auth.trust(identity, true);
    std::promise<void> entered, release;
    auto ready = entered.get_future();
    auto proceed = release.get_future();
    mcp::ServerConnection connection(value, identity, std::make_shared<std::atomic<bool>>(false),
        [&](const auto&, const auto&) -> std::unique_ptr<mcp::Transport> {
            entered.set_value();
            proceed.wait();
            throw mcp::AuthenticationRequired();
        }, auth);
    auto starting = std::async(std::launch::async, [&] { connection.connect(false, {}); });
    ready.wait();
    connection.retire();
    release.set_value();
    EXPECT_THROW(starting.get(), mcp::AuthenticationRequired);
    EXPECT_EQ(connection.snapshot().state, mcp::ConnectionState::DISCONNECTED);
    EXPECT_TRUE(connection.snapshot().tools.empty());
    EXPECT_TRUE(connection.snapshot().error.empty());
    EXPECT_THROW(connection.connect(false, {}), std::runtime_error);
}

TEST_F(McpManagerTest, StaleAdministrativeActionsDoNotDisconnectTheNewConfiguration) {
    connect();
    const auto old = status();
    auto value = old.server;
    std::get<mcp::Local>(value.connection).command = {"new-server"};
    manager->save(value);
    manager->trust("docs", status().identity, true);
    manager->connect("docs");
    EXPECT_THROW(manager->trust("docs", old.identity, false), std::runtime_error);
    EXPECT_THROW(manager->authenticate("docs", old.identity, {}), std::runtime_error);
    EXPECT_EQ(status().state, mcp::ConnectionState::CONNECTED);
    const auto binding = status();
    EXPECT_NO_THROW(manager->call("docs", binding.identity, binding.tools.front(), Json::object()));
}

TEST_F(McpManagerTest, PreparationOfAnAlreadyConnectedServerDoesNotWaitForItsToolCall) {
    connect();
    const auto binding = status();
    auto pending = std::async(std::launch::async, [&] {
        return manager->call("docs", binding.identity, binding.tools.front(), {{"hang", true}});
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (peer->waiting == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    auto preparation = std::async(std::launch::async, [&] { manager->prepare(); });
    EXPECT_EQ(preparation.wait_for(std::chrono::milliseconds(200)), std::future_status::ready);
    manager->disconnect("docs");
    EXPECT_THROW(pending.get(), core::Cancelled);
    preparation.get();
    EXPECT_EQ(status().state, mcp::ConnectionState::DISCONNECTED);
    EXPECT_TRUE(status().error.empty());
}

TEST_F(McpManagerTest, ExplicitDisconnectSuppressesAutomaticRestartEvenAfterConfigReload) {
    connect();
    manager->disconnect("docs");
    auto value = status().server;
    value.requestTimeoutMs += 1;
    manager->save(value);
    manager->enable("docs", false);
    manager->enable("docs", true);
    manager->prepare();
    EXPECT_EQ(peer->created, 1);
    manager->connect("docs");
    EXPECT_EQ(peer->created, 2);
    EXPECT_EQ(status().state, mcp::ConnectionState::CONNECTED);
}

TEST_F(McpManagerTest, AutomaticFailureDoesNotRetryOrPreventHealthyConnections) {
    int failures = 0;
    manager = std::make_shared<mcp::Manager>(workspace, [&](const auto& server, const auto&) -> std::unique_ptr<mcp::Transport> {
        if (server.name == "a") { ++failures; throw std::runtime_error("unavailable"); }
        return std::make_unique<Peer>(peer);
    });
    for (const auto* name : {"a", "b"}) {
        auto value = server(name);
        value.enabled = true;
        manager->save(value);
        manager->trust(name, status(name).identity, true);
    }
    manager->prepare();
    manager->prepare();
    EXPECT_EQ(failures, 1);
    EXPECT_EQ(peer->created, 1);
    EXPECT_EQ(status("a").state, mcp::ConnectionState::FAILED);
    EXPECT_EQ(status("b").state, mcp::ConnectionState::CONNECTED);
}

TEST_F(McpManagerTest, DisconnectBeforeFirstUseSurvivesANewManagerLifetime) {
    auto value = server();
    value.enabled = true;
    manager->save(value);
    manager->trust("docs", status().identity, true);
    manager->disconnect("docs");
    manager->prepare();
    EXPECT_EQ(peer->created, 0);
    manager = std::make_shared<mcp::Manager>(workspace, [&](const auto&, const auto&) { return std::make_unique<Peer>(peer); });
    manager->restore();
    manager->prepare();
    EXPECT_EQ(peer->created, 0);
    manager->connect("docs");
    EXPECT_EQ(peer->created, 1);
}

TEST_F(McpManagerTest, GlobalChoicesRestoreDisabledConnectionsWithoutChangingConfigurationFiles) {
    connect();
    manager->enable("docs", false);
    const auto configured = mcp::ConfigStore(workspace).load().servers.at("docs");
    EXPECT_FALSE(configured.enabled); // enable() never rewrote the original file.
    manager.reset();
    manager = std::make_shared<mcp::Manager>(workspace, [&](const auto&, const auto&) { return std::make_unique<Peer>(peer); });
    manager->restore();
    EXPECT_EQ(status().state, mcp::ConnectionState::CONNECTED);
    EXPECT_FALSE(status().server.enabled);
    EXPECT_TRUE(mcp::makeTools(manager).empty());
    EXPECT_EQ(peer->created, 2);
    manager->enable("docs", true);
    auto changed = configured;
    manager->save(changed);
    EXPECT_TRUE(status().server.enabled); // Global choice wins over the file default.
}

TEST_F(McpManagerTest, DaemonChoicesAreSharedByNameButCredentialsAndTrustRemainWorkspaceSpecific) {
    const auto other = root / "other";
    fs::create_directories(other);
    auto value = server();
    value.connection = mcp::Remote{"https://example.test/mcp", mcp::Authentication::TOKEN};
    mcp::ConfigStore(workspace).save(value);
    std::get<mcp::Remote>(value.connection).url = "https://other.test/mcp";
    mcp::ConfigStore(other).save(value);
    std::vector<std::string> tokens;
    mcp::Connections connections([&](const auto&, const auto& credentials) {
        tokens.push_back(credentials.accessToken);
        return std::make_unique<Peer>(peer);
    });
    const auto a = connections.forWorkspace(workspace), b = connections.forWorkspace(other);
    const auto identityA = a->list().servers.front().identity, identityB = b->list().servers.front().identity;
    EXPECT_NE(identityA, identityB);
    a->trust("docs", identityA, true);
    mcp::Credentials credentials;
    credentials.accessToken = "token-a";
    a->authenticate("docs", identityA, credentials);
    a->enable("docs", true);
    a->connect("docs");
    connections.restore();
    EXPECT_TRUE(b->list().servers.front().server.enabled);
    EXPECT_EQ(b->list().servers.front().state, mcp::ConnectionState::NEEDS_TRUST);
    b->trust("docs", identityB, true);
    connections.restore();
    EXPECT_EQ(b->list().servers.front().state, mcp::ConnectionState::NEEDS_AUTH);
    EXPECT_EQ(tokens, std::vector<std::string>{"token-a"});
    credentials.accessToken = "token-b";
    b->authenticate("docs", identityB, credentials);
    b->connect("docs");
    connections.restore();
    EXPECT_EQ(tokens, (std::vector<std::string>{"token-a", "token-b"}));
    b->enable("docs", false);
    EXPECT_TRUE(mcp::makeTools(a).empty());
    EXPECT_EQ(a->list().servers.front().state, mcp::ConnectionState::CONNECTED);
    b->disconnect("docs");
    connections.restore();
    a->prepare();
    EXPECT_EQ(a->list().servers.front().state, mcp::ConnectionState::DISCONNECTED);
    EXPECT_EQ(tokens.size(), 2);
    a->connect("docs");
    connections.restore();
    EXPECT_EQ(b->list().servers.front().state, mcp::ConnectionState::CONNECTED);
    EXPECT_FALSE(b->list().servers.front().server.enabled);
    EXPECT_EQ(tokens.size(), 4);
}

TEST_F(McpManagerTest, PersistedConnectionFailureIsNotRetriedUntilExplicitConnectOrRestart) {
    connect();
    manager.reset();
    int failures = 0;
    manager = std::make_shared<mcp::Manager>(workspace, [&](const auto&, const auto&) -> std::unique_ptr<mcp::Transport> {
        ++failures;
        throw mcp::TransportError("Unavailable");
    });
    manager->restore();
    manager->restore();
    manager->prepare();
    auto value = server();
    ++value.requestTimeoutMs;
    manager->save(value);
    manager->restore();
    EXPECT_EQ(failures, 1);
    EXPECT_THROW(manager->connect("docs"), mcp::TransportError);
    EXPECT_EQ(failures, 2);
    manager.reset();
    manager = std::make_shared<mcp::Manager>(workspace, [&](const auto&, const auto&) { return std::make_unique<Peer>(peer); });
    manager->restore();
    EXPECT_EQ(status().state, mcp::ConnectionState::CONNECTED);
}

TEST_F(McpManagerTest, BackgroundRestoreKeepsListingResponsiveAndWaitsForTurnPreparation) {
    connect(); // Persist a connection preference and approval for the new manager.
    mcp::Connections connections([state = peer](const auto&, const auto&) { return std::make_unique<Peer>(state); });
    peer->blockInitialize = true;
    auto restored = connections.restoreInBackground(workspace);
    auto fresh = connections.forWorkspace(workspace);
    for (int i = 0; i < 100 && peer->waiting == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_GT(peer->waiting, 0);
    EXPECT_EQ(fresh->list().servers.front().state, mcp::ConnectionState::CONNECTING);
    std::atomic<bool> cancel{false};
    auto waiting = std::async(std::launch::async, [&] { connections.waitForRestore(workspace, [&] { return cancel.load(); }); });
    EXPECT_EQ(waiting.wait_for(std::chrono::milliseconds(30)), std::future_status::timeout);
    cancel = true;
    EXPECT_THROW(waiting.get(), core::Cancelled);
    peer->releaseInitialize = true;
    restored.get();
    EXPECT_NO_THROW(connections.waitForRestore(workspace));
    EXPECT_EQ(fresh->list().servers.front().state, mcp::ConnectionState::CONNECTED);
}

TEST_F(McpManagerTest, GlobalPreferencesDoNotInventServersAndFailedWritesDoNotChangeLiveChoices) {
    connect();
    const auto empty = root / "empty";
    fs::create_directories(empty);
    mcp::Manager absent(empty, [&](const auto&, const auto&) { return std::make_unique<Peer>(peer); });
    absent.restore();
    EXPECT_TRUE(absent.list().servers.empty());
    EXPECT_EQ(peer->created, 1);
    const auto path = root / "XDG_CONFIG_HOME/sos/mcp-preferences.json";
    mcp::JsonFile(path).update([](Json& json) { json["docs"]["enabled"] = "invalid"; });
    EXPECT_THROW(manager->enable("docs", false), std::exception);
    EXPECT_TRUE(status().server.enabled);
    EXPECT_EQ(status().state, mcp::ConnectionState::CONNECTED);
    EXPECT_THROW((mcp::Manager(workspace)), std::exception);
}

TEST_F(McpManagerTest, CancellationDuringAutomaticStartupPropagatesAndDoesNotRetry) {
    bool cancelled = false;
    int attempts = 0;
    manager = std::make_shared<mcp::Manager>(workspace, [&](const auto&, const auto&) -> std::unique_ptr<mcp::Transport> {
        ++attempts;
        cancelled = true;
        throw core::Cancelled();
    });
    auto value = server();
    value.enabled = true;
    manager->save(value);
    manager->trust("docs", status().identity, true);
    EXPECT_THROW(manager->prepare([&] { return cancelled; }), core::Cancelled);
    cancelled = false;
    manager->prepare();
    EXPECT_EQ(attempts, 1);
}

TEST_F(McpManagerTest, ConcurrentPreparationsConnectOnceAndShutdownPreventsNewLaunches) {
    auto value = server();
    value.enabled = true;
    manager->save(value);
    manager->trust("docs", status().identity, true);
    auto first = std::async(std::launch::async, [&] { manager->prepare(); });
    auto second = std::async(std::launch::async, [&] { manager->prepare(); });
    first.get();
    second.get();
    EXPECT_EQ(peer->created, 1);
    manager->cancelAll();
    EXPECT_THROW(manager->prepare(), core::Cancelled);
    EXPECT_THROW(manager->connect("docs"), core::Cancelled);
}

class McpCallingProvider : public Provider {
public:
    ProviderFormat getFormat() const override { return ProviderFormat::OPENAI_RESPONSES; }
    Json generate(const Json&, const std::string&, const std::vector<::ToolDefinition>& tools, const std::string&) override {
        if (generations++ == 0) {
            EXPECT_EQ(tools.size(), 1);
            if (!tools.empty()) return {{"id", "response"}, {"status", "completed"}, {"output", Json::array({
                {{"type", "function_call"}, {"id", "item"}, {"call_id", "call"},
                 {"name", tools.front().id}, {"arguments", "{}"}}
            })}};
        }
        return {{"id", "done"}, {"status", "completed"}, {"output", Json::array()}};
    }
private:
    int generations = 0;
};

TEST_F(McpManagerTest, DaemonConnectionsBindAnAgentAndShareTheCanonicalWorkspace) {
    auto value = server();
    value.enabled = true;
    manager->save(value);
    manager->trust("docs", status().identity, true);
    mcp::Connections connections([&](const auto&, const auto&) { return std::make_unique<Peer>(peer); });
    auto shared = connections.forWorkspace(workspace);
    EXPECT_EQ(shared, connections.forWorkspace(workspace / "."));
    ToolContext context{workspace, workspace};
    context.ask = [](const auto&) {};
    Agent agent(std::make_unique<McpCallingProvider>(), "model", {}, context);
    connections.bind(agent, workspace);
    EXPECT_EQ(peer->created, 0); // Binding itself does not launch anything.
    agent.append(Message{"user", MessageRole::USER, {TextContent{"Use the MCP tool"}}});
    agent.run().get();
    EXPECT_EQ(peer->created, 1);
    EXPECT_EQ(peer->calls, 1);
    shared->disconnect("docs");
    connections.forWorkspace(workspace)->prepare();
    EXPECT_EQ(peer->created, 1);
    connections.cancelAll();
    EXPECT_THROW(connections.forWorkspace(workspace), core::Cancelled);
}

TEST_F(McpManagerTest, AgentPreservesMcpNamesWhenTheCallFails) {
    auto value = server();
    value.enabled = true;
    manager->save(value);
    manager->trust("docs", status().identity, true);
    peer->rpcError = true;
    mcp::Connections connections([&](const auto&, const auto&) { return std::make_unique<Peer>(peer); });
    Agent agent(std::make_unique<McpCallingProvider>(), "model", {}, ToolContext{workspace, workspace});
    agent.setPermissionHandler([](const auto&, const auto&) { return PermissionAnswer{}; }, workspace);
    connections.bind(agent, workspace);
    agent.append(Message{"user", MessageRole::USER, {TextContent{"Use the MCP tool"}}});
    agent.run().get();
    const auto messages = agent.getMessages();
    ASSERT_EQ(messages.size(), 4);
    const auto& result = std::get<ToolResultContent>(messages[2].content.front());
    EXPECT_TRUE(result.isError);
    EXPECT_EQ(result.metadata.at("mcp_server"), "docs");
    EXPECT_EQ(result.metadata.at("mcp_tool"), "read");
}

TEST_F(McpManagerTest, ReceivedToolNotificationsRefreshBindingsWithoutReplayingOrReconnecting) {
    auto value = server();
    value.enabled = true;
    manager->save(value);
    const auto identity = status().identity;
    manager->trust("docs", identity, true);
    manager->connect("docs");
    const auto original = status().tools.front();
    peer->notifyTools = true;
    EXPECT_NO_THROW(manager->call("docs", identity, original, Json::object()));
    EXPECT_EQ(peer->calls, 1);
    EXPECT_EQ(peer->created, 1);
    EXPECT_FALSE(status().tools.front().readOnly);
    EXPECT_THROW(manager->call("docs", identity, original, Json::object()), std::runtime_error);
    EXPECT_EQ(peer->calls, 1);
    peer->readOnly = true;
    manager->prepare();
    EXPECT_TRUE(status().tools.front().readOnly);
    EXPECT_EQ(peer->created, 1);
}

TEST_F(McpManagerTest, ManagementProtocolAddsApprovesConnectsAndNeverReturnsTokens) {
    mcp::Connections connections([&](const auto&, const auto&) { return std::make_unique<Peer>(peer); });
    const auto run = [&](const Json& json) {
        const auto parsed = json.get<IntegrationRequest>();
        EXPECT_TRUE(parsed.mcpRequest.has_value());
        return mcp::manage(connections, workspace, *parsed.mcpRequest, {});
    };
    auto listed = run({{"type", "mcp.save"}, {"name", "remote"}, {"scope", "workspace"},
        {"config", {{"type", "http"}, {"url", "https://example.test/mcp"}, {"authentication", "token"}}}});
    EXPECT_EQ(listed["servers"][0]["state"], "needs_trust");
    EXPECT_EQ(peer->created, 0);
    const auto identity = listed["servers"][0]["identity"];
    const auto command = [&](const std::string& type) { return Json{{"type", type}, {"name", "remote"}, {"identity", identity}}; };
    auto trust = command("mcp.trust"); trust["accepted"] = true;
    run(trust);
    auto token = command("mcp.token"); token["token"] = "never-return-this-secret";
    EXPECT_EQ(run(token).dump().find("never-return-this-secret"), std::string::npos);
    auto enable = command("mcp.enable"); enable["enabled"] = true;
    run(enable);
    listed = run(command("mcp.connect"));
    EXPECT_EQ(listed["servers"][0]["state"], "connected");
    EXPECT_EQ(listed["servers"][0]["tool_count"], 1);
    auto stale = command("mcp.remove"); stale["identity"] = std::string(64, '0');
    EXPECT_THROW(run(stale), std::runtime_error);
    EXPECT_EQ(run(command("mcp.disconnect"))["servers"][0]["state"], "disconnected");
    EXPECT_TRUE(run(command("mcp.remove"))["servers"].empty());
}

TEST_F(McpManagerTest, ManagementRejectsInvalidRequestsWithoutLaunchingAnything) {
    EXPECT_THROW((Json{{"type", "mcp.save"}, {"name", "x"}, {"scope", "unknown"},
        {"config", {{"type", "stdio"}, {"command", {"server"}}}}}.get<IntegrationRequest>()), std::invalid_argument);
    EXPECT_THROW((Json{{"type", "mcp.execute"}, {"name", "x"}}.get<IntegrationRequest>()), std::invalid_argument);
    EXPECT_EQ(peer->created, 0);
}
}
