#include "skill_catalog.hpp"
#include "skill_template.hpp"
#include "../tools/fs/common.hpp"
#include "../common/path_resolution.hpp"
#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

namespace {
namespace fs = std::filesystem;
using nlohmann::json;
constexpr std::size_t MAX_SKILL_BYTES = 128 * 1024;
constexpr std::size_t MAX_COMPATIBILITY_CHARACTERS = 500;
bool identifier(const std::string& name) {
    return !name.empty() && name.size() <= 64 && name.front() >= 'a' && name.front() <= 'z' &&
        name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-_") == std::string::npos;
}
void validateFrontmatter(const YAML::Node& node) {
    if (!node.IsMap()) throw std::invalid_argument("Expected a frontmatter mapping");
    const std::set<std::string> supported = {"name", "description", "arguments", "argument-hint",
        "argument-descriptions", "license", "compatibility", "metadata"};
    std::set<std::string> seen;
    for (const auto& field : node) {
        const auto key = field.first.as<std::string>();
        if (!supported.contains(key)) continue;
        if (!seen.insert(key).second) throw std::invalid_argument("Duplicate frontmatter field: " + key);
    }
}
std::string text(const YAML::Node& node) {
    if (!node.IsScalar()) throw std::invalid_argument("Expected a nonempty string");
    auto value = node.as<std::string>();
    if (fs_tools::trim(value).empty()) throw std::invalid_argument("Expected a nonempty string");
    return value;
}
void parseOptionalMetadata(const YAML::Node& yaml, SkillDefinition& skill) {
    if (const auto hint = yaml["argument-hint"]) {
        if (!hint.IsScalar()) throw std::invalid_argument("argument-hint must be a string");
        skill.argumentHint = hint.as<std::string>();
    }
    if (const auto descriptions = yaml["argument-descriptions"]) {
        if (!descriptions.IsMap())
            throw std::invalid_argument("argument-descriptions must be a string-to-string mapping");
        skill.argumentDescriptions.emplace();
        for (const auto& entry : descriptions) {
            const auto name = text(entry.first);
            if (!skill.argumentDescriptions->emplace(name, text(entry.second)).second)
                throw std::invalid_argument("Duplicate argument description: " + name);
        }
    }
    if (const auto license = yaml["license"]) skill.license = text(license);
    if (const auto compatibility = yaml["compatibility"]) {
        skill.compatibility = text(compatibility);
        const auto characters = std::count_if(skill.compatibility->begin(), skill.compatibility->end(),
            [](unsigned char byte) { return (byte & 0xC0) != 0x80; });
        if (characters > MAX_COMPATIBILITY_CHARACTERS)
            throw std::invalid_argument("compatibility exceeds 500 characters");
    }
    if (const auto metadata = yaml["metadata"]) {
        if (!metadata.IsMap()) throw std::invalid_argument("metadata must be a string-to-string mapping");
        skill.metadata.emplace();
        for (const auto& entry : metadata) {
            if (!entry.first.IsScalar() || !entry.second.IsScalar())
                throw std::invalid_argument("metadata keys and values must be strings");
            if (!skill.metadata->emplace(entry.first.as<std::string>(), entry.second.as<std::string>()).second)
                throw std::invalid_argument("Duplicate metadata key: " + entry.first.as<std::string>());
        }
    }
}
json optionalMetadata(const SkillDefinition& skill) {
    auto result = json::object();
    if (skill.argumentHint) result["argument-hint"] = *skill.argumentHint;
    if (skill.argumentDescriptions) result["argument-descriptions"] = *skill.argumentDescriptions;
    if (skill.license) result["license"] = *skill.license;
    if (skill.compatibility) result["compatibility"] = *skill.compatibility;
    if (skill.metadata) result["metadata"] = *skill.metadata;
    return result;
}
// Do not follow symlinks during implicit discovery, including directory ancestors.
void localPath(const fs::path& root, const fs::path& path) {
    const auto relative = path.lexically_relative(root);
    if (relative.empty() || relative.is_absolute()) throw std::invalid_argument("Invalid skill path");
    auto current = root;
    for (const auto& part : relative) {
        if (part == "..") throw std::invalid_argument("Skill path escapes its allowed root");
        current /= part;
        if (fs::is_symlink(fs::symlink_status(current)))
            throw std::invalid_argument("Symlinks are not supported in skills: " + current.string());
    }
}
std::vector<std::string> resourcePaths(const SkillDefinition& skill, const std::string& category) {
    std::vector<std::string> paths;
    const auto directory = skill.location / category;
    localPath(skill.root, directory);
    if (fs::exists(directory)) {
        for (const auto& entry : fs::recursive_directory_iterator(directory)) {
            localPath(skill.root, entry.path());
            if (entry.is_regular_file()) paths.push_back(entry.path().lexically_relative(skill.location).generic_string());
            if (paths.size() > 1000) throw std::invalid_argument("Skill has more than 1000 files in " + category);
        }
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}
std::vector<std::string> parseArgumentNames(const YAML::Node& args) {
    std::vector<std::string> result;
    if (!args) return result;
    if (args.IsMap())
        throw std::invalid_argument("The SOS arguments mapping is no longer supported; use a list of names and argument-hint");
    if (args.IsScalar()) {
        std::istringstream input(args.as<std::string>());
        for (std::string name; input >> name;) result.push_back(name);
    } else if (args.IsSequence()) {
        for (const auto& entry : args) result.push_back(text(entry));
    } else throw std::invalid_argument("arguments must be a list of names or a space-separated string");
    static const std::regex NAME(R"([A-Za-z_][A-Za-z0-9_-]*)");
    std::set<std::string> names;
    for (const auto& name : result)
        if (!std::regex_match(name, NAME) || name == "ARGUMENTS" || !names.insert(name).second)
            throw std::invalid_argument("Invalid, reserved or duplicate argument name: " + name);
    return result;
}

SkillDefinition parse(const fs::path& root, const fs::path& location) {
    const auto path = location / "SKILL.md";
    localPath(root, path);
    if (fs::file_size(path) > MAX_SKILL_BYTES) throw std::invalid_argument("SKILL.md exceeds 128 KiB");
    std::istringstream input(fs_tools::readText(path));
    std::string line, header;
    if (!std::getline(input, line) || fs_tools::trim(line) != "---")
        throw std::invalid_argument("SKILL.md must start with YAML frontmatter");
    bool closed = false;
    while (std::getline(input, line)) {
        if (fs_tools::trim(line) == "---") { closed = true; break; }
        header += line + '\n';
    }
    if (!closed) throw std::invalid_argument("Unclosed frontmatter");
    auto yaml = YAML::Load(header);
    validateFrontmatter(yaml);
    SkillDefinition skill;
    skill.name = text(yaml["name"]);
    if (!identifier(skill.name) || skill.name != location.filename().string())
        throw std::invalid_argument("Skill name must match its directory and use a lowercase identifier");
    skill.description = text(yaml["description"]);
    parseOptionalMetadata(yaml, skill);
    skill.location = location;
    skill.root = root;
    skill.content = std::string(std::istreambuf_iterator<char>(input), {});
    if (fs_tools::trim(skill.content).empty()) throw std::invalid_argument("Skill content is empty");
    skill.arguments = parseArgumentNames(yaml["arguments"]);
    return skill;
}
}

void from_json(const nlohmann::json& json, SkillInvocation& invocation) {
    if (!json.is_object() || !json.contains("name") || !json.at("name").is_string())
        throw std::invalid_argument("Skill invocation requires a name");
    invocation.name = json.at("name").get<std::string>();
    if (!identifier(invocation.name)) throw std::invalid_argument("Invalid skill name");
    invocation.input.clear();
    if (json.contains("input")) {
        if (!json.at("input").is_string()) throw std::invalid_argument("Skill input must be a string");
        invocation.input = json.at("input").get<std::string>();
    }
    for (const auto& [key, value] : json.items())
        if (key != "name" && key != "input") throw std::invalid_argument("Unknown skill invocation field: " + key);
}

void to_json(nlohmann::json& json, const SkillInvocation& invocation) {
    json = {{"name", invocation.name}, {"input", invocation.input}};
}

SkillCatalog::SkillCatalog(const std::filesystem::path& workspace) : root(fs::canonical(workspace)) {
    userDirectory = PathResolution::skillsDirectory();
    discover(userDirectory.parent_path().parent_path(), userDirectory, "user");
    discover(root, root / ".sos/skills", "workspace");
    std::sort(diagnostics.begin(), diagnostics.end());
}

void SkillCatalog::discover(const fs::path& boundary, const fs::path& directory, const std::string& source) {
    try {
        localPath(boundary, directory);
        if (!fs::exists(directory)) return;
        for (const auto& entry : fs::directory_iterator(directory)) {
            try {
                if (source == "workspace") skills.erase(entry.path().filename().string());
                localPath(boundary, entry.path());
                if (!entry.is_directory()) continue;
                auto skill = parse(boundary, entry.path());
                skill.source = source;
                skills.emplace(skill.name, std::move(skill));
            } catch (const std::exception& error) { diagnostics.push_back(entry.path().string() + ": " + error.what()); }
        }
    } catch (const std::exception& error) { diagnostics.push_back(error.what()); }
}

std::filesystem::path SkillCatalog::location(const std::string& name) const {
    const auto found = skills.find(name);
    if (found == skills.end()) throw std::invalid_argument("Unknown skill: " + name);
    return found->second.location;
}

nlohmann::json SkillCatalog::metadata(bool includeDisplayDetails) const {
    auto result = nlohmann::json::array();
    for (const auto& [name, skill] : skills) {
        result.push_back({{"name", name}, {"description", skill.description}, {"arguments", skill.arguments}, {"source", skill.source}});
        result.back().update(optionalMetadata(skill));
        if (includeDisplayDetails) {
            for (const std::string category : {"scripts", "references", "assets"}) {
                try { result.back()[category] = resourcePaths(skill, category); }
                catch (const std::exception& error) { result.back()[category + "_error"] = error.what(); }
            }
        }
    }
    return result;
}

std::string SkillCatalog::prompt() const {
    return "SOS skills (metadata only). This catalog replaces earlier catalogs.\n"
        "Skill directories: workspace " + (root / ".sos/skills").string() + "; user " + userDirectory.string() +
        "\n" +
        metadata().dump(2) + "\nSkill diagnostics:\n" + nlohmann::json(diagnostics).dump();
}

std::string SkillCatalog::load(const SkillInvocation& invocation) const {
    const auto found = skills.find(invocation.name);
    if (found == skills.end()) throw std::invalid_argument("Unknown skill: " + invocation.name);
    const auto& skill = found->second;
    localPath(skill.root, skill.location / "SKILL.md");
    const auto scripts = resourcePaths(skill, "scripts");
    std::string output = "Name: " + skill.name + "\nLocation: " + skill.location.string();
    const auto attributes = optionalMetadata(skill);
    if (!attributes.empty()) output += "\n\nSkill attributes (JSON data):\n" + attributes.dump(2);
    output += "\n\nInput (JSON string data):\n" + nlohmann::json(invocation.input).dump();
    output += "\n\nContent:\n" + skill_templates::expand(skill, invocation) +
        "\n\nAssociated scripts (paths only; not executed):\n";
    for (const auto& script : scripts) output += "- " + script + '\n';
    if (scripts.empty()) output += "(none)\n";
    return output;
}

nlohmann::json SkillCatalog::displayArguments(const SkillInvocation& invocation) const {
    const auto& skill = skills.at(invocation.name);
    std::vector<std::string> values;
    try { values = skill_templates::splitArguments(invocation.input); }
    catch (const std::invalid_argument&) {
        // Raw-input skills accept unmatched quotes; display must not reject them.
        values = {invocation.input};
    }
    return {{"argument_names", skill.arguments}, {"argument_values", values}};
}

ToolDefinition SkillTool::getDefinition() const {
    return {"skill", "Load a workspace or user SOS skill and expand $ARGUMENTS, $N, $ARGUMENTS[N] and declared $name placeholders using raw input. Arguments are positional, not required or validated against a schema. Does not execute scripts.",
        {{"type", "object"}, {"properties", {{"name", {{"type", "string"}}},
            {"input", {{"type", "string"}, {"description", "Raw argument text, preserved as typed. Defaults to empty."}}}}}, {"required", {"name"}}, {"additionalProperties", false}}};
}
ToolResult SkillTool::execute(const nlohmann::json& arguments, const ToolContext& context) const {
    fs_tools::checkpoint(context);
    const auto invocation = arguments.get<SkillInvocation>();
    if (!context.ask) throw std::invalid_argument("Skill requires a permission handler");
    context.ask({"read", {(catalog->location(invocation.name) / "SKILL.md").string()}, {"*"}, nlohmann::json::object()});
    const auto output = catalog->load(invocation);
    return {invocation.name, output, catalog->displayArguments(invocation)};
}
