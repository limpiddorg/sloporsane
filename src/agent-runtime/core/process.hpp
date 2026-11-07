#ifndef SOS_PROCESS_RESULT_HPP
#define SOS_PROCESS_RESULT_HPP

#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include <chrono>
#include <string_view>
#include <optional>

namespace core {
struct ProcessResult {
    int exitCode;
    std::string output;
    std::string error;
    bool stopped;
    bool timedOut = false;
};

struct ProcessOptions {
    std::chrono::milliseconds timeout{30000};
    bool mergeError = false;
    bool captureOutput = true;
    std::function<void(std::string_view)> onOutput;
    std::function<void(std::string_view)> onStdout;
    std::optional<std::filesystem::path> inputFile;
};

// Executes an argv directly, without a shell. A false onLine result stops the process.
ProcessResult runProcess(const std::filesystem::path& executable, const std::vector<std::string>& arguments,
    const std::filesystem::path& directory, const std::function<void()>& checkCancelled = {},
    const std::function<bool(const std::string&)>& onLine = {}, const ProcessOptions& options = {});
}

#endif // SOS_PROCESS_RESULT_HPP
