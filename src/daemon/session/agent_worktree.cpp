#include "agent_worktree.hpp"
#include "worktree_listing.hpp"
#include "../snapshots/git_snapshot.hpp"
#include <agent-runtime/common/path_resolution.hpp>
#include <openssl/sha.h>
#include <string_view>

extern char** environ;
namespace fs = std::filesystem;

AgentWorktree::AgentWorktree(const fs::path& workspace) {
    const auto found = GitSnapshot::discover(workspace);
    if (!found) throw std::runtime_error("Worktree requires a Git repository");
    root = *found;
}

std::string AgentWorktree::git(const std::vector<std::string>& arguments) const {
    std::vector<std::string> args;
    for (auto variable = environ; *variable; ++variable) {
        const std::string entry(*variable);
        if (entry.starts_with("GIT_")) args.insert(args.end(), {"-u", entry.substr(0, entry.find('='))});
    }
    args.insert(args.end(), {"GIT_TERMINAL_PROMPT=0", "LC_ALL=C", "git", "-c", "core.hooksPath=/dev/null",
        "-c", "core.fsmonitor=false", "-c", "gc.auto=0", "-c", "maintenance.auto=false"});
    args.insert(args.end(), arguments.begin(), arguments.end());
    const auto result = core::runProcess("/usr/bin/env", args, root);
    if (result.exitCode || result.stopped || result.timedOut)
        throw std::runtime_error("Worktree Git operation failed: " + result.error);
    auto output = result.output;
    if (!output.empty() && output.back() == '\n') output.pop_back();
    return output;
}

nlohmann::json AgentWorktree::list() const {
    auto entries = nlohmann::json::array();
    const auto output = git({"worktree", "list", "--porcelain"});
    nlohmann::json entry;
    for (const auto& field : worktreeListingFields(output)) {
        if (field.starts_with("worktree ")) {
            if (!entry.is_null()) entries.push_back(entry);
            entry = {{"path", field.substr(9)}};
        } else if (field.starts_with("HEAD ")) entry["head"] = field.substr(5);
        else if (field.starts_with("branch ")) entry["branch"] = field.substr(7);
        else if (field == "detached") entry["detached"] = true;
        else if (field == "bare") entry["bare"] = true;
        else if (field.starts_with("prunable")) entry["prunable"] = true;
    }
    if (!entry.is_null()) entries.push_back(entry);
    if (!entries.empty()) entries.front()["primary"] = true;
    return entries;
}

bool AgentWorktree::containsWorkspace(const fs::path& worktree, const fs::path& workspace) {
    if (worktree.empty() || workspace.empty()) return false;
    const auto relative = fs::weakly_canonical(workspace).lexically_relative(fs::weakly_canonical(worktree));
    return !relative.empty() && *relative.begin() != "..";
}

void AgentWorktree::remove(const fs::path& path, const std::vector<fs::path>& opened) const {
    const auto target = select(path);
    const auto entries = list();
    if (fs::weakly_canonical(entries.front().at("path").get<std::string>()) == target)
        throw std::logic_error("The primary checkout cannot be deleted");
    for (const auto& workspace : opened) {
        if (containsWorkspace(target, workspace))
            throw std::logic_error("Worktree is opened in a session and cannot be deleted");
    }
    // Git refuses dirty or locked worktrees. Never force or delete the branch.
    git({"worktree", "remove", target.string()});
}

fs::path AgentWorktree::select(const fs::path& path) const {
    if (!path.is_absolute()) throw std::invalid_argument("Worktree path must be absolute");
    const auto target = fs::canonical(path);
    for (const auto& entry : list()) {
        if (entry.value("bare", false) || entry.value("prunable", false)) continue;
        if (fs::weakly_canonical(entry.at("path").get<std::string>()) == target) return target;
    }
    throw std::invalid_argument("Destination must be a registered worktree of the same repository");
}

fs::path AgentWorktree::repositoryStorageDirectory() const {
    // Shared by all worktrees of this checkout, but distinct for separate clones.
    const auto common = fs::canonical(root / git({"rev-parse", "--git-common-dir"})).string();
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(common.data()), common.size(), hash);
    constexpr char HEX[] = "0123456789abcdef";
    std::string identifier;
    for (std::size_t i = 0; i < 8; ++i) {
        identifier += HEX[hash[i] >> 4];
        identifier += HEX[hash[i] & 15];
    }
    auto label = fs::path(list().front().at("path").get<std::string>()).filename().string();
    for (auto& c : label) {
        if (std::string_view("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_").find(c) == std::string_view::npos)
            c = '-';
    }
    if (label.empty()) label = "repo";
    return PathResolution::workspacesDirectory() / (label + "-" + identifier);
}

fs::path AgentWorktree::create(const std::string& name) const {
    if (name.empty() || name.size() > 64 || name.front() == '-' ||
        name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos)
        throw std::invalid_argument("Worktree name must contain 1-64 ASCII letters, digits, underscores or hyphens, without a leading hyphen");
    const auto head = git({"rev-parse", "--verify", "HEAD^{commit}"});
    const auto destination = fs::weakly_canonical(repositoryStorageDirectory() / name);
    const auto relative = destination.lexically_relative(root);
    if (!relative.empty() && *relative.begin() != "..")
        throw std::runtime_error("Worktree storage must be outside the source repository");
    if (fs::exists(destination)) throw std::runtime_error("Worktree destination already exists");
    fs::create_directories(destination.parent_path());
    git({"worktree", "add", "-b", "sos/" + name, destination.string(), head});
    return fs::canonical(destination);
}
