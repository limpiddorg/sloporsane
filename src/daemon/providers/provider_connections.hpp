#ifndef SOS_PROVIDER_CONNECTIONS_HPP
#define SOS_PROVIDER_CONNECTIONS_HPP

#include "../config.hpp"
#include "provider_selection.hpp"
#include "../permissions/permission_manager.hpp"
#include "../permissions/permission_verifier.hpp"
#include <agent-runtime/providers/provider.hpp>
#include <agent-runtime/providers/provider_auth.hpp>
#include <agent-runtime/providers/model_catalog.hpp>
#include <agent-runtime/providers/api_key_validator.hpp>

struct CustomProviderDefinition {
    std::string name;
    std::string baseUrl;
};

struct ProviderDescriptor {
    std::string id;
    bool connected;
    std::string model;
    std::optional<AuthMethod> method;
    std::string name;
    bool custom = false;
    std::string baseUrl;
};
void to_json(nlohmann::json& json, const ProviderDescriptor& provider);

struct ModelDescriptor {
    std::string provider;
    std::string model;
    AuthMethod method;
    std::optional<std::uint64_t> maxContext;
    ImageSupport imageInput = ImageSupport::UNKNOWN;
    std::string providerName;
};
void to_json(nlohmann::json& json, const ModelDescriptor& model);

struct ModelCatalogError {
    std::string provider;
    std::string error;
};
void to_json(nlohmann::json& json, const ModelCatalogError& error);

struct ConnectedModels {
    std::vector<ModelDescriptor> models;
    std::vector<ModelCatalogError> errors;
};

class ProviderConnections {
public:
    using OAuthStarter = std::function<std::unique_ptr<ProviderAuthFlow>(const std::string&,
        std::shared_ptr<std::atomic<bool>>)>;
    explicit ProviderConnections(std::shared_ptr<AuthStore> auth = std::make_shared<AuthStore>(),
        std::filesystem::path config = PathResolution::configFile(), OAuthStarter oauthStarter = {},
        ModelCatalog catalog = ModelCatalog{}, ApiKeyValidator keyValidator = ApiKeyValidator{});
    ProviderSelection selection() const;
    std::string defaultReasoning(const ProviderSelection& selection) const;
    void select(const ProviderSelection& selection);
    void setReasoning(const std::string& reasoning);
    PermissionMode permissionMode() const;
    int permissionConfidence() const;
    void setPermissionMode(PermissionMode mode, std::optional<int> confidence = {});
    bool hasPermissionKey() const;
    void savePermissionKey(const std::string& key);
    PermissionVerifierConfig permissionModel() const;
    nlohmann::json permissionModelInfo() const;
    void savePermissionSettings(PermissionMode mode, std::optional<int> threshold,
        std::optional<PermissionVerifierConfig> model = {}, const std::string& key = "");
    PermissionManager::Verifier permissionVerifier(const std::string& workspace, const std::string& context) const;
    std::unique_ptr<Provider> create(const std::string& provider) const;
    bool hasAuth(const std::string& provider) const;
    std::optional<AuthMethod> activeAuthMethod(const std::string& provider) const;
    void requireAuth(const std::string& provider) const;
    std::string firstCompatibleModel(const std::string& provider, core::CancellationCheck cancelled) const;
    ConnectedModels listModels(core::CancellationCheck cancelled) const;
    std::vector<ProviderDescriptor> list(const ProviderSelection& selection) const;
    std::string addCustom(const std::string& name, const std::string& baseUrl, const std::string& key);
    void deleteCustom(const std::string& id);
    static void validateCustomName(const std::string& name);
    void saveApiKey(const std::string& provider, const std::string& key);
    void validateApiKey(const std::string& provider, const std::string& key, core::CancellationCheck cancelled) const;
    void saveOAuth(const std::string& provider, const ProviderCredential& credential);
    std::unique_ptr<ProviderAuthFlow> startOAuth(const std::string& provider, const std::string& method,
        std::shared_ptr<std::atomic<bool>> cancelled) const;
private:
    CustomProviderDefinition customDefinition(const std::string& id) const;
    static CustomProviderDefinition parseCustomDefinition(const nlohmann::json& definition);
    std::string customKey(const std::string& id) const;
    ProviderAuthorization authorize(const std::string& provider, core::CancellationCheck cancelled) const;
    void saveCredential(const std::string& provider, const ProviderCredential& credential);
    std::shared_ptr<AuthStore> auth;
    std::filesystem::path config;
    OAuthStarter oauthStarter;
    ModelCatalog catalog;
    ApiKeyValidator keyValidator;
    mutable std::recursive_mutex permissionMutex;
};

#endif // SOS_PROVIDER_CONNECTIONS_HPP
