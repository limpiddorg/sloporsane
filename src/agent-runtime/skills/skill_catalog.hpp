#ifndef SOS_SKILL_CATALOG_HPP
#define SOS_SKILL_CATALOG_HPP

#include "../tools/tool.hpp"
#include <map>
#include <memory>
#include <optional>

struct SkillDefinition {
    std::string name;
    std::string description;
    std::optional<std::string> license;
    std::optional<std::string> compatibility;
    std::optional<std::map<std::string, std::string>> metadata;
    std::filesystem::path location;
    std::filesystem::path root;
    std::string source;
    std::vector<std::string> arguments;
    std::optional<std::string> argumentHint;
    std::optional<std::map<std::string, std::string>> argumentDescriptions;
    std::string content;
};
struct SkillInvocation {
    std::string name;
    std::string input;
};
inline constexpr std::size_t MAX_STACKED_SKILLS = 6;
void from_json(const nlohmann::json& json, SkillInvocation& invocation);
void to_json(nlohmann::json& json, const SkillInvocation& invocation);

// A snapshot refreshed between turns. Workspace definitions override user skills.
class SkillCatalog {
public:
    SkillCatalog() = default;
    explicit SkillCatalog(const std::filesystem::path& workspace);
    nlohmann::json metadata(bool includeDisplayDetails = false) const;
    std::string prompt() const;
    std::string load(const SkillInvocation& invocation) const;
    nlohmann::json displayArguments(const SkillInvocation& invocation) const;
    std::filesystem::path location(const std::string& name) const;
    const std::vector<std::string>& errors() const { return diagnostics; }
private:
    std::filesystem::path root;
    std::filesystem::path userDirectory;
    void discover(const std::filesystem::path& boundary, const std::filesystem::path& directory, const std::string& source);
    std::map<std::string, SkillDefinition> skills;
    std::vector<std::string> diagnostics;
};

class SkillTool : public Tool {
public:
    explicit SkillTool(std::shared_ptr<SkillCatalog> catalog) : catalog(std::move(catalog)) {}
    ToolDefinition getDefinition() const override;
    ToolResult execute(const nlohmann::json& arguments, const ToolContext& context) const override;
private:
    std::shared_ptr<SkillCatalog> catalog;
};
#endif
