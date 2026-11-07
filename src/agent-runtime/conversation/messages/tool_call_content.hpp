#ifndef SOS_TOOL_CALL_CONTENT_HPP
#define SOS_TOOL_CALL_CONTENT_HPP

#include <nlohmann/json.hpp>

#include <string>
#include "../provider_format.hpp"
#include <optional>

struct ToolCallContent {
    std::string callId;
    std::string toolName;
    nlohmann::json arguments = nlohmann::json::object();
    std::optional<ProviderFormat> provider;
    nlohmann::json providerData = nlohmann::json::object();
};

#endif // SOS_TOOL_CALL_CONTENT_HPP
