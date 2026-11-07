#include "chat_completions_provider.hpp"
#include "generation_retry.hpp"
#include <atomic>
#include <set>
#include <limits>

namespace {
using Json = nlohmann::json;
Json chatContent(const Json& content) {
    if (content.is_string()) return content;
    if (!content.is_array()) throw std::invalid_argument("Invalid conversation content");
    auto parts = Json::array();
    for (const auto& part : content) {
        const auto type = part.value("type", "");
        if (type == "input_text" || type == "output_text")
            parts.push_back({{"type", "text"}, {"text", part.at("text")}});
        else if (type == "input_image")
            parts.push_back({{"type", "image_url"}, {"image_url", {{"url", part.at("image_url")}}}});
        else throw std::invalid_argument("Custom Chat Completions does not support this attachment type");
    }
    return parts;
}
}

std::string ChatCompletionsProvider::normalizeBaseUrl(std::string url) {
    while (!url.empty() && url.back() == '/') url.pop_back();
    const auto start = url.starts_with("https://") ? 8U : url.starts_with("http://") ? 7U : 0U;
    if (!start || url.size() <= start || url.size() > 2048 ||
        url.find_first_of(" \t\r\n?#@") != std::string::npos || url[start] == '/' ||
        url.ends_with("/chat/completions") || url.ends_with("/models") || url.ends_with("/responses"))
        throw std::invalid_argument("Enter an HTTP(S) base URL, not a models or generation endpoint");
    return url;
}

ChatCompletionsProvider::ChatCompletionsProvider(std::string baseUrl, std::string key, std::string name)
    : baseUrl(normalizeBaseUrl(std::move(baseUrl))), key(std::move(key)), name(std::move(name)) {
    if (!this->key.empty()) validateApiKeyFormat(this->key);
}

Json ChatCompletionsProvider::prepareRequest(const Json& input, const std::string& model,
    const std::vector<ToolDefinition>& tools) {
    if (!input.is_array() || model.empty()) throw std::invalid_argument("Expected a model and conversation items");
    auto messages = Json::array();
    auto pendingImages = Json::array();
    const auto flushImages = [&] {
        if (!pendingImages.empty()) {
            messages.push_back({{"role", "user"}, {"content", pendingImages}});
            pendingImages.clear();
        }
    };
    for (const auto& item : input) {
        const auto type = item.value("type", "message");
        if (type != "function_call_output") flushImages();
        if (type == "reasoning") continue; // Opaque reasoning from Responses is not portable.
        if (type == "function_call") {
            if (messages.empty() || messages.back().value("role", "") != "assistant")
                messages.push_back({{"role", "assistant"}, {"content", nullptr}});
            auto& message = messages.back();
            if (!message.contains("tool_calls")) message["tool_calls"] = Json::array();
            message["tool_calls"].push_back({{"id", item.at("call_id")}, {"type", "function"},
                {"function", {{"name", item.at("name")}, {"arguments", item.at("arguments")}}}});
        } else if (type == "function_call_output") {
            const auto content = chatContent(item.at("output"));
            // Chat tool results are text; images are carried in a following user message.
            std::string text;
            if (content.is_string()) text = content.get<std::string>();
            else for (const auto& part : content) {
                if (part.at("type") == "text") text += part.at("text").get<std::string>();
                else pendingImages.push_back(part);
            }
            messages.push_back({{"role", "tool"}, {"tool_call_id", item.at("call_id")}, {"content", text}});
        } else if (type == "message") {
            auto role = item.at("role").get<std::string>();
            if (role == "developer") role = "system";
            messages.push_back({{"role", role}, {"content", chatContent(item.at("content"))}});
        } else throw std::invalid_argument("Unsupported conversation item for Chat Completions");
    }
    flushImages();
    Json request{{"model", model}, {"messages", messages}, {"stream", false}};
    if (!tools.empty()) {
        request["tools"] = Json::array();
        for (const auto& tool : tools)
            request["tools"].push_back({{"type", "function"}, {"function", {{"name", tool.id},
                {"description", tool.description}, {"parameters", tool.parameters}}}});
    }
    return request;
}

Json ChatCompletionsProvider::normalizeResponse(const Json& response) try {
    if (response.is_object() && response.contains("error") && !response.at("error").is_null()) {
        const auto detail = "Custom provider " + describeApiError(response.at("error"));
        if (retryableApiError(response.at("error"))) throw RetryableGenerationError(detail, true);
        throw std::runtime_error(detail);
    }
    if (!response.is_object() || !response.contains("choices") || !response["choices"].is_array() || response["choices"].empty())
        throw RetryableGenerationError("Custom provider returned an invalid Chat Completions response");
    const auto& choice = response["choices"][0];
    const auto finish = choice.value("finish_reason", "");
    if (finish != "stop" && finish != "tool_calls") {
        if (retryableApiError({{"reason", finish}}))
            throw RetryableGenerationError("Custom provider returned an incomplete completion");
        throw std::runtime_error("Custom provider returned an incomplete completion");
    }
    const auto& message = choice.at("message");
    static std::atomic<std::uint64_t> sequence{0};
    const auto id = response.value("id", "chat-" + std::to_string(++sequence));
    auto output = Json::array();
    if (message.contains("content") && !message["content"].is_null()) {
        const auto text = message["content"].get<std::string>();
        if (!text.empty()) output.push_back({{"type", "message"}, {"id", id + "-text"}, {"role", "assistant"},
            {"content", Json::array({{{"type", "output_text"}, {"text", text}, {"annotations", Json::array()}}})}});
    }
    if (message.contains("tool_calls") && !message["tool_calls"].is_null()) {
        for (const auto& call : message.at("tool_calls")) {
            if (call.at("type") != "function" || call.at("id").get<std::string>().empty())
                throw RetryableGenerationError("Invalid custom provider tool call");
            const auto& function = call.at("function");
            output.push_back({{"type", "function_call"}, {"call_id", call.at("id")},
                {"name", function.at("name")}, {"arguments", function.at("arguments")}});
        }
    }
    if (output.empty()) throw RetryableGenerationError("Custom provider returned no text or tool calls");
    Json result{{"id", id}, {"status", "completed"}, {"output", output}};
    if (response.contains("usage") && response["usage"].is_object()) {
        const auto& usage = response["usage"];
        result["usage"] = {{"input_tokens", usage.value("prompt_tokens", Json(nullptr))},
            {"output_tokens", usage.value("completion_tokens", Json(nullptr))}};
    }
    return result;
} catch (const Json::exception&) {
    throw RetryableGenerationError("Custom provider returned a malformed Chat Completions response");
}

Json ChatCompletionsProvider::generate(const Json& input, const std::string& model,
    const std::vector<ToolDefinition>& tools, const std::string&) {
    const auto text = postProviderResponse(baseUrl + "/chat/completions", key, {{"Content-Type", "application/json"}},
        prepareRequest(input, model, tools), cancellationCheck, name);
    return normalizeResponse(Json::parse(text, nullptr, false));
}

std::optional<std::uint64_t> ChatCompletionsProvider::getUsedContext(const Json& response) const {
    const auto usage = response.find("usage");
    if (usage == response.end() || !usage->is_object()) return {};
    const auto input = usage->find("input_tokens"), output = usage->find("output_tokens");
    if (input == usage->end() || output == usage->end() || !input->is_number_integer() || !output->is_number_integer() ||
        *input < 0 || *output < 0) return {};
    const auto a = input->get<std::uint64_t>(), b = output->get<std::uint64_t>();
    if (b > std::numeric_limits<std::uint64_t>::max() - a) return {};
    return a + b;
}

std::vector<ModelInfo> ChatCompletionsProvider::discoverModels(const std::string& baseUrl, const std::string& key,
    core::CancellationCheck cancelled) {
    if (!key.empty()) validateApiKeyFormat(key);
    const auto catalog = getProviderJson(normalizeBaseUrl(baseUrl) + "/models", {.bearer = key}, cancelled, "Custom model discovery");
    const auto entries = catalog.find("data");
    if (entries == catalog.end() || !entries->is_array()) throw std::runtime_error("Expected a models catalog with a data array");
    std::vector<ModelInfo> models;
    std::set<std::string> seen;
    for (const auto& entry : *entries) {
        if (!entry.is_object() || !entry.contains("id") || !entry["id"].is_string()) continue;
        const auto id = entry["id"].get<std::string>();
        if (id.empty() || id.size() > 256 || id.find_first_of(" \t\r\n") != std::string::npos) continue;
        if (seen.insert(id).second) models.push_back({id, {}, {}, {}});
    }
    if (models.empty()) throw std::runtime_error("Custom provider exposes no models");
    return models;
}
