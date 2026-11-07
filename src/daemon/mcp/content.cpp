#include "content.hpp"
#include <agent-runtime/tools/tool.hpp>
#include <set>

namespace mcp {
namespace {
using Json = nlohmann::json;
void bounded(const std::string& text, std::size_t maximum, bool required = false) {
    if ((required && text.empty()) || text.size() > maximum || text.find('\0') != std::string::npos)
        throw std::runtime_error("Invalid MCP content catalogue");
}
const char* kindName(ContentKind kind) {
    switch (kind) {
        case ContentKind::RESOURCE: return "resource";
        case ContentKind::TEMPLATE: return "template";
        case ContentKind::PROMPT: return "prompt";
    }
    throw std::logic_error("Unknown MCP content kind");
}
class Preview {
public:
    void append(const std::string& value) {
        if (text.size() + value.size() + 2 > 1024 * 1024) throw std::runtime_error("MCP preview exceeds 1 MiB");
        text += value + "\n\n";
    }
    void resource(const Json& item) {
        append("Resource: " + item.at("uri").get<std::string>());
        if (item.contains("text")) append(item.at("text"));
        else if (item.value("mimeType", "").starts_with("image/")) image(item.at("mimeType"), item.at("blob"));
        else { append("[Binary resource cannot be added to this composer]"); supported = false; }
    }
    void block(const Json& item) {
        const auto type = item.at("type").get<std::string>();
        if (type == "text") append(item.at("text"));
        else if (type == "resource") resource(item.at("resource"));
        else if (type == "image") image(item.at("mimeType"), item.at("data"));
        else if (type == "resource_link") append("Resource link (not fetched): " + item.at("uri").get<std::string>());
        else { append("[Unsupported content: " + type + "]"); supported = false; }
    }
    Json result() {
        validateImages(images);
        Json attachments = Json::array();
        for (const auto& image : images) attachments.push_back({{"mime", image.mime}, {"data", image.data}});
        return {{"text", text}, {"images", attachments}, {"addable", supported && (!text.empty() || !images.empty())}};
    }
private:
    void image(std::string mime, std::string data) {
        Image value;
        value.mime = std::move(mime); value.data = std::move(data); value.origin = "mcp";
        images.push_back(std::move(value));
        validateImages(images);
        append("[Attached MCP image]");
    }
    std::string text;
    std::vector<Image> images;
    bool supported = true;
};
}
ContentKind contentKind(const std::string& value) {
    if (value == "resource") return ContentKind::RESOURCE;
    if (value == "template") return ContentKind::TEMPLATE;
    if (value == "prompt") return ContentKind::PROMPT;
    throw std::invalid_argument("Invalid MCP content kind");
}
ContentEntry parseContentEntry(ContentKind kind, const Json& json) {
    ContentEntry entry{kind, json.at("name"), json.value("description", ""),
        kind == ContentKind::PROMPT ? json.at("name") : json.at(kind == ContentKind::RESOURCE ? "uri" : "uriTemplate"), {}};
    bounded(entry.name, 1024, true); bounded(entry.description, 65536); bounded(entry.target, 16384, true);
    if (kind == ContentKind::PROMPT && json.contains("arguments")) {
        const auto& arguments = json.at("arguments");
        if (!arguments.is_array() || arguments.size() > 128) throw std::runtime_error("Invalid MCP prompt arguments");
        std::set<std::string> names;
        for (const auto& argument : arguments) {
            ContentArgument item{argument.at("name"), argument.value("description", ""), argument.value("required", false)};
            bounded(item.name, 128, true); bounded(item.description, 16384);
            if (!names.insert(item.name).second) throw std::runtime_error("Duplicate MCP prompt argument");
            entry.arguments.push_back(std::move(item));
        }
    }
    return entry;
}
void to_json(Json& json, const ContentEntry& entry) {
    Json arguments = Json::array();
    for (const auto& argument : entry.arguments) arguments.push_back({{"name", argument.name},
        {"description", argument.description}, {"required", argument.required}});
    json = {{"kind", kindName(entry.kind)}, {"name", entry.name}, {"description", entry.description},
        {"target", entry.target}, {"arguments", arguments}};
}
Json previewContent(ContentKind kind, const Json& result) {
    Preview preview;
    if (kind == ContentKind::PROMPT) {
        const auto& messages = result.at("messages");
        if (!messages.is_array() || messages.size() > 512) throw std::runtime_error("Invalid MCP prompt messages");
        for (const auto& message : messages) {
            const auto role = message.at("role").get<std::string>();
            if (role != "user" && role != "assistant") throw std::runtime_error("Invalid MCP prompt role");
            preview.append("Prompt message (" + role + "):");
            preview.block(message.at("content"));
        }
    } else {
        const auto& contents = result.at("contents");
        if (!contents.is_array() || contents.size() > 512) throw std::runtime_error("Invalid MCP resource contents");
        for (const auto& item : contents) preview.resource(item);
    }
    return preview.result();
}
}
