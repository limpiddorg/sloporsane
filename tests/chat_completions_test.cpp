#include <agent-runtime/providers/chat_completions_provider.hpp>
#include <agent-runtime/providers/generation_retry.hpp>
#include <agent-runtime/conversation/transformers/openai/openai_responses_api_message_provider_transformer.hpp>
#include <agent-runtime/tools/fs/read.hpp>
#include <gtest/gtest.h>

namespace {
using Json = nlohmann::json;

TEST(ChatCompletionsTest, ConvertsConversationAndMultipleCallsWithoutResponsesOptions) {
    const auto input = Json::array({
        {{"role", "system"}, {"content", "Instructions"}},
        {{"role", "user"}, {"content", "Read files"}},
        {{"type", "reasoning"}, {"encrypted_content", "opaque"}},
        {{"type", "message"}, {"role", "assistant"}, {"content", Json::array({{{"type", "output_text"}, {"text", "Reading"}}})}},
        {{"type", "function_call"}, {"call_id", "a"}, {"name", "read"}, {"arguments", "{}"}},
        {{"type", "function_call"}, {"call_id", "b"}, {"name", "read"}, {"arguments", "{}"}},
        {{"type", "function_call_output"}, {"call_id", "a"}, {"output", "file a"}},
        {{"type", "function_call_output"}, {"call_id", "b"}, {"output", "file b"}}
    });
    const auto request = ChatCompletionsProvider::prepareRequest(input, "local-model", {ReadTool{}.getDefinition()});
    ASSERT_EQ(request["messages"].size(), 5);
    EXPECT_EQ(request["messages"][2]["tool_calls"].size(), 2);
    EXPECT_EQ(request["messages"][3], (Json{{"role", "tool"}, {"tool_call_id", "a"}, {"content", "file a"}}));
    EXPECT_EQ(request["tools"][0]["function"]["name"], "read");
    EXPECT_FALSE(request.contains("reasoning"));
    EXPECT_FALSE(request.contains("include"));
    EXPECT_FALSE(request.contains("store"));
    EXPECT_FALSE(request["stream"].get<bool>());
}

TEST(ChatCompletionsTest, ConvertsImagesAndToolAttachments) {
    const auto request = ChatCompletionsProvider::prepareRequest(Json::array({
        {{"role", "user"}, {"content", Json::array({{{"type", "input_image"}, {"image_url", "data:image/png;base64,AA"}}})}},
        {{"type", "function_call_output"}, {"call_id", "a"}, {"output", Json::array({
            {{"type", "input_text"}, {"text", "image"}}, {{"type", "input_image"}, {"image_url", "https://example.test/image.png"}}})}},
        {{"type", "function_call_output"}, {"call_id", "b"}, {"output", "second result"}}
    }), "model", {});
    EXPECT_EQ(request["messages"][0]["content"][0]["image_url"]["url"], "data:image/png;base64,AA");
    EXPECT_EQ(request["messages"][1]["role"], "tool");
    EXPECT_EQ(request["messages"][1]["content"], "image");
    EXPECT_EQ(request["messages"][2]["role"], "tool");
    EXPECT_EQ(request["messages"][2]["content"], "second result");
    EXPECT_EQ(request["messages"][3]["role"], "user");
}

TEST(ChatCompletionsTest, NormalizesTextToolsAndUsageAndReplaysThem) {
    const auto response = Json::parse(R"({"id":"chat-1","choices":[{"finish_reason":"tool_calls","message":{"role":"assistant","content":"Looking","tool_calls":[{"id":"call-1","type":"function","function":{"name":"read","arguments":"{\"filePath\":\"README.md\"}"}}]}}],"usage":{"prompt_tokens":42,"completion_tokens":10}})");
    const auto normalized = ChatCompletionsProvider::normalizeResponse(response);
    OpenAiResponsesApiMessageProviderTransformer transformer;
    const auto message = transformer.fromProviderFormat(normalized);
    ASSERT_EQ(message.content.size(), 2);
    const auto replay = ChatCompletionsProvider::prepareRequest(transformer.toProviderFormat(message), "model", {});
    EXPECT_EQ(replay["messages"][0]["tool_calls"][0]["id"], "call-1");
    ChatCompletionsProvider provider("http://localhost:1234/v1", "", "Local");
    EXPECT_EQ(provider.getUsedContext(normalized), 52);
    EXPECT_EQ(provider.getOutputTokens(normalized), 10);
    EXPECT_FALSE(provider.getContextLimit("model"));
    EXPECT_TRUE(provider.getReasoningEfforts("model").empty());
    EXPECT_EQ(provider.getModelInfo("model").imageInput, ImageSupport::UNKNOWN);
}

TEST(ChatCompletionsTest, RejectsPartialAndMalformedCompletions) {
    EXPECT_THROW(ChatCompletionsProvider::normalizeResponse(Json::object()), RetryableGenerationError);
    EXPECT_THROW(ChatCompletionsProvider::normalizeResponse({{"choices", Json::array()}}), RetryableGenerationError);
    for (const auto reason : {"length", "content_filter", ""})
        EXPECT_THROW(ChatCompletionsProvider::normalizeResponse({{"choices", Json::array({{{"finish_reason", reason},
            {"message", {{"content", "Partial"}}}}})}}), std::runtime_error);
    EXPECT_THROW(ChatCompletionsProvider::normalizeResponse({{"choices", Json::array({{{"finish_reason", "stop"},
        {"message", {{"content", nullptr}}}}})}}), std::runtime_error);
}

TEST(ChatCompletionsTest, RetriesMalformedOutputAndUnknownApiFailures) {
    const auto success = Json::parse(R"({"choices":[{"finish_reason":"stop","message":{"content":"OK"}}]})");
    const std::vector<Json> failures{
        {{"error", {{"message", "private diagnostic"}}}},
        {{"choices", Json::array({{{"finish_reason", "stop"}}})}},
        {{"choices", Json::array({{{"finish_reason", "stop"}, {"message", {{"content", 42}}}}})}},
        {{"choices", Json::array({{{"finish_reason", "stop"}, {"message", {{"content", nullptr}}}}})}},
        {{"choices", Json::array({{{"finish_reason", ""}, {"message", {{"content", "Partial"}}}}})}},
        {{"choices", Json::array({{{"finish_reason", "tool_calls"}, {"message", {{"tool_calls", Json::array({
            {{"type", "function"}, {"id", ""}}})}}}}})}}
    };
    for (const auto& failure : failures) {
        int calls = 0;
        const auto response = generateWithRetry([&] {
            return ChatCompletionsProvider::normalizeResponse(++calls == 1 ? failure : success);
        }, {}, {}, [](auto) { FAIL() << "First retry should be immediate"; });
        EXPECT_EQ(calls, 2);
        EXPECT_EQ(response["output"][0]["content"][0]["text"], "OK");
    }
}

TEST(ChatCompletionsTest, DoesNotRetryQuotaOrCompletionLimits) {
    for (const auto& failure : std::vector<Json>{
        {{"error", {{"code", "insufficient_quota"}}}},
        {{"choices", Json::array({{{"finish_reason", "length"}, {"message", {{"content", "Partial"}}}}})}},
        {{"choices", Json::array({{{"finish_reason", "content_filter"}, {"message", {{"content", "Partial"}}}}})}}
    }) {
        int calls = 0;
        EXPECT_THROW(generateWithRetry([&] {
            ++calls;
            return ChatCompletionsProvider::normalizeResponse(failure);
        }, {}, [](auto) { FAIL() << "Permanent error retried"; }), std::runtime_error);
        EXPECT_EQ(calls, 1);
    }
}

TEST(ChatCompletionsTest, ValidatesBaseUrlWithoutSendingSecretsInUrl) {
    EXPECT_EQ(ChatCompletionsProvider::normalizeBaseUrl("http://localhost:1234/v1/"), "http://localhost:1234/v1");
    EXPECT_EQ(ChatCompletionsProvider::normalizeBaseUrl("https://server.test/api"), "https://server.test/api");
    for (const auto url : {"", "ftp://server", "https://", "https:///v1", "https://user:secret@server/v1", "https://server/v1?key=secret",
        "https://server/v1/chat/completions", "https://server/v1/models", "https://server/v1/responses", "http://host\nHeader"})
        EXPECT_THROW(ChatCompletionsProvider::normalizeBaseUrl(url), std::invalid_argument);
}
}
