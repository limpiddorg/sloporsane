#ifndef SOS_OPEN_AI_RESPONSES_API_MESSAGE_PROVIDER_TRANSFORMER_HPP
#define SOS_OPEN_AI_RESPONSES_API_MESSAGE_PROVIDER_TRANSFORMER_HPP

#include "../message_provider_transformer.hpp"

class OpenAiResponsesApiMessageProviderTransformer : public MessageProviderTransformer {
public:
    Message fromProviderFormat(const nlohmann::json& response) const override;
    std::vector<nlohmann::json> toProviderFormat(const Message& message) const override;
};

#endif // SOS_OPEN_AI_RESPONSES_API_MESSAGE_PROVIDER_TRANSFORMER_HPP
