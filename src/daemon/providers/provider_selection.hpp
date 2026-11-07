#ifndef SOS_PROVIDER_SELECTION_HPP
#define SOS_PROVIDER_SELECTION_HPP

#include "../config.hpp"
#include <string>
#include <optional>
#include <cstdint>

struct ProviderSelection {
    std::string provider = "openrouter";
    std::string model = daemon_config::DEFAULT_MODEL;
    std::string reasoning;
    std::optional<std::uint64_t> contextLimit;
};

#endif // SOS_PROVIDER_SELECTION_HPP
