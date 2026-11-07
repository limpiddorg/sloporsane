#include "provider_auth.hpp"
#include "openai_oauth.hpp"
#include <cstdlib>
#include <fstream>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <thread>

const char* authMethodName(AuthMethod method) {
    switch (method) {
        case AuthMethod::API_KEY: return "api";
        case AuthMethod::BROWSER: return "browser";
        case AuthMethod::DEVICE: return "device";
    }
    throw std::invalid_argument("Unknown authentication method");
}

AuthMethod parseAuthMethod(const std::string& value) {
    if (value == "api") return AuthMethod::API_KEY;
    if (value == "browser") return AuthMethod::BROWSER;
    if (value == "device") return AuthMethod::DEVICE;
    throw std::invalid_argument("Unknown authentication method");
}

namespace {
AuthMethod credentialMethod(const ProviderCredential& credential) {
    if (credential.kind == AuthKind::OAUTH && !credential.method)
        throw std::invalid_argument("OAuth credentials require an explicit authentication method");
    const auto method = credential.method.value_or(AuthMethod::API_KEY);
    if ((credential.kind == AuthKind::API) != (method == AuthMethod::API_KEY))
        throw std::invalid_argument("Authentication method does not match credential type");
    return method;
}
}

void validateApiKeyFormat(const std::string& key) {
    if (key.empty() || key.size() > 16384 || key.find_first_of(" \r\n\t") != std::string::npos)
        throw std::invalid_argument("API key must be nonempty and contain no whitespace");
}

void validateProviderId(const std::string& provider) {
    const bool custom = provider.starts_with("custom-") && provider.size() > 7 && provider.size() <= 80 &&
        provider.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-_", 7) == std::string::npos;
    if (provider != "openai" && provider != "openrouter" && !custom) throw std::invalid_argument("Unknown provider");
}

nlohmann::json readPrivateJson(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return nlohmann::json::object();
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Cannot read provider settings");
    auto value = nlohmann::json::parse(input, nullptr, false);
    if (!value.is_object()) throw std::runtime_error("Invalid provider settings file");
    return value;
}

void writePrivateJson(const std::filesystem::path& path, const nlohmann::json& value) {
    std::filesystem::create_directories(path.parent_path());
    std::string temporary = path.string() + ".XXXXXX";
    const int fd = ::mkstemp(temporary.data());
    if (fd < 0) throw std::runtime_error("Cannot create provider settings file");
    bool closed = false;
    try {
        if (::fchmod(fd, 0600) != 0) throw std::runtime_error("Cannot protect provider settings file");
        const auto data = value.dump(2) + "\n";
        std::size_t written = 0;
        while (written < data.size()) {
            const auto count = ::write(fd, data.data() + written, data.size() - written);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw std::runtime_error("Cannot write provider settings file");
            written += static_cast<std::size_t>(count);
        }
        if (::fsync(fd) != 0) throw std::runtime_error("Cannot flush provider settings file");
        const int result = ::close(fd);
        closed = true;
        if (result != 0) throw std::runtime_error("Cannot close provider settings file");
        std::filesystem::rename(temporary, path);
    } catch (...) {
        if (!closed) ::close(fd);
        ::unlink(temporary.c_str());
        throw;
    }
}

AuthStore::AuthStore(std::filesystem::path path) : path(std::move(path)) {}

std::string AuthStore::permissionKey() const {
    std::lock_guard lock(mutex);
    const auto data = readPrivateJson(path);
    if (!data.contains("permission_verifier")) return {};
    const auto key = data.at("permission_verifier").at("key").get<std::string>();
    validateApiKeyFormat(key);
    return key;
}

void AuthStore::setPermissionKey(const std::string& key) {
    validateApiKeyFormat(key);
    std::lock_guard lock(mutex);
    auto data = readPrivateJson(path);
    data["permission_verifier"] = {{"provider", "openrouter"}, {"type", "api"}, {"key", key}};
    writePrivateJson(path, data);
}

bool AuthStore::contains(const std::string& provider) const {
    validateProviderId(provider);
    std::lock_guard lock(mutex);
    return readPrivateJson(path).contains(provider);
}

void AuthStore::remove(const std::string& provider) {
    validateProviderId(provider);
    std::lock_guard lock(mutex);
    auto data = readPrivateJson(path);
    data.erase(provider);
    writePrivateJson(path, data);
}

ProviderCredential AuthStore::get(const std::string& provider) const {
    validateProviderId(provider);
    std::lock_guard lock(mutex);
    const auto data = readPrivateJson(path);
    if (!data.contains(provider)) throw std::runtime_error("Connect " + provider + " with /connect first");
    try {
        const auto& entry = data.at(provider);
        ProviderCredential result;
        if (entry.at("type") == "api") {
            result.key = entry.at("key").get<std::string>();
            validateApiKeyFormat(result.key);
        } else if (entry.at("type") == "oauth" && provider == "openai") {
            result.kind = AuthKind::OAUTH;
            result.access = entry.at("access").get<std::string>();
            result.refresh = entry.at("refresh").get<std::string>();
            result.expires = entry.at("expires").get<std::int64_t>();
            result.accountId = entry.value("accountId", "");
            if (result.refresh.empty()) throw std::invalid_argument("Missing refresh token");
        } else throw std::invalid_argument("Unknown credential type");
        result.method = parseAuthMethod(entry.at("method").get<std::string>());
        credentialMethod(result);
        return result;
    } catch (...) { throw std::runtime_error("Invalid stored credential for " + provider + "; reconnect with /connect"); }
}

void AuthStore::set(const std::string& provider, const ProviderCredential& credential) {
    validateProviderId(provider);
    nlohmann::json entry;
    const auto method = credentialMethod(credential);
    if (credential.kind == AuthKind::API) {
        validateApiKeyFormat(credential.key);
        entry = {{"type", "api"}, {"key", credential.key}};
    } else {
        if (provider != "openai" || credential.access.empty() || credential.refresh.empty())
            throw std::invalid_argument("Invalid OAuth credential");
        entry = {{"type", "oauth"}, {"access", credential.access}, {"refresh", credential.refresh},
                 {"expires", credential.expires}, {"accountId", credential.accountId}};
    }
    entry["method"] = authMethodName(method);
    std::lock_guard lock(mutex);
    auto data = readPrivateJson(path);
    data[provider] = std::move(entry);
    writePrivateJson(path, data);
}

ProviderAuthorization OpenRouterProviderAuth::authorize(core::CancellationCheck cancelled) {
    core::checkCancellation(cancelled);
    return {.bearer = store->get("openrouter").key};
}

ProviderAuthorization OpenAIProviderAuth::authorize(core::CancellationCheck cancelled) {
    // Both API replacement and OAuth refresh use this lock, avoiding stale refresh writes.
    std::unique_lock lock(store->refreshMutex, std::defer_lock);
    while (!lock.try_lock()) {
        core::checkCancellation(cancelled);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    core::checkCancellation(cancelled);
    auto credential = store->get("openai");
    if (credential.kind == AuthKind::API) return {.bearer = credential.key};
    if (credential.access.empty() || credential.expires <= oauthNow() + 60000) {
        credential = refreshOpenAICredential(credential, cancelled, settings);
        store->set("openai", credential);
    }
    return {.bearer = credential.access, .accountId = credential.accountId, .codex = true};
}
