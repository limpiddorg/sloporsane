#include <agent-runtime/common/sse_decoder.hpp>
#include <agent-runtime/providers/responses_stream_assembler.hpp>
#include <agent-runtime/providers/openai_provider.hpp>
#include <agent-runtime/conversation/conversation.hpp>
#include <gtest/gtest.h>

namespace {
using Json = nlohmann::json;

Json message(const std::string& id, const std::string& text) {
    return {{"type", "message"}, {"id", id}, {"role", "assistant"}, {"status", "completed"}, {"phase", "final_answer"},
        {"content", Json::array({{{"type", "output_text"}, {"text", text}, {"annotations", Json::array()}}})}};
}

Json itemDone(std::size_t index, const Json& item) {
    return {{"type", "response.output_item.done"}, {"output_index", index}, {"item", item}};
}

Json completed(Json output = Json::array()) {
    return {{"type", "response.completed"}, {"response", {{"id", "response-test"}, {"status", "completed"},
        {"output", std::move(output)}, {"usage", {{"input_tokens", 42}, {"output_tokens", 7}}},
        {"model", "gpt-6-astra"}, {"metadata", {{"test", "preserved"}}}}}};
}

std::string frame(const Json& event) { return "data: " + event.dump() + "\n\n"; }

TEST(ResponsesStreamAssemblerTest, ReconstructsRealCodexTitleShapeWithoutDuplicatingTextEvents) {
    const auto title = message("title", "Bouton de changement de thème");
    const auto stream = frame({{"type", "response.created"}}) +
        frame({{"type", "response.output_item.added"}, {"output_index", 0}, {"item", {{"id", "title"}, {"status", "in_progress"}}}}) +
        frame({{"type", "response.output_text.delta"}, {"output_index", 0}, {"delta", "Bouton de "}}) +
        frame({{"type", "response.output_text.done"}, {"output_index", 0}, {"text", "Bouton de changement de thème"}}) +
        frame(itemDone(0, title)) + frame(completed()) + "data: [DONE]\n\n";
    const auto result = completedResponsesEvent(stream);
    EXPECT_EQ(result.at("output"), Json::array({title}));
    EXPECT_EQ(result.at("usage"), completed().at("response").at("usage"));
    EXPECT_EQ(result.at("metadata"), completed().at("response").at("metadata"));
}

TEST(ResponsesStreamAssemblerTest, OrdersCompletedItemsAndPreservesReplayMetadata) {
    const Json reasoning{{"id", "reasoning"}, {"type", "reasoning"}, {"summary", Json::array()}, {"encrypted_content", "opaque-test"}};
    const auto text = message("text", "I will read the file.");
    const Json tool{{"type", "function_call"}, {"id", "tool"}, {"call_id", "call"}, {"name", "read"},
        {"arguments", R"({"filePath":"example.cpp"})"}, {"namespace", "functions"}, {"status", "completed"}};
    ResponsesStreamAssembler assembler;
    assembler.consume(itemDone(2, tool));
    assembler.consume(itemDone(0, reasoning));
    assembler.consume(itemDone(1, text));
    assembler.consume(itemDone(1, text));
    assembler.consume(completed());
    const auto result = assembler.finish();
    EXPECT_EQ(result.at("output"), Json::array({reasoning, text, tool}));
    EXPECT_EQ(assembler.finish(), result);
    Conversation conversation(ProviderFormat::OPENAI_RESPONSES);
    conversation.append(result);
    const auto& replay = conversation.getProviderFormat();
    ASSERT_EQ(replay.size(), 3);
    EXPECT_EQ(replay[0].at("encrypted_content"), "opaque-test");
    EXPECT_EQ(replay[1].at("phase"), "final_answer");
    EXPECT_EQ(replay[2].at("arguments"), tool.at("arguments"));
    EXPECT_EQ(replay[2].at("namespace"), "functions");
}

TEST(ResponsesStreamAssemblerTest, MergesFullAndPartialFinalOutputWithoutDuplicatingItems) {
    const auto first = message("first", "First");
    const auto second = message("second", "Second");
    for (const auto& finalOutput : {Json::array({first, second}), Json::array({second})}) {
        ResponsesStreamAssembler assembler;
        assembler.consume(itemDone(0, first));
        assembler.consume(itemDone(1, second));
        assembler.consume(completed(finalOutput));
        EXPECT_EQ(assembler.finish().at("output"), Json::array({first, second}));
    }
    ResponsesStreamAssembler finalOnly;
    finalOnly.consume(completed(Json::array({first, second})));
    EXPECT_EQ(finalOnly.finish(), completed(Json::array({first, second})).at("response"));
    ResponsesStreamAssembler missingDone;
    missingDone.consume(itemDone(1, second));
    missingDone.consume(completed(Json::array({first, second})));
    EXPECT_EQ(missingDone.finish().at("output"), Json::array({first, second}));
}

TEST(ResponsesStreamAssemblerTest, RejectsFailuresTruncationAndMissingItems) {
    for (const auto* failure : {"error", "response.failed", "response.incomplete", "response.cancelled"}) {
        ResponsesStreamAssembler assembler;
        assembler.consume(itemDone(0, message("text", "Partial answer")));
        EXPECT_THROW(assembler.consume({{"type", failure}, {"error", "private upstream details"}}), std::runtime_error);
        EXPECT_THROW(assembler.finish(), std::runtime_error);
    }
    ResponsesStreamAssembler truncated;
    truncated.consume(itemDone(0, message("text", "Partial answer")));
    EXPECT_THROW(truncated.finish(), std::runtime_error);
    ResponsesStreamAssembler missing;
    missing.consume({{"type", "response.output_item.added"}, {"output_index", 0}});
    missing.consume(completed());
    EXPECT_THROW(missing.finish(), std::runtime_error);
    ResponsesStreamAssembler sparse;
    sparse.consume(itemDone(1, message("text", "Missing index zero")));
    sparse.consume(completed());
    EXPECT_THROW(sparse.finish(), std::runtime_error);
    EXPECT_THROW(completedResponsesEvent("data: [DONE]\n\n"), std::runtime_error);
    EXPECT_THROW(completedResponsesEvent("data: {broken}\n\n"), std::runtime_error);
}

TEST(ResponsesStreamAssemblerTest, RejectsInvalidIndicesAndConflictingIdentities) {
    for (const auto& index : {Json(-1), Json(0.5), Json("0")}) {
        ResponsesStreamAssembler assembler;
        auto event = itemDone(0, message("text", "Text"));
        event["output_index"] = index;
        EXPECT_THROW(assembler.consume(event), std::runtime_error);
    }
    ResponsesStreamAssembler conflict;
    conflict.consume(itemDone(0, message("text", "Original")));
    EXPECT_THROW(conflict.consume(itemDone(0, message("different", "Other"))), std::runtime_error);
    ResponsesStreamAssembler duplicate;
    duplicate.consume(itemDone(0, message("same", "Original")));
    EXPECT_THROW(duplicate.consume(itemDone(1, message("same", "Original"))), std::runtime_error);
    ResponsesStreamAssembler incomplete;
    auto final = completed();
    final["response"]["status"] = "incomplete";
    EXPECT_THROW(incomplete.consume(final), std::runtime_error);
}

TEST(SseDecoderTest, HandlesCommentsMultilineFieldsBomAndLineEndings) {
    std::vector<std::string> data;
    forEachSseData("\xEF\xBB\xBF: heartbeat\r\nevent: test\r\nid: 1\r\ndata: {\r\ndata:  \"value\": 1}\r\n\r\n"
        "event: ignored\n\ndata\n\ndata: final\r\rdata: no-blank-line",
        [&](std::string_view value) { data.emplace_back(value); });
    EXPECT_EQ(data, (std::vector<std::string>{"{\n \"value\": 1}", "", "final", "no-blank-line"}));
}
}
