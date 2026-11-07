#include "stdio_transport.hpp"
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <thread>

namespace mcp {
namespace {
constexpr std::size_t MAX_MESSAGE_BYTES = 4 * 1024 * 1024;
struct SpawnActions {
    posix_spawn_file_actions_t value;
    SpawnActions() { if (posix_spawn_file_actions_init(&value)) throw std::runtime_error("Cannot prepare MCP process"); }
    ~SpawnActions() { posix_spawn_file_actions_destroy(&value); }
};
struct SpawnAttributes {
    posix_spawnattr_t value;
    SpawnAttributes() { if (posix_spawnattr_init(&value)) throw std::runtime_error("Cannot prepare MCP process"); }
    ~SpawnAttributes() { posix_spawnattr_destroy(&value); }
};
void check(int error) { if (error) throw std::runtime_error("Cannot configure MCP process"); }
}

StdioTransport::StdioTransport(const Local& local, const std::filesystem::path& workspace, const Credentials& credentials) {
    try { launch(local, workspace, credentials); }
    catch (...) { close(); throw; }
}
StdioTransport::~StdioTransport() { close(); }

void StdioTransport::launch(const Local& local, const std::filesystem::path& workspace, const Credentials& credentials) {
    validate(local);
    validateEnvironment(credentials.environment);
    const auto cwd = local.cwd.empty() ? workspace : local.cwd.is_absolute() ? local.cwd : workspace / local.cwd;
    if (!std::filesystem::is_directory(cwd)) throw std::runtime_error("MCP working directory does not exist");
    std::map<std::string, std::string> environment;
    // Do not implicitly expose provider tokens or arbitrary shell credentials.
    for (const auto* key : {"PATH", "HOME", "TMPDIR", "LANG", "LC_ALL", "SYSTEMROOT"})
        if (const auto* value = std::getenv(key)) environment[key] = value;
    for (const auto& [key, value] : local.environment) environment[key] = value;
    for (const auto& [key, value] : credentials.environment) environment[key] = value;
    std::vector<std::string> entries;
    for (const auto& [key, value] : environment) entries.push_back(key + "=" + value);
    std::vector<char*> env, args;
    for (auto& entry : entries) env.push_back(entry.data());
    env.push_back(nullptr);
    for (const auto& arg : local.command) args.push_back(const_cast<char*>(arg.c_str()));
    args.push_back(nullptr);

    int pair[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) throw std::runtime_error("Cannot create MCP process channel");
    for (auto& fd : pair) {
        if (fd >= 3) continue;
        const auto copy = ::fcntl(fd, F_DUPFD_CLOEXEC, 3);
        if (copy < 0) {
            ::close(pair[0]);
            ::close(pair[1]);
            throw std::runtime_error("Cannot configure MCP channel descriptors");
        }
        ::close(fd);
        fd = copy;
    }
    socket = pair[0];
    const int child = pair[1];
    try {
        if (::fcntl(socket, F_SETFD, FD_CLOEXEC) < 0 || ::fcntl(child, F_SETFD, FD_CLOEXEC) < 0 ||
            ::fcntl(socket, F_SETFL, O_NONBLOCK) < 0) throw std::runtime_error("Cannot configure MCP channel");
#ifdef SO_NOSIGPIPE
        const int enabled = 1;
        if (::setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)))
            throw std::runtime_error("Cannot protect MCP channel");
#endif
        SpawnActions actions;
        check(posix_spawn_file_actions_adddup2(&actions.value, child, STDIN_FILENO));
        check(posix_spawn_file_actions_adddup2(&actions.value, child, STDOUT_FILENO));
        check(posix_spawn_file_actions_addopen(&actions.value, STDERR_FILENO, "/dev/null", O_WRONLY, 0));
        check(posix_spawn_file_actions_addclose(&actions.value, socket));
        check(posix_spawn_file_actions_addclose(&actions.value, child));
        check(posix_spawn_file_actions_addchdir_np(&actions.value, cwd.c_str()));
        SpawnAttributes attributes;
        check(posix_spawnattr_setflags(&attributes.value, POSIX_SPAWN_SETPGROUP));
        check(posix_spawnattr_setpgroup(&attributes.value, 0));
        if (posix_spawnp(&pid, args.front(), &actions.value, &attributes.value, args.data(), env.data())) {
            pid = -1;
            throw std::runtime_error("Cannot start MCP executable");
        }
        ::close(child);
    } catch (...) { ::close(child); throw; }
}

void StdioTransport::wait(short events, Deadline deadline, core::CancellationCheck cancelled) {
    for (;;) {
        core::checkCancellation(cancelled);
        if (socket < 0) throw std::runtime_error("MCP process is disconnected");
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) throw std::runtime_error("MCP request timed out");
        pollfd descriptor{socket, events, 0};
        const auto result = ::poll(&descriptor, 1, static_cast<int>(std::min<std::int64_t>(remaining, 25)));
        if (result < 0 && errno == EINTR) continue;
        if (result < 0) throw std::runtime_error("MCP process channel failed");
        if (result > 0) return; // recv/send distinguish EOF, buffered bytes and errors.
    }
}

void StdioTransport::send(const nlohmann::json& message, Deadline deadline, core::CancellationCheck cancelled) {
    const auto bytes = message.dump() + "\n";
    if (bytes.size() > MAX_MESSAGE_BYTES) throw std::runtime_error("MCP message exceeds 4 MiB");
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        wait(POLLOUT, deadline, cancelled);
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags = MSG_NOSIGNAL;
#endif
        const auto count = ::send(socket, bytes.data() + offset, bytes.size() - offset, flags);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count <= 0) throw std::runtime_error("MCP process channel closed while sending");
        offset += count;
    }
}

nlohmann::json StdioTransport::receive(Deadline deadline, core::CancellationCheck cancelled) {
    std::array<char, 4096> buffer{};
    for (;;) {
        const auto newline = pending.find('\n');
        if (newline != std::string::npos) {
            auto result = nlohmann::json::parse(pending.substr(0, newline), nullptr, false);
            pending.erase(0, newline + 1);
            if (result.is_discarded()) throw std::runtime_error("Invalid MCP process JSON");
            return result;
        }
        wait(POLLIN, deadline, cancelled);
        const auto count = ::recv(socket, buffer.data(), buffer.size(), 0);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count <= 0) throw std::runtime_error("MCP process exited before replying");
        pending.append(buffer.data(), count);
        if (pending.size() > MAX_MESSAGE_BYTES && pending.find('\n') >= MAX_MESSAGE_BYTES)
            throw std::runtime_error("MCP message exceeds 4 MiB");
    }
}

void StdioTransport::close() noexcept {
    if (socket >= 0) { ::close(socket); socket = -1; }
    pending.clear();
    if (pid <= 0) return;
    ::kill(-pid, SIGTERM);
    int status = 0;
    for (unsigned attempt = 0; attempt < 10; ++attempt) {
        // Keep the leader unreaped until the final group signal: a child which
        // ignores SIGTERM must not survive merely because its parent exited.
        // WNOWAIT also keeps our PID reserved while signalling the group.
        siginfo_t info{};
        const auto result = ::waitid(P_PID, pid, &info, WEXITED | WNOHANG | WNOWAIT);
        if (result < 0 && errno == ECHILD) { pid = -1; return; }
        if (result == 0 && info.si_pid == pid) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ::kill(-pid, SIGKILL);
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    pid = -1;
}
}
