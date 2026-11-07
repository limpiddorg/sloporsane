#ifndef SOS_PROVIDER_HPP
#define SOS_PROVIDER_HPP

#include "../conversation/provider_format.hpp"
#include "../tools/tool.hpp"
#include "../context_usage.hpp"
#include "../core/cancellation.hpp"
#include "model_info.hpp"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

class Provider {
public:
    virtual ~Provider() = default;
    // Stable conversation identity, independent of individual generation requests.
    virtual void setSessionId(const std::string& id) {}
    // Configure before starting a request; the callback must be non-throwing and worker-safe.
    void setCancellationCheck(core::CancellationCheck check) { cancellationCheck = std::move(check); }
    virtual ProviderFormat getFormat() const = 0;
    // Blocking call, executed on the agent worker.
    virtual nlohmann::json generate(const nlohmann::json& input, const std::string& model,
        const std::vector<ToolDefinition>& tools = {}, const std::string& reasoningEffort = "medium") = 0;
    // Best-effort model metadata; unknown capabilities remain unset.
    virtual std::optional<std::uint64_t> getContextLimit(const std::string& model) { return std::nullopt; }
    virtual ModelInfo getModelInfo(const std::string& model) { return {model, getContextLimit(model), {}, {}}; }
    // Selectable efforts in ascending order, including none when supported. Empty means unknown.
    virtual std::vector<std::string> getReasoningEfforts(const std::string& model) { return {}; }
    virtual std::optional<std::uint64_t> getUsedContext(const nlohmann::json& response) const { return std::nullopt; }
    // Responses providers expose output usage separately from input/context usage.
    virtual std::optional<std::uint64_t> getOutputTokens(const nlohmann::json& response) const {
        const auto usage = response.find("usage");
        if (usage == response.end() || !usage->is_object()) return std::nullopt;
        const auto tokens = usage->find("output_tokens");
        if (tokens == usage->end() || !tokens->is_number_integer() ||
            (!tokens->is_number_unsigned() && tokens->get<std::int64_t>() < 0)) return std::nullopt;
        return tokens->get<std::uint64_t>();
    }

protected:
    core::CancellationCheck cancellationCheck;
};

#endif // SOS_PROVIDER_HPP
