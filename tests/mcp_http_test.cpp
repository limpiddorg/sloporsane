#include <daemon/mcp/http_transport.hpp>
#include <daemon/mcp/client.hpp>
#include <daemon/mcp/stdio_transport.hpp>
#include <gtest/gtest.h>

namespace {
using Json = nlohmann::json;
class McpHttpTest : public testing::Test {
protected:
    std::unique_ptr<mcp::StdioTransport> server;
    std::string base;
    void SetUp() override {
        const auto fixture = std::filesystem::path(__FILE__).parent_path() / "fixtures/mcp_http.py";
        server = std::make_unique<mcp::StdioTransport>(mcp::Local{{"python3", fixture.string()}, {}, {}},
            std::filesystem::temp_directory_path());
        const auto response = server->receive(deadline(), {});
        base = "http://127.0.0.1:" + std::to_string(response.at("port").get<int>());
    }
    mcp::Deadline deadline() { return std::chrono::steady_clock::now() + std::chrono::seconds(3); }
    std::unique_ptr<mcp::Client> client(const std::string& path, int timeout = 1000, std::string token = {}) {
        auto transport = std::make_unique<mcp::HttpTransport>(mcp::Remote{base + path}, std::move(token));
        auto result = std::make_unique<mcp::Client>(std::move(transport), timeout);
        result->initialize(2000);
        return result;
    }
    Json requests() {
        server->send({{"stats", true}}, deadline(), {});
        return server->receive(deadline(), {}).at("requests");
    }
};

TEST_F(McpHttpTest, JsonSessionHeadersAndExplicitBearerAreSentWithoutReplaying) {
    auto connection = client("/token", 1000, "fixture-token");
    ASSERT_EQ(connection->tools().size(), 1);
    EXPECT_EQ(connection->call("read", Json::object()).at("content").at(0).at("text"), "fixture result");
    EXPECT_EQ(connection->call("error", Json::object()).at("isError"), true);
    EXPECT_NO_THROW(connection->tools());
    const auto recorded = requests();
    ASSERT_EQ(recorded.size(), 6);
    EXPECT_FALSE(recorded[0]["headers"].contains("Mcp-Session-Id"));
    for (std::size_t index = 0; index < recorded.size(); ++index) {
        EXPECT_EQ(recorded[index]["headers"]["Authorization"], "Bearer fixture-token");
        if (index == 0) continue;
        EXPECT_EQ(recorded[index]["headers"]["Mcp-Session-Id"], "fixture-session");
        EXPECT_EQ(recorded[index]["headers"]["MCP-Protocol-Version"], "2025-11-25");
    }
}

TEST_F(McpHttpTest, ExplicitDisconnectTerminatesSessionOnceAndAcceptsUnsupportedDelete) {
    auto connection = client("/token", 1000, "fixture-token");
    connection->disconnect();
    auto recorded = requests();
    ASSERT_EQ(recorded.size(), 3);
    EXPECT_EQ(recorded.back()["message"]["method"], "DELETE");
    EXPECT_EQ(recorded.back()["headers"]["Authorization"], "Bearer fixture-token");
    EXPECT_EQ(recorded.back()["headers"]["Mcp-Session-Id"], "fixture-session");
    EXPECT_EQ(recorded.back()["headers"]["MCP-Protocol-Version"], "2025-11-25");
    connection->disconnect();
    EXPECT_EQ(requests().size(), 3);
    auto unsupported = client("/delete-unsupported");
    EXPECT_NO_THROW(unsupported->disconnect());
    auto failed = client("/delete-failed");
    EXPECT_THROW(failed->disconnect(), std::runtime_error);
    EXPECT_THROW(failed->tools(), std::runtime_error);
}

TEST_F(McpHttpTest, NegotiatedModernVersionIsUsedForCallsAndTermination) {
    auto connection = client("/modern");
    EXPECT_EQ(connection->tools().size(), 1);
    EXPECT_NO_THROW(connection->call("read", Json::object()));
    connection->disconnect();
    const auto recorded = requests();
    EXPECT_EQ(recorded.front()["message"]["params"]["protocolVersion"], "2025-11-25");
    for (std::size_t index = 1; index < recorded.size(); ++index)
        EXPECT_EQ(recorded[index]["headers"]["MCP-Protocol-Version"], "2025-11-25");
}

TEST_F(McpHttpTest, StreamingInitializationAndPingDoNotWaitForTheStreamToClose) {
    const auto started = std::chrono::steady_clock::now();
    auto connection = client("/sse");
    ASSERT_EQ(connection->tools().size(), 1);
    EXPECT_NO_THROW(connection->call("ping", Json::object()));
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(3));
    const auto recorded = requests();
    ASSERT_EQ(recorded.size(), 5);
    EXPECT_EQ(recorded.back()["message"]["id"], "server-ping");
    EXPECT_TRUE(recorded.back()["message"].contains("result"));
    EXPECT_EQ(recorded.back()["headers"]["Mcp-Session-Id"], "fixture-session");
}

TEST_F(McpHttpTest, RedirectsAndAuthenticationFailuresDoNotLeakSecretsOrRetry) {
    for (const auto& path : {"/redirect", "/auth"}) {
        const auto before = requests().size();
        try {
            auto connection = client(path);
            FAIL() << "Request should have failed";
        } catch (const std::runtime_error& error) {
            EXPECT_EQ(std::string(error.what()).find("secret diagnostic"), std::string::npos);
        }
        EXPECT_EQ(requests().size(), before + 1);
    }
    EXPECT_THROW(mcp::HttpTransport(mcp::Remote{base + "/token"}, "bad\r\nheader"), std::invalid_argument);
}

TEST_F(McpHttpTest, NotionChallengeRemainsAnAuthenticationRequest) {
    try {
        client("/notion-challenge");
        FAIL() << "Authentication should be required";
    } catch (const mcp::AuthenticationRequired& error) {
        EXPECT_EQ(error.challenge.metadata, "https://mcp.notion.com/.well-known/oauth-protected-resource/mcp");
    }
    EXPECT_EQ(requests().size(), 1);
}

TEST_F(McpHttpTest, HttpFailureExposesOnlyTheStatusNotTheResponseBody) {
    try {
        client("/unavailable");
        FAIL() << "Connection should fail";
    } catch (const mcp::TransportError& error) {
        EXPECT_EQ(std::string(error.what()), "Unexpected MCP HTTP status 503");
    }
    EXPECT_EQ(requests().size(), 1);
}

TEST_F(McpHttpTest, StreamingAcceptsCarriageReturnsAndBomButRejectsTruncatedEvents) {
    auto connection = client("/sse-cr");
    EXPECT_EQ(connection->tools().size(), 1);
    EXPECT_NO_THROW(connection->call("read", Json::object()));
    EXPECT_THROW(connection->call("truncated", Json::object()), std::runtime_error);
    EXPECT_THROW(connection->tools(), std::runtime_error);
}

TEST_F(McpHttpTest, StreamingCompletionStopsWithoutParsingTheRemainingBufferOrWaitingForClosure) {
    const auto started = std::chrono::steady_clock::now();
    auto connection = client("/sse-tail");
    EXPECT_EQ(connection->tools().size(), 1);
    EXPECT_EQ(connection->call("read", Json::object()).at("content").at(0).at("text"), "fixture result");
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(3));
    EXPECT_EQ(requests().size(), 4);
}

TEST_F(McpHttpTest, StreamingReceiveErrorBeforeCompletionStillFailsWithoutRetry) {
    auto connection = client("/sse");
    const auto before = requests().size();
    // Depending on socket shutdown timing, curl sees a reset or an EOF with
    // an incomplete event. Both must fail and invalidate the connection.
    EXPECT_THROW(connection->call("reset", Json::object()), std::runtime_error);
    EXPECT_THROW(connection->tools(), std::runtime_error);
    EXPECT_EQ(requests().size(), before + 1);
}

TEST_F(McpHttpTest, InterruptedOrInvalidCallsCloseTheConnectionAndNeverRetry) {
    for (const auto* operation : {"hang", "cancel", "disconnect", "missing", "malformed", "wrong-type", "wrong-session", "oversized"}) {
        SCOPED_TRACE(operation);
        auto connection = client("/json", 200);
        const auto before = requests().size();
        const auto started = std::chrono::steady_clock::now();
        if (std::string(operation) == "cancel") {
            EXPECT_THROW(connection->call("hang", Json::object(), [started] {
                return std::chrono::steady_clock::now() - started > std::chrono::milliseconds(50);
            }), core::Cancelled);
        } else EXPECT_THROW(connection->call(operation, Json::object()), std::exception);
        EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(2));
        EXPECT_THROW(connection->tools(), std::runtime_error);
        EXPECT_EQ(requests().size(), before + 1);
    }
}
}
