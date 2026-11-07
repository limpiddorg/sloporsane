#include "attachments.hpp"
#include <agent-runtime/core/process.hpp>
#include <agent-runtime/common/path_resolution.hpp>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <unistd.h>
namespace sos::tui {
std::string base64(const std::string& bytes) {
    constexpr char ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const auto a = static_cast<unsigned char>(bytes[i]);
        const auto b = i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i + 1]) : 0;
        const auto c = i + 2 < bytes.size() ? static_cast<unsigned char>(bytes[i + 2]) : 0;
        output += ALPHABET[a >> 2]; output += ALPHABET[((a & 3) << 4) | (b >> 4)];
        output += i + 1 < bytes.size() ? ALPHABET[((b & 15) << 2) | (c >> 6)] : '=';
        output += i + 2 < bytes.size() ? ALPHABET[c & 63] : '=';
    }
    return output;
}
namespace {
Image fromBytes(std::string bytes, std::string name, std::string origin) {
    if (bytes.empty() || bytes.size() > MAX_IMAGE_BYTES) throw std::runtime_error("An image must contain between 1 byte and 5 MiB");
    Image image;
    if (bytes.starts_with("\x89PNG")) image.mime = "image/png";
    else if (bytes.starts_with("\xff\xd8")) image.mime = "image/jpeg";
    else image.mime = "image/webp";
    image.data = base64(bytes); image.name = std::move(name); image.origin = std::move(origin);
    return validateImage(std::move(image));
}
}
Image loadImage(const std::filesystem::path& path) {
    if (std::filesystem::file_size(path) > MAX_IMAGE_BYTES) throw std::runtime_error("Image exceeds 5 MiB");
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot read image: " + path.string());
    std::string bytes(MAX_IMAGE_BYTES + 1, '\0');
    file.read(bytes.data(), bytes.size()); bytes.resize(file.gcount());
    return fromBytes(std::move(bytes), path.filename().string(), path.string());
}
std::optional<std::filesystem::path> imagePath(const std::string& pasted) {
    auto value = pasted;
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return std::nullopt;
    value = value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    if (value.size() >= 2 && ((value.front() == '\'' && value.back() == '\'') || (value.front() == '"' && value.back() == '"'))) value = value.substr(1, value.size() - 2);
    for (std::size_t i = 0; (i = value.find("\\ ", i)) != std::string::npos;) value.replace(i, 2, " ");
    if (value.starts_with("~/")) value = (PathResolution::home() / value.substr(2)).string();
    auto extension = std::filesystem::path(value).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension != ".png" && extension != ".jpg" && extension != ".jpeg" && extension != ".webp") return std::nullopt;
    std::error_code error;
    if (!std::filesystem::is_regular_file(value, error)) return std::nullopt;
    return std::filesystem::absolute(value);
}
Image clipboardImage() {
    if (std::getenv("SSH_CONNECTION") || std::getenv("SSH_TTY")) throw std::runtime_error("Remote clipboard unavailable; paste an image file path");
    core::ProcessOptions options;
    options.timeout = std::chrono::seconds(5);
#ifdef __APPLE__
    // osascript returns PNG clipboard bytes as an AppleScript hexadecimal data literal.
    auto result = core::runProcess("/usr/bin/osascript", {"-e", "the clipboard as «class PNGf»"}, std::filesystem::current_path(), {}, {}, options);
    if (result.exitCode != 0) throw std::runtime_error("No PNG image in the native clipboard");
    auto begin = result.output.find("PNGf");
    auto end = result.output.rfind("»");
    if (begin == std::string::npos || end == std::string::npos) throw std::runtime_error("Invalid clipboard image");
    const auto hex = result.output.substr(begin + 4, end - begin - 4);
    if (hex.size() % 2 || hex.size() > 2 * MAX_IMAGE_BYTES) throw std::runtime_error("Invalid clipboard image size");
    std::string bytes;
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        auto digit = [](char c) -> int { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'A' && c <= 'F') return c - 'A' + 10; if (c >= 'a' && c <= 'f') return c - 'a' + 10; throw std::runtime_error("Invalid clipboard encoding"); };
        bytes += static_cast<char>((digit(hex[i]) << 4) | digit(hex[i + 1]));
    }
#else
    auto result = std::getenv("WAYLAND_DISPLAY")
        ? core::runProcess("wl-paste", {"--no-newline", "--type", "image/png"}, std::filesystem::current_path(), {}, {}, options)
        : core::runProcess("xclip", {"-selection", "clipboard", "-t", "image/png", "-o"}, std::filesystem::current_path(), {}, {}, options);
    if (result.exitCode != 0) throw std::runtime_error("No PNG image in the native clipboard; install wl-paste or xclip if needed");
    auto bytes = std::move(result.output);
#endif
    return fromBytes(std::move(bytes), "Pasted Image", "clipboard");
}
void openBrowser(const std::string& url) {
    if (!url.starts_with("https://") && !url.starts_with("http://")) throw std::runtime_error("Invalid authorization URL");
    core::ProcessOptions options; options.timeout = std::chrono::seconds(5);
#ifdef __APPLE__
    const auto result = core::runProcess("/usr/bin/open", {url}, std::filesystem::current_path(), {}, {}, options);
#else
    const auto result = core::runProcess("xdg-open", {url}, std::filesystem::current_path(), {}, {}, options);
#endif
    if (result.exitCode != 0) throw std::runtime_error("Cannot open browser; copy the displayed URL");
}
}
