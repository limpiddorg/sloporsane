#include "oauth.hpp"
#include "oauth_http.hpp"
#include "oauth_callback.hpp"
#include <algorithm>
#include <sstream>
#include <set>

namespace mcp {
namespace {
using Json = nlohmann::json;
struct Metadata {
    std::string resource, authorization, token, registration, scope, revocation;
};
bool supports(const Json& metadata, const char* key, const std::string& value) {
    if (!metadata.contains(key) || !metadata.at(key).is_array()) return false;
    const auto& list = metadata.at(key);
    return std::find(list.begin(), list.end(), value) != list.end();
}
Json successful(const oauth::HttpResult& response) {
    if (response.status < 200 || response.status >= 300)
        throw std::runtime_error("MCP OAuth endpoint refused the request (HTTP " + std::to_string(response.status) + ")");
    return response.body;
}
std::string combinedScopes(const std::string& challenged, const std::string& configured) {
    std::istringstream input(challenged + " " + configured);
    std::set<std::string> seen;
    std::string scope, result;
    while (input >> scope) if (seen.insert(scope).second) {
        if (!result.empty()) result += ' ';
        result += scope;
    }
    return result;
}
Metadata discover(const Remote& remote, const AuthorizationChallenge& previous, core::CancellationCheck cancelled) {
    const auto base = oauth::origin(remote.url);
    const auto probe = oauth::request(remote.url, "", "", cancelled, {{}, true, true});
    auto challenge = oauth::bearerChallenge(probe.challenges);
    if (!previous.metadata.empty()) challenge["resource_metadata"] = previous.metadata;
    if (!previous.scope.empty()) challenge["scope"] = combinedScopes(previous.scope, challenge.contains("scope") ? challenge.at("scope") : "");
    const auto metadataUrl = challenge.contains("resource_metadata") ? challenge.at("resource_metadata") :
        base + "/.well-known/oauth-protected-resource" + (oauth::path(remote.url) == "/" ? "" : oauth::path(remote.url));
    oauth::validateEndpoint(metadataUrl);
    if (base.starts_with("https://") && !oauth::origin(metadataUrl).starts_with("https://"))
        throw std::runtime_error("MCP OAuth metadata must not downgrade HTTPS");
    auto resourceResponse = oauth::request(metadataUrl, "", "", cancelled);
    if (resourceResponse.status == 404 && !challenge.contains("resource_metadata"))
        resourceResponse = oauth::request(base + "/.well-known/oauth-protected-resource", "", "", cancelled);
    std::string resource = remote.url, issuer = base, scope = combinedScopes(challenge.contains("scope") ? challenge.at("scope") : "", remote.oauthScope);
    if (resourceResponse.status >= 200 && resourceResponse.status < 300) {
        const auto metadata = resourceResponse.body;
        resource = metadata.at("resource").get<std::string>();
        // A root resource is common (Notion). It may cover the endpoint, but
        // metadata must not redirect resource identity to an unrelated origin.
        if (oauth::origin(resource) != base) throw std::runtime_error("MCP OAuth resource does not match server origin");
        const auto resourcePath = oauth::path(resource), endpointPath = oauth::path(remote.url);
        if (resourcePath != "/" && endpointPath != resourcePath && !endpointPath.starts_with(resourcePath + "/"))
            throw std::runtime_error("MCP OAuth resource does not cover the endpoint");
        const auto& issuers = metadata.at("authorization_servers");
        if (!issuers.is_array() || issuers.empty() || issuers.size() > 16) throw std::runtime_error("Invalid MCP OAuth authorization servers");
        issuer = issuers.front().get<std::string>();
        if (scope.empty() && metadata.contains("scopes_supported")) {
            for (const auto& item : metadata.at("scopes_supported")) {
                if (!scope.empty()) scope += ' ';
                scope += item.get<std::string>();
            }
        }
    } else successful(resourceResponse); // No guessed authorization server when resource metadata is absent.
    oauth::validateEndpoint(issuer);
    if (base.starts_with("https://") && !oauth::origin(issuer).starts_with("https://"))
        throw std::runtime_error("MCP OAuth issuer must not downgrade HTTPS");
    const auto issuerPath = oauth::path(issuer) == "/" ? "" : oauth::path(issuer);
    auto response = oauth::request(oauth::origin(issuer) + "/.well-known/oauth-authorization-server" + issuerPath, "", "", cancelled);
    if (response.status == 404)
        response = oauth::request(oauth::origin(issuer) + "/.well-known/openid-configuration" + issuerPath, "", "", cancelled);
    if (response.status == 404 && !issuerPath.empty())
        response = oauth::request(issuer + (issuer.ends_with('/') ? "" : "/") + ".well-known/openid-configuration", "", "", cancelled);
    const auto metadata = successful(response);
    if (metadata.at("issuer").get<std::string>() != issuer)
        throw std::runtime_error("MCP OAuth issuer mismatch");
    if (!supports(metadata, "code_challenge_methods_supported", "S256") || !supports(metadata, "response_types_supported", "code"))
        throw std::runtime_error("MCP OAuth server must support authorization code with S256 PKCE");
    Metadata result{resource, metadata.at("authorization_endpoint"), metadata.at("token_endpoint"),
        metadata.value("registration_endpoint", ""), scope, metadata.value("revocation_endpoint", "")};
    oauth::validateEndpoint(result.authorization);
    oauth::validateEndpoint(result.token);
    if (!result.registration.empty()) oauth::validateEndpoint(result.registration);
    if (!result.revocation.empty()) oauth::validateEndpoint(result.revocation);
    if (oauth::origin(issuer).starts_with("https://")) {
        for (const auto* endpoint : {&result.authorization, &result.token, &result.registration, &result.revocation})
            if (!endpoint->empty() && !oauth::origin(*endpoint).starts_with("https://"))
                throw std::runtime_error("MCP OAuth endpoint must not downgrade HTTPS");
    }
    if (result.scope.size() > 16384) throw std::runtime_error("MCP OAuth scope exceeds limit");
    return result;
}
Credentials tokens(const Json& json, Credentials credentials) {
    try {
        auto type = json.at("token_type").get<std::string>();
        std::transform(type.begin(), type.end(), type.begin(), [](unsigned char ch) { return std::tolower(ch); });
        if (type != "bearer") throw std::runtime_error("Invalid token type");
        credentials.accessToken = json.at("access_token");
        credentials.refreshToken = json.value("refresh_token", credentials.refreshToken);
        if (credentials.accessToken.empty() || credentials.accessToken.size() > 16384 || credentials.refreshToken.size() > 16384)
            throw std::runtime_error("Invalid tokens");
        credentials.expiresAt = 0;
        if (json.contains("expires_in")) {
            const auto& seconds = json.at("expires_in");
            if (!seconds.is_number_integer() || seconds <= 0 || seconds > 31536000) throw std::runtime_error("Invalid expiry");
            credentials.expiresAt = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count() + seconds.get<std::int64_t>();
        }
        return credentials;
    } catch (const std::exception&) { throw std::runtime_error("Invalid MCP OAuth token response"); }
}
Credentials exchange(Credentials credentials, std::map<std::string, std::string> fields, core::CancellationCheck cancelled) {
    fields["client_id"] = credentials.clientId;
    if (!credentials.resource.empty()) fields["resource"] = credentials.resource;
    if (credentials.tokenAuthMethod == "client_secret_post") fields["client_secret"] = credentials.clientSecret;
    oauth::HttpOptions options;
    if (credentials.tokenAuthMethod == "client_secret_basic") {
        fields.erase("client_id");
        options.authorization = oauth::basicAuthorization(credentials.clientId, credentials.clientSecret);
    }
    const auto response = oauth::request(credentials.tokenEndpoint, oauth::form(fields), "application/x-www-form-urlencoded", cancelled, options);
    if (response.status == 400 || response.status == 401 || response.status == 403) throw AuthenticationRequired();
    return tokens(successful(response), std::move(credentials));
}
Credentials registerClient(const Remote& remote, const Metadata& metadata, const std::string& redirect,
    const Credentials& registration, core::CancellationCheck cancelled) {
    if (remote.clientId.starts_with("https://")) {
        if (oauth::path(remote.clientId) == "/") throw std::runtime_error("Client metadata URL requires a path");
        const auto document = successful(oauth::request(remote.clientId, "", "", cancelled));
        if (document.at("client_id") != remote.clientId || !document.at("client_name").is_string() ||
            document.at("client_name").get<std::string>().empty() || !document.at("redirect_uris").is_array() ||
            std::find(document.at("redirect_uris").begin(), document.at("redirect_uris").end(), redirect) == document.at("redirect_uris").end())
            throw std::runtime_error("Client metadata does not match this client ID and callback; configure its registered callback port");
    }
    Credentials credentials;
    credentials.tokenEndpoint = metadata.token;
    credentials.resource = metadata.resource;
    credentials.revocationEndpoint = metadata.revocation;
    credentials.clientId = remote.clientId;
    if (!remote.clientId.empty() && remote.clientId == registration.clientId) {
        if (!registration.clientSecret.empty() && !registration.tokenEndpoint.empty() && registration.tokenEndpoint != metadata.token)
            throw std::runtime_error("OAuth token endpoint changed; review and re-enter client credentials");
        credentials.clientSecret = registration.clientSecret;
        credentials.tokenAuthMethod = registration.tokenAuthMethod;
    }
    if (credentials.clientId.empty()) {
        if (metadata.registration.empty()) throw std::runtime_error("MCP OAuth requires a registered client ID; this server does not offer registration");
        const auto registered = successful(oauth::request(metadata.registration, Json{
            {"client_name", "SOS"}, {"redirect_uris", {redirect}},
            {"grant_types", {"authorization_code", "refresh_token"}}, {"response_types", {"code"}},
            {"token_endpoint_auth_method", "none"}}.dump(), "application/json", cancelled));
        credentials.clientId = registered.at("client_id");
        credentials.clientSecret = registered.value("client_secret", "");
        credentials.tokenAuthMethod = registered.value("token_endpoint_auth_method", "none");
    }
    if (credentials.tokenAuthMethod != "none" && credentials.tokenAuthMethod != "client_secret_post" && credentials.tokenAuthMethod != "client_secret_basic")
        throw std::runtime_error("Unsupported MCP OAuth client authentication");
    if (credentials.tokenAuthMethod != "none" && credentials.clientSecret.empty()) throw std::runtime_error("Missing OAuth client secret");
    if (credentials.clientId.empty() || credentials.clientId.size() > 16384 || credentials.clientSecret.size() > 16384)
        throw std::runtime_error("Invalid MCP OAuth client registration");
    return credentials;
}

}
OAuthFlow::~OAuthFlow() {
    if (cancelled) cancelled->store(true);
    if (completion.valid()) completion.wait();
}
std::unique_ptr<OAuthFlow> startOAuth(const Remote& remote, core::CancellationCheck cancelled, std::shared_ptr<std::atomic<bool>> stop,
    const Credentials& registration, const AuthorizationChallenge& challenge) {
    if (!supportsOAuth(remote.authentication)) throw std::invalid_argument("Select MCP OAuth authentication first");
    const auto metadata = discover(remote, challenge, cancelled);
    auto callback = std::make_shared<oauth::Callback>(remote.callbackPort);
    const auto credentials = registerClient(remote, metadata, callback->redirect(), registration, cancelled);
    auto flow = std::make_unique<OAuthFlow>();
    flow->cancelled = stop ? std::move(stop) : std::make_shared<std::atomic<bool>>(false);
    const auto verifier = oauth::randomCode(), state = oauth::randomCode();
    std::map<std::string, std::string> parameters{{"response_type", "code"}, {"client_id", credentials.clientId},
        {"redirect_uri", callback->redirect()}, {"state", state}, {"code_challenge", oauth::challenge(verifier)},
        {"code_challenge_method", "S256"}, {"resource", metadata.resource}};
    if (!metadata.scope.empty()) parameters["scope"] = metadata.scope;
    flow->url = metadata.authorization + (metadata.authorization.find('?') == std::string::npos ? "?" : "&") + oauth::form(parameters);
    core::checkCancellation(cancelled);
    flow->completion = std::async(std::launch::async, [callback, credentials, verifier, state, stop = flow->cancelled] {
        const auto check = [stop] { return stop->load(); };
        const auto code = callback->wait(state, check);
        return exchange(credentials, {{"grant_type", "authorization_code"}, {"code", code},
            {"redirect_uri", callback->redirect()}, {"code_verifier", verifier}}, check);
    });
    return flow;
}
Credentials refreshOAuth(const Credentials& previous, core::CancellationCheck cancelled) {
    if (previous.refreshToken.empty() || previous.tokenEndpoint.empty() || previous.clientId.empty()) throw AuthenticationRequired();
    return exchange(previous, {{"grant_type", "refresh_token"}, {"refresh_token", previous.refreshToken}}, cancelled);
}

void revokeOAuth(const Credentials& credentials, core::CancellationCheck cancelled) {
    if (credentials.revocationEndpoint.empty()) throw std::runtime_error("OAuth server does not advertise token revocation");
    for (const auto& [token, hint] : {std::pair{credentials.refreshToken, "refresh_token"}, std::pair{credentials.accessToken, "access_token"}}) {
        if (token.empty()) continue;
        std::map<std::string, std::string> fields{{"token", token}, {"token_type_hint", hint}, {"client_id", credentials.clientId}};
        oauth::HttpOptions options{{}, true}; // RFC 7009 success may have an empty body.
        if (credentials.tokenAuthMethod == "client_secret_post") fields["client_secret"] = credentials.clientSecret;
        if (credentials.tokenAuthMethod == "client_secret_basic") {
            fields.erase("client_id");
            options.authorization = oauth::basicAuthorization(credentials.clientId, credentials.clientSecret);
        }
        successful(oauth::request(credentials.revocationEndpoint, oauth::form(fields), "application/x-www-form-urlencoded", cancelled, options));
    }
}
}
