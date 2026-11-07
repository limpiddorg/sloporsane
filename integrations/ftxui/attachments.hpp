#ifndef SOS_TUI_ATTACHMENTS_HPP
#define SOS_TUI_ATTACHMENTS_HPP
#include <agent-runtime/image.hpp>
#include <filesystem>
#include <optional>
#include <string>
namespace sos::tui {
std::string base64(const std::string& bytes);
Image loadImage(const std::filesystem::path& path);
std::optional<std::filesystem::path> imagePath(const std::string& pasted);
Image clipboardImage();
void openBrowser(const std::string& url);
void copyNativeClipboard(const std::string& text);
}
#endif
