#ifndef SOS_OPEN_AI_RESPONSES_API_CONVERSATION_PROVIDER_TRANSFORMER_HPP
#define SOS_OPEN_AI_RESPONSES_API_CONVERSATION_PROVIDER_TRANSFORMER_HPP

#include "../conversation_provider_transformer.hpp"
#include "../message_provider_transformer.hpp"

class OpenAiResponsesApiConversationProviderTransformer : public ConversationProviderTransformer {
public:
    // The referenced transformer must outlive this object and produce Responses input items.
    explicit OpenAiResponsesApiConversationProviderTransformer(const MessageProviderTransformer& messageTransformer);
    OpenAiResponsesApiConversationProviderTransformer(MessageProviderTransformer&&) = delete;
    OpenAiResponsesApiConversationProviderTransformer(const MessageProviderTransformer&&) = delete;

    std::vector<nlohmann::json> toProviderFormat(const Conversation& conversation) const override;

private:
    const MessageProviderTransformer& messageTransformer;
};

#endif // SOS_OPEN_AI_RESPONSES_API_CONVERSATION_PROVIDER_TRANSFORMER_HPP
