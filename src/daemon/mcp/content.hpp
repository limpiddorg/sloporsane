#ifndef SOS_MCP_CONTENT_HPP
#define SOS_MCP_CONTENT_HPP

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <map>

namespace mcp {
enum class ContentKind { RESOURCE, TEMPLATE, PROMPT };
struct ContentArgument {
    std::string name, description;
    bool required = false;
};
struct ContentEntry {
    ContentKind kind;
    std::string name, description, target;
    std::vector<ContentArgument> arguments;
};
struct ContentSelection {
    ContentKind kind = ContentKind::RESOURCE;
    std::string target, uri;
    std::map<std::string, std::string> arguments;
};
ContentKind contentKind(const std::string& value);
void to_json(nlohmann::json& json, const ContentEntry& entry);
ContentEntry parseContentEntry(ContentKind kind, const nlohmann::json& json);
// Preview only: never interpreted as system/assistant messages or fetched links.
// Unsupported binary/audio content is reported, never silently included.
nlohmann::json previewContent(ContentKind kind, const nlohmann::json& result);
}
#endif // SOS_MCP_CONTENT_HPP
