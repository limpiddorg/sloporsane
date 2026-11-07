#ifndef SOS_MESSAGE_JSON_HPP
#define SOS_MESSAGE_JSON_HPP

#include "message.hpp"

// Internal persistence/wire format, independent of provider response envelopes.
void to_json(nlohmann::json& json, const Message& message);
void from_json(const nlohmann::json& json, Message& message);

#endif // SOS_MESSAGE_JSON_HPP
