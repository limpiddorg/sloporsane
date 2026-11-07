#ifndef SOS_TEXT_CONTENT_HPP
#define SOS_TEXT_CONTENT_HPP

#include <string>
#include "../provider_format.hpp"
#include <optional>
#include <nlohmann/json.hpp>

struct TextContent {
    std::string text;
    std::optional<ProviderFormat> provider;
    nlohmann::json providerData = nlohmann::json::object();
};

#endif // SOS_TEXT_CONTENT_HPP
