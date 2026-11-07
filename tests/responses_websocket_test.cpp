#include <agent-runtime/providers/responses_websocket.hpp>
#include <agent-runtime/providers/generation_retry.hpp>
#include <agent-runtime/conversation/transformers/openai/openai_responses_api_message_provider_transformer.hpp>
#include <ixwebsocket/IXWebSocketServer.h>
#include <ixwebsocket/IXGetFreePort.h>
#include <gtest/gtest.h>
#include <mutex>
#include <atomic>
#include <future>
#include <thread>

namespace {
using Json = nlohmann::json;
using namespace std::chrono_literals;
class WebSocketFixture : public testing::Test {
protected:
    ix::WebSocketServer server{ix::getFreePort(), "127.0.0.1"};
    ResponsesWebSocket client;
    std::mutex mutex;
    std::vector<Json> requests;
    std::atomic<int> connections = 0;
    std::atomic<bool> missing = false, stall = false, fail = false;
    std::string url;
    const cpr::Header headers{{"session-id", "test"}};
    ProviderRequestTimeouts timeouts{2s, 1s};
    Json request = {{"model", "test"}, {"stream", true}, {"store", false}, {"instructions", "Stable rules"},
        {"input", Json::array({{{"role", "user"}, {"content", "Hello"}}})}};
    void SetUp() override {
        server.disablePerMessageDeflate();
        server.setOnClientMessageCallback([this](auto, ix::WebSocket& socket, const ix::WebSocketMessagePtr& message) {
            if (message->type == ix::WebSocketMessageType::Open) { ++connections; return; }
            if (message->type != ix::WebSocketMessageType::Message) return;
            const auto payload = Json::parse(message->str);
            std::size_t number;
            {
                std::lock_guard lock(mutex);
                requests.push_back(payload);
                number = requests.size();
            }
            if (stall) return;
            if (fail) { socket.close(); return; }
            if (missing && payload.contains("previous_response_id")) {
                socket.sendText(Json{{"type", "error"}, {"error", {{"code", "previous_response_not_found"}}}}.dump());
                return;
            }
            const Json content = {{"type", "output_text"}, {"text", "Answer"}, {"annotations", Json::array()}};
            const Json item = {{"type", "message"}, {"id", "message-" + std::to_string(number)},
                {"role", "assistant"}, {"status", "completed"}, {"content", Json::array({content})}};
            const Json response = {{"id", "response-" + std::to_string(number)}, {"status", "completed"},
                {"output", Json::array({item})}};
            socket.sendText(Json{{"type", "response.completed"}, {"response", response}}.dump());
        });
        ASSERT_TRUE(server.listen().first);
        server.start();
        url = "http://127.0.0.1:" + std::to_string(server.getPort()) + "/responses";
    }
    void TearDown() override { client.reset(); server.stop(); }
    Json generate(core::CancellationCheck cancelled = {}) {
        auto response = client.generate(url, headers, request, cancelled, timeouts);
        EXPECT_TRUE(response.has_value());
        return response.value_or(Json::object());
    }
    void appendAnswer(const Json& response) {
        OpenAiResponsesApiMessageProviderTransformer transformer;
        for (const auto& item : transformer.toProviderFormat(transformer.fromProviderFormat(response)))
            request["input"].push_back(item);
        request["input"].push_back({{"role", "user"}, {"content", "Continue"}});
    }
    std::vector<Json> captured() { std::lock_guard lock(mutex); return requests; }
};

TEST_F(WebSocketFixture, ReusesConnectionAndSendsOnlyNewItemsForMatchingContext) {
    appendAnswer(generate());
    generate();
    const auto sent = captured();
    ASSERT_EQ(sent.size(), 2);
    EXPECT_EQ(connections.load(), 1);
    EXPECT_EQ(sent[0]["type"], "response.create");
    EXPECT_FALSE(sent[0].contains("stream"));
    EXPECT_FALSE(sent[0].contains("previous_response_id"));
    EXPECT_EQ(sent[1]["previous_response_id"], "response-1");
    ASSERT_EQ(sent[1]["input"].size(), 1);
    EXPECT_EQ(sent[1]["input"][0]["content"], "Continue");
    EXPECT_EQ(client.statistics().continuations, 1);
}

TEST_F(WebSocketFixture, AppendedSystemAndDeveloperAnnouncementsPreserveContinuation) {
    request["input"].insert(request["input"].begin(), {{"role", "system"}, {"content", "Base rules"}});
    appendAnswer(generate());
    request["input"].push_back({{"role", "system"}, {"content", "Execution phase"}});
    request["input"].push_back({{"role", "developer"}, {"content", "Permission update"}});
    const auto history = request;
    generate();
    EXPECT_EQ(request, history);
    const auto sent = captured();
    ASSERT_EQ(sent.size(), 2);
    EXPECT_EQ(sent[1]["previous_response_id"], "response-1");
    EXPECT_EQ(sent[1]["instructions"], sent[0]["instructions"]);
    ASSERT_EQ(sent[1]["input"].size(), 3);
    EXPECT_EQ(sent[1]["input"][1], request["input"][request["input"].size() - 2]);
    EXPECT_EQ(sent[1]["input"][2], request["input"].back());
}

TEST_F(WebSocketFixture, ChangedInstructionsAndRewrittenHistoryRestartWithFullContext) {
    appendAnswer(generate());
    request["instructions"] = "Changed rules";
    appendAnswer(generate());
    request["input"][0]["content"] = "Edited earlier message";
    generate();
    const auto sent = captured();
    ASSERT_EQ(sent.size(), 3);
    EXPECT_FALSE(sent[1].contains("previous_response_id"));
    EXPECT_FALSE(sent[2].contains("previous_response_id"));
    EXPECT_EQ(sent[2]["input"], request["input"]);
}

TEST_F(WebSocketFixture, MissingServerStateRetriesOnceWithFullContext) {
    appendAnswer(generate());
    missing = true;
    generate();
    const auto sent = captured();
    ASSERT_EQ(sent.size(), 3);
    EXPECT_TRUE(sent[1].contains("previous_response_id"));
    EXPECT_FALSE(sent[2].contains("previous_response_id"));
    EXPECT_EQ(sent[2]["input"], request["input"]);
}

TEST_F(WebSocketFixture, ChangedIdentityReconnectsAndDoesNotReusePreviousState) {
    appendAnswer(generate());
    auto changed = headers;
    changed["session-id"] = "other";
    ASSERT_TRUE(client.generate(url, changed, request, {}, timeouts));
    const auto sent = captured();
    ASSERT_EQ(sent.size(), 2);
    EXPECT_EQ(connections.load(), 2);
    EXPECT_FALSE(sent[1].contains("previous_response_id"));
}

TEST_F(WebSocketFixture, SilentGenerationTimesOutAndCancellationStaysDistinct) {
    stall = true;
    timeouts = {100ms, 50ms};
    EXPECT_THROW(generate(), RetryableGenerationError);
    EXPECT_THROW(generate([] { return true; }), core::Cancelled);
}

TEST_F(WebSocketFixture, DroppedGenerationIsNotSilentlyReplayed) {
    fail = true;
    EXPECT_THROW(generate(), RetryableGenerationError);
    EXPECT_EQ(captured().size(), 1);
}

TEST_F(WebSocketFixture, CancellationClosesInflightConnectionAndNextCallStartsFresh) {
    stall = true;
    std::atomic<bool> cancelled = false;
    auto running = std::async(std::launch::async, [&] { return generate([&] { return cancelled.load(); }); });
    for (int i = 0; i < 200 && captured().empty(); ++i) std::this_thread::sleep_for(5ms);
    ASSERT_EQ(captured().size(), 1);
    cancelled = true;
    EXPECT_THROW(running.get(), core::Cancelled);
    stall = false;
    generate();
    EXPECT_EQ(connections.load(), 2);
    EXPECT_FALSE(captured().back().contains("previous_response_id"));
}
}
