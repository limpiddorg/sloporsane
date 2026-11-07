#include "json_file.hpp"
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mcp {
namespace {
constexpr std::size_t MAX_FILE_BYTES = 1024 * 1024;
struct Descriptor {
    int value;
    ~Descriptor() { if (value >= 0) ::close(value); }
    Descriptor(const Descriptor&) = delete;
    explicit Descriptor(int value) : value(value) {}
};

void requireRegular(int fd) {
    struct stat status{};
    if (::fstat(fd, &status) != 0 || !S_ISREG(status.st_mode))
        throw std::runtime_error("MCP settings must be a regular file");
}

void writeAll(int fd, const std::string& bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::runtime_error("Cannot write MCP settings");
        offset += static_cast<std::size_t>(count);
    }
}
}

JsonFile::JsonFile(std::filesystem::path path) : path(std::move(path)) {}

nlohmann::json JsonFile::read() const {
    Descriptor fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (fd.value < 0) {
        if (errno == ENOENT) return nlohmann::json::object();
        throw std::runtime_error("Cannot read MCP settings");
    }
    requireRegular(fd.value);
    std::string bytes;
    std::array<char, 4096> buffer{};
    for (;;) {
        const auto count = ::read(fd.value, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) throw std::runtime_error("Cannot read MCP settings");
        if (count == 0) break;
        bytes.append(buffer.data(), count);
        if (bytes.size() > MAX_FILE_BYTES) throw std::runtime_error("MCP settings exceed 1 MiB");
    }
    auto json = nlohmann::json::parse(bytes, nullptr, false);
    if (!json.is_object()) throw std::runtime_error("Invalid MCP settings JSON");
    return json;
}

void JsonFile::update(const std::function<void(nlohmann::json&)>& change) const {
    std::filesystem::create_directories(path.parent_path());
    Descriptor lock(::open((path.string() + ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600));
    if (lock.value < 0) throw std::runtime_error("Cannot lock MCP settings");
    requireRegular(lock.value);
    while (::flock(lock.value, LOCK_EX) != 0) {
        if (errno != EINTR) throw std::runtime_error("Cannot lock MCP settings");
    }
    auto json = read();
    change(json);
    const auto bytes = json.dump(2) + "\n";
    if (bytes.size() > MAX_FILE_BYTES) throw std::runtime_error("MCP settings exceed 1 MiB");
    auto temporary = path.string() + ".XXXXXX";
    Descriptor output(::mkstemp(temporary.data()));
    if (output.value < 0) throw std::runtime_error("Cannot create MCP settings");
    try {
        if (::fchmod(output.value, 0600) != 0 || ::fcntl(output.value, F_SETFD, FD_CLOEXEC) != 0)
            throw std::runtime_error("Cannot protect MCP settings");
        writeAll(output.value, bytes);
        if (::fsync(output.value) != 0) throw std::runtime_error("Cannot flush MCP settings");
        std::filesystem::rename(temporary, path);
    } catch (...) {
        ::unlink(temporary.c_str());
        throw;
    }
}
}
