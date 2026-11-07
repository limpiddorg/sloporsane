#include "openai_responses_api_conversation_provider_transformer.hpp"

#include <iterator>

OpenAiResponsesApiConversationProviderTransformer::OpenAiResponsesApiConversationProviderTransformer(
    const MessageProviderTransformer& messageTransformer
) : messageTransformer(messageTransformer) {}

std::vector<nlohmann::json> OpenAiResponsesApiConversationProviderTransformer::toProviderFormat(
    const Conversation& conversation
) const {
    std::vector<nlohmann::json> result;
    for (const auto& message : conversation.getMessages()) {
        auto items = messageTransformer.toProviderFormat(message);
        result.insert(result.end(), std::make_move_iterator(items.begin()), std::make_move_iterator(items.end()));
    }
    return result;
}
