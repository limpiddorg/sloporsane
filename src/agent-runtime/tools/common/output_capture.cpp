#include "output_capture.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>

namespace {
constexpr std::size_t MAX_BYTES = 50 * 1024;
constexpr std::size_t MAX_LINES = 2000;
}

std::string tool_output::utf8(std::string_view input) {
    // Replace invalid UTF-8 from arbitrary processes before it enters JSON messages.
    return nlohmann::json::parse(nlohmann::json(std::string(input)).dump(-1, ' ', false,
        nlohmann::json::error_handler_t::replace)).get<std::string>();
}

std::string tool_output::base64(std::string_view bytes) {
    constexpr char ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((bytes.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const auto a = static_cast<unsigned char>(bytes[i]);
        const auto b = i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i + 1]) : 0;
        const auto c = i + 2 < bytes.size() ? static_cast<unsigned char>(bytes[i + 2]) : 0;
        result += ALPHABET[a >> 2];
        result += ALPHABET[((a & 3) << 4) | (b >> 4)];
        result += i + 1 < bytes.size() ? ALPHABET[((b & 15) << 2) | (c >> 6)] : '=';
        result += i + 2 < bytes.size() ? ALPHABET[c & 63] : '=';
    }
    return result;
}

OutputCapture::OutputCapture(std::filesystem::path directory, bool keepTail)
    : directory(std::move(directory)), keepTail(keepTail) {}
OutputCapture::~OutputCapture() { if (file >= 0) close(file); }

void OutputCapture::spill(std::string_view bytes) {
    while (!bytes.empty()) {
        const auto written = write(file, bytes.data(), bytes.size());
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) throw std::runtime_error("Could not save tool output");
        bytes.remove_prefix(static_cast<std::size_t>(written));
    }
}

void OutputCapture::append(std::string_view bytes) {
    lineCount += std::count(bytes.begin(), bytes.end(), '\n');
    const auto lines = lineCount + (!bytes.empty() && bytes.back() != '\n' ? 1 : 0);
    if (file < 0 && (tail.size() + bytes.size() > MAX_BYTES || lines > MAX_LINES)) {
        std::filesystem::create_directories(directory);
        auto pattern = (directory / "output-XXXXXX").string();
        file = mkstemp(pattern.data());
        if (file < 0) throw std::runtime_error("Could not create tool output file");
        fcntl(file, F_SETFD, FD_CLOEXEC);
        path = pattern;
        spill(tail);
    }
    if (file >= 0) spill(bytes);
    tail.append(bytes);
    if (tail.size() > MAX_BYTES) {
        if (keepTail) {
            auto start = tail.size() - MAX_BYTES;
            while (start < tail.size() && (static_cast<unsigned char>(tail[start]) & 0xc0) == 0x80) ++start;
            tail.erase(0, start);
        } else {
            auto end = MAX_BYTES;
            while (end && (static_cast<unsigned char>(tail[end]) & 0xc0) == 0x80) --end;
            tail.resize(end);
        }
    }
}

std::string OutputCapture::finish() {
    if (keepTail) {
        std::size_t lines = !tail.empty() && tail.back() != '\n' ? 1 : 0;
        for (std::size_t i = tail.size(); i > 0; --i) {
            if (tail[i - 1] == '\n' && ++lines > MAX_LINES) { tail.erase(0, i); break; }
        }
    } else {
        std::size_t lines = 0;
        for (std::size_t i = 0; i < tail.size(); ++i) {
            if (tail[i] == '\n' && ++lines == MAX_LINES) { tail.resize(i + 1); break; }
        }
    }
    if (file >= 0) {
        const int fd = file;
        file = -1;
        if (close(fd) != 0) throw std::runtime_error("Could not close tool output file");
    }
    return tool_output::utf8(tail);
}

bool OutputCapture::isTruncated() const { return !path.empty(); }
const std::filesystem::path& OutputCapture::getPath() const { return path; }
