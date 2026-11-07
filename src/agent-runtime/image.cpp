#include "image.hpp"
#include <openssl/evp.h>
#include <algorithm>
#include <array>
#include <stdexcept>

std::string Image::url() const {
    if (data.empty()) throw std::invalid_argument("Image pixels are unavailable");
    return "data:" + mime + ";base64," + data;
}
void to_json(nlohmann::json& json, const Image& image) {
    json = {{"id", image.id}, {"mime", image.mime}, {"width", image.width}, {"height", image.height},
        {"name", image.name}, {"origin", image.origin}};
    if (!image.data.empty()) json["data"] = image.data;
}
void from_json(const nlohmann::json& json, Image& image) {
    image = {json.value("id", ""), json.at("mime").get<std::string>(), json.value("data", ""),
        json.value("width", 0u), json.value("height", 0u), json.value("name", ""), json.value("origin", "")};
}
Image validateImage(Image image) {
    const auto& encoded = image.data;
    if (encoded.empty() || encoded.size() > 4 * ((MAX_IMAGE_BYTES + 2) / 3) || encoded.size() % 4)
        throw std::invalid_argument("Image must contain base64 pixels, at most 5 MiB");
    const auto padding = encoded.ends_with("==") ? 2u : encoded.ends_with("=") ? 1u : 0u;
    for (std::size_t i = 0; i < encoded.size() - padding; ++i) {
        const auto c = encoded[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/'))
            throw std::invalid_argument("Invalid image base64");
    }
    std::vector<unsigned char> bytes(encoded.size() / 4 * 3);
    const auto decoded = EVP_DecodeBlock(bytes.data(), reinterpret_cast<const unsigned char*>(encoded.data()), encoded.size());
    if (decoded < static_cast<int>(padding)) throw std::invalid_argument("Invalid image base64");
    bytes.resize(decoded - padding);
    if (bytes.size() > MAX_IMAGE_BYTES) throw std::invalid_argument("Image exceeds 5 MiB");
    std::string canonical(4 * ((bytes.size() + 2) / 3), '\0');
    EVP_EncodeBlock(reinterpret_cast<unsigned char*>(canonical.data()), bytes.data(), bytes.size());
    if (canonical != encoded) throw std::invalid_argument("Noncanonical image base64");
    const auto be = [&](std::size_t p, int count) {
        if (p + count > bytes.size()) throw std::invalid_argument("Truncated image header");
        std::uint32_t value = 0;
        for (int i = 0; i < count; ++i) value = (value << 8) | bytes[p + i];
        return value;
    };
    const auto le = [&](std::size_t p, int count) {
        if (p + count > bytes.size()) throw std::invalid_argument("Truncated image header");
        std::uint32_t value = 0;
        for (int i = count - 1; i >= 0; --i) value = (value << 8) | bytes[p + i];
        return value;
    };
    const auto matches = [&](std::size_t p, std::string_view value) {
        return p + value.size() <= bytes.size() && std::equal(value.begin(), value.end(), bytes.begin() + p,
            [](char a, unsigned char b) { return static_cast<unsigned char>(a) == b; });
    };
    image.width = image.height = 0;
    std::string mime;
    if (matches(0, std::string_view("\x89PNG\r\n\x1a\n", 8)) && matches(12, "IHDR") && be(8, 4) == 13) {
        mime = "image/png";
        image.width = be(16, 4); image.height = be(20, 4);
    } else if (matches(0, std::string_view("\xff\xd8", 2))) {
        mime = "image/jpeg";
        for (std::size_t p = 2; p + 4 <= bytes.size();) {
            if (bytes[p++] != 0xff) break;
            while (p < bytes.size() && bytes[p] == 0xff) ++p;
            if (p >= bytes.size()) break;
            const auto marker = bytes[p++];
            if (marker == 0xd9 || marker == 0xda) break;
            const auto length = be(p, 2);
            if (length < 2 || p + length > bytes.size()) break;
            if ((marker >= 0xc0 && marker <= 0xc3) || (marker >= 0xc5 && marker <= 0xc7) ||
                (marker >= 0xc9 && marker <= 0xcb) || (marker >= 0xcd && marker <= 0xcf)) {
                if (length < 8) break;
                image.height = be(p + 3, 2); image.width = be(p + 5, 2); break;
            }
            p += length;
        }
    } else if (matches(0, "RIFF") && matches(8, "WEBP") && le(4, 4) + 8ull == bytes.size()) {
        mime = "image/webp";
        if (matches(12, "VP8X") && bytes.size() >= 30) {
            image.width = 1 + le(24, 3); image.height = 1 + le(27, 3);
        } else if (matches(12, "VP8L") && bytes.size() >= 25 && bytes[20] == 0x2f) {
            const auto bits = le(21, 4);
            image.width = 1 + (bits & 0x3fff); image.height = 1 + ((bits >> 14) & 0x3fff);
        } else if (matches(12, "VP8 ") && matches(23, std::string_view("\x9d\x01\x2a", 3))) {
            image.width = le(26, 2) & 0x3fff; image.height = le(28, 2) & 0x3fff;
        }
    }
    if (mime.empty() || mime != image.mime || !image.width || !image.height)
        throw std::invalid_argument("Expected a PNG, JPEG or WebP image with valid dimensions");
    if (image.width > 16384 || image.height > 16384 || std::uint64_t(image.width) * image.height > 40000000)
        throw std::invalid_argument("Image exceeds 16384 pixels per side or 40 megapixels");
    if (image.name.size() > 4096 || image.origin.size() > 4096)
        throw std::invalid_argument("Image metadata is too long");
    std::array<unsigned char, 32> digest{};
    unsigned int size = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), digest.data(), &size, EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("Cannot hash image");
    image.id = "img_";
    constexpr char HEX[] = "0123456789abcdef";
    for (const auto b : digest) { image.id += HEX[b >> 4]; image.id += HEX[b & 15]; }
    return image;
}
void validateImages(std::vector<Image>& images) {
    if (images.size() > MAX_IMAGES) throw std::invalid_argument("At most 8 images may be attached to a message");
    std::size_t total = 0;
    for (auto& image : images) {
        image = validateImage(std::move(image));
        total += image.data.size() / 4 * 3 - (image.data.ends_with("==") ? 2 : image.data.ends_with("=") ? 1 : 0);
    }
    if (total > MAX_IMAGES_BYTES) throw std::invalid_argument("Images exceed the 10 MiB message budget");
}
std::uint64_t estimateImageTokens(const Image& image) {
    // Conservative local estimate, not a provider billing formula. Never count base64 as text.
    return 2048 + 170 * ((image.width + 511ull) / 512) * ((image.height + 511ull) / 512);
}
