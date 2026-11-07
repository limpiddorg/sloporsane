#ifndef SOS_CONVERSATION_PROVIDER_TRANSFORMER_HPP
#define SOS_CONVERSATION_PROVIDER_TRANSFORMER_HPP

#include "../conversation.hpp"

#include <nlohmann/json.hpp>
#include <vector>

class ConversationProviderTransformer {
public:
    virtual ~ConversationProviderTransformer() = default;

    virtual std::vector<nlohmann::json> toProviderFormat(const Conversation& conversation) const = 0;
};

#endif // SOS_CONVERSATION_PROVIDER_TRANSFORMER_HPP
