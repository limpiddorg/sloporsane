#include <agent-runtime/conversation/transformers/openai/openai_responses_api_conversation_provider_transformer.hpp>
#include <agent-runtime/conversation/transformers/openai/openai_responses_api_message_provider_transformer.hpp>
#include <agent-runtime/conversation/messages/message_json.hpp>

#include <gtest/gtest.h>
#include <stdexcept>
#include <type_traits>

namespace {

using nlohmann::json;

class ResponsesTransformerTest : public testing::Test {
protected:
    OpenAiResponsesApiMessageProviderTransformer transformer;
    const json response = json::parse(R"({
        "id":"resp_1", "status":"completed", "output":[
            {"type":"reasoning", "id":"rs_1", "summary":[
                {"type":"summary_text","text":"Inspect the headers."},
                {"type":"summary_text","text":"Then compare them."}
            ], "encrypted_content":"opaque-original-data"},
            {"type":"message", "id":"msg_1", "role":"assistant", "status":"completed",
             "phase":"commentary", "content":[
                {"type":"output_text","text":"I will read ","annotations":[]},
                {"type":"output_text","text":"both files.","annotations":[]}
            ]},
            {"type":"function_call","id":"fc_1","call_id":"call_1","name":"read",
             "arguments":"{\"path\":\"agent.hpp\"}"},
            {"type":"function_call","id":"fc_2","call_id":"call_2","name":"read",
             "arguments":"{\"path\":\"message.hpp\"}"}
        ]
    })");
};

TEST_F(ResponsesTransformerTest, ConvertsOneResponseToOneOrderedMessage) {
    const auto message = transformer.fromProviderFormat(response);
    EXPECT_EQ(message.id, "resp_1");
    EXPECT_EQ(message.role, MessageRole::ASSISTANT);
    ASSERT_EQ(message.content.size(), 5);
    EXPECT_EQ(std::get<ReasoningContent>(message.content[0]).text,
        "Inspect the headers.\nThen compare them.");
    EXPECT_EQ(std::get<TextContent>(message.content[1]).text, "I will read ");
    EXPECT_EQ(std::get<ToolCallContent>(message.content[3]).arguments.at("path"), "agent.hpp");
    EXPECT_EQ(std::get<ToolCallContent>(message.content[4]).callId, "call_2");
}

TEST_F(ResponsesTransformerTest, UserSkillSequenceReplaysAsOrdinaryUserCallAndResult) {
    Message user{"user", MessageRole::USER, {TextContent{"//demo \"literal value\""}}};
    user.providerText = "Call the skill \"demo\" with arguments (JSON data):\n{\"subject\":\"literal value\"}";
    const json arguments{{"name", "demo"}, {"arguments", {{"subject", "literal value"}}}};
    Message call{"call", MessageRole::ASSISTANT, {ToolCallContent{"call", "skill", arguments}}};
    call.userSkill = "demo";
    Message result{"result", MessageRole::TOOL, {ToolResultContent{"call", "Original loaded instructions", false}}};
    Conversation conversation(ProviderFormat::OPENAI_RESPONSES);
    for (const auto& message : {user, call, result}) conversation.append(json(message).get<Message>());
    const auto wire = conversation.getProviderFormat();
    ASSERT_EQ(wire.size(), 3);
    EXPECT_EQ(wire[0]["role"], "user");
    EXPECT_NE(wire[0].dump().find("Call the skill"), std::string::npos);
    EXPECT_EQ(wire[1]["type"], "function_call");
    EXPECT_EQ(wire[1]["name"], "skill");
    EXPECT_EQ(json::parse(wire[1]["arguments"].get<std::string>()), arguments);
    EXPECT_EQ(wire[2]["type"], "function_call_output");
    EXPECT_EQ(wire[1]["call_id"], wire[2]["call_id"]);
    EXPECT_EQ(wire[2]["output"], "Original loaded instructions");
    EXPECT_EQ(wire.dump().find("//demo"), std::string::npos);
    EXPECT_EQ(wire.dump().find("user_skill"), std::string::npos);
    EXPECT_EQ(wire.dump().find("provider_text"), std::string::npos);
    EXPECT_EQ(std::get<TextContent>(conversation.getMessages()[0].content[0]).text, "//demo \"literal value\"");
    conversation.rebuildProviderContext(3);
    EXPECT_EQ(conversation.getProviderFormat(), wire);
    user.images = {validateImage({"", "image/png", "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a6a0AAAAASUVORK5CYII=", 0, 0, "pixel.png", "clipboard"})};
    EXPECT_EQ(transformer.toProviderFormat(user).size(), 2);
}

TEST_F(ResponsesTransformerTest, PreservesMetadataAndTextGroupingOnReplay) {
    const auto replay = transformer.toProviderFormat(transformer.fromProviderFormat(response));
    ASSERT_EQ(replay.size(), 4);
    EXPECT_EQ(json(replay), response.at("output"));
}

TEST_F(ResponsesTransformerTest, ModelSwitchRebuildsPortableContextWithoutChangingHistory) {
    Conversation conversation(ProviderFormat::OPENAI_RESPONSES);
    conversation.append(response);
    conversation.rebuildProviderContext(1);
    auto wire = conversation.getProviderFormat().dump();
    EXPECT_EQ(wire.find("opaque-original-data"), std::string::npos);
    EXPECT_NE(wire.find("call_1"), std::string::npos);
    EXPECT_NE(wire.find("agent.hpp"), std::string::npos);
    const auto& reasoning = std::get<ReasoningContent>(conversation.getMessages()[0].content[0]);
    EXPECT_EQ(reasoning.providerData.at("encrypted_content"), "opaque-original-data");
    auto next = response;
    next["id"] = "new-response";
    next["output"] = json::array({response["output"][0]});
    next["output"][0]["id"] = "new-reasoning";
    next["output"][0]["encrypted_content"] = "new-model-data";
    conversation.append(next);
    conversation.rebuildProviderContext(1);
    wire = conversation.getProviderFormat().dump();
    EXPECT_EQ(wire.find("opaque-original-data"), std::string::npos);
    EXPECT_NE(wire.find("new-model-data"), std::string::npos);
    EXPECT_THROW(conversation.rebuildProviderContext(3), std::invalid_argument);
}

TEST_F(ResponsesTransformerTest, PreservesOpaqueReasoningWithoutInventingText) {
    auto opaque = response;
    opaque["output"] = json::array({response["output"][0]});
    opaque["output"][0]["summary"] = json::array();
    const auto message = transformer.fromProviderFormat(opaque);
    EXPECT_FALSE(std::get<ReasoningContent>(message.content[0]).text.has_value());
    EXPECT_EQ(transformer.toProviderFormat(message)[0], opaque["output"][0]);
}

TEST_F(ResponsesTransformerTest, DisplaysSummaryAndTextTogetherWithoutChangingReplay) {
    auto both = response;
    both["output"] = json::array({response["output"][0]});
    both["output"][0]["content"] = json::array({{{"type", "reasoning_text"}, {"text", "Provider supplied reasoning text."}}});
    auto message = transformer.fromProviderFormat(both);
    auto& reasoning = std::get<ReasoningContent>(message.content[0]);
    EXPECT_EQ(reasoning.text, "Summary\nInspect the headers.\nThen compare them.\n\nReasoning\nProvider supplied reasoning text.");
    EXPECT_EQ(transformer.toProviderFormat(message)[0], both["output"][0]);
    reasoning.text = "Inspect the headers.\nThen compare them.";
    EXPECT_THROW(transformer.toProviderFormat(message), std::invalid_argument);
    both["output"][0]["summary"] = json::array();
    message = transformer.fromProviderFormat(both);
    EXPECT_EQ(std::get<ReasoningContent>(message.content[0]).text, "Provider supplied reasoning text.");
    EXPECT_EQ(transformer.toProviderFormat(message)[0], both["output"][0]);
}

TEST_F(ResponsesTransformerTest, PreservesFinalAnswerPhaseWithoutReasoning) {
    auto plain = response;
    plain["output"] = json::array({response["output"][1]});
    plain["output"][0]["phase"] = "final_answer";
    const auto message = transformer.fromProviderFormat(plain);
    ASSERT_EQ(message.content.size(), 2);
    EXPECT_EQ(transformer.toProviderFormat(message)[0].at("phase"), "final_answer");
}

TEST_F(ResponsesTransformerTest, FlattensConversationAndEncodesToolResults) {
    OpenAiResponsesApiConversationProviderTransformer conversations(transformer);
    Conversation conversation(ProviderFormat::OPENAI_RESPONSES);
    conversation.append(Message{"system_1", MessageRole::SYSTEM, {TextContent{"Be helpful."}}});
    conversation.append(Message{"user_1", MessageRole::USER, {TextContent{"Read both files."}}});
    conversation.append(transformer.fromProviderFormat(response));
    conversation.append(Message{"tool_1", MessageRole::TOOL, {
        ToolResultContent{"call_1", "file contents", false},
        ToolResultContent{"call_2", json{{"reason", "permission denied"}}, true}
    }});
    const auto input = conversations.toProviderFormat(conversation);
    ASSERT_EQ(input.size(), 8);
    EXPECT_EQ(input[0].at("role"), "system");
    EXPECT_EQ(input[1].at("role"), "user");
    for (std::size_t index = 0; index < 4; ++index)
        EXPECT_EQ(input[index + 2], response["output"][index]);
    const json expected{{"type", "function_call_output"}, {"call_id", "call_1"}, {"output", "file contents"}};
    EXPECT_EQ(input[6], expected);
    EXPECT_EQ(json::parse(input[7].at("output").get<std::string>()).at("error").at("reason"), "permission denied");
    EXPECT_EQ(json(input), conversation.getProviderFormat());
    EXPECT_TRUE(conversations.toProviderFormat(Conversation{ProviderFormat::OPENAI_RESPONSES}).empty());
}

TEST_F(ResponsesTransformerTest, RejectsIncompleteResponsesAndItems) {
    auto invalid = response;
    invalid["status"] = "incomplete";
    EXPECT_THROW(transformer.fromProviderFormat(invalid), std::invalid_argument);
    invalid = response;
    invalid["output"][2]["status"] = "in_progress";
    EXPECT_THROW(transformer.fromProviderFormat(invalid), std::invalid_argument);
}

TEST_F(ResponsesTransformerTest, RejectsInvalidToolArguments) {
    auto invalid = response;
    invalid["output"][2]["arguments"] = "{broken";
    EXPECT_THROW(transformer.fromProviderFormat(invalid), json::parse_error);
    invalid["output"][2]["arguments"] = "[]";
    EXPECT_THROW(transformer.fromProviderFormat(invalid), std::invalid_argument);
}

TEST_F(ResponsesTransformerTest, RejectsUnsupportedContentRatherThanDroppingIt) {
    auto invalid = response;
    invalid["output"][0]["type"] = "web_search_call";
    EXPECT_THROW(transformer.fromProviderFormat(invalid), std::invalid_argument);
    invalid = response;
    invalid["output"][1]["content"][0] = {{"type", "refusal"}, {"refusal", "Refused"}};
    EXPECT_THROW(transformer.fromProviderFormat(invalid), std::invalid_argument);
}

TEST_F(ResponsesTransformerTest, RejectsMissingRequiredFields) {
    auto invalid = response;
    invalid["output"][1].erase("id");
    EXPECT_THROW(transformer.fromProviderFormat(invalid), json::out_of_range);
}

TEST_F(ResponsesTransformerTest, RejectsIncompatibleRoleAndProvider) {
    auto message = transformer.fromProviderFormat(response);
    message.role = MessageRole::USER;
    EXPECT_THROW(transformer.toProviderFormat(message), std::invalid_argument);
    message = transformer.fromProviderFormat(response);
    std::get<TextContent>(message.content[1]).provider = static_cast<ProviderFormat>(-1);
    EXPECT_THROW(transformer.toProviderFormat(message), std::invalid_argument);
}

TEST_F(ResponsesTransformerTest, RejectsEditedReasoningWithOriginalMetadata) {
    auto message = transformer.fromProviderFormat(response);
    std::get<ReasoningContent>(message.content[0]).text = "Edited reasoning";
    EXPECT_THROW(transformer.toProviderFormat(message), std::invalid_argument);
}

TEST_F(ResponsesTransformerTest, ConversationAppendsBothFormatsAndPreservesHistory) {
    Conversation conversation(ProviderFormat::OPENAI_RESPONSES);
    EXPECT_EQ(conversation.getProvider(), ProviderFormat::OPENAI_RESPONSES);
    EXPECT_EQ(conversation.getProviderFormat(), json::array());
    conversation.append(Message{"user_1", MessageRole::USER, {TextContent{"Read the files."}}});
    const auto firstItem = conversation.getProviderFormat().at(0);
    conversation.append(response);
    ASSERT_EQ(conversation.getMessages().size(), 2);
    EXPECT_EQ(conversation.getMessages()[1].id, "resp_1");
    EXPECT_EQ(std::get<TextContent>(conversation.getMessages()[1].content[1]).provider, ProviderFormat::OPENAI_RESPONSES);
    ASSERT_EQ(conversation.getProviderFormat().size(), 5);
    EXPECT_EQ(conversation.getProviderFormat()[0], firstItem);
    for (std::size_t index = 0; index < 4; ++index)
        EXPECT_EQ(conversation.getProviderFormat()[index + 1], response["output"][index]);

    // Appending one of our own messages must also survive vector reallocation.
    conversation.append(conversation.getMessages()[0]);
    EXPECT_EQ(conversation.getMessages().size(), 3);
    EXPECT_EQ(conversation.getProviderFormat().back(), firstItem);
}

TEST_F(ResponsesTransformerTest, FailedAppendLeavesBothRepresentationsUnchanged) {
    Conversation conversation(ProviderFormat::OPENAI_RESPONSES);
    conversation.append(Message{"user_1", MessageRole::USER, {TextContent{"Hello"}}});
    const auto original = conversation.getProviderFormat();
    Message invalid{"bad", MessageRole::USER, {
        TextContent{"Valid prefix"}, ToolCallContent{"call", "read", json::object()}
    }};
    EXPECT_THROW(conversation.append(invalid), std::invalid_argument);
    EXPECT_EQ(conversation.getMessages().size(), 1);
    EXPECT_EQ(conversation.getProviderFormat(), original);
    auto malformed = response;
    malformed["output"][2]["arguments"] = "{broken";
    EXPECT_THROW(conversation.append(malformed), json::parse_error);
    EXPECT_EQ(conversation.getMessages().size(), 1);
    EXPECT_EQ(conversation.getMessages()[0].id, "user_1");
    EXPECT_EQ(conversation.getProviderFormat(), original);
    conversation.append(response);
    EXPECT_EQ(conversation.getMessages().size(), 2);
}

TEST(ConversationTest, ExposesReadOnlyViewsAndRejectsUnknownProvider) {
    static_assert(std::is_same_v<decltype(std::declval<Conversation&>().getMessages()), const std::vector<Message>&>);
    static_assert(std::is_same_v<decltype(std::declval<Conversation&>().getProviderFormat()), const json&>);
    EXPECT_THROW(Conversation{static_cast<ProviderFormat>(-1)}, std::invalid_argument);
}

class RecordingTransformer : public MessageProviderTransformer {
public:
    mutable std::vector<std::string> ids;

    Message fromProviderFormat(const json&) const override { return {}; }
    std::vector<json> toProviderFormat(const Message& message) const override {
        ids.push_back(message.id);
        return {message.id, message.id + "-second"};
    }
};

TEST(ConversationTransformerTest, UsesInjectedTransformerByReferenceInOrder) {
    RecordingTransformer messages;
    OpenAiResponsesApiConversationProviderTransformer conversations(messages);
    Conversation conversation(ProviderFormat::OPENAI_RESPONSES);
    conversation.append(Message{"first"});
    conversation.append(Message{"second"});
    EXPECT_EQ(conversations.toProviderFormat(conversation),
        (std::vector<json>{"first", "first-second", "second", "second-second"}));
    EXPECT_EQ(messages.ids, (std::vector<std::string>{"first", "second"}));
    static_assert(!std::is_constructible_v<OpenAiResponsesApiConversationProviderTransformer,
        OpenAiResponsesApiMessageProviderTransformer&&>);
}

}
