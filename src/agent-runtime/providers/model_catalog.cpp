#include "model_catalog.hpp"
#include "provider_http.hpp"
#include <algorithm>
#include <regex>
#include <set>

namespace {
bool contains(const nlohmann::json& object, const char* field, const char* value) {
    const auto entry = object.find(field);
    return entry != object.end() && entry->is_array() && std::find(entry->begin(), entry->end(), value) != entry->end();
}

std::uint64_t creationTime(const nlohmann::json& model) {
    const auto created = model.find("created");
    if (created == model.end() || !created->is_number_integer()) return 0;
    if (!created->is_number_unsigned() && created->get<std::int64_t>() < 0) return 0;
    return created->get<std::uint64_t>();
}

std::optional<std::uint64_t> codexPriority(const nlohmann::json& model) {
    const auto priority = model.find("priority");
    if (priority == model.end() || !priority->is_number_integer()) return {};
    if (!priority->is_number_unsigned() && priority->get<std::int64_t>() < 0) return {};
    return priority->get<std::uint64_t>();
}
}

ModelCatalog::ModelCatalog(ModelCatalogEndpoints endpoints) : endpoints(std::move(endpoints)) {}

std::string ModelCatalog::firstCompatibleModel(const std::string& provider, const ProviderAuthorization& auth,
    core::CancellationCheck cancelled) const {
    const auto models = listCompatibleModels(provider, auth, std::move(cancelled));
    if (models.empty()) throw std::runtime_error("No compatible conversation model with tools found");
    return models.front();
}

std::vector<std::string> ModelCatalog::listCompatibleModels(const std::string& provider, const ProviderAuthorization& auth,
    core::CancellationCheck cancelled) const {
    std::vector<std::string> result;
    for (const auto& model : listModelInfo(provider, auth, std::move(cancelled))) result.push_back(model.id);
    return result;
}

std::vector<ModelInfo> ModelCatalog::listModelInfo(const std::string& provider, const ProviderAuthorization& auth,
    core::CancellationCheck cancelled) const {
    if (provider != "openai" && provider != "openrouter")
        throw std::invalid_argument("Use custom model discovery for custom providers");
    validateProviderId(provider);
    const auto format = provider == "openrouter" ? ModelCatalogFormat::OPENROUTER :
        auth.codex ? ModelCatalogFormat::CODEX : ModelCatalogFormat::OPENAI;
    auto url = format == ModelCatalogFormat::OPENROUTER ? endpoints.openrouter :
        format == ModelCatalogFormat::CODEX ? endpoints.codex : endpoints.openai;
    if (format == ModelCatalogFormat::OPENROUTER)
        url += (url.find('?') == std::string::npos ? "?" : "&") + std::string("sort=newest");
    const auto catalog = format == ModelCatalogFormat::CODEX ? codexCatalog(endpoints, auth, cancelled) :
        load(url, auth, std::move(cancelled));
    const auto ids = compatibleModels(catalog, format);
    const auto fastModels = format == ModelCatalogFormat::CODEX ? codexFastModels(catalog) :
        std::map<std::string, std::string>{};
    std::vector<ModelInfo> result;
    for (const auto& id : ids) {
        const auto& entries = catalog.at(format == ModelCatalogFormat::CODEX ? "models" : "data");
        const auto entry = std::find_if(entries.begin(), entries.end(), [&](const auto& item) {
            const auto key = format == ModelCatalogFormat::CODEX ? "slug" : "id";
            return item.is_object() && item.contains(key) && item.at(key) == id;
        });
        const auto limit = [&](const char* field) -> std::optional<std::uint64_t> {
            const auto value = entry->find(field);
            if (value == entry->end() || !value->is_number_integer()) return {};
            if (!value->is_number_unsigned() && value->get<std::int64_t>() <= 0) return {};
            const auto tokens = value->get<std::uint64_t>();
            return tokens ? std::optional{tokens} : std::nullopt;
        };
        ModelInfo info{id, {}, {}, {}, {}};
        if (format == ModelCatalogFormat::CODEX)
            info.imageInput = declaredImageSupport(entry->value("input_modalities", nlohmann::json(nullptr)));
        else if (format == ModelCatalogFormat::OPENROUTER)
            info.imageInput = declaredImageSupport(entry->at("architecture").value("input_modalities", nlohmann::json(nullptr)));
        else info.imageInput = openAIImageSupport(id);
        if (format == ModelCatalogFormat::CODEX) {
            info.contextLimit = limit("context_window");
            info.maxContextLimit = limit("max_context_window");
            if (!info.contextLimit) info.contextLimit = info.maxContextLimit;
        } else if (format == ModelCatalogFormat::OPENROUTER) info.contextLimit = limit("context_length");
        result.push_back(info);
        if (fastModels.contains(id + "-fast")) {
            info.id += "-fast";
            result.push_back(std::move(info));
        }
    }
    return result;
}

nlohmann::json ModelCatalog::codexCatalog(const ModelCatalogEndpoints& endpoints, const ProviderAuthorization& auth,
    core::CancellationCheck cancelled) {
    auto catalog = getProviderJson(endpoints.codex, auth, cancelled, "Model catalog");
    auto models = catalog.find("models");
    if (models == catalog.end() || !models->is_array()) throw std::runtime_error("Invalid model catalog");
    if (endpoints.codexPublic.empty()) return catalog;
    try {
        // This request is anonymous: never send an OAuth token to the public source.
        const auto publicCatalog = getProviderJson(endpoints.codexPublic, {}, cancelled, "Public model catalog");
        const auto entries = publicCatalog.find("models");
        if (entries == publicCatalog.end() || !entries->is_array()) return catalog;
        std::set<std::string> known;
        for (const auto& entry : *models)
            if (entry.is_object() && entry.contains("slug") && entry["slug"].is_string())
                known.insert(entry["slug"].get<std::string>());
        for (const auto& entry : *entries) {
            if (!entry.is_object() || !entry.contains("slug") || !entry["slug"].is_string() ||
                entry.value("visibility", "") != "list" || entry.value("supported_in_api", false) != true) continue;
            if (known.insert(entry["slug"].get<std::string>()).second) models->push_back(entry);
        }
    } catch (const core::Cancelled&) {
        throw;
    } catch (const std::exception&) {
        // Authenticated discovery remains usable if the supplementary source fails.
    }
    return catalog;
}

nlohmann::json ModelCatalog::load(const std::string& url, const ProviderAuthorization& auth,
    core::CancellationCheck cancelled) const {
    return getProviderJson(url, auth, std::move(cancelled), "Model catalog");
}

std::map<std::string, std::string> ModelCatalog::codexFastModels(const nlohmann::json& catalog) {
    const auto ids = compatibleModels(catalog, ModelCatalogFormat::CODEX);
    const std::set<std::string> compatible(ids.begin(), ids.end());
    std::set<std::string> occupied;
    for (const auto& entry : catalog.at("models"))
        if (entry.is_object() && entry.contains("slug") && entry["slug"].is_string())
            occupied.insert(entry["slug"].get<std::string>());
    std::map<std::string, std::string> result;
    for (const auto& entry : catalog.at("models")) {
        if (!entry.is_object() || !entry.contains("slug") || !entry["slug"].is_string()) continue;
        const auto id = entry["slug"].get<std::string>();
        if (!compatible.contains(id) || id.size() > 251 || occupied.contains(id + "-fast")) continue;
        bool fast = contains(entry, "additional_speed_tiers", "fast");
        const auto tiers = entry.find("service_tiers");
        if (tiers != entry.end() && tiers->is_array())
            for (const auto& tier : *tiers)
                if (tier.is_object() && tier.contains("id") && tier["id"] == "priority") fast = true;
        if (fast) result.emplace(id + "-fast", id);
    }
    return result;
}

bool ModelCatalog::isCompatible(const nlohmann::json& model, ModelCatalogFormat format) {
    if (format == ModelCatalogFormat::OPENROUTER) {
        const auto architecture = model.find("architecture");
        return architecture != model.end() && architecture->is_object() &&
            contains(*architecture, "input_modalities", "text") && contains(*architecture, "output_modalities", "text") &&
            contains(model, "supported_parameters", "tools");
    }
    if (format == ModelCatalogFormat::CODEX)
        return model.value("visibility", "") == "list" &&
            (!model.contains("input_modalities") || contains(model, "input_modalities", "text"));
    // OpenAI /models exposes IDs, not tool/Responses capabilities. Accept known
    // compatible families and dated snapshots; exclude audio, search and chat-only aliases.
    static const std::regex RESPONSES_MODELS(
        R"(^(gpt-6-astra|gpt-5(\.[0-9]+)?(-(mini|nano|codex|max)){0,2}|gpt-4\.1(-(mini|nano))?|gpt-4o(-mini)?|o[34](-mini)?)(-[0-9]{4}-[0-9]{2}-[0-9]{2})?$)");
    return std::regex_match(model.value("id", ""), RESPONSES_MODELS);
}

std::vector<std::string> ModelCatalog::compatibleModels(const nlohmann::json& catalog, ModelCatalogFormat format) {
    const auto entries = catalog.find(format == ModelCatalogFormat::CODEX ? "models" : "data");
    if (entries == catalog.end() || !entries->is_array()) throw std::runtime_error("Invalid model catalog");
    std::vector<std::string> compatible;
    std::vector<const nlohmann::json*> models;
    std::set<std::string> seen;
    for (const auto& model : *entries) {
        if (!model.is_object()) continue;
        const auto id = model.find(format == ModelCatalogFormat::CODEX ? "slug" : "id");
        if (id == model.end() || !id->is_string()) continue;
        const auto value = id->get<std::string>();
        if (value.empty() || value.size() > 256 || value.find_first_of(" \t\r\n") != std::string::npos) continue;
        if (isCompatible(model, format) && seen.insert(value).second) models.push_back(&model);
    }
    // Missing/invalid dates follow dated models; equal dates retain provider order.
    if (format == ModelCatalogFormat::OPENAI)
        std::stable_sort(models.begin(), models.end(), [](const auto* left, const auto* right) {
            return creationTime(*left) > creationTime(*right);
        });
    if (format == ModelCatalogFormat::CODEX)
        std::stable_sort(models.begin(), models.end(), [](const auto* left, const auto* right) {
            const auto a = codexPriority(*left), b = codexPriority(*right);
            if (!a || !b) return a.has_value() && !b.has_value();
            return *a < *b;
        });
    for (const auto* model : models)
        compatible.push_back(model->at(format == ModelCatalogFormat::CODEX ? "slug" : "id").get<std::string>());
    return compatible;
}

std::string ModelCatalog::selectFirstCompatible(const nlohmann::json& catalog, ModelCatalogFormat format) {
    const auto compatible = compatibleModels(catalog, format);
    if (compatible.empty()) throw std::runtime_error("No compatible conversation model with tools found");
    return compatible.front();
}
