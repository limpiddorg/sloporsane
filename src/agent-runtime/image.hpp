#ifndef SOS_IMAGE_HPP
#define SOS_IMAGE_HPP

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <cstdint>

// Pixels travel separately from text. An empty data field is a persisted/display reference.
struct Image {
    std::string id;
    std::string mime;
    std::string data; // canonical base64, never a remote URL
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::string name;
    std::string origin;
    std::string url() const;
};
inline constexpr std::size_t MAX_IMAGE_BYTES = 5 * 1024 * 1024;
inline constexpr std::size_t MAX_IMAGES = 8;
inline constexpr std::size_t MAX_IMAGES_BYTES = 10 * 1024 * 1024;
void to_json(nlohmann::json& json, const Image& image);
void from_json(const nlohmann::json& json, Image& image);
// Validate untrusted bytes, derive dimensions and content-addressed ID.
Image validateImage(Image image);
void validateImages(std::vector<Image>& images);
std::uint64_t estimateImageTokens(const Image& image);

#endif // SOS_IMAGE_HPP
