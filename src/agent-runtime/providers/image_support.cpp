#include "image_support.hpp"
#include <algorithm>

const char* imageSupportName(ImageSupport support) {
    switch (support) {
        case ImageSupport::SUPPORTED: return "supported";
        case ImageSupport::UNSUPPORTED: return "unsupported";
        case ImageSupport::UNKNOWN: return "unknown";
    }
    throw std::invalid_argument("Invalid image capability");
}

ImageSupport declaredImageSupport(const nlohmann::json& modalities) {
    if (!modalities.is_array() || std::any_of(modalities.begin(), modalities.end(),
        [](const auto& item) { return !item.is_string(); })) return ImageSupport::UNKNOWN;
    return std::find(modalities.begin(), modalities.end(), "image") != modalities.end()
        ? ImageSupport::SUPPORTED : ImageSupport::UNSUPPORTED;
}

ImageSupport openAIImageSupport(std::string_view model) {
    // Verified against https://developers.openai.com/api/docs/models/{model}.
    // Deliberately no family/prefix guessing, including arbitrary dated aliases.
    if (model == "gpt-4.1" || model == "gpt-4.1-2025-04-14" ||
        model == "gpt-5.4" || model == "gpt-5.4-2026-03-05") return ImageSupport::SUPPORTED;
    if (model == "o3-mini" || model == "o3-mini-2025-01-31") return ImageSupport::UNSUPPORTED;
    return ImageSupport::UNKNOWN;
}

ImageInputError::ImageInputError(ImageSupport support)
    : std::runtime_error(support == ImageSupport::UNSUPPORTED
        ? "This model does not support image input. Choose a vision-capable model or remove the images."
        : "Image support could not be verified for this model. Choose a vision-capable model or remove the images.") {}

void requireImageSupport(ImageSupport support) {
    if (support != ImageSupport::SUPPORTED) throw ImageInputError(support);
}
