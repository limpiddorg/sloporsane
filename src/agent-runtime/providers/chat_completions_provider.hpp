#ifndef SOS_CHAT_COMPLETIONS_PROVIDER_HPP
#define SOS_CHAT_COMPLETIONS_PROVIDER_HPP

#include "provider.hpp"
#include "provider_http.hpp"

// Adapts the runtime's portable Responses items to the classic Chat wire protocol.
// No Responses request or opaque Responses reasoning is sent to the custom server.
class ChatCompletionsProvider : public Provider {
public:
    ChatCompletionsProvider(std::string baseUrl, std::string key, std::string name);
    ProviderFormat getFormat() const override { return ProviderFormat::OPENAI_RESPONSES; }
    nlohmann::json generate(const nlohmann::json& input, const std::string& model,
        const std::vector<ToolDefinition>& tools = {}, const std::string& reasoningEffort = "medium") override;
    std::optional<std::uint64_t> getUsedContext(const nlohmann::json& response) const override;
    static nlohmann::json prepareRequest(const nlohmann::json& input, const std::string& model,
        const std::vector<ToolDefinition>& tools);
    static nlohmann::json normalizeResponse(const nlohmann::json& response);
    static std::vector<ModelInfo> discoverModels(const std::string& baseUrl, const std::string& key,
        core::CancellationCheck cancelled = {});
    static std::string normalizeBaseUrl(std::string url);
private:
    std::string baseUrl;
    std::string key;
    std::string name;
};

#endif // SOS_CHAT_COMPLETIONS_PROVIDER_HPP
