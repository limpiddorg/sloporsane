#ifndef SOS_REASONING_CONTENT_HPP
#define SOS_REASONING_CONTENT_HPP

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include "../provider_format.hpp"

struct ReasoningContent {
    // Some providers return opaque data without readable reasoning text.
    std::optional<std::string> text;
    std::optional<ProviderFormat> provider;
    nlohmann::json providerData = nlohmann::json::object();
};

#endif // SOS_REASONING_CONTENT_HPP
