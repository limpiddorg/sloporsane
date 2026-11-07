#include "preferences.hpp"
#include <agent-runtime/common/path_resolution.hpp>

namespace mcp {
namespace {
std::map<std::string, Preference> decode(const nlohmann::json& json) {
    std::map<std::string, Preference> result;
    for (const auto& [name, entry] : json.items()) {
        if (!entry.is_object()) throw std::runtime_error("Invalid MCP preferences");
        Preference choice;
        if (entry.contains("enabled")) choice.enabled = entry.at("enabled").get<bool>();
        if (entry.contains("connected")) choice.connected = entry.at("connected").get<bool>();
        result.emplace(name, choice);
    }
    return result;
}
}

Preferences::Preferences() : file(PathResolution::mcpPreferencesFile()), choices(decode(file.read())) {}

void Preferences::attach(const std::shared_ptr<ServerConnection>& connection) {
    std::lock_guard lock(mutex);
    const auto name = connection->snapshot().server.name;
    if (const auto found = choices.find(name); found != choices.end()) {
        if (found->second.enabled) connection->setPreferredEnabled(*found->second.enabled);
        if (found->second.connected) connection->wantConnection(*found->second.connected, false);
    }
    connections.push_back(connection);
}

void Preferences::enable(const std::string& name, bool value) { update(name, value, false); }
void Preferences::connection(const std::string& name, bool value, bool enabled) { update(name, value, true, enabled); }

void Preferences::update(const std::string& name, bool value, bool connection, std::optional<bool> enabled) {
    std::lock_guard lock(mutex);
    // Save before publishing. A filesystem failure leaves live choices unchanged.
    file.update([&](nlohmann::json& json) {
        decode(json);
        json[name][connection ? "connected" : "enabled"] = value;
        if (enabled && !json[name].contains("enabled")) json[name]["enabled"] = *enabled;
        enabled = json[name].contains("enabled") ? std::optional(json[name]["enabled"].get<bool>()) : std::nullopt;
    });
    auto& choice = choices[name];
    (connection ? choice.connected : choice.enabled) = value;
    if (enabled) choice.enabled = enabled;
    for (auto iterator = connections.begin(); iterator != connections.end();) {
        const auto entry = iterator->lock();
        if (!entry) { iterator = connections.erase(iterator); continue; }
        if (entry->snapshot().server.name == name) {
            if (connection) entry->wantConnection(value);
            if (choice.enabled) entry->setPreferredEnabled(*choice.enabled);
        }
        ++iterator;
    }
}
}
