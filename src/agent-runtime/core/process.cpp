#include "process.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace core {
namespace {
struct Pipe {
    std::array<int, 2> fd{-1, -1};
    Pipe() {
        if (pipe(fd.data()) != 0) throw std::runtime_error("Could not create process pipe");
        for (const auto value : fd) fcntl(value, F_SETFD, FD_CLOEXEC);
    }
    void closeEnd(std::size_t index) { if (fd[index] >= 0) close(fd[index]); fd[index] = -1; }
    ~Pipe() { closeEnd(0); closeEnd(1); }
};
void checked(int code) {
    if (code) throw std::runtime_error(std::string("Process setup failed: ") + std::strerror(code));
}
}

ProcessResult runProcess(const std::filesystem::path& executable, const std::vector<std::string>& arguments,
    const std::filesystem::path& directory, const std::function<void()>& checkCancelled,
    const std::function<bool(const std::string&)>& onLine, const ProcessOptions& options) {
    if (options.timeout.count() <= 0) throw std::invalid_argument("Process timeout must be positive");
    if (checkCancelled) checkCancelled();
    std::vector<std::string> storage{executable.string()};
    storage.insert(storage.end(), arguments.begin(), arguments.end());
    for (const auto& value : storage)
        if (value.find('\0') != std::string::npos) throw std::invalid_argument("Null byte in process argument");
    std::vector<char*> argv;
    for (auto& value : storage) argv.push_back(value.data());
    argv.push_back(nullptr);
    Pipe output;
    Pipe error;
    posix_spawn_file_actions_t actions;
    checked(posix_spawn_file_actions_init(&actions));
    struct ActionsGuard { posix_spawn_file_actions_t* value; ~ActionsGuard() { posix_spawn_file_actions_destroy(value); } } guard{&actions};
    checked(posix_spawn_file_actions_addopen(&actions, STDIN_FILENO,
        options.inputFile ? options.inputFile->c_str() : "/dev/null", O_RDONLY, 0));
    checked(posix_spawn_file_actions_adddup2(&actions, output.fd[1], STDOUT_FILENO));
    checked(posix_spawn_file_actions_adddup2(&actions, options.mergeError ? output.fd[1] : error.fd[1], STDERR_FILENO));
    checked(posix_spawn_file_actions_addchdir_np(&actions, directory.c_str()));
    posix_spawnattr_t attributes;
    checked(posix_spawnattr_init(&attributes));
    struct AttributesGuard { posix_spawnattr_t* value; ~AttributesGuard() { posix_spawnattr_destroy(value); } } attrGuard{&attributes};
    checked(posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP));
    checked(posix_spawnattr_setpgroup(&attributes, 0));
    pid_t pid;
    checked(posix_spawnp(&pid, executable.c_str(), &actions, &attributes, argv.data(), environ));
    output.closeEnd(1);
    error.closeEnd(1);
    ProcessResult result{0, {}, {}, false};
    int status = 0;
    bool reaped = false;
    const auto reap = [&] {
        if (!reaped) {
            while (waitpid(pid, &status, 0) < 0) {
                if (errno != EINTR) throw std::runtime_error("Could not wait for child process");
            }
            reaped = true;
        }
    };
    try {
        const auto start = std::chrono::steady_clock::now();
        std::string pending;
        auto terminationTime = std::chrono::steady_clock::time_point::max();
        while (!reaped || output.fd[0] >= 0 || error.fd[0] >= 0) {
            if (checkCancelled) checkCancelled();
            const auto now = std::chrono::steady_clock::now();
            if (!result.timedOut && std::chrono::duration_cast<std::chrono::milliseconds>(now - start) >= options.timeout) {
                result.timedOut = true;
                kill(-pid, SIGKILL);
                terminationTime = now;
            }
            if (terminationTime != std::chrono::steady_clock::time_point::max() && now - terminationTime > std::chrono::seconds(1)) {
                // A detached descendant may still own an inherited pipe.
                output.closeEnd(0);
                error.closeEnd(0);
            }
            if (!reaped) {
                const auto waited = waitpid(pid, &status, WNOHANG);
                if (waited == pid) reaped = true;
                else if (waited < 0 && errno != EINTR) throw std::runtime_error("Could not wait for child process");
            }
            if (reaped && output.fd[0] < 0 && error.fd[0] < 0) break;
            pollfd descriptors[]{{output.fd[0], POLLIN, 0}, {error.fd[0], POLLIN, 0}};
            if (poll(descriptors, 2, 100) < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error("Could not poll child process");
            }
            for (int index = 0; index < 2; ++index) {
                if (!descriptors[index].revents) continue;
                auto& pipe = index == 0 ? output : error;
                char buffer[8192];
                const auto count = read(pipe.fd[0], buffer, sizeof(buffer));
                if (count < 0) {
                    if (errno == EINTR) continue;
                    throw std::runtime_error("Could not read child process output");
                }
                if (count == 0) { pipe.closeEnd(0); continue; }
                auto& destination = index == 0 ? result.output : result.error;
                if (options.captureOutput) destination.append(buffer, static_cast<std::size_t>(count));
                if (options.onOutput) options.onOutput(std::string_view(buffer, static_cast<std::size_t>(count)));
                if (index == 0 && options.onStdout) options.onStdout(std::string_view(buffer, static_cast<std::size_t>(count)));
                if (destination.size() > 8 * 1024 * 1024) throw std::runtime_error("Process output exceeded 8 MiB");
                if (index == 0 && onLine && !result.stopped) {
                    pending.append(buffer, static_cast<std::size_t>(count));
                    for (;;) {
                        const auto end = pending.find('\n');
                        if (end == std::string::npos) break;
                        auto line = pending.substr(0, end);
                        pending.erase(0, end + 1);
                        if (!onLine(line)) {
                            result.stopped = true;
                            kill(-pid, SIGKILL);
                            terminationTime = std::chrono::steady_clock::now();
                            break;
                        }
                    }
                }
            }
        }
        if (onLine && !result.stopped && !pending.empty()) result.stopped = !onLine(pending);
        reap();
    } catch (...) {
        kill(-pid, SIGKILL);
        if (!reaped) {
            try { reap(); } catch (...) {}
        }
        throw;
    }
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    return result;
}
}
