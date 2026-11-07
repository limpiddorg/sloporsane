#include "openai_responses_api_message_provider_transformer.hpp"
#include "responses_types.hpp"

#include <stdexcept>
#include <type_traits>

namespace {

constexpr ProviderFormat PROVIDER = ProviderFormat::OPENAI_RESPONSES;

void requireRole(MessageRole actual, MessageRole expected) {
    if (actual != expected) throw std::invalid_argument("Content is incompatible with the message role");
}

void requireProvider(const std::optional<ProviderFormat>& provider) {
    if (provider && provider != PROVIDER)
        throw std::invalid_argument("Cannot replay another provider's metadata with OpenAI");
}

std::string roleName(MessageRole role) {
    switch (role) {
        case MessageRole::SYSTEM: return "system";
        case MessageRole::USER: return "user";
        case MessageRole::ASSISTANT: return "assistant";
        case MessageRole::TOOL: break;
    }
    throw std::invalid_argument("Tool messages must contain tool results");
}

}

Message OpenAiResponsesApiMessageProviderTransformer::fromProviderFormat(const nlohmann::json& json) const {
    const auto response = json.get<openai_responses::Response>();
    Message message{response.id, MessageRole::ASSISTANT, {}};
    for (const auto& item : response.output) {
        std::visit([&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, openai_responses::TextItem>) {
                for (const auto& part : value.content) {
                    const openai_responses::TextMetadata metadata{value.id, value.phase, part.annotations};
                    message.content.emplace_back(TextContent{part.text, PROVIDER, metadata});
                }
            } else if constexpr (std::is_same_v<T, openai_responses::FunctionCallItem>) {
                auto arguments = nlohmann::json::parse(value.arguments);
                if (!arguments.is_object()) throw std::invalid_argument("Tool arguments must be a JSON object");
                message.content.emplace_back(ToolCallContent{
                    value.callId, value.name, std::move(arguments), PROVIDER, value.metadata
                });
            } else {
                message.content.emplace_back(ReasoningContent{
                    openai_responses::reasoningText(value), PROVIDER, value
                });
            }
        }, item);
    }
    return message;
}

std::vector<nlohmann::json> OpenAiResponsesApiMessageProviderTransformer::toProviderFormat(const Message& message) const {
    if (message.recap) return {};
    if (message.providerText) {
        requireRole(message.role, MessageRole::USER);
        Message rewritten = message;
        rewritten.content = {TextContent{*message.providerText}};
        rewritten.providerText.reset();
        return toProviderFormat(rewritten);
    }
    std::vector<nlohmann::json> result;
    std::optional<openai_responses::TextItem> pendingText;
    const auto flushText = [&] {
        if (pendingText) {
            result.emplace_back(*pendingText);
            pendingText.reset();
        }
    };

    for (const auto& content : message.content) {
        std::visit([&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, TextContent>) {
                requireProvider(value.provider);
                if (!value.provider) {
                    flushText();
                    result.emplace_back(openai_responses::InputTextItem{roleName(message.role), value.text});
                } else {
                    requireRole(message.role, MessageRole::ASSISTANT);
                    const auto metadata = value.providerData.template get<openai_responses::TextMetadata>();
                    if (pendingText && (pendingText->id != metadata.itemId || pendingText->phase != metadata.phase))
                        flushText();
                    if (!pendingText)
                        pendingText = openai_responses::TextItem{metadata.itemId, "assistant", metadata.phase, {}};
                    pendingText->content.push_back({"output_text", value.text, metadata.annotations});
                }
            } else if constexpr (std::is_same_v<T, ToolCallContent>) {
                flushText();
                requireRole(message.role, MessageRole::ASSISTANT);
                requireProvider(value.provider);
                if (!value.arguments.is_object()) throw std::invalid_argument("Tool arguments must be a JSON object");
                const auto metadata = !value.provider ? openai_responses::FunctionCallMetadata{} :
                    value.providerData.template get<openai_responses::FunctionCallMetadata>();
                result.emplace_back(openai_responses::FunctionCallItem{
                    metadata, value.callId, value.toolName, value.arguments.dump()
                });
            } else if constexpr (std::is_same_v<T, ReasoningContent>) {
                flushText();
                requireRole(message.role, MessageRole::ASSISTANT);
                if (value.provider != PROVIDER)
                    throw std::invalid_argument("Reasoning requires original OpenAI metadata");
                const auto item = value.providerData.template get<openai_responses::ReasoningItem>();
                if (value.text != openai_responses::reasoningText(item))
                    throw std::invalid_argument("Cannot replay modified reasoning with its original metadata");
                result.emplace_back(item);
            } else {
                flushText();
                requireRole(message.role, MessageRole::TOOL);
                // Responses has no is_error field for function outputs. Encode errors explicitly.
                const auto output = value.isError ? nlohmann::json{{"error", value.output}}.dump() :
                    (value.output.is_string() ? value.output.template get<std::string>() : value.output.dump());
                result.emplace_back(openai_responses::FunctionResultItem{value.callId, output, value.attachments});
            }
        }, content);
    }
    flushText();
    if (!message.images.empty()) {
        if (message.role != MessageRole::USER && message.role != MessageRole::TOOL)
            throw std::invalid_argument("Images require a user or tool message");
        auto parts = nlohmann::json::array();
        for (const auto& image : message.images) {
            parts.push_back({{"type", "input_text"}, {"text", "[Image " + image.id + ": " + image.name + "]"}});
            parts.push_back({{"type", "input_image"}, {"image_url", image.url()}});
        }
        // A user input item is portable across Responses providers, including
        // those that only accept text function_call_output. It follows the tool result.
        result.push_back({{"role", "user"}, {"content", std::move(parts)}});
    }
    return result;
}
