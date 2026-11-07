#include "auth_store.hpp"
#include "json_file.hpp"
#include <agent-runtime/common/path_resolution.hpp>

namespace mcp {
namespace {
using Json = nlohmann::json;
void requireIdentity(const std::string& identity) {
    if (identity.size() != 64 || identity.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::invalid_argument("Invalid MCP credential identity");
}

void validate(const Credentials& value) {
    for (const auto* text : {&value.accessToken, &value.refreshToken, &value.clientId, &value.clientSecret,
        &value.tokenEndpoint, &value.resource, &value.tokenAuthMethod, &value.revocationEndpoint})
        if (text->size() > 16384 || text->find_first_of("\r\n") != std::string::npos || text->find('\0') != std::string::npos)
            throw std::invalid_argument("Invalid MCP credential value");
    if (value.expiresAt < 0 || value.environment.size() > 128)
        throw std::invalid_argument("Invalid MCP credentials");
    validateEnvironment(value.environment);
    if (!value.tokenEndpoint.empty()) mcp::validate(Remote{value.tokenEndpoint});
    if (!value.resource.empty()) mcp::validate(Remote{value.resource});
    if (!value.revocationEndpoint.empty()) mcp::validate(Remote{value.revocationEndpoint});
    if (value.tokenAuthMethod != "none" && value.tokenAuthMethod != "client_secret_post" && value.tokenAuthMethod != "client_secret_basic")
        throw std::invalid_argument("Unsupported MCP OAuth token authentication method");
}

Json document(Json json) {
    if (json.empty()) return {{"version", 1}, {"servers", Json::object()}};
    if (json.size() != 2 || !json.contains("version") || !json.at("version").is_number_integer() ||
        json.at("version") != 1 || !json.contains("servers") || !json.at("servers").is_object())
        throw std::runtime_error("Invalid MCP credentials file");
    return json;
}

Json encode(const Credentials& value) {
    return {{"access_token", value.accessToken}, {"refresh_token", value.refreshToken}, {"expires_at", value.expiresAt},
        {"client_id", value.clientId}, {"client_secret", value.clientSecret}, {"environment", value.environment},
        {"token_endpoint", value.tokenEndpoint}, {"resource", value.resource}, {"token_auth_method", value.tokenAuthMethod},
        {"revocation_endpoint", value.revocationEndpoint}};
}

Credentials decode(const Json& json) {
    try {
        if (!json.is_object() || json.size() != 10 || !json.at("expires_at").is_number_integer() ||
            json.at("expires_at") < 0 || json.at("expires_at") > INT64_MAX)
            throw std::runtime_error("Invalid MCP credentials");
        Credentials result{json.at("access_token"), json.at("refresh_token"), json.at("expires_at"),
            json.at("client_id"), json.at("client_secret"), json.at("environment")};
        json.at("token_endpoint").get_to(result.tokenEndpoint);
        json.at("resource").get_to(result.resource);
        json.at("token_auth_method").get_to(result.tokenAuthMethod);
        json.at("revocation_endpoint").get_to(result.revocationEndpoint);
        validate(result);
        return result;
    } catch (const std::exception&) { throw std::runtime_error("Invalid MCP credentials"); }
}
}

AuthStore::AuthStore(std::filesystem::path path) : path(std::move(path)) {}
AuthStore::AuthStore() : AuthStore(PathResolution::mcpAuthFile()) {}

bool AuthStore::trusted(const std::string& identity) const {
    requireIdentity(identity);
    const auto servers = document(JsonFile(path).read()).at("servers");
    if (!servers.contains(identity)) return false;
    const auto& entry = servers.at(identity);
    return entry.is_object() && entry.contains("trusted") && entry.at("trusted") == true;
}

void AuthStore::trust(const std::string& identity, bool accepted) const {
    requireIdentity(identity);
    JsonFile(path).update([&](Json& json) {
        json = document(std::move(json));
        json["servers"][identity]["trusted"] = accepted;
    });
}

std::optional<Credentials> AuthStore::get(const std::string& identity) const {
    requireIdentity(identity);
    const auto servers = document(JsonFile(path).read()).at("servers");
    if (!servers.contains(identity)) return {};
    const auto& entry = servers.at(identity);
    if (!entry.is_object()) throw std::runtime_error("Invalid MCP credentials");
    if (!entry.contains("credentials")) return {};
    return decode(entry.at("credentials"));
}

void AuthStore::set(const std::string& identity, const Credentials& credentials) const {
    requireIdentity(identity);
    validate(credentials);
    JsonFile(path).update([&](Json& json) {
        json = document(std::move(json));
        json["servers"][identity]["credentials"] = encode(credentials);
    });
}

void AuthStore::forget(const std::string& identity) const {
    requireIdentity(identity);
    JsonFile(path).update([&](Json& json) {
        json = document(std::move(json));
        json["servers"].erase(identity);
    });
}

void AuthStore::replaceIfCurrent(const std::string& identity, const Credentials& previous, const Credentials& next) const {
    requireIdentity(identity);
    validate(next);
    JsonFile(path).update([&](Json& json) {
        json = document(std::move(json));
        const auto& servers = json.at("servers");
        if (!servers.contains(identity) || !servers.at(identity).value("trusted", false) ||
            !servers.at(identity).contains("credentials") || decode(servers.at(identity).at("credentials")) != previous)
            throw std::runtime_error("MCP credentials or trust changed during OAuth refresh");
        json["servers"][identity]["credentials"] = encode(next);
    });
}
}
