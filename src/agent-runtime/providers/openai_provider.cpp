#include "openai_provider.hpp"
#include "responses_request.hpp"
#include "responses_stream_assembler.hpp"
#include "provider_http.hpp"
#include "generation_retry.hpp"
#include "../common/sse_decoder.hpp"
#include <limits>
#include <algorithm>
#include <cstdlib>

namespace {
std::vector<std::string> familyReasoningEfforts(const std::string& model, bool codex) {
    std::vector<std::string> result;
    if (model == "gpt-6-astra" || model == "gpt-6-astra-fast" || model.starts_with("gpt-5.2") || model.starts_with("gpt-5.3") ||
        model.starts_with("gpt-5.4") || model.starts_with("gpt-5.5") || model.starts_with("gpt-5.6"))
        result = {"low", "medium", "high", "xhigh"};
    else if (model.starts_with("gpt-5") || model.starts_with("o3") || model.starts_with("o4"))
        result = {"low", "medium", "high"};
    // Documented API families; Codex variants use their authenticated catalog instead.
    if (!codex && model.find("codex") == std::string::npos && model.find("pro") == std::string::npos &&
        (model.starts_with("gpt-5.1") || model.starts_with("gpt-5.2") || model.starts_with("gpt-5.4")))
        result.insert(result.begin(), "none");
    return result;
}
}

OpenAIProvider::OpenAIProvider(std::shared_ptr<ProviderAuth> auth, std::string apiEndpoint, std::string codexEndpoint,
    ModelCatalogEndpoints catalogEndpoints, ProviderRequestTimeouts timeouts)
    : auth(std::move(auth)), apiEndpoint(std::move(apiEndpoint)), codexEndpoint(std::move(codexEndpoint)),
      catalogEndpoints(std::move(catalogEndpoints)), timeouts(timeouts) {
    timeouts.validate();
}

void OpenAIProvider::setSessionId(const std::string& id) {
    if (id.find_first_of("\r\n") != std::string::npos)
        throw std::invalid_argument("Invalid provider session identity");
    if (sessionId != id) { websocket.reset(); webSocketUnavailable = false; }
    sessionId = id;
}

void OpenAIProvider::setWebSocketEnabled(bool enabled) {
    websocket.reset();
    webSocketEnabled = enabled;
    webSocketUnavailable = false;
}

std::optional<std::uint64_t> OpenAIProvider::getContextLimit(const std::string& model) {
    const auto authorization = auth->authorize(cancellationCheck);
    if (!authorization.codex) {
        imageInputs.clear();
        fastModels.clear();
        contextLimits.clear();
        maxContextLimits.clear();
        reasoningEfforts.clear();
        catalogLoaded = false;
        return std::nullopt;
    }
    if (!catalogLoaded || catalogAccount != authorization.accountId) {
        imageInputs.clear();
        contextLimits.clear();
        maxContextLimits.clear();
        fastModels.clear();
        reasoningEfforts.clear();
        catalogLoaded = false;
        const auto catalog = ModelCatalog::codexCatalog(catalogEndpoints, authorization, cancellationCheck);
        const auto models = catalog.find("models");
        if (models == catalog.end() || !models->is_array()) throw std::runtime_error("Invalid model catalog response");
        for (const auto& entry : *models) {
            if (!entry.is_object() || !entry.contains("slug") || !entry["slug"].is_string()) continue;
            const auto positiveLimit = [&entry](const char* field) -> std::optional<std::uint64_t> {
                const auto value = entry.find(field);
                if (value == entry.end() || !value->is_number_integer()) return std::nullopt;
                if (!value->is_number_unsigned() && value->get<std::int64_t>() <= 0) return std::nullopt;
                const auto limit = value->get<std::uint64_t>();
                return limit ? std::optional{limit} : std::nullopt;
            };
            auto limit = positiveLimit("context_window");
            // Match Codex resolution: the override maximum is only a fallback.
            if (!limit) limit = positiveLimit("max_context_window");
            contextLimits[entry["slug"].get<std::string>()] = limit;
            maxContextLimits[entry["slug"].get<std::string>()] = positiveLimit("max_context_window");
            imageInputs[entry["slug"].get<std::string>()] = declaredImageSupport(entry.value("input_modalities", nlohmann::json(nullptr)));
            if (const auto levels = entry.find("supported_reasoning_levels"); levels != entry.end() && levels->is_array()) {
                auto& supported = reasoningEfforts[entry["slug"].get<std::string>()];
                for (const auto* effort : {"none", "low", "medium", "high", "xhigh", "max", "ultra"})
                    if (std::any_of(levels->begin(), levels->end(), [effort](const auto& level) {
                        return level == effort || (level.is_object() && level.contains("effort") && level["effort"] == effort);
                    })) supported.emplace_back(effort);
            }
        }
        fastModels = ModelCatalog::codexFastModels(catalog);
        for (const auto& [alias, base] : fastModels) {
            contextLimits[alias] = contextLimits.at(base);
            maxContextLimits[alias] = maxContextLimits.at(base);
            imageInputs[alias] = imageInputs.at(base);
            if (reasoningEfforts.contains(base)) reasoningEfforts[alias] = reasoningEfforts.at(base);
        }
        catalogAccount = authorization.accountId;
        catalogLoaded = true;
    }
    const auto found = contextLimits.find(model);
    return found == contextLimits.end() ? std::nullopt : found->second;
}

ProviderFormat OpenAIProvider::getFormat() const { return ProviderFormat::OPENAI_RESPONSES; }

ModelInfo OpenAIProvider::getModelInfo(const std::string& model) {
    const auto context = getContextLimit(model);
    const auto maximum = maxContextLimits.find(model);
    const auto images = imageInputs.find(model);
    const auto support = auth->authorize(cancellationCheck).codex
        ? (images == imageInputs.end() ? ImageSupport::UNKNOWN : images->second) : openAIImageSupport(model);
    return {model, context, {}, {}, context && maximum != maxContextLimits.end() ? maximum->second : std::nullopt, support};
}

nlohmann::json completedResponsesEvent(const std::string& stream) {
    ResponsesStreamAssembler assembler;
    forEachSseData(stream, [&](std::string_view data) {
        if (data.empty() || data == "[DONE]") return;
        assembler.consume(nlohmann::json::parse(data, nullptr, false));
    });
    return assembler.finish();
}

nlohmann::json OpenAIProvider::generate(const nlohmann::json& input, const std::string& model,
    const std::vector<ToolDefinition>& tools, const std::string& reasoningEffort) {
    if (!input.is_array() || model.empty()) throw std::invalid_argument("Expected a model and Responses input items");
    const auto authorization = auth->authorize(cancellationCheck);
    const auto request = prepareRequest(input, model, tools, reasoningEffort, authorization.codex);
    // IXWebSocket does not implement the HTTP proxy path. Keep configured proxy
    // traffic on the existing curl transport rather than bypassing that proxy.
    const bool proxy = std::getenv("HTTPS_PROXY") || std::getenv("https_proxy") ||
        std::getenv("HTTP_PROXY") || std::getenv("http_proxy") || std::getenv("ALL_PROXY") || std::getenv("all_proxy");
    if (authorization.codex && !sessionId.empty() && webSocketEnabled && !webSocketUnavailable && !proxy) {
        auto headers = requestHeaders(authorization);
        headers["Authorization"] = "Bearer " + authorization.bearer;
        if (auto response = websocket.generate(codexEndpoint, headers, request, cancellationCheck, timeouts)) return *response;
        webSocketUnavailable = true;
    }
    if (!authorization.codex) websocket.reset();
    return readResponse(postRequest(request, authorization), authorization.codex);
}

nlohmann::json OpenAIProvider::prepareRequest(const nlohmann::json& input, const std::string& model,
    const std::vector<ToolDefinition>& tools, const std::string& reasoningEffort, bool codex) {
    const auto apiModel = resolveModel(model);
    nlohmann::json request = ResponsesRequest{apiModel, input, tools, reasoningEffort};
    if (apiModel != model) request["service_tier"] = "priority";
    // Generation must not introduce a second catalog request just to format reasoning.
    const auto known = reasoningEfforts.find(apiModel);
    if ((codex && known != reasoningEfforts.end() ? known->second : familyReasoningEfforts(apiModel, codex)).empty())
        request.erase("reasoning");
    if (codex) {
        // Codex requires instructions, but conversation rules stay in their original
        // input positions. Hoisting later announcements would rewrite the prefix.
        request["instructions"] = "Follow the conversation instructions.";
        // The Codex endpoint rejects typed system messages, but accepts the
        // equivalent EasyInputMessage without type. Normalize only the wire copy.
        for (auto& item : request["input"])
            if (item.value("role", "") == "system") item.erase("type");
        request["stream"] = true;
        if (!sessionId.empty()) request["prompt_cache_key"] = sessionId;
    }
    return request;
}

cpr::Header OpenAIProvider::requestHeaders(const ProviderAuthorization& authorization) const {
    cpr::Header headers{{"Content-Type", "application/json"}, {"User-Agent", "sos"}};
    if (authorization.codex) {
        // ChatGPT uses the session header for cache affinity, not just the body key.
        if (!sessionId.empty()) headers["session-id"] = sessionId;
        if (!authorization.accountId.empty()) headers["ChatGPT-Account-Id"] = authorization.accountId;
        headers["originator"] = "sos";
        headers["Accept"] = "text/event-stream";
    }
    return headers;
}

std::string OpenAIProvider::postRequest(const nlohmann::json& request, const ProviderAuthorization& authorization) const {
    return postProviderResponse(authorization.codex ? codexEndpoint : apiEndpoint,
        authorization.bearer, requestHeaders(authorization), request, cancellationCheck, "OpenAI", timeouts);
}

nlohmann::json OpenAIProvider::readResponse(const std::string& output, bool codex) {
    auto result = codex ? completedResponsesEvent(output) : nlohmann::json::parse(output, nullptr, false);
    checkGenerationResponse(result, "OpenAI");
    return result;
}

std::string OpenAIProvider::resolveModel(const std::string& model) {
    if (!model.ends_with("-fast")) return model;
    getContextLimit(model);
    if (const auto found = fastModels.find(model); found != fastModels.end()) return found->second;
    if (contextLimits.contains(model)) return model; // A real catalog slug, not a synthetic alias.
    throw std::runtime_error("Fast model is unavailable in the current Codex catalog");
}

std::string OpenAIProvider::defaultReasoningEffort(const std::string& model) {
    const auto efforts = familyReasoningEfforts(model, true);
    return efforts.empty() ? "" : efforts.back();
}

std::vector<std::string> OpenAIProvider::getReasoningEfforts(const std::string& model) {
    const bool codex = auth->authorize(cancellationCheck).codex;
    if (codex) {
        getContextLimit(model);
        if (const auto found = reasoningEfforts.find(model); found != reasoningEfforts.end()) return found->second;
    }
    return familyReasoningEfforts(model, codex);
}

std::optional<std::uint64_t> OpenAIProvider::getUsedContext(const nlohmann::json& response) const {
    const auto usage = response.find("usage");
    if (usage == response.end() || !usage->is_object()) return std::nullopt;
    const auto input = usage->find("input_tokens"), output = usage->find("output_tokens");
    if (input == usage->end() || output == usage->end() || !input->is_number_unsigned() || !output->is_number_unsigned())
        return std::nullopt;
    const auto a = input->get<std::uint64_t>(), b = output->get<std::uint64_t>();
    if (b > std::numeric_limits<std::uint64_t>::max() - a) return std::nullopt;
    return a + b;
}
