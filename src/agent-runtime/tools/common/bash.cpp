#include "bash.hpp"
#include "bash_analysis.hpp"
#include "output_capture.hpp"
#include "../fs/common.hpp"
#include "../../core/process.hpp"

#include <stdexcept>
#include <limits>
#include <sstream>

void from_json(const nlohmann::json& json, BashArguments& arguments) {
    json.at("command").get_to(arguments.command);
    if (arguments.command.find_first_not_of(" \t\r\n") == std::string::npos || arguments.command.find('\0') != std::string::npos)
        throw std::invalid_argument("command must be nonempty and contain no null bytes");
    json.at("mainCommand").get_to(arguments.mainCommand);
    if (arguments.mainCommand.find_first_not_of(" \t\r\n") == std::string::npos || arguments.mainCommand.size() > 64 ||
        arguments.mainCommand.find_first_of("\r\n") != std::string::npos || arguments.mainCommand.find('\0') != std::string::npos)
        throw std::invalid_argument("mainCommand must be a nonempty single-line label of at most 64 bytes, without null bytes");
    arguments.workdir = json.contains("workdir") ? std::optional(json.at("workdir").get<std::string>()) : std::nullopt;
    if (json.contains("timeout")) {
        const auto& timeout = json.at("timeout");
        if (!timeout.is_number_integer() || timeout <= 0 || timeout > std::numeric_limits<std::int64_t>::max())
            throw std::invalid_argument("timeout must be a positive integer in milliseconds");
        arguments.timeout = timeout.get<std::int64_t>();
    }
}

ToolDefinition BashTool::getDefinition() const {
    return {"bash", "Execute a command using /bin/bash in the specified workdir (default: workspace directory). "
        "Supports pipes, redirects and &&. Use workdir instead of cd and quote paths containing spaces. "
        "Use this tool for package installation, builds, tests, git and launching programs; prefer the filesystem tools for file operations. "
        "To monitor an external operation until it finishes (such as a CI run or deployment), use wait instead. "
        "Read files with read, not cat, head or sed; use glob and grep for discovery and search. "
        "Each call is a fresh non-interactive shell with stdin closed: shell variables and cd do not persist. "
        "timeout is in milliseconds and defaults to 120000. "
        "On timeout the process group is terminated. Output includes stdout/stderr and exit status. "
        "Above 2000 lines or 50 KiB, full output is saved to a file and the tail is returned. "
        "Do not launch a long-running interactive program when a terminating test can verify the result.",
        {{"type", "object"}, {"properties", {
            {"command", {{"type", "string"}, {"description", "The shell command to execute"}}},
            {"mainCommand", {{"type", "string"}, {"minLength", 1}, {"maxLength", 64},
                {"description", "Short display-only name of the main command, without arguments (e.g. git, pytest, cmake). "
                    "For python -m pytest use pytest; for compound commands choose the one matching the main intent. "
                    "Does not affect execution or permissions."}}},
            {"workdir", {{"type", "string"}, {"description", "Working directory; defaults to the workspace"}}},
            {"timeout", {{"type", "integer"}, {"minimum", 1}, {"description", "Timeout in milliseconds"}}}
        }}, {"required", {"command", "mainCommand"}}}};
}

ToolResult BashTool::execute(const nlohmann::json& arguments, const ToolContext& context) const {
    const auto params = arguments.get<BashArguments>();
    const auto cwd = fs_tools::resolve(context, params.workdir.value_or(context.directory.string()));
    const auto analysis = analyzeBash(params.command, cwd);
    ToolPermissionRequest permission{"bash", analysis.commands, analysis.approvalScopes,
        {{"command", params.command}, {"workdir", cwd.string()}, {"timeout", params.timeout}}};
    permission.readOnly = analysis.readOnly;
    context.ask(permission);
    auto pathContext = context;
    pathContext.ask = [&](ToolPermissionRequest request) {
        request.readOnly = analysis.readOnly;
        context.ask(request);
    };
    if (!std::filesystem::is_directory(cwd)) throw std::invalid_argument("workdir must be an existing directory");
    fs_tools::checkExternal(pathContext, cwd, true);
    for (const auto& path : analysis.paths) fs_tools::checkExternal(pathContext, path, std::filesystem::is_directory(path));
    OutputCapture capture(context.worktree / ".sos/tool-output");
    core::ProcessOptions options;
    options.timeout = std::chrono::milliseconds(params.timeout);
    options.mergeError = true;
    options.captureOutput = false;
    options.onOutput = [&](std::string_view chunk) { capture.append(chunk); };
    core::ProcessResult result{};
    std::string output;
    // An empty path list requests a workspace checkpoint, not an editing-tool
    // journal entry. The owner also finalizes it when runProcess throws.
    fs_tools::mutate(context, {}, [&] {
        result = core::runProcess("/bin/bash", {"-c", params.command}, cwd, context.checkCancelled, {}, options);
        output = capture.finish();
    });
    if (output.empty()) output = "(no output)";
    if (capture.isTruncated()) output = "...output truncated...\n\nFull output saved to: " + capture.getPath().string() + "\n\n" + output;
    output += "\n\n<shell_metadata>\n";
    if (result.timedOut) output += "Command terminated after exceeding timeout " + std::to_string(params.timeout) + " ms.\n";
    else output += "Exit code: " + std::to_string(result.exitCode) + "\n";
    output += "</shell_metadata>";
    nlohmann::json metadata{{"exit", result.timedOut ? nlohmann::json(nullptr) : nlohmann::json(result.exitCode)},
        {"timedOut", result.timedOut}, {"truncated", capture.isTruncated()}};
    if (capture.isTruncated()) metadata["outputPath"] = capture.getPath().string();
    return {params.command, output, metadata, {}};
}

ToolDefinition WaitTool::getDefinition() const {
    auto definition = BashTool::getDefinition();
    definition.id = "wait";
    definition.description =
        "Wait for an external operation to finish by running a monitoring command using /bin/bash. "
        "Use only to observe an external operation, such as a specific CI run, deployment or remote job. "
        "Provide a short status phrase (at most five words, preferably four or five) describing what is being awaited, "
        "e.g. Waiting for CI completion or Monitoring deployment progress. Waiting is common but not required. "
        "Do not include trailing dots; the UI animates them. This label does not affect execution or permissions. "
        "Use bash for ordinary execution, including tests, builds and installations, regardless of duration. "
        "Prefer an existing waiting command, e.g. gh run watch <run-id> --exit-status. "
        "If none exists, supply a bounded polling script with explicit success and failure conditions, "
        "a delay between checks and bounded retries for transient errors. A sleep alone does not verify completion. "
        "The command must stay alive until the observed condition is met; merely triggering a job is not enough. "
        "No intermediate output wakes the model: this tool returns only on process exit, timeout or cancellation. "
        "Timeout defaults to 3600000 ms (one hour); choose an explicit duration when appropriate. "
        "Timeout or cancellation stops the local process group, not the remote operation. "
        "Uses the same permissions, workdir, closed stdin and output capture as bash. "
        "Output includes stdout/stderr and exit status. Above 2000 lines or 50 KiB, full output is saved "
        "to a file and the tail is returned. Use workdir instead of cd.";
    definition.parameters["properties"]["status"] = {{"type", "string"}, {"minLength", 1}, {"maxLength", 80},
        {"description", "Display-only status phrase, at most five words (prefer four or five), without trailing dots. "
            "Use wording appropriate to the operation, usually Waiting, but Monitoring or other wording is welcome."}};
    definition.parameters["required"].push_back("status");
    return definition;
}

ToolResult WaitTool::execute(const nlohmann::json& arguments, const ToolContext& context) const {
    const auto status = arguments.at("status").get<std::string>();
    std::istringstream words(status);
    std::size_t count = 0;
    for (std::string word; words >> word;) ++count;
    if (count == 0 || count > 5 || status.size() > 80 || status.find_first_of("\r\n") != std::string::npos ||
        status.find('\0') != std::string::npos)
        throw std::invalid_argument("status must be a single-line phrase of at most five words and 80 bytes");
    auto params = arguments;
    if (!params.contains("timeout")) params["timeout"] = 3600000;
    return BashTool::execute(params, context);
}
