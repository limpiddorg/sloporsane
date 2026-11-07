#ifndef SOS_RIPGREP_HPP
#define SOS_RIPGREP_HPP

#include <filesystem>
#include <functional>
#include <string>

namespace core {
// The optional download function allows installation tests without network access.
using ArchiveDownload = std::function<std::string(const std::string& url)>;
std::filesystem::path ensureRipgrep(const std::filesystem::path& binDirectory = {},
    const ArchiveDownload& download = {});
}

#endif // SOS_RIPGREP_HPP
