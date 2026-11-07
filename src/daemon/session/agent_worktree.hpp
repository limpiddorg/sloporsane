#ifndef SOS_AGENT_WORKTREE_HPP
#define SOS_AGENT_WORKTREE_HPP

#include <agent-runtime/core/process.hpp>
#include <nlohmann/json.hpp>

// User-visible Git workspaces, independent of Implement's disposable checkout.
class AgentWorktree {
public:
    explicit AgentWorktree(const std::filesystem::path& workspace);
    nlohmann::json list() const;
    std::filesystem::path create(const std::string& name) const;
    std::filesystem::path select(const std::filesystem::path& path) const;
    void remove(const std::filesystem::path& path, const std::vector<std::filesystem::path>& opened) const;
    static bool containsWorkspace(const std::filesystem::path& worktree, const std::filesystem::path& workspace);
private:
    std::string git(const std::vector<std::string>& arguments) const;
    std::filesystem::path repositoryStorageDirectory() const;
    std::filesystem::path root;
};

#endif // SOS_AGENT_WORKTREE_HPP
