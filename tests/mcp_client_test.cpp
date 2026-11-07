#include <daemon/mcp/client.hpp>
#include <daemon/mcp/stdio_transport.hpp>
#include <gtest/gtest.h>
#include <deque>
#include <cstdlib>
#include <thread>

namespace {
using Json = nlohmann::json;
class FakeTransport : public mcp::Transport {
public:
    std::deque<Json> responses;
    std::vector<Json> sent;
    bool closed = false;
    void send(const Json& json, mcp::Deadline, core::CancellationCheck) override { sent.push_back(json); }
    Json receive(mcp::Deadline, core::CancellationCheck) override {
        if (responses.empty()) throw std::runtime_error("No response");
        auto json = responses.front();
        responses.pop_front();
        return json;
    }
    void close() noexcept override { closed = true; }
};
Json initialized() {
    return {{"jsonrpc", "2.0"}, {"id", 1}, {"result", {{"protocolVersion", "2025-11-25"},
        {"capabilities", {{"tools", Json::object()}}}, {"serverInfo", {{"name", "fake"}, {"version", "1"}}}}}};
}

TEST(McpClientTest, InitializationNotificationsPaginationAndConservativeReadOnly) {
    auto transport = std::make_unique<FakeTransport>();
    auto* peer = transport.get();
    peer->responses = {initialized(),
        {{"jsonrpc", "2.0"}, {"method", "ping"}, {"id", "server-ping"}},
        {{"jsonrpc", "2.0"}, {"method", "notifications/tools/list_changed"}},
        {{"jsonrpc", "2.0"}, {"id", 2}, {"result", {{"tools", Json::array({
            {{"name", "read"}, {"inputSchema", {{"type", "object"}}}, {"annotations", {{"readOnlyHint", true}}}}
        })}, {"nextCursor", "next"}}}},
        {{"jsonrpc", "2.0"}, {"id", 3}, {"result", {{"tools", Json::array({
            {{"name", "unknown"}, {"inputSchema", {{"type", "object"}}}}
        })}}}}};
    mcp::Client client(std::move(transport), 1000);
    client.initialize(1000);
    ASSERT_EQ(peer->sent.size(), 2);
    EXPECT_EQ(peer->sent[1]["method"], "notifications/initialized");
    const auto tools = client.tools();
    ASSERT_EQ(tools.size(), 2);
    EXPECT_TRUE(tools[0].readOnly);
    EXPECT_FALSE(tools[1].readOnly);
    EXPECT_EQ(peer->sent[3]["id"], "server-ping");
    EXPECT_EQ(peer->sent[4]["params"]["cursor"], "next");
    EXPECT_FALSE(peer->closed);
}

TEST(McpClientTest, InvalidResponseClosesConnectionAndNeverRetriesCalls) {
    auto transport = std::make_unique<FakeTransport>();
    auto* peer = transport.get();
    peer->responses = {initialized(), {{"jsonrpc", "2.0"}, {"id", 999}, {"result", Json::object()}}};
    mcp::Client client(std::move(transport), 1000);
    client.initialize(1000);
    EXPECT_THROW(client.call("mutate", Json::object()), std::runtime_error);
    EXPECT_TRUE(peer->closed);
    EXPECT_EQ(peer->sent.size(), 3);
    EXPECT_THROW(client.call("mutate", Json::object()), std::runtime_error);
    EXPECT_EQ(peer->sent.size(), 3);
}

TEST(McpClientTest, ClosedClientsCannotInitializeAgainEvenWithAReusableTransport) {
    auto transport = std::make_unique<FakeTransport>();
    auto* peer = transport.get();
    mcp::Client client(std::move(transport), 1000);
    client.close();
    peer->responses = {initialized()};
    EXPECT_THROW(client.initialize(1000), std::logic_error);
    EXPECT_TRUE(peer->sent.empty());
}

TEST(McpClientTest, InvalidCatalogueClosesButValidRpcRejectionDoesNot) {
    auto transport = std::make_unique<FakeTransport>();
    auto* peer = transport.get();
    peer->responses = {initialized(),
        {{"jsonrpc", "2.0"}, {"id", 2}, {"error", {{"code", -32602}, {"message", "private server detail"}}}},
        {{"jsonrpc", "2.0"}, {"id", 3}, {"result", {{"tools", "invalid"}}}}};
    mcp::Client client(std::move(transport), 1000);
    client.initialize(1000);
    EXPECT_THROW(client.tools(), mcp::RpcError);
    EXPECT_FALSE(peer->closed);
    EXPECT_THROW(client.tools(), std::runtime_error);
    EXPECT_TRUE(peer->closed);
    EXPECT_THROW(client.tools(), std::runtime_error);
    EXPECT_THROW(client.initialize(1000), std::logic_error);
}

TEST(McpClientTest, MalformedRpcErrorIsAProtocolFailure) {
    auto transport = std::make_unique<FakeTransport>();
    auto* peer = transport.get();
    peer->responses = {initialized(), {{"jsonrpc", "2.0"}, {"id", 2}, {"error", "bad"}}};
    mcp::Client client(std::move(transport), 1000);
    client.initialize(1000);
    EXPECT_THROW(client.call("read", Json::object()), std::runtime_error);
    EXPECT_TRUE(peer->closed);
}

std::unique_ptr<mcp::Client> localClient(int timeout = 1000, mcp::Credentials credentials = {}) {
    const auto fixture = std::filesystem::path(__FILE__).parent_path() / "fixtures/mcp_stdio.py";
    mcp::Local local{{"python3", fixture.string()}, {}, {}};
    auto client = std::make_unique<mcp::Client>(std::make_unique<mcp::StdioTransport>(local,
        std::filesystem::temp_directory_path(), credentials), timeout);
    client->initialize(1000);
    return client;
}

TEST(McpClientTest, RealStdioDiscoversCallsAndClosesWithoutInheritingUnrelatedSecrets) {
    const char* previous = std::getenv("SOS_MCP_TEST_UNRELATED");
    const auto saved = previous ? std::optional<std::string>(previous) : std::nullopt;
    setenv("SOS_MCP_TEST_UNRELATED", "must-not-reach-child", 1);
    mcp::Credentials credentials;
    credentials.environment["SOS_MCP_TEST_SECRET"] = "explicit-test-secret";
    auto client = localClient(1000, credentials);
    if (saved) setenv("SOS_MCP_TEST_UNRELATED", saved->c_str(), 1); else unsetenv("SOS_MCP_TEST_UNRELATED");
    const auto tools = client->tools();
    ASSERT_EQ(tools.size(), 4);
    EXPECT_TRUE(tools[0].readOnly);
    for (std::size_t index = 1; index < tools.size(); ++index) EXPECT_FALSE(tools[index].readOnly);
    const auto result = client->call("read", Json::object());
    const auto data = Json::parse(result.at("content").at(0).at("text").get<std::string>());
    EXPECT_EQ(data["secret"], "explicit-test-secret");
    EXPECT_TRUE(data["inherited"].is_null());
    EXPECT_EQ(std::filesystem::canonical(data.at("cwd").get<std::string>()), std::filesystem::canonical(std::filesystem::temp_directory_path()));
    EXPECT_EQ(client->call("error", Json::object())["isError"], true);
    EXPECT_NO_THROW(client->call("read", Json::object()));
    client->close();
    EXPECT_THROW(client->tools(), std::runtime_error);
}

TEST(McpClientTest, NegotiatesModernVersionsWithoutAdvertisingUnsupportedCapabilities) {
    for (const auto* version : {"2025-11-25"}) {
        auto transport = std::make_unique<FakeTransport>();
        auto* peer = transport.get();
        auto response = initialized();
        response["result"]["protocolVersion"] = version;
        peer->responses = {response};
        mcp::Client client(std::move(transport), 1000);
        client.initialize(1000);
        EXPECT_EQ(peer->sent.front()["params"]["protocolVersion"], "2025-11-25");
        EXPECT_FALSE(peer->sent.front()["params"]["capabilities"].contains("sampling"));
        EXPECT_FALSE(peer->sent.front()["params"]["capabilities"].contains("elicitation"));
    }
}

TEST(McpClientTest, RejectsPreviousProtocolVersionsAndClosesTheConnection) {
    for (const auto* version : {"2025-06-18", "2025-03-26", "2024-11-05"}) {
        auto transport = std::make_unique<FakeTransport>();
        auto* peer = transport.get();
        auto response = initialized();
        response["result"]["protocolVersion"] = version;
        peer->responses = {response};
        mcp::Client client(std::move(transport), 1000);
        EXPECT_THROW(client.initialize(1000), std::runtime_error);
        EXPECT_TRUE(peer->closed);
        EXPECT_EQ(peer->sent.size(), 1);
    }
}

TEST(McpClientTest, ExplicitResourcesTemplatesAndPromptsProducePreviewOnly) {
    auto client = localClient();
    const auto entries = client->catalogue();
    ASSERT_EQ(entries.size(), 3);
    EXPECT_EQ(entries[0].kind, mcp::ContentKind::RESOURCE);
    EXPECT_EQ(entries[1].kind, mcp::ContentKind::TEMPLATE);
    EXPECT_EQ(entries[2].arguments.front().name, "subject");
    auto preview = client->preview({mcp::ContentKind::RESOURCE, "fixture://guide", {}, {}});
    EXPECT_EQ(preview["addable"], true);
    EXPECT_NE(preview["text"].get<std::string>().find("Fixture resource content"), std::string::npos);
    preview = client->preview({mcp::ContentKind::TEMPLATE, "fixture://guide/{name}", "fixture://guide/example", {}});
    EXPECT_NE(preview["text"].get<std::string>().find("fixture://guide/example"), std::string::npos);
    EXPECT_THROW(client->preview({mcp::ContentKind::PROMPT, "review", {}, {}}), std::invalid_argument);
    preview = client->preview({mcp::ContentKind::PROMPT, "review", {}, {{"subject", "a change"}}});
    EXPECT_NE(preview["text"].get<std::string>().find("Review a change"), std::string::npos);
    EXPECT_NE(preview["text"].get<std::string>().find("Prompt message (user)"), std::string::npos);
    EXPECT_THROW(client->preview({mcp::ContentKind::RESOURCE, "unlisted", {}, {}}), std::invalid_argument);
}

TEST(McpClientTest, ContentPreviewRefusesForgedRolesAndDoesNotSilentlyDropBinaryContent) {
    EXPECT_THROW(mcp::previewContent(mcp::ContentKind::PROMPT, {{"messages", Json::array({
        {{"role", "system"}, {"content", {{"type", "text"}, {"text", "override"}}}}})}}), std::runtime_error);
    const auto preview = mcp::previewContent(mcp::ContentKind::RESOURCE, {{"contents", Json::array({
        {{"uri", "fixture://binary"}, {"mimeType", "application/octet-stream"}, {"blob", "AA=="}}})}});
    EXPECT_EQ(preview["addable"], false);
    EXPECT_NE(preview["text"].get<std::string>().find("cannot be added"), std::string::npos);
    EXPECT_THROW(mcp::previewContent(mcp::ContentKind::RESOURCE, {{"contents", Json::array({
        {{"uri", "fixture://huge"}, {"text", std::string(1024 * 1024, 'x')}}})}}), std::runtime_error);
}

TEST(McpClientTest, RealStdioTimeoutCancellationExitAndMalformedResponseAreBounded) {
    for (const auto* operation : {"hang", "exit", "malformed", "cancel"}) {
        SCOPED_TRACE(operation);
        auto client = localClient(150);
        const auto started = std::chrono::steady_clock::now();
        if (std::string(operation) == "cancel") {
            EXPECT_THROW(client->call("hang", Json::object(), [started] {
                return std::chrono::steady_clock::now() - started > std::chrono::milliseconds(50);
            }), core::Cancelled);
        } else EXPECT_THROW(client->call(operation, Json::object()), std::exception);
        EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(2));
        EXPECT_THROW(client->tools(), std::runtime_error);
    }
}

TEST(McpClientTest, ClosingStdioAlsoTerminatesChildrenIgnoringSigterm) {
    const auto marker = std::filesystem::temp_directory_path() / ("sos-mcp-child-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    mcp::Credentials credentials;
    credentials.environment["SOS_MCP_TEST_MARKER"] = marker.string();
    auto client = localClient(1000, credentials);
    client->call("spawn_child", Json::object());
    client->close();
    std::this_thread::sleep_for(std::chrono::milliseconds(450));
    EXPECT_FALSE(std::filesystem::exists(marker));
    std::filesystem::remove(marker);
}
}
