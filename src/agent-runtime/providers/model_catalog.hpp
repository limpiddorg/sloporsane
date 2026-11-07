#ifndef SOS_MODEL_CATALOG_HPP
#define SOS_MODEL_CATALOG_HPP

#include "provider_auth.hpp"
#include "model_info.hpp"
#include <vector>
#include <map>

enum class ModelCatalogFormat { OPENAI, OPENROUTER, CODEX };

struct ModelCatalogEndpoints {
    std::string openai = "https://api.openai.com/v1/models";
    std::string openrouter = "https://openrouter.ai/api/v1/models";
    // Backend discovery is version-gated; match the supported Codex catalog schema.
    std::string codex = "https://chatgpt.com/backend-api/codex/models?client_version=0.155.0";
    // Public Codex model definitions. Empty disables supplementary discovery.
    std::string codexPublic = "https://raw.githubusercontent.com/openai/codex/main/codex-rs/models-manager/models.json";
};

class ModelCatalog {
public:
    explicit ModelCatalog(ModelCatalogEndpoints endpoints = {});
    std::vector<std::string> listCompatibleModels(const std::string& provider, const ProviderAuthorization& auth,
        core::CancellationCheck cancelled = {}) const;
    std::vector<ModelInfo> listModelInfo(const std::string& provider, const ProviderAuthorization& auth,
        core::CancellationCheck cancelled = {}) const;
    std::string firstCompatibleModel(const std::string& provider, const ProviderAuthorization& auth,
        core::CancellationCheck cancelled = {}) const;
    static std::vector<std::string> compatibleModels(const nlohmann::json& catalog, ModelCatalogFormat format);
    // Synthetic selection ID -> real API model ID. Never shadows a catalog slug.
    static std::map<std::string, std::string> codexFastModels(const nlohmann::json& catalog);
    static nlohmann::json codexCatalog(const ModelCatalogEndpoints& endpoints, const ProviderAuthorization& auth,
        core::CancellationCheck cancelled = {});
    static std::string selectFirstCompatible(const nlohmann::json& catalog, ModelCatalogFormat format);
private:
    nlohmann::json load(const std::string& url, const ProviderAuthorization& auth, core::CancellationCheck cancelled) const;
    static bool isCompatible(const nlohmann::json& model, ModelCatalogFormat format);
    ModelCatalogEndpoints endpoints;
};

#endif // SOS_MODEL_CATALOG_HPP
