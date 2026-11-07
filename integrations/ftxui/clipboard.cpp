#include "attachments.hpp"
#include <agent-runtime/common/path_resolution.hpp>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
extern char** environ;
namespace sos::tui {
namespace {
struct Temporary {
    std::string path;
    ~Temporary() { if (!path.empty()) ::unlink(path.c_str()); }
};
bool copyWith(const std::vector<std::string>& command, const std::string& path) {
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, path.c_str(), O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    std::vector<char*> args;
    for (const auto& item : command) args.push_back(const_cast<char*>(item.c_str()));
    args.push_back(nullptr);
    pid_t child = -1;
    const auto error = ::posix_spawnp(&child, args.front(), &actions, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (error != 0) return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    int status = 0;
    do {
        const auto result = ::waitpid(child, &status, WNOHANG);
        if (result == child) return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (result < 0 && errno != EINTR) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    ::kill(child, SIGKILL);
    while (::waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
    return false;
}
}
void copyNativeClipboard(const std::string& text) {
    if (std::getenv("SSH_CONNECTION") || std::getenv("SSH_TTY") || text.empty()) return;
    const auto directory = PathResolution::cacheDirectory();
    std::filesystem::create_directories(directory);
    Temporary file{(directory / "clipboard-XXXXXX").string()};
    const auto descriptor = ::mkstemp(file.path.data());
    if (descriptor < 0) { file.path.clear(); return; }
    ::close(descriptor);
    { std::ofstream output(file.path, std::ios::binary); output << text; if (!output) return; }
#ifdef __APPLE__
    copyWith({"/usr/bin/pbcopy"}, file.path);
#else
    if (std::getenv("WAYLAND_DISPLAY") && copyWith({"wl-copy"}, file.path)) return;
    if (copyWith({"xclip", "-selection", "clipboard"}, file.path)) return;
    copyWith({"xsel", "--clipboard", "--input"}, file.path);
#endif
}
}
