#ifndef SOS_OPEN_ROUTER_PROVIDER_HPP
#define SOS_OPEN_ROUTER_PROVIDER_HPP

#include "provider.hpp"
#include "provider_auth.hpp"
#include "provider_http.hpp"
#include <cpr/session.h>
#include <map>

class OpenRouterProvider : public Provider {
public:
    explicit OpenRouterProvider(std::shared_ptr<ProviderAuth> auth);
    explicit OpenRouterProvider(std::string apiKey,
        std::string endpoint = "https://openrouter.ai/api/v1/responses",
        std::string modelsEndpoint = "https://openrouter.ai/api/v1/models", ProviderRequestTimeouts timeouts = {});

    ProviderFormat getFormat() const override;
    nlohmann::json generate(const nlohmann::json& input, const std::string& model,
        const std::vector<ToolDefinition>& tools = {}, const std::string& reasoningEffort = "medium") override;
    std::optional<std::uint64_t> getContextLimit(const std::string& model) override;
    ModelInfo getModelInfo(const std::string& model) override;
    std::vector<std::string> getReasoningEfforts(const std::string& model) override;
    std::optional<std::uint64_t> getUsedContext(const nlohmann::json& response) const override;

private:
    std::shared_ptr<ProviderAuth> auth;
    void loadCatalog();
    std::string apiKey;
    std::string endpoint;
    ProviderRequestTimeouts timeouts;
    cpr::Session modelsClient;
    bool catalogLoaded = false;
    std::map<std::string, std::optional<std::uint64_t>> contextLimits;
    std::map<std::string, std::optional<std::uint64_t>> outputLimits;
    std::map<std::string, ImageSupport> imageInputs;
    std::map<std::string, std::vector<std::string>> reasoningEfforts;
};

#endif // SOS_OPEN_ROUTER_PROVIDER_HPP
