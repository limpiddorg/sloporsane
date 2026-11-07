#ifndef SOS_MESSAGE_PROVIDER_TRANSFORMER_HPP
#define SOS_MESSAGE_PROVIDER_TRANSFORMER_HPP

#include "../messages/message.hpp"

#include <nlohmann/json.hpp>
#include <vector>

class MessageProviderTransformer {
public:
    virtual ~MessageProviderTransformer() = default;

    // A complete provider response becomes one assistant message.
    virtual Message fromProviderFormat(const nlohmann::json& response) const = 0;
    // One internal message may expand into multiple provider input items.
    virtual std::vector<nlohmann::json> toProviderFormat(const Message& message) const = 0;
};

#endif // SOS_MESSAGE_PROVIDER_TRANSFORMER_HPP
