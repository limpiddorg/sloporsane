#ifndef SOS_IMAGE_SUPPORT_HPP
#define SOS_IMAGE_SUPPORT_HPP

#include <nlohmann/json.hpp>
#include <string_view>
#include <stdexcept>

enum class ImageSupport { UNKNOWN, UNSUPPORTED, SUPPORTED };

const char* imageSupportName(ImageSupport support);
// A missing or malformed declaration is unknown, never a positive capability.
ImageSupport declaredImageSupport(const nlohmann::json& modalities);
// Exact documented API IDs only. Codex uses its authenticated catalog instead.
ImageSupport openAIImageSupport(std::string_view model);

class ImageInputError : public std::runtime_error {
public:
    explicit ImageInputError(ImageSupport support);
};

void requireImageSupport(ImageSupport support);

#endif // SOS_IMAGE_SUPPORT_HPP
