#include "config.hpp"
#include "json_file.hpp"
#include <agent-runtime/common/path_resolution.hpp>
#include <curl/curl.h>
#include "fingerprint.hpp"
#include <algorithm>
#include <memory>

namespace mcp {
namespace {
using Json = nlohmann::json;
bool safeName(const std::string& value) {
    return !value.empty() && value.size() <= 64 && value.find_first_not_of(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == std::string::npos;
}
void requireName(const std::string& value) {
    if (!safeName(value)) throw std::invalid_argument("MCP name must contain 1-64 ASCII letters, digits, underscores or hyphens");
}
void requireText(const std::string& value) {
    if (value.size() > 16384 || value.find('\0') != std::string::npos)
        throw std::invalid_argument("Invalid MCP configuration text");
}
void fields(const Json& json, std::initializer_list<const char*> allowed) {
    if (!json.is_object()) throw std::invalid_argument("MCP settings must be an object");
    for (const auto& [key, value] : json.items())
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
            throw std::invalid_argument("Unknown MCP configuration field");
}
void validateUrl(const std::string& value) {
    requireText(value);
    std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(curl_url(), curl_url_cleanup);
    if (!url || curl_url_set(url.get(), CURLUPART_URL, value.c_str(), 0) != CURLUE_OK)
        throw std::invalid_argument("Invalid MCP URL");
    const auto part = [&](CURLUPart key) {
        char* raw = nullptr;
        if (curl_url_get(url.get(), key, &raw, 0) != CURLUE_OK) return std::string{};
        std::unique_ptr<char, decltype(&curl_free)> memory(raw, curl_free);
        return std::string(raw);
    };
    const auto scheme = part(CURLUPART_SCHEME), host = part(CURLUPART_HOST);
    if (host.empty() || (scheme != "https" && !(scheme == "http" &&
        (host == "localhost" || host == "127.0.0.1" || host == "[::1]"))))
        throw std::invalid_argument("MCP requires HTTPS, or HTTP on loopback");
    if (!part(CURLUPART_USER).empty() || !part(CURLUPART_PASSWORD).empty() || !part(CURLUPART_FRAGMENT).empty())
        throw std::invalid_argument("MCP URL must not contain credentials or a fragment");
}
Json document(const Json& json) {
    fields(json, {"mcpServers"});
    if (json.empty()) return {{"mcpServers", Json::object()}};
    if (json.contains("mcpServers")) {
        if (!json.at("mcpServers").is_object())
            throw std::invalid_argument("Invalid MCP configuration document");
        return {{"mcpServers", json.at("mcpServers")}};
    }
    throw std::invalid_argument("Invalid MCP configuration document");
}

// Omit default values when writing the portable configuration.
Json configurationEntry(const Server& server) {
    Json entry = server;
    entry.erase("type");
    if (const auto* local = std::get_if<Local>(&server.connection)) {
        entry["command"] = local->command.front();
        entry["args"] = std::vector<std::string>(local->command.begin() + 1, local->command.end());
        entry["env"] = local->environment;
        entry.erase("environment");
        if (local->environment.empty()) entry.erase("env");
        if (local->cwd.empty()) entry.erase("cwd");
    } else if (std::get<Remote>(server.connection).authentication == Authentication::AUTO) entry.erase("authentication");
    if (server.enabled) entry.erase("enabled");
    if (server.startupTimeoutMs == 10000) entry.erase("startup_timeout_ms");
    if (server.requestTimeoutMs == 30000) entry.erase("request_timeout_ms");
    return entry;
}
}

const char* scopeName(Scope scope) { return scope == Scope::GLOBAL ? "global" : "workspace"; }
bool supportsOAuth(Authentication authentication) {
    return authentication == Authentication::OAUTH || authentication == Authentication::AUTO;
}

void validateEnvironment(const std::map<std::string, std::string>& environment) {
    if (environment.size() > 128) throw std::invalid_argument("Too many MCP environment entries");
    for (const auto& [key, value] : environment) {
        if (key.empty() || key.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos)
            throw std::invalid_argument("Invalid MCP environment name");
        requireText(value);
    }
}

void validate(const Server& server) {
    requireName(server.name);
    for (const auto timeout : {server.startupTimeoutMs, server.requestTimeoutMs})
        if (timeout < 100 || timeout > 600000) throw std::invalid_argument("MCP timeouts must be between 100 and 600000 ms");
    std::visit([](const auto& connection) { validate(connection); }, server.connection);
}

void validate(const Local& local) {
    if (local.command.empty() || local.command.front().empty() || local.command.size() > 128)
        throw std::invalid_argument("MCP command needs an executable and at most 127 arguments");
    for (const auto& arg : local.command) requireText(arg);
    requireText(local.cwd.string());
    validateEnvironment(local.environment);
}

void validate(const Remote& remote) {
    validateUrl(remote.url);
    requireText(remote.clientId);
    requireText(remote.oauthScope);
    if (!supportsOAuth(remote.authentication) && (!remote.clientId.empty() || !remote.oauthScope.empty() || remote.callbackPort != 0))
        throw std::invalid_argument("OAuth options require OAuth authentication");
}

void to_json(Json& json, const Server& server) {
    json = {{"enabled", server.enabled}, {"startup_timeout_ms", server.startupTimeoutMs}, {"request_timeout_ms", server.requestTimeoutMs}};
    if (const auto* local = std::get_if<Local>(&server.connection)) {
        json.update({{"type", "stdio"}, {"command", local->command.front()},
            {"args", std::vector<std::string>(local->command.begin() + 1, local->command.end())},
            {"cwd", local->cwd.string()}, {"env", local->environment}});
    } else {
        const auto& remote = std::get<Remote>(server.connection);
        const auto auth = remote.authentication == Authentication::AUTO ? "auto" : remote.authentication == Authentication::OAUTH ? "oauth" : remote.authentication == Authentication::TOKEN ? "token" : "none";
        json.update({{"type", "http"}, {"url", remote.url}, {"authentication", auth}});
        if (!remote.clientId.empty()) json["client_id"] = remote.clientId;
        if (!remote.oauthScope.empty()) json["oauth_scope"] = remote.oauthScope;
        if (remote.callbackPort) json["callback_port"] = remote.callbackPort;
    }
}

void from_json(const Json& json, Server& server) {
    try {
        const auto type = json.value("type", json.contains("command") ? "stdio" : "http");
        if (type == "stdio") {
            fields(json, {"type", "command", "args", "env", "cwd", "enabled", "startup_timeout_ms", "request_timeout_ms"});
            std::vector<std::string> command{json.at("command").get<std::string>()};
            const auto args = json.value("args", std::vector<std::string>{});
            command.insert(command.end(), args.begin(), args.end());
            server.connection = Local{command, json.value("cwd", ""),
                json.value("env", std::map<std::string, std::string>{})};
        } else if (type == "http") {
            fields(json, {"type", "url", "authentication", "client_id", "oauth_scope", "callback_port", "enabled", "startup_timeout_ms", "request_timeout_ms"});
            const auto auth = json.value("authentication", "auto");
            if (auth != "none" && auth != "token" && auth != "oauth" && auth != "auto") throw std::invalid_argument("Unknown MCP authentication method");
            server.connection = Remote{json.at("url"), auth == "auto" ? Authentication::AUTO : auth == "oauth" ? Authentication::OAUTH : auth == "token" ? Authentication::TOKEN : Authentication::NONE,
                json.value("client_id", ""), json.value("oauth_scope", "")};
            if (json.contains("callback_port")) {
                const auto& port = json.at("callback_port");
                if (!port.is_number_integer() || port < 0 || port > 65535) throw std::invalid_argument("Invalid OAuth callback port");
                std::get<Remote>(server.connection).callbackPort = port.get<unsigned short>();
            }
        } else throw std::invalid_argument("MCP transport must be stdio or http");
        server.enabled = json.value("enabled", true);
        const auto timeout = [&](const char* key, int fallback) {
            if (!json.contains(key)) return fallback;
            const auto& value = json.at(key);
            if (!value.is_number_integer() || value < 100 || value > 600000)
                throw std::invalid_argument("MCP timeouts must be integers between 100 and 600000 ms");
            return value.get<int>();
        };
        server.startupTimeoutMs = timeout("startup_timeout_ms", 10000);
        server.requestTimeoutMs = timeout("request_timeout_ms", 30000);
    } catch (const Json::exception&) { throw std::invalid_argument("Invalid MCP configuration fields"); }
}

ConfigStore::ConfigStore(std::filesystem::path workspace) : workspace(std::filesystem::canonical(workspace)) {}

std::filesystem::path ConfigStore::path(Scope scope) const {
    return scope == Scope::GLOBAL ? PathResolution::mcpConfigFile() : workspace / ".sos/mcps.json";
}

Configuration ConfigStore::load() const {
    Configuration result;
    for (const auto scope : {Scope::GLOBAL, Scope::WORKSPACE}) {
        Json json;
        try { json = document(JsonFile(path(scope)).read()); }
        catch (const std::exception&) {
            // A malformed workspace could shadow any global entry: fail closed.
            result.servers.clear();
            result.issues.push_back({scope, "", "Cannot load MCP configuration; fix the file before connecting"});
            return result;
        }
        for (const auto& [name, entry] : json.at("mcpServers").items()) {
            result.servers.erase(name); // Even an invalid local entry shadows global.
            try {
                auto server = entry.get<Server>();
                server.name = name;
                server.scope = scope;
                validate(server);
                result.servers.emplace(name, std::move(server));
            } catch (const std::exception&) {
                result.issues.push_back({scope, safeName(name) ? name : "", "Invalid MCP server configuration"});
            }
        }
    }
    return result;
}

void ConfigStore::save(const Server& server) const {
    validate(server);
    JsonFile(path(server.scope)).update([&](Json& json) {
        json = document(json);
        json["mcpServers"][server.name] = configurationEntry(server);
    });
}

void ConfigStore::remove(Scope scope, const std::string& name) const {
    requireName(name);
    JsonFile(path(scope)).update([&](Json& json) {
        json = document(json);
        json["mcpServers"].erase(name);
    });
}

std::string ConfigStore::identity(const Server& server) const {
    validate(server);
    Json connection = server;
    connection.erase("enabled");
    connection.erase("startup_timeout_ms");
    connection.erase("request_timeout_ms");
    // Local programs depend on their launch directory even when configured globally.
    const bool workspaceBound = server.scope == Scope::WORKSPACE || std::holds_alternative<Local>(server.connection);
    return fingerprint(Json{{"name", server.name}, {"scope", scopeName(server.scope)},
        {"workspace", workspaceBound ? workspace.string() : ""}, {"connection", connection}}.dump());
}
}
