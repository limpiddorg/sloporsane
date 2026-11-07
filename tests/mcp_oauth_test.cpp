#include <daemon/mcp/oauth.hpp>
#include <daemon/mcp/oauth_http.hpp>
#include <daemon/mcp/oauth_credentials.hpp>
#include <daemon/mcp/stdio_transport.hpp>
#include <daemon/mcp/manager.hpp>
#include <daemon/mcp/connections.hpp>
#include <gtest/gtest.h>
#include <cstdlib>

namespace {
using Json = nlohmann::json;
namespace fs = std::filesystem;
class McpOAuthTest : public testing::Test {
protected:
    fs::path root;
    std::map<std::string, std::optional<std::string>> environment;
    std::unique_ptr<mcp::StdioTransport> fixture;
    std::string base;
    void SetUp() override {
        root = fs::temp_directory_path() / ("sos-oauth-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root);
        for (const auto* key : {"XDG_DATA_HOME", "XDG_CONFIG_HOME"}) {
            auto previous = std::getenv(key);
            environment[key] = previous ? std::optional<std::string>(previous) : std::nullopt;
            setenv(key, (root / key).c_str(), 1);
        }
        const auto script = fs::path(__FILE__).parent_path() / "fixtures/mcp_oauth.py";
        fixture = std::make_unique<mcp::StdioTransport>(mcp::Local{{"python3", script.string()}, {}, {}}, root);
        base = fixture->receive(std::chrono::steady_clock::now() + std::chrono::seconds(5), {}).at("url");
    }
    void TearDown() override {
        fixture.reset();
        for (const auto& [key, previous] : environment) {
            if (previous) setenv(key.c_str(), previous->c_str(), 1); else unsetenv(key.c_str());
        }
        fs::remove_all(root);
    }
    mcp::Remote remote() { return {base + "/mcp", mcp::Authentication::OAUTH}; }
    Json get(const std::string& url) { return mcp::oauth::request(url, "", "", {}).body; }
    void mode(const std::string& value) { get(base + "/control?mode=" + value); }
};

TEST_F(McpOAuthTest, BrowserPkceRegistrationRefreshAndRealMcpCall) {
    auto flow = mcp::startOAuth(remote());
    EXPECT_NE(flow->url.find("code_challenge_method=S256"), std::string::npos);
    EXPECT_EQ(get(flow->url).at("callback_status"), 200);
    auto credentials = flow->completion.get();
    EXPECT_EQ(credentials.refreshToken, "initial-refresh");
    EXPECT_EQ(credentials.tokenEndpoint, base + "/token");
    mcp::Manager manager(root);
    mcp::Server server{"fixture", mcp::Scope::WORKSPACE, remote(), true};
    manager.save(server);
    const auto identity = manager.list().servers.front().identity;
    manager.trust("fixture", identity, true);
    manager.authenticate("fixture", identity, credentials);
    manager.connect("fixture"); // refresh before initialize, not after a failed call
    const auto status = manager.list().servers.front();
    ASSERT_EQ(status.tools.size(), 1);
    EXPECT_EQ(manager.call("fixture", identity, status.tools.front(), Json::object())["content"][0]["text"], "OAuth MCP result");
    const auto stored = mcp::AuthStore().get(identity);
    ASSERT_TRUE(stored);
    EXPECT_EQ(stored->refreshToken, "rotated-refresh");
    EXPECT_EQ(get(base + "/stats")["refreshes"], 1);
    EXPECT_EQ(get(base + "/stats")["calls"], 1);
}

TEST_F(McpOAuthTest, AutomaticAuthenticationWaitsForExplicitOAuthThenRefreshesCredentials) {
    auto server = Json{{"url", base + "/mcp"}}.get<mcp::Server>();
    server.name = "fixture";
    server.scope = mcp::Scope::WORKSPACE;
    mcp::Manager manager(root);
    manager.save(server);
    const auto identity = manager.list().servers.front().identity;
    manager.trust(server.name, identity, true);
    EXPECT_THROW(manager.connect(server.name), mcp::AuthenticationRequired);
    const auto status = manager.list().servers.front();
    EXPECT_EQ(status.state, mcp::ConnectionState::NEEDS_AUTH);
    EXPECT_FALSE(mcp::AuthStore().get(identity));
    auto flow = mcp::startOAuth(std::get<mcp::Remote>(server.connection), {}, {}, {}, status.authorization);
    get(flow->url);
    manager.authenticate(server.name, identity, flow->completion.get());
    manager.connect(server.name);
    const auto connected = manager.list().servers.front();
    ASSERT_EQ(connected.state, mcp::ConnectionState::CONNECTED);
    ASSERT_EQ(connected.tools.size(), 1);
    EXPECT_NO_THROW(manager.call(server.name, identity, connected.tools.front(), Json::object()));
    EXPECT_EQ(get(base + "/stats")["refreshes"], 1);
    EXPECT_EQ(get(base + "/stats")["calls"], 1);
}

class OAuthCallingProvider : public Provider {
public:
    bool sawResult = false;
    ProviderFormat getFormat() const override { return ProviderFormat::OPENAI_RESPONSES; }
    Json generate(const Json& input, const std::string&, const std::vector<::ToolDefinition>& tools, const std::string&) override {
        if (first) {
            first = false;
            if (tools.size() != 1) throw std::runtime_error("Expected one authenticated MCP tool");
            return {{"id", "response"}, {"status", "completed"}, {"output", Json::array({
                {{"type", "function_call"}, {"id", "item"}, {"call_id", "call"}, {"name", tools.front().id}, {"arguments", "{}"}}})}};
        }
        sawResult = input.dump().find("OAuth MCP result") != std::string::npos;
        return {{"id", "done"}, {"status", "completed"}, {"output", Json::array()}};
    }
private:
    bool first = true;
};

TEST_F(McpOAuthTest, AgentUsesActualOAuthHttpTransportAndReturnsResultToTheProvider) {
    auto flow = mcp::startOAuth(remote());
    get(flow->url);
    mcp::Connections connections;
    auto manager = connections.forWorkspace(root);
    manager->save({"fixture", mcp::Scope::WORKSPACE, remote(), true});
    const auto identity = manager->list().servers.front().identity;
    manager->trust("fixture", identity, true);
    manager->authenticate("fixture", identity, flow->completion.get());
    auto provider = std::make_unique<OAuthCallingProvider>();
    auto* observed = provider.get();
    ToolContext context{root, root};
    int approvals = 0;
    context.ask = [&](const auto&) { ++approvals; };
    Agent agent(std::move(provider), "fixture-model", {}, context);
    connections.bind(agent, root);
    agent.append(Message{"user", MessageRole::USER, {TextContent{"Use the authenticated tool"}}});
    agent.run().get();
    EXPECT_TRUE(observed->sawResult);
    EXPECT_EQ(approvals, 1);
    EXPECT_EQ(get(base + "/stats")["calls"], 1);
    EXPECT_EQ(get(base + "/stats")["refreshes"], 1);
}

TEST_F(McpOAuthTest, ChallengeMetadataAndOidcPathInsertionAreSupported) {
    for (const auto* variant : {"challenge", "oidc", "oidc-append", "well-known"}) {
        mode(variant);
        auto flow = mcp::startOAuth(remote());
        EXPECT_NE(flow->url.find("scope=read"), std::string::npos);
        EXPECT_EQ(flow->url.find("excessive-scope"), std::string::npos);
        get(flow->url);
        EXPECT_EQ(flow->completion.get().revocationEndpoint, base + "/revoke");
    }
}

TEST_F(McpOAuthTest, InsufficientScopeRequiresExplicitStepUpWithoutReplayingTheRejectedCall) {
    mode("stepup");
    auto flow = mcp::startOAuth(remote());
    get(flow->url);
    mcp::Manager manager(root);
    manager.save({"fixture", mcp::Scope::WORKSPACE, remote(), true});
    const auto identity = manager.list().servers.front().identity;
    manager.trust("fixture", identity, true);
    manager.authenticate("fixture", identity, flow->completion.get());
    manager.connect("fixture");
    auto current = manager.list().servers.front();
    EXPECT_THROW(manager.call("fixture", identity, current.tools.front(), Json::object()), mcp::AuthenticationRequired);
    current = manager.list().servers.front();
    EXPECT_EQ(current.state, mcp::ConnectionState::NEEDS_AUTH);
    EXPECT_EQ(current.authorization.scope, "write");
    EXPECT_EQ(get(base + "/stats")["denied"], 1);
    EXPECT_EQ(get(base + "/stats")["calls"], 0);
    auto stepup = mcp::startOAuth(remote(), {}, {}, *mcp::AuthStore().get(identity), current.authorization);
    EXPECT_NE(stepup->url.find("scope=write%20read"), std::string::npos);
    get(stepup->url);
    manager.authenticate("fixture", identity, stepup->completion.get());
    manager.connect("fixture");
    current = manager.list().servers.front();
    EXPECT_TRUE(current.authorization.scope.empty());
    EXPECT_NO_THROW(manager.call("fixture", identity, current.tools.front(), Json::object()));
    EXPECT_EQ(get(base + "/stats")["denied"], 1);
    EXPECT_EQ(get(base + "/stats")["calls"], 1);
}

TEST_F(McpOAuthTest, PreregisteredConfidentialClientsUseBasicOrPostAndTokensCanBeRevoked) {
    for (const auto* method : {"basic", "post"}) {
        mode(method);
        auto settings = remote();
        settings.clientId = "fixture-client";
        mcp::Credentials registered;
        registered.clientId = settings.clientId;
        registered.clientSecret = "fixture-secret";
        registered.tokenAuthMethod = std::string("client_secret_") + method;
        auto flow = mcp::startOAuth(settings, {}, {}, registered);
        get(flow->url);
        auto credentials = flow->completion.get();
        const std::string identity(64, method[0] == 'b' ? 'b' : 'c');
        mcp::AuthStore store;
        store.trust(identity, true);
        store.set(identity, credentials);
        mcp::OAuthCredentials provider(store, identity);
        EXPECT_EQ(provider.bearer({}), "fresh-access");
        provider.revoke({});
        EXPECT_TRUE(store.trusted(identity));
        EXPECT_TRUE(store.get(identity)->accessToken.empty());
        EXPECT_TRUE(store.get(identity)->refreshToken.empty());
        EXPECT_THROW(provider.bearer({}), mcp::AuthenticationRequired);
    }
    EXPECT_EQ(get(base + "/stats")["registrations"], 0);
    EXPECT_EQ(get(base + "/stats")["revocations"], 4);
}

TEST(McpOAuthProtocolTest, ChallengeParserHandlesSchemesQuotedCommasEscapesAndRejectsConflicts) {
    const auto fields = mcp::oauth::bearerChallenge({"Basic realm=\"a,b\", bEaReR resource_metadata=\"https://example.test/meta\", scope=\"read write\""});
    EXPECT_EQ(fields.at("scope"), "read write");
    EXPECT_EQ(fields.at("resource_metadata"), "https://example.test/meta");
    EXPECT_THROW(mcp::oauth::bearerChallenge({"Bearer scope=\"a\"", "Bearer scope=\"b\""}), std::runtime_error);
    EXPECT_THROW(mcp::oauth::bearerChallenge({"Bearer scope=\"unfinished"}), std::runtime_error);
    EXPECT_EQ(mcp::oauth::basicAuthorization("a", "b"), "Basic YTpi");
}

TEST_F(McpOAuthTest, InvalidIssuerOrMissingPkcePreventAuthorization) {
    mode("bad-issuer");
    EXPECT_THROW(mcp::startOAuth(remote()), std::runtime_error);
    mode("no-pkce");
    EXPECT_THROW(mcp::startOAuth(remote()), std::runtime_error);
    EXPECT_EQ(get(base + "/stats")["registrations"], 0);
}

TEST_F(McpOAuthTest, WrongStateCannotCompleteAndCancellationClosesFlow) {
    mode("wrong-state");
    auto flow = mcp::startOAuth(remote());
    EXPECT_EQ(get(flow->url).at("callback_status"), 400);
    EXPECT_EQ(flow->completion.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout);
    flow->cancelled->store(true);
    EXPECT_THROW(flow->completion.get(), core::Cancelled);
}

TEST_F(McpOAuthTest, DenialAndInvalidPkceNeverIssueTokens) {
    for (const auto* value : {"deny", "wrong-pkce"}) {
        mode(value);
        auto flow = mcp::startOAuth(remote());
        get(flow->url);
        EXPECT_THROW(flow->completion.get(), std::runtime_error);
    }
}

TEST_F(McpOAuthTest, ConcurrentRefreshRotatesOnceAndRevokedTrustCannotBeOverwritten) {
    auto flow = mcp::startOAuth(remote());
    get(flow->url);
    const auto previous = flow->completion.get();
    const auto identity = std::string(64, 'a');
    mcp::AuthStore store;
    store.trust(identity, true);
    store.set(identity, previous);
    mcp::OAuthCredentials first(store, identity), second(store, identity);
    auto a = std::async(std::launch::async, [&] { return first.bearer({}); });
    auto b = std::async(std::launch::async, [&] { return second.bearer({}); });
    EXPECT_EQ(a.get(), "fresh-access");
    EXPECT_EQ(b.get(), "fresh-access");
    EXPECT_EQ(get(base + "/stats")["refreshes"], 1);
    store.trust(identity, false);
    EXPECT_THROW(store.replaceIfCurrent(identity, *store.get(identity), previous), std::runtime_error);
    EXPECT_THROW(first.bearer({}), mcp::AuthenticationRequired);
}

TEST_F(McpOAuthTest, RefreshRejectionPreservesStoredCredentialsWithoutReplay) {
    auto flow = mcp::startOAuth(remote());
    get(flow->url);
    const auto previous = flow->completion.get();
    mode("refresh-denied");
    const auto identity = std::string(64, 'b');
    mcp::AuthStore store;
    store.trust(identity, true);
    store.set(identity, previous);
    mcp::OAuthCredentials provider(store, identity);
    EXPECT_THROW(provider.bearer({}), mcp::AuthenticationRequired);
    EXPECT_EQ(*store.get(identity), previous);
    EXPECT_EQ(get(base + "/stats")["calls"], 0);
}
}
