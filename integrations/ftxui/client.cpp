#include "client.hpp"
#include <agent-runtime/common/path_resolution.hpp>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
namespace sos::tui {
namespace {
using namespace std::chrono_literals;
struct Descriptor {
    int value;
    ~Descriptor() { if (value >= 0) ::close(value); }
};
std::runtime_error error(const std::string& action) {
    return std::runtime_error(action + ": " + std::strerror(errno));
}
}
Client::~Client() { close(); }
bool Client::tryConnect(const std::string& path) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) throw std::runtime_error("Socket path is too long");
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    Descriptor fd{::socket(AF_UNIX, SOCK_STREAM, 0)};
    if (fd.value < 0) throw error("Cannot create socket");
    ::fcntl(fd.value, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    int enabled = 1;
    ::setsockopt(fd.value, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
    if (::connect(fd.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        if (errno == ENOENT || errno == ECONNREFUSED) return false;
        throw error("Cannot connect to daemon");
    }
    descriptor_ = fd.value;
    fd.value = -1;
    return true;
}
Json Client::connect(const std::string& socket, const std::filesystem::path& daemon) {
    const auto path = std::filesystem::absolute(socket).string();
    if (!tryConnect(path)) {
        Descriptor lock{::open((path + ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600)};
        if (lock.value < 0) throw error("Cannot open startup lock");
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (::flock(lock.value, LOCK_EX | LOCK_NB) != 0) {
            if (errno != EWOULDBLOCK) throw error("Cannot lock daemon startup");
            if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("Daemon startup lock timed out");
            std::this_thread::sleep_for(50ms);
        }
        if (!tryConnect(path)) {
            if (::access(daemon.c_str(), X_OK) != 0) throw std::runtime_error("Missing executable sosd next to sos-ftxui");
            struct stat status{};
            if (::lstat(path.c_str(), &status) == 0) {
                if (!S_ISSOCK(status.st_mode)) throw std::runtime_error("Socket path is occupied by a non-socket file");
                if (::unlink(path.c_str()) != 0) throw error("Cannot remove stale socket");
            } else if (errno != ENOENT) throw error("Cannot inspect socket path");
            const auto logPath = PathResolution::daemonLog();
            std::filesystem::create_directories(logPath.parent_path());
            posix_spawn_file_actions_t actions;
            posix_spawn_file_actions_init(&actions);
            posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
            posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, logPath.c_str(), O_CREAT | O_APPEND | O_WRONLY, 0600);
            posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
            posix_spawnattr_t attributes;
            posix_spawnattr_init(&attributes);
#ifdef POSIX_SPAWN_SETSID
            posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
#endif
            auto binary = std::filesystem::absolute(daemon).string();
            char* arguments[] = {binary.data(), const_cast<char*>(path.c_str()), nullptr};
            pid_t pid = 0;
            const int result = posix_spawn(&pid, binary.c_str(), &actions, &attributes, arguments, environ);
            posix_spawn_file_actions_destroy(&actions);
            posix_spawnattr_destroy(&attributes);
            if (result != 0) throw std::runtime_error("Cannot start sosd: " + std::string(std::strerror(result)));
            // Reap the shared daemon without tying its lifetime to this integration.
            std::thread([pid] { while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {} }).detach();
            while (!tryConnect(path)) {
                if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("Daemon startup timed out; see " + logPath.string());
                std::this_thread::sleep_for(50ms);
            }
        }
    }
    timeval timeout{5, 0};
    ::setsockopt(descriptor_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    auto hello = receive();
    if (hello.value("type", "") != "daemon.ready" || hello.value("protocolVersion", 0) != 7) {
        throw std::runtime_error("Unsupported daemon handshake: expected protocol 7. Close old integrations and rebuild.");
    }
    timeout.tv_sec = 0;
    ::setsockopt(descriptor_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    timeout.tv_sec = 5;
    ::setsockopt(descriptor_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    ready_ = true;
    return hello;
}
Json Client::receive() {
    while (true) {
        const auto newline = buffer_.find('\n');
        if (newline != std::string::npos) {
            if (newline + 1 > MAX_FRAME_BYTES) throw std::runtime_error("Daemon frame exceeds 16 MiB");
            auto message = Json::parse(buffer_.substr(0, newline));
            buffer_.erase(0, newline + 1);
            if (!message.is_object() || !message.contains("type") || !message["type"].is_string())
                throw std::runtime_error("Invalid daemon message");
            return message;
        }
        if (buffer_.size() >= MAX_FRAME_BYTES) throw std::runtime_error("Daemon frame exceeds 16 MiB");
        char bytes[65536];
        const auto count = ::recv(descriptor_, bytes, sizeof(bytes), 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::runtime_error("Daemon disconnected or receive timed out");
        buffer_.append(bytes, static_cast<std::size_t>(count));
    }
}
void Client::send(const Json& request) {
    const auto bytes = request.dump() + '\n';
    if (bytes.size() > MAX_FRAME_BYTES) throw std::runtime_error("Message exceeds the daemon's 16 MiB limit");
    std::lock_guard guard(writer_);
    std::size_t offset = 0;
    while (offset < bytes.size()) {
#ifdef MSG_NOSIGNAL
        constexpr int FLAGS = MSG_NOSIGNAL;
#else
        constexpr int FLAGS = 0;
#endif
        const auto count = ::send(descriptor_, bytes.data() + offset, bytes.size() - offset, FLAGS);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw error("Cannot send to daemon");
        offset += static_cast<std::size_t>(count);
    }
}
void Client::start(std::function<void(Json)> callback) {
    reader_ = std::thread([this, callback = std::move(callback)] {
        try {
            while (!stopping_) {
                auto event = receive();
                if (event["type"] == "integration.ping") {
                    send({{"type", "integration.pong"}, {"pingId", event.at("pingId").get<std::string>()}});
                } else callback(std::move(event));
            }
        } catch (const std::exception& exception) {
            if (!stopping_) callback({{"type", "client.disconnected"}, {"error", exception.what()}});
        }
    });
}
void Client::close() {
    if (descriptor_ < 0) return;
    stopping_ = true;
    if (ready_) { try { send({{"type", "integration.close"}}); } catch (...) {} }
    ::shutdown(descriptor_, SHUT_RDWR);
    if (reader_.joinable()) reader_.join();
    ::close(descriptor_);
    descriptor_ = -1;
    ready_ = false;
}
}
