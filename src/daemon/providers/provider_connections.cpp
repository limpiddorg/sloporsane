#include "provider_connections.hpp"
#include "../permissions/permission_verifier.hpp"
#include <agent-runtime/providers/openai_provider.hpp>
#include <agent-runtime/providers/openrouter_provider.hpp>
#include <agent-runtime/providers/openai_oauth.hpp>
#include <agent-runtime/providers/chat_completions_provider.hpp>
#include <openssl/rand.h>

void to_json(nlohmann::json& json, const ProviderDescriptor& provider) {
    json = {{"id", provider.id}, {"connected", provider.connected}, {"model", provider.model}};
    json["name"] = provider.name.empty() ? provider.id : provider.name;
    json["custom"] = provider.custom;
    if (provider.custom) json["base_url"] = provider.baseUrl;
    json["auth_method"] = provider.method ? nlohmann::json(authMethodName(*provider.method)) : nlohmann::json(nullptr);
}

void to_json(nlohmann::json& json, const ModelDescriptor& model) {
    json = {{"provider", model.provider}, {"model", model.model}, {"auth_method", authMethodName(model.method)}};
    json["max_context"] = model.maxContext ? nlohmann::json(*model.maxContext) : nlohmann::json(nullptr);
    json["image_input"] = imageSupportName(model.imageInput);
    if (!model.providerName.empty()) json["provider_name"] = model.providerName;
}

void to_json(nlohmann::json& json, const ModelCatalogError& error) {
    json = {{"provider", error.provider}, {"error", error.error}};
}

ProviderConnections::ProviderConnections(std::shared_ptr<AuthStore> auth, std::filesystem::path config,
    OAuthStarter oauthStarter, ModelCatalog catalog, ApiKeyValidator keyValidator)
    : auth(std::move(auth)), config(std::move(config)), oauthStarter(std::move(oauthStarter)),
      catalog(std::move(catalog)), keyValidator(std::move(keyValidator)) {}

ProviderSelection ProviderConnections::selection() const {
    const auto data = readPrivateJson(config);
    ProviderSelection result;
    if (data.contains("provider")) {
        result.provider = data.at("provider").get<std::string>();
        validateProviderId(result.provider);
        result.model = data.value("model", result.provider == "openai" ? "gpt-5.4" : daemon_config::DEFAULT_MODEL);
    }
    result.reasoning = data.value("reasoning", defaultReasoning(result));
    if (data.contains("context_limit") && !data["context_limit"].is_null()) {
        if (!data["context_limit"].is_number_unsigned() || data["context_limit"].get<std::uint64_t>() < 32768)
            throw std::runtime_error("Configured context limit must be at least 32768 tokens");
        result.contextLimit = data["context_limit"].get<std::uint64_t>();
    }
    if (result.reasoning.empty()) throw std::runtime_error("Configured reasoning effort must not be empty");
    return result;
}

PermissionMode ProviderConnections::permissionMode() const {
    return parsePermissionMode(readPrivateJson(config).value("permission_mode", "ask"));
}

int ProviderConnections::permissionConfidence() const {
    const auto data = readPrivateJson(config);
    const auto value = data.value("permission_confidence", nlohmann::json(80));
    if (!value.is_number_integer() || value < 0 || value > 100)
        throw std::runtime_error("Configured confidence must be an integer between 0 and 100");
    return value.get<int>();
}

void ProviderConnections::setPermissionMode(PermissionMode mode, std::optional<int> confidence) {
    savePermissionSettings(mode, confidence);
}

PermissionVerifierConfig ProviderConnections::permissionModel() const {
    std::lock_guard lock(permissionMutex);
    const auto data = readPrivateJson(config);
    return data.contains("permission_model") ? data.at("permission_model").get<PermissionVerifierConfig>()
                                             : PermissionVerifierConfig{};
}

nlohmann::json ProviderConnections::permissionModelInfo() const {
    std::lock_guard lock(permissionMutex);
    nlohmann::json result = permissionModel();
    result["has_key"] = hasPermissionKey();
    return result;
}

bool ProviderConnections::hasPermissionKey() const {
    std::lock_guard lock(permissionMutex);
    const auto data = readPrivateJson(config);
    const auto model = permissionModel();
    return !auth->permissionKey(decisionProtocolName(model.protocol), model.endpoint,
        data.value("permission_credential", "")).empty();
}

void ProviderConnections::savePermissionKey(const std::string& key) {
    savePermissionSettings(permissionMode(), {}, {}, key);
}

void ProviderConnections::savePermissionSettings(PermissionMode mode, std::optional<int> threshold,
    std::optional<PermissionVerifierConfig> model, const std::string& key) {
    std::lock_guard lock(permissionMutex);
    auto data = readPrivateJson(config);
    const auto selected = model.value_or(permissionModel());
    selected.validate();
    const int percentage = threshold.value_or(permissionConfidence());
    if (percentage < 0 || percentage > 100) throw std::invalid_argument("Probability must be between 0 and 100");
    // Validate the mode before staging any credential.
    const auto modeName = permissionModeName(mode);
    const auto previous = permissionModel();
    std::string revision = data.value("permission_credential", "");
    if (selected.protocol != previous.protocol || selected.endpoint != previous.endpoint) revision.clear();
    if (!key.empty()) validateApiKeyFormat(key);
    const auto existing = auth->permissionKey(decisionProtocolName(selected.protocol), selected.endpoint, revision);
    if (mode == PermissionMode::AUTO && percentage != 0 && key.empty() && existing.empty())
        throw std::invalid_argument("Enter an API key for the selected AI Check endpoint first");
    if (!key.empty()) revision = auth->stagePermissionKey(decisionProtocolName(selected.protocol), selected.endpoint, key);
    // Staged revisions are immutable. A failed settings write leaves the old configuration and key active.
    data["permission_mode"] = modeName;
    data["permission_confidence"] = percentage;
    data["permission_model"] = selected;
    data["permission_credential"] = revision;
    writePrivateJson(config, data);
}

PermissionManager::Verifier ProviderConnections::permissionVerifier(const std::string& workspace,
    const std::string& context) const {
    std::lock_guard lock(permissionMutex);
    const auto model = permissionModel();
    const auto key = auth->permissionKey(decisionProtocolName(model.protocol), model.endpoint,
        readPrivateJson(config).value("permission_credential", ""));
    // Only the current user's request, not tool output, reasoning or stored credentials.
    std::string excerpt = context;
    if (excerpt.size() > 2000) {
        std::size_t end = 2000;
        while (end && (static_cast<unsigned char>(excerpt[end]) & 0xc0) == 0x80) --end;
        excerpt.resize(end);
        excerpt += " [truncated]";
    }
    return [model, key, workspace, excerpt](const ToolPermissionRequest& request, const std::function<void()>& check) {
        return PermissionVerifier(key, model).verify(request, workspace, excerpt, [check] {
            try { check(); return false; }
            catch (...) { return true; }
        });
    };
}

void ProviderConnections::select(const ProviderSelection& selection) {
    validateProviderId(selection.provider);
    if (selection.model.empty() || selection.model.size() > 256 || selection.model.find_first_of(" \t\r\n") != std::string::npos)
        throw std::invalid_argument("Invalid model ID");
    requireAuth(selection.provider);
    if (selection.contextLimit && *selection.contextLimit < 32768)
        throw std::invalid_argument("Context limit must be at least 32768 tokens");
    auto data = readPrivateJson(config);
    const auto reasoning = selection.reasoning.empty()
        ? data.value("reasoning", data.contains("provider") ? this->selection().reasoning : defaultReasoning(selection))
        : selection.reasoning;
    data["provider"] = selection.provider;
    data["model"] = selection.model;
    data["reasoning"] = reasoning;
    data["context_limit"] = selection.contextLimit ? nlohmann::json(*selection.contextLimit) : nlohmann::json(nullptr);
    writePrivateJson(config, data);
}

void ProviderConnections::setReasoning(const std::string& reasoning) {
    if (reasoning.empty()) throw std::invalid_argument("Reasoning effort must not be empty");
    const auto current = selection();
    auto data = readPrivateJson(config);
    data["provider"] = current.provider;
    data["model"] = current.model;
    data["reasoning"] = reasoning;
    writePrivateJson(config, data);
}

std::string ProviderConnections::defaultReasoning(const ProviderSelection& selection) const {
    if (selection.provider == "openai") {
        const auto effort = OpenAIProvider::defaultReasoningEffort(selection.model);
        if (!effort.empty()) return effort;
    }
    return daemon_config::DEFAULT_REASONING_EFFORT;
}

std::unique_ptr<Provider> ProviderConnections::create(const std::string& provider) const {
    validateProviderId(provider);
    if (provider.starts_with("custom-")) {
        const auto definition = customDefinition(provider);
        return std::make_unique<ChatCompletionsProvider>(definition.baseUrl, customKey(provider), definition.name);
    }
    if (provider == "openai") return std::make_unique<OpenAIProvider>(std::make_shared<OpenAIProviderAuth>(auth));
    return std::make_unique<OpenRouterProvider>(std::make_shared<OpenRouterProviderAuth>(auth));
}

bool ProviderConnections::hasAuth(const std::string& provider) const {
    return activeAuthMethod(provider).has_value();
}

std::optional<AuthMethod> ProviderConnections::activeAuthMethod(const std::string& provider) const {
    try {
        if (provider.starts_with("custom-")) { customDefinition(provider); return AuthMethod::API_KEY; }
        return auth->get(provider).method;
    }
    catch (const std::exception&) { return std::nullopt; }
}

void ProviderConnections::requireAuth(const std::string& provider) const {
    if (provider.starts_with("custom-")) { customDefinition(provider); customKey(provider); }
    else auth->get(provider);
}

std::string ProviderConnections::firstCompatibleModel(const std::string& provider, core::CancellationCheck cancelled) const {
    if (provider.starts_with("custom-"))
        return ChatCompletionsProvider::discoverModels(customDefinition(provider).baseUrl, customKey(provider), cancelled).front().id;
    const auto authorization = authorize(provider, cancelled);
    return catalog.firstCompatibleModel(provider, authorization, std::move(cancelled));
}

ProviderAuthorization ProviderConnections::authorize(const std::string& provider, core::CancellationCheck cancelled) const {
    validateProviderId(provider);
    if (provider != "openai" && provider != "openrouter")
        throw std::invalid_argument("Custom providers use their own endpoint and credentials");
    return provider == "openai" ? OpenAIProviderAuth(auth).authorize(cancelled) :
        OpenRouterProviderAuth(auth).authorize(cancelled);
}

ConnectedModels ProviderConnections::listModels(core::CancellationCheck cancelled) const {
    ConnectedModels result;
    for (const std::string id : {"openai", "openrouter"}) {
        core::checkCancellation(cancelled);
        const auto method = activeAuthMethod(id);
        if (!method) continue;
        try {
            const auto authorization = authorize(id, cancelled);
            for (const auto& model : catalog.listModelInfo(id, authorization, cancelled))
                result.models.push_back({id, model.id, *method, model.maxContextLimit ? model.maxContextLimit : model.contextLimit,
                    model.imageInput});
        } catch (const core::Cancelled&) {
            throw;
        } catch (const std::exception& error) {
            result.errors.push_back({id, error.what()});
        }
    }
    for (const auto& provider : list(selection())) {
        if (!provider.custom) continue;
        core::checkCancellation(cancelled);
        try {
            for (const auto& model : ChatCompletionsProvider::discoverModels(provider.baseUrl, customKey(provider.id), cancelled))
                result.models.push_back({provider.id, model.id, AuthMethod::API_KEY, {}, model.imageInput, provider.name});
        } catch (const core::Cancelled&) { throw; }
        catch (const std::exception& error) { result.errors.push_back({provider.name, error.what()}); }
    }
    core::checkCancellation(cancelled);
    return result;
}

std::vector<ProviderDescriptor> ProviderConnections::list(const ProviderSelection& selection) const {
    std::vector<ProviderDescriptor> result;
    for (const std::string id : {"openai", "openrouter"}) {
        const auto method = activeAuthMethod(id);
        result.push_back({id, method.has_value(), selection.provider == id ? selection.model :
            std::string(id == "openai" ? "gpt-5.4" : daemon_config::DEFAULT_MODEL), method});
    }
    const auto data = readPrivateJson(config);
    const auto definitions = data.value("custom_providers", nlohmann::json::object());
    for (const auto& [id, definition] : definitions.items()) {
        validateProviderId(id);
        if (!id.starts_with("custom-")) throw std::runtime_error("Invalid custom provider ID");
        const auto custom = parseCustomDefinition(definition);
        result.push_back({id, true, selection.provider == id ? selection.model : "", AuthMethod::API_KEY,
            custom.name, true, custom.baseUrl});
    }
    std::sort(result.begin() + 2, result.end(), [](const auto& a, const auto& b) {
        return a.name < b.name;
    });
    return result;
}

CustomProviderDefinition ProviderConnections::customDefinition(const std::string& id) const {
    validateProviderId(id);
    const auto definitions = readPrivateJson(config).value("custom_providers", nlohmann::json::object());
    if (!id.starts_with("custom-") || !definitions.contains(id))
        throw std::runtime_error("Custom provider no longer exists; select another provider with /connect");
    return parseCustomDefinition(definitions.at(id));
}

CustomProviderDefinition ProviderConnections::parseCustomDefinition(const nlohmann::json& definition) {
    CustomProviderDefinition result{definition.at("name").get<std::string>(),
        ChatCompletionsProvider::normalizeBaseUrl(definition.at("base_url").get<std::string>())};
    validateCustomName(result.name);
    return result;
}

std::string ProviderConnections::customKey(const std::string& id) const {
    return auth->contains(id) ? auth->get(id).key : "";
}

void ProviderConnections::validateCustomName(const std::string& name) {
    if (name.empty() || name.size() > 80 || name.find_first_not_of(" \t") == std::string::npos ||
        std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c == 127; }))
        throw std::invalid_argument("Enter a provider name of at most 80 bytes without control characters");
}

std::string ProviderConnections::addCustom(const std::string& name, const std::string& baseUrl, const std::string& key) {
    validateCustomName(name);
    const auto url = ChatCompletionsProvider::normalizeBaseUrl(baseUrl);
    auto data = readPrivateJson(config);
    for (const auto& provider : list(selection()))
        if (provider.name == name || provider.id == name || name == "OpenAI" || name == "OpenRouter")
            throw std::invalid_argument("A provider with this name already exists");
    unsigned char bytes[16];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) throw std::runtime_error("Cannot generate provider identity");
    std::string id = "custom-";
    constexpr char HEX[] = "0123456789abcdef";
    for (auto byte : bytes) { id += HEX[byte >> 4]; id += HEX[byte & 15]; }
    if (!key.empty()) saveApiKey(id, key);
    data["custom_providers"][id] = {{"name", name}, {"base_url", url}};
    try { writePrivateJson(config, data); }
    catch (...) { if (!key.empty()) auth->remove(id); throw; }
    return id;
}

void ProviderConnections::deleteCustom(const std::string& id) {
    customDefinition(id);
    if (selection().provider == id)
        throw std::invalid_argument("Select another provider with /model or /connect before deleting the current provider");
    // Keep all saved conversations and never recycle an instance identity.
    auto data = readPrivateJson(config);
    data["custom_providers"].erase(id);
    const auto previous = auth->contains(id) ? std::optional{auth->get(id)} : std::nullopt;
    auth->remove(id);
    try { writePrivateJson(config, data); }
    catch (...) { if (previous) auth->set(id, *previous); throw; }
}

void ProviderConnections::saveCredential(const std::string& provider, const ProviderCredential& credential) {
    std::unique_lock lock(auth->refreshMutex, std::try_to_lock);
    if (!lock.owns_lock()) throw std::runtime_error("Authentication refresh in progress; retry shortly");
    auth->set(provider, credential);
}

void ProviderConnections::saveApiKey(const std::string& provider, const std::string& key) {
    saveCredential(provider, ProviderCredential{.key = key});
}

void ProviderConnections::validateApiKey(const std::string& provider, const std::string& key,
    core::CancellationCheck cancelled) const {
    keyValidator.validate(provider, key, std::move(cancelled));
}

void ProviderConnections::saveOAuth(const std::string& provider, const ProviderCredential& credential) {
    saveCredential(provider, credential);
}

std::unique_ptr<ProviderAuthFlow> ProviderConnections::startOAuth(const std::string& provider,
    const std::string& method, std::shared_ptr<std::atomic<bool>> cancelled) const {
    if (provider != "openai") throw std::invalid_argument("OAuth is only available for OpenAI");
    if (oauthStarter) return oauthStarter(method, std::move(cancelled));
    return startOpenAIOAuth(method, std::move(cancelled));
}
