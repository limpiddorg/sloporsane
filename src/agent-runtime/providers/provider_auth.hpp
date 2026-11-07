#ifndef SOS_PROVIDER_AUTH_HPP
#define SOS_PROVIDER_AUTH_HPP

#include "../core/cancellation.hpp"
#include "../common/path_resolution.hpp"
#include <filesystem>
#include <chrono>
#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <nlohmann/json.hpp>

enum class AuthKind { API, OAUTH };
enum class AuthMethod { API_KEY, BROWSER, DEVICE };
const char* authMethodName(AuthMethod method);
AuthMethod parseAuthMethod(const std::string& value);

struct ProviderCredential {
    AuthKind kind = AuthKind::API;
    std::string key;
    std::string access;
    std::string refresh;
    std::string accountId;
    std::int64_t expires = 0;
    std::optional<AuthMethod> method;
};

struct ProviderAuthorization {
    std::string bearer;
    std::string accountId;
    bool codex = false;
};

struct OpenAIOAuthSettings {
    std::string issuer = "https://auth.openai.com";
    std::string clientId = "app_EMoamEEZ73f0CkXaXp7hrann";
    unsigned short callbackPort = 1455;
    std::chrono::seconds deadline{600};
};

struct ProviderAuthFlow {
    ~ProviderAuthFlow() { if (cancelled) cancelled->store(true); }
    std::string url;
    std::string instructions;
    std::shared_ptr<std::atomic<bool>> cancelled;
    std::future<ProviderCredential> completion;
};

// Shared by title generation, agents and connection dialogs. Never serialize it in sessions.
class AuthStore {
public:
    explicit AuthStore(std::filesystem::path path = PathResolution::authFile());
    ProviderCredential get(const std::string& provider) const;
    bool contains(const std::string& provider) const;
    void remove(const std::string& provider);
    void set(const std::string& provider, const ProviderCredential& credential);
    std::string permissionKey() const;
    void setPermissionKey(const std::string& key);
    std::mutex refreshMutex;
private:
    std::filesystem::path path;
    mutable std::mutex mutex;
};

// Atomic owner-only JSON storage, also used for the non-secret provider selection.
void writePrivateJson(const std::filesystem::path& path, const nlohmann::json& value);
nlohmann::json readPrivateJson(const std::filesystem::path& path);
void validateProviderId(const std::string& provider);
void validateApiKeyFormat(const std::string& key);

class ProviderAuth {
public:
    virtual ~ProviderAuth() = default;
    virtual ProviderAuthorization authorize(core::CancellationCheck cancelled = {}) = 0;
};

class OpenRouterProviderAuth : public ProviderAuth {
public:
    explicit OpenRouterProviderAuth(std::shared_ptr<AuthStore> store) : store(std::move(store)) {}
    ProviderAuthorization authorize(core::CancellationCheck cancelled = {}) override;
private:
    std::shared_ptr<AuthStore> store;
};

class OpenAIProviderAuth : public ProviderAuth {
public:
    explicit OpenAIProviderAuth(std::shared_ptr<AuthStore> store, OpenAIOAuthSettings settings = {})
        : store(std::move(store)), settings(std::move(settings)) {}
    ProviderAuthorization authorize(core::CancellationCheck cancelled = {}) override;
private:
    std::shared_ptr<AuthStore> store;
    OpenAIOAuthSettings settings;
};

#endif // SOS_PROVIDER_AUTH_HPP
