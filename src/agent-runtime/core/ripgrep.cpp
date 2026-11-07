#include "ripgrep.hpp"
#include "process.hpp"
#include "../common/path_resolution.hpp"

#include <cpr/cpr.h>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <sys/utsname.h>
#include <unistd.h>

namespace core {
namespace {
namespace fs = std::filesystem;
constexpr const char* VERSION = "15.1.0";

bool executable(const fs::path& path) {
    std::error_code error;
    return fs::is_regular_file(path, error) && access(path.c_str(), X_OK) == 0;
}

std::string platform() {
    utsname info{};
    if (uname(&info) != 0) throw std::runtime_error("Could not detect ripgrep platform");
    const std::string os = info.sysname;
    const std::string arch = info.machine;
    if (os == "Darwin" && arch == "arm64") return "aarch64-apple-darwin";
    if (os == "Darwin" && arch == "x86_64") return "x86_64-apple-darwin";
    if (os == "Linux" && arch == "aarch64") return "aarch64-unknown-linux-gnu";
    if (os == "Linux" && arch == "x86_64") return "x86_64-unknown-linux-musl";
    throw std::runtime_error("Unsupported ripgrep platform: " + os + "/" + arch);
}
}

fs::path ensureRipgrep(const fs::path& binDirectory, const ArchiveDownload& download) {
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    if (const auto* value = std::getenv("PATH")) {
        std::istringstream paths(value);
        std::string directory;
        while (std::getline(paths, directory, ':')) {
            const auto candidate = (directory.empty() ? fs::current_path() : fs::path(directory)) / "rg";
            if (executable(candidate)) return fs::absolute(candidate);
        }
    }
    const auto bin = binDirectory.empty() ? PathResolution::managedToolsDirectory() : fs::absolute(binDirectory);
    const auto target = bin / "rg";
    if (executable(target)) return target;

    const auto folder = std::string("ripgrep-") + VERSION + "-" + platform();
    const auto url = "https://github.com/BurntSushi/ripgrep/releases/download/" + std::string(VERSION) + "/" + folder + ".tar.gz";
    fs::create_directories(bin);
    auto pattern = (bin / "ripgrep-XXXXXX").string();
    if (!mkdtemp(pattern.data())) throw std::runtime_error("Could not create ripgrep installation directory");
    const fs::path temporary = pattern;
    struct Cleanup {
        fs::path path;
        ~Cleanup() { std::error_code error; fs::remove_all(path, error); }
    } cleanup{temporary};
    std::string bytes;
    if (download) bytes = download(url);
    else {
        const auto response = cpr::Get(cpr::Url{url}, cpr::ConnectTimeout{15000}, cpr::Timeout{60000});
        if (response.error.code != cpr::ErrorCode::OK || response.status_code != 200)
            throw std::runtime_error("Could not download ripgrep " + std::string(VERSION));
        bytes = response.text;
    }
    if (bytes.empty()) throw std::runtime_error("Empty ripgrep archive");
    const auto archive = temporary / "archive.tar.gz";
    {
        std::ofstream file(archive, std::ios::binary);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        file.close();
        if (!file) throw std::runtime_error("Could not save ripgrep archive");
    }
    // Extract only the binary member, not arbitrary archive entries.
    const auto extraction = runProcess("/usr/bin/tar", {"-xzf", archive.string(), "-C", temporary.string(), folder + "/rg"}, temporary);
    if (extraction.exitCode != 0) throw std::runtime_error("Could not extract ripgrep: " + extraction.error);
    const auto binary = temporary / folder / "rg";
    if (!fs::is_regular_file(fs::symlink_status(binary))) throw std::runtime_error("Ripgrep archive does not contain a regular binary");
    fs::permissions(binary, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
        fs::perms::others_read | fs::perms::others_exec);
    fs::rename(binary, target);
    return target;
}
}
