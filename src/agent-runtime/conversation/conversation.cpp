#include "conversation.hpp"

#include "transformers/openai/openai_responses_api_message_provider_transformer.hpp"

#include <stdexcept>
#include <algorithm>
#include <type_traits>

namespace {
Message portableMessage(Message message) {
    std::erase_if(message.content, [](const auto& part) { return std::holds_alternative<ReasoningContent>(part); });
    for (auto& part : message.content) std::visit([](auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, TextContent> || std::is_same_v<T, ToolCallContent>) {
            value.provider.reset();
            value.providerData = nlohmann::json::object();
        }
    }, part);
    return message;
}
}

Conversation::Conversation(ProviderFormat provider) : provider(provider) {
    switch (provider) {
        case ProviderFormat::OPENAI_RESPONSES:
            messageTransformer = std::make_unique<OpenAiResponsesApiMessageProviderTransformer>();
            break;
        default:
            throw std::invalid_argument("Unsupported conversation provider");
    }
}

void Conversation::append(const Message& message) {
    if (message.recap) return;
    // Prepare everything before modifying either representation (also handles self-append).
    Message preparedMessage = message;
    auto items = messageTransformer->toProviderFormat(preparedMessage);
    auto& history = providerFormat.get_ref<nlohmann::json::array_t&>();
    if (items.size() > history.max_size() - history.size())
        throw std::length_error("Provider history is too large");

    const auto previousSize = history.size();
    try {
        for (auto& item : items) history.push_back(std::move(item));
        messages.push_back(std::move(preparedMessage));
    } catch (...) {
        // Only the newly appended items are removed; existing history is never copied.
        history.resize(previousSize);
        throw;
    }
}

void Conversation::append(const nlohmann::json& providerResponse) {
    append(messageTransformer->fromProviderFormat(providerResponse));
}

ProviderFormat Conversation::getProvider() const { return provider; }

void Conversation::rebuildProviderContext(std::size_t portableBefore) {
    if (portableBefore > messages.size()) throw std::invalid_argument("Invalid provider context boundary");
    auto rebuilt = nlohmann::json::array();
    for (std::size_t index = 0; index < messages.size(); ++index) {
        const auto message = index < portableBefore ? portableMessage(messages[index]) : messages[index];
        for (auto& item : messageTransformer->toProviderFormat(message)) rebuilt.push_back(std::move(item));
    }
    providerFormat = std::move(rebuilt);
}

void Conversation::setLastGenerationMetadata(GenerationMetadata metadata) {
    if (messages.empty() || messages.back().role != MessageRole::ASSISTANT)
        throw std::logic_error("Generation metadata requires an assistant message");
    messages.back().metadata = std::move(metadata);
}

void Conversation::setLastElapsedMs(std::uint64_t elapsedMs) {
    if (messages.empty()) throw std::logic_error("Activity timing requires a message");
    messages.back().elapsedMs = elapsedMs;
}

void Conversation::setLastModelCall(ModelCallMetadata metadata) {
    if (messages.empty() || messages.back().role != MessageRole::ASSISTANT)
        throw std::logic_error("Model call metadata requires an assistant message");
    messages.back().modelCall = std::move(metadata);
}

const std::vector<Message>& Conversation::getMessages() const { return messages; }

const nlohmann::json& Conversation::getProviderFormat() const { return providerFormat; }
