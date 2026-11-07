#include "oauth_callback.hpp"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <algorithm>
#include <array>
#include <map>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace mcp::oauth {
namespace {
std::string base64(const unsigned char* bytes, std::size_t size) {
    std::string result(4 * ((size + 2) / 3) + 1, '\0');
    EVP_EncodeBlock(reinterpret_cast<unsigned char*>(result.data()), bytes, size);
    result.pop_back();
    std::replace(result.begin(), result.end(), '+', '-');
    std::replace(result.begin(), result.end(), '/', '_');
    while (result.ends_with('=')) result.pop_back();
    return result;
}
std::string decode(const std::string& value) {
    const auto hex = [](char ch) -> unsigned {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        throw std::runtime_error("Invalid OAuth callback encoding");
    };
    std::string result;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%') {
            if (i + 2 >= value.size()) throw std::runtime_error("Invalid OAuth callback encoding");
            result += static_cast<char>((hex(value[i + 1]) << 4) | hex(value[i + 2]));
            i += 2;
        } else result += value[i] == '+' ? ' ' : value[i];
    }
    return result;
}
std::map<std::string, std::string> parse(const std::string& request) {
    constexpr std::string_view PREFIX = "GET /callback?";
    if (!request.starts_with(PREFIX) || request.find("\r\n\r\n") == std::string::npos) return {};
    const auto end = request.find(' ', PREFIX.size());
    if (end == std::string::npos) return {};
    const auto query = request.substr(PREFIX.size(), end - PREFIX.size());
    std::map<std::string, std::string> result;
    for (std::size_t start = 0; start < query.size();) {
        const auto next = query.find('&', start);
        const auto item = query.substr(start, next - start);
        const auto equal = item.find('=');
        if (equal == std::string::npos || !result.emplace(decode(item.substr(0, equal)), decode(item.substr(equal + 1))).second)
            return {};
        if (next == std::string::npos) break;
        start = next + 1;
    }
    return result;
}
struct Socket { int fd; ~Socket() { ::close(fd); } };
void reply(int fd, bool valid) {
    const std::string text = valid ? "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nAuthorization received. Return to SOS."
        : "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nInvalid authorization callback.";
#ifdef SO_NOSIGPIPE
    int value = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &value, sizeof(value));
#endif
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
#endif
    ::send(fd, text.data(), text.size(), flags);
}
}
std::string randomCode() {
    std::array<unsigned char, 32> bytes;
    if (RAND_bytes(bytes.data(), bytes.size()) != 1) throw std::runtime_error("Cannot generate OAuth randomness");
    return base64(bytes.data(), bytes.size());
}
std::string challenge(const std::string& verifier) {
    unsigned char bytes[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(verifier.data()), verifier.size(), bytes);
    return base64(bytes, sizeof(bytes));
}
Callback::Callback(unsigned short port) {
    socket = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    socklen_t length = sizeof(address);
    if (socket < 0 || ::fcntl(socket, F_SETFD, FD_CLOEXEC) < 0 ||
        ::bind(socket, reinterpret_cast<sockaddr*>(&address), length) || ::listen(socket, 4) ||
        ::getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length)) {
        if (socket >= 0) ::close(socket);
        throw std::runtime_error("Cannot listen for MCP OAuth callback");
    }
    uri = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "/callback";
}
Callback::~Callback() { ::close(socket); }
std::string Callback::wait(const std::string& state, core::CancellationCheck cancelled) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
    for (;;) {
        core::checkCancellation(cancelled);
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("MCP OAuth authorization timed out");
        pollfd ready{socket, POLLIN, 0};
        if (::poll(&ready, 1, 50) <= 0) continue;
        Socket client{::accept(socket, nullptr, nullptr)};
        if (client.fd < 0) continue;
        ::fcntl(client.fd, F_SETFD, FD_CLOEXEC);
        ::fcntl(client.fd, F_SETFL, O_NONBLOCK);
        std::string request;
        const auto readDeadline = std::min(deadline, std::chrono::steady_clock::now() + std::chrono::seconds(2));
        while (request.size() < 16384 && request.find("\r\n\r\n") == std::string::npos && std::chrono::steady_clock::now() < readDeadline) {
            core::checkCancellation(cancelled);
            pollfd readable{client.fd, POLLIN, 0};
            if (::poll(&readable, 1, 50) <= 0) continue;
            char bytes[1024];
            const auto count = ::recv(client.fd, bytes, sizeof(bytes), 0);
            if (count <= 0) break;
            request.append(bytes, count);
        }
        std::map<std::string, std::string> params;
        try { params = parse(request); } catch (const std::exception&) {}
        const bool sameState = params.contains("state") && params.at("state") == state;
        const bool valid = sameState && params.contains("code") && !params.at("code").empty() && !params.contains("error");
        reply(client.fd, valid);
        if (valid) return params.at("code");
        if (sameState && params.contains("error")) throw std::runtime_error("MCP OAuth authorization was denied");
    }
}
}
