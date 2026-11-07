#ifndef SOS_OPENAI_PROVIDER_HPP
#define SOS_OPENAI_PROVIDER_HPP

#include "provider.hpp"
#include "provider_auth.hpp"
#include "provider_http.hpp"
#include "model_catalog.hpp"
#include "responses_websocket.hpp"
#include <unordered_map>

class OpenAIProvider : public Provider {
public:
    explicit OpenAIProvider(std::shared_ptr<ProviderAuth> auth,
        std::string apiEndpoint = "https://api.openai.com/v1/responses",
        std::string codexEndpoint = "https://chatgpt.com/backend-api/codex/responses",
        ModelCatalogEndpoints catalogEndpoints = {}, ProviderRequestTimeouts timeouts = {});
    ProviderFormat getFormat() const override;
    void setSessionId(const std::string& id) override;
    void setWebSocketEnabled(bool enabled);
    const ResponsesWebSocket::Statistics& webSocketStatistics() const { return websocket.statistics(); }
    nlohmann::json generate(const nlohmann::json& input, const std::string& model,
        const std::vector<ToolDefinition>& tools = {}, const std::string& reasoningEffort = "medium") override;
    std::vector<std::string> getReasoningEfforts(const std::string& model) override;
    // A network-free default for global preference initialization.
    static std::string defaultReasoningEffort(const std::string& model);
    std::optional<std::uint64_t> getContextLimit(const std::string& model) override;
    ModelInfo getModelInfo(const std::string& model) override;
    std::optional<std::uint64_t> getUsedContext(const nlohmann::json& response) const override;
private:
    std::string resolveModel(const std::string& model);
    nlohmann::json prepareRequest(const nlohmann::json& input, const std::string& model,
        const std::vector<ToolDefinition>& tools, const std::string& reasoningEffort, bool codex);
    std::string postRequest(const nlohmann::json& request, const ProviderAuthorization& authorization) const;
    cpr::Header requestHeaders(const ProviderAuthorization& authorization) const;
    static nlohmann::json readResponse(const std::string& output, bool codex);
    std::shared_ptr<ProviderAuth> auth;
    std::string sessionId;
    ResponsesWebSocket websocket;
    bool webSocketEnabled = true;
    bool webSocketUnavailable = false;
    std::string apiEndpoint;
    std::string codexEndpoint;
    ModelCatalogEndpoints catalogEndpoints;
    ProviderRequestTimeouts timeouts;
    bool catalogLoaded = false;
    std::string catalogAccount;
    std::map<std::string, std::string> fastModels;
    std::unordered_map<std::string, std::optional<std::uint64_t>> contextLimits;
    std::unordered_map<std::string, std::optional<std::uint64_t>> maxContextLimits;
    std::map<std::string, std::vector<std::string>> reasoningEfforts;
    std::map<std::string, ImageSupport> imageInputs;
};

// Codex is streamed over HTTP, but the runtime consumes a completed Responses object.
nlohmann::json completedResponsesEvent(const std::string& stream);

#endif // SOS_OPENAI_PROVIDER_HPP
