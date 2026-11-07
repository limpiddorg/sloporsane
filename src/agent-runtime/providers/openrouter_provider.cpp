#include "openrouter_provider.hpp"
#include "responses_request.hpp"
#include "generation_retry.hpp"

#include <stdexcept>
#include <algorithm>
#include <limits>

namespace {
std::optional<std::uint64_t> tokenCount(const nlohmann::json& value) {
    if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<std::int64_t>() < 0))
        return std::nullopt;
    return value.get<std::uint64_t>();
}
}

OpenRouterProvider::OpenRouterProvider(std::string apiKey, std::string endpoint, std::string modelsEndpoint,
    ProviderRequestTimeouts timeouts) : apiKey(apiKey), endpoint(std::move(endpoint)), timeouts(timeouts) {
    timeouts.validate();
    if (apiKey.empty() || apiKey.find_first_of("\r\n") != std::string::npos)
        throw std::invalid_argument("OpenRouter requires a nonempty API key without line breaks");
    modelsClient.SetUrl(cpr::Url{modelsEndpoint});
    modelsClient.SetBearer(cpr::Bearer{apiKey});
    modelsClient.SetConnectTimeout(cpr::ConnectTimeout{3000});
    modelsClient.SetTimeout(cpr::Timeout{5000});
    modelsClient.SetRedirect(cpr::Redirect{false});
    const auto progress = [this](cpr::cpr_off_t, cpr::cpr_off_t, cpr::cpr_off_t, cpr::cpr_off_t, intptr_t) {
        return !cancellationCheck || !cancellationCheck();
    };
    modelsClient.SetProgressCallback(cpr::ProgressCallback{progress});
}

OpenRouterProvider::OpenRouterProvider(std::shared_ptr<ProviderAuth> auth)
    : OpenRouterProvider(auth->authorize().bearer) { this->auth = std::move(auth); }

ProviderFormat OpenRouterProvider::getFormat() const { return ProviderFormat::OPENAI_RESPONSES; }

nlohmann::json OpenRouterProvider::generate(const nlohmann::json& input, const std::string& model,
    const std::vector<ToolDefinition>& tools, const std::string& reasoningEffort) {
    if (!input.is_array() || model.empty())
        throw std::invalid_argument("Expected a model and an array of Responses input items");
    if (reasoningEffort.empty()) throw std::invalid_argument("Expected a reasoning effort");
    core::checkCancellation(cancellationCheck);
    const auto bearer = auth ? auth->authorize(cancellationCheck).bearer : apiKey;
    const auto response = postProviderResponse(endpoint, bearer, {{"Content-Type", "application/json"}},
        ResponsesRequest{model, input, tools, reasoningEffort}, cancellationCheck, "OpenRouter", timeouts);
    auto json = nlohmann::json::parse(response, nullptr, false);
    checkGenerationResponse(json, "OpenRouter");
    return json;
}

void OpenRouterProvider::loadCatalog() {
    core::checkCancellation(cancellationCheck);
    if (!catalogLoaded) {
        if (auth) modelsClient.SetBearer(cpr::Bearer{auth->authorize(cancellationCheck).bearer});
        const auto response = modelsClient.Get();
        core::checkCancellation(cancellationCheck);
        if (retryableTransportError(response.error.code))
            throw RetryableGenerationError("Model catalog connection problem");
        if (response.error.code != cpr::ErrorCode::OK)
            throw std::runtime_error("Model catalog transport error");
        if (retryableHttpStatus(response.status_code))
            throw RetryableGenerationError("Model catalog HTTP error " + std::to_string(response.status_code), true);
        if (response.status_code < 200 || response.status_code >= 300)
            throw std::runtime_error("Model catalog HTTP error " + std::to_string(response.status_code));
        const auto catalog = nlohmann::json::parse(response.text, nullptr, false);
        if (!catalog.is_object() || !catalog.contains("data") || !catalog["data"].is_array())
            throw std::runtime_error("Invalid model catalog response");
        for (const auto& entry : catalog["data"]) {
            if (!entry.is_object() || !entry.contains("id") || !entry["id"].is_string()) continue;
            auto limit = tokenCount(entry.value("context_length", nlohmann::json(nullptr)));
            if (limit == 0) limit.reset();
            contextLimits[entry["id"].get<std::string>()] = limit;
            const auto architecture = entry.find("architecture");
            imageInputs[entry["id"].get<std::string>()] = architecture != entry.end() && architecture->is_object()
                ? declaredImageSupport(architecture->value("input_modalities", nlohmann::json(nullptr))) : ImageSupport::UNKNOWN;
            const auto top = entry.find("top_provider");
            if (top != entry.end() && top->is_object()) {
                auto output = tokenCount(top->value("max_completion_tokens", nlohmann::json(nullptr)));
                if (output == 0) output.reset();
                outputLimits[entry["id"].get<std::string>()] = output;
            }
            const auto reasoning = entry.find("reasoning");
            if (reasoning == entry.end() || !reasoning->is_object()) continue;
            const auto efforts = reasoning->find("supported_efforts");
            if (efforts == reasoning->end() || (!efforts->is_null() && !efforts->is_array())) continue;
            auto& supported = reasoningEfforts[entry["id"].get<std::string>()];
            // A null list does not establish that reasoning can be disabled.
            if (efforts->is_array() && std::find(efforts->begin(), efforts->end(), "none") != efforts->end())
                supported.emplace_back("none");
            for (const auto* effort : {"low", "medium", "high", "xhigh", "max"})
                if (efforts->is_null() || std::find(efforts->begin(), efforts->end(), effort) != efforts->end())
                    supported.emplace_back(effort);
        }
        catalogLoaded = true;
    }
}

std::optional<std::uint64_t> OpenRouterProvider::getContextLimit(const std::string& model) {
    loadCatalog();
    const auto it = contextLimits.find(model);
    return it == contextLimits.end() ? std::nullopt : it->second;
}

std::vector<std::string> OpenRouterProvider::getReasoningEfforts(const std::string& model) {
    loadCatalog();
    const auto it = reasoningEfforts.find(model);
    return it == reasoningEfforts.end() ? std::vector<std::string>{} : it->second;
}

ModelInfo OpenRouterProvider::getModelInfo(const std::string& model) {
    const auto context = getContextLimit(model);
    const auto output = outputLimits.find(model);
    const auto images = imageInputs.find(model);
    return {model, context, {}, output == outputLimits.end() ? std::nullopt : output->second, {},
        images == imageInputs.end() ? ImageSupport::UNKNOWN : images->second};
}

std::optional<std::uint64_t> OpenRouterProvider::getUsedContext(const nlohmann::json& response) const {
    const auto usage = response.find("usage");
    if (usage == response.end() || !usage->is_object()) return std::nullopt;
    const auto input = tokenCount(usage->value("input_tokens", nlohmann::json(nullptr)));
    const auto output = tokenCount(usage->value("output_tokens", nlohmann::json(nullptr)));
    if (!input || !output || *output > std::numeric_limits<std::uint64_t>::max() - *input) return std::nullopt;
    return *input + *output;
}
