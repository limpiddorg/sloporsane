#ifndef SOS_MODEL_INFO_HPP
#define SOS_MODEL_INFO_HPP

#include <cstdint>
#include <optional>
#include <string>
#include "image_support.hpp"

struct ModelInfo {
    std::string id;
    std::optional<std::uint64_t> contextLimit;
    std::optional<std::uint64_t> inputLimit;
    std::optional<std::uint64_t> outputLimit;
    // Advertised maximum for display; does not change the operational context budget.
    std::optional<std::uint64_t> maxContextLimit;
    ImageSupport imageInput = ImageSupport::UNKNOWN;
};

#endif // SOS_MODEL_INFO_HPP
