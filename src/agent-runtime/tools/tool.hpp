#ifndef SOS_TOOL_HPP
#define SOS_TOOL_HPP

#include "../file_attachment.hpp"
#include "../image.hpp"
#include "permission.hpp"

#include <nlohmann/json.hpp>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

struct ToolDefinition {
    std::string id;
    std::string description;
    nlohmann::json parameters;
};

struct ToolResult {
    std::string title;
    std::string output;
    // Tool-specific data for the caller/UI; not automatically sent to the model.
    nlohmann::json metadata = nlohmann::json::object();
    std::vector<FileAttachment> attachments;
    std::vector<Image> images;
};

struct ApprovedToolAction {
    std::string id;
    std::string tool;
    nlohmann::json arguments;
    std::filesystem::path directory;
    std::filesystem::path workspace;
    std::optional<double> confidence;
};

using FileMutationHandler = std::function<void(const std::vector<std::filesystem::path>&, const std::function<void()>&, const std::string&)>;

struct ToolContext {
    std::filesystem::path directory;
    std::filesystem::path worktree;
    // Returns only when allowed; throws on denial. The caller supplies the policy.
    std::function<void(const ToolPermissionRequest&)> ask;
    // Optional cancellation checkpoint; throws if execution should stop.
    std::function<void()> checkCancelled;
    // When set, this is the sole initially authorized root, independent of the session's working directory.
    std::optional<std::filesystem::path> permissionRoot;
    std::function<Image(const std::string&)> readImage;
    std::function<ApprovedToolAction(const std::string&)> approvedAction;
    std::function<std::string(const std::string&, const std::vector<std::string>&,
        const std::function<void()>&)> askQuestion;
    // Optional owner hook after permission/revalidation. Editing tools hold the
    // mutation lock; Bash passes no paths and does not hold it during execution.
    FileMutationHandler mutateFiles;
};

class Tool {
public:
    virtual ~Tool() = default;
    virtual ToolDefinition getDefinition() const = 0;
    virtual ToolResult execute(const nlohmann::json& arguments, const ToolContext& context) const = 0;
};

#endif // SOS_TOOL_HPP
