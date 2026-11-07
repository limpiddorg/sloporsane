#include "provider_connection.hpp"
#include "../transport/await_future.hpp"
#include <map>
#include <agent-runtime/providers/chat_completions_provider.hpp>

void from_json(const nlohmann::json& json, ProviderRequest& request) {
    static const std::map<std::string, ProviderCommand> COMMANDS{
        {"provider.create", ProviderCommand::CREATE_CUSTOM}, {"provider.delete", ProviderCommand::DELETE_CUSTOM},
        {"provider.list", ProviderCommand::LIST}, {"provider.select", ProviderCommand::SELECT},
        {"model.list", ProviderCommand::LIST_MODELS}, {"model.select", ProviderCommand::SELECT_MODEL},
        {"model.cancel", ProviderCommand::CANCEL_MODELS},
        {"auth.api", ProviderCommand::API_KEY}, {"auth.start", ProviderCommand::START_OAUTH},
        {"auth.poll", ProviderCommand::POLL_OAUTH}, {"auth.cancel", ProviderCommand::CANCEL_OAUTH}};
    const auto found = COMMANDS.find(json.at("type").get<std::string>());
    if (found == COMMANDS.end()) throw std::invalid_argument("Unknown provider request");
    request.command = found->second;
    if (request.command == ProviderCommand::SELECT || request.command == ProviderCommand::SELECT_MODEL ||
        request.command == ProviderCommand::API_KEY || request.command == ProviderCommand::START_OAUTH ||
        request.command == ProviderCommand::DELETE_CUSTOM) {
        request.provider = json.at("provider").get<std::string>();
        validateProviderId(request.provider);
    }
    switch (request.command) {
        case ProviderCommand::CREATE_CUSTOM:
            request.name = json.at("name").get<std::string>();
            request.baseUrl = json.at("base_url").get<std::string>();
            request.key = json.value("key", "");
            break;
        case ProviderCommand::DELETE_CUSTOM: break;
        case ProviderCommand::SELECT: request.model = json.value("model", ""); break;
        case ProviderCommand::SELECT_MODEL:
            request.model = json.at("model").get<std::string>();
            if (request.model.empty()) throw std::invalid_argument("Model ID must not be empty");
            if (json.contains("context_limit")) {
                const auto& limit = json.at("context_limit");
                if (!limit.is_number_integer() || (!limit.is_number_unsigned() && limit.get<std::int64_t>() < 32768) ||
                    limit.get<std::uint64_t>() < 32768)
                    throw std::invalid_argument("Context limit must be an integer of at least 32768 tokens");
                request.contextLimit = limit.get<std::uint64_t>();
            }
            break;
        case ProviderCommand::API_KEY: request.key = json.at("key").get<std::string>(); break;
        case ProviderCommand::START_OAUTH: request.method = json.at("method").get<std::string>(); break;
        case ProviderCommand::POLL_OAUTH:
        case ProviderCommand::CANCEL_OAUTH: request.authId = json.value("authId", ""); break;
        case ProviderCommand::LIST:
        case ProviderCommand::LIST_MODELS:
        case ProviderCommand::CANCEL_MODELS: break;
    }
}

ProviderConnection::ProviderConnection(std::shared_ptr<ProviderConnections> providers) : providers(std::move(providers)) {
    if (this->providers) selected = this->providers->selection();
}

ProviderSelection ProviderConnection::selection() const {
    auto current = conversationSelection ? *conversationSelection : (providers ? providers->selection() : selected);
    if (current.reasoning.empty()) current.reasoning = daemon_config::DEFAULT_REASONING_EFFORT;
    return current;
}
void ProviderConnection::restoreSelection(std::optional<ProviderSelection> selection) {
    conversationSelection = std::move(selection);
}
void ProviderConnection::setReasoning(const std::string& reasoning) {
    if (reasoning.empty()) throw std::invalid_argument("Reasoning effort must not be empty");
    auto current = selection();
    current.reasoning = reasoning;
    if (providers) providers->select(current);
    else selected = current;
    if (conversationSelection) conversationSelection = current;
}
void ProviderConnection::setModel(const std::string& model) {
    auto current = selection();
    current.model = model;
    current.contextLimit.reset();
    if (providers) providers->select(current);
    else selected = std::move(current);
    if (conversationSelection) conversationSelection = providers ? providers->selection() : selected;
}
bool ProviderConnection::matches(const std::string& id) const { return id.empty() || id == authId; }
void ProviderConnection::cancel() { if (cancellation) cancellation->store(true); }

void ProviderConnection::prepare(const ProviderRequest& request) {
    if ((request.command == ProviderCommand::START_OAUTH || request.command == ProviderCommand::API_KEY ||
         request.command == ProviderCommand::LIST_MODELS || request.command == ProviderCommand::CREATE_CUSTOM ||
         (request.command == ProviderCommand::SELECT && request.model.empty())) && !flow && !cancellation)
        cancellation = std::make_shared<std::atomic<bool>>(false);
    if (request.command == ProviderCommand::CANCEL_OAUTH && matches(request.authId)) cancel();
    if (request.command == ProviderCommand::CANCEL_MODELS && !flow) cancel();
}

void ProviderConnection::requireNoPendingAuthentication() const {
    if (flow) throw std::logic_error("Complete or cancel the pending OAuth connection first");
}

asio::awaitable<nlohmann::json> ProviderConnection::handle(const ProviderRequest& request) {
    if (!providers) throw std::runtime_error("Provider connections are unavailable");
    try {
        switch (request.command) {
            case ProviderCommand::CREATE_CUSTOM: co_return co_await createCustom(request);
            case ProviderCommand::DELETE_CUSTOM:
                requireNoPendingAuthentication();
                providers->deleteCustom(request.provider);
                co_return listProviders();
            case ProviderCommand::LIST: co_return listProviders();
            case ProviderCommand::LIST_MODELS: co_return co_await listModels();
            case ProviderCommand::CANCEL_MODELS:
                requireNoPendingAuthentication();
                cancellation.reset();
                co_return nlohmann::json{{"type", "model.cancelled"}};
            case ProviderCommand::SELECT_MODEL: {
                if (request.contextLimit) {
                    const auto found = std::find_if(listedModels.begin(), listedModels.end(), [&](const auto& model) {
                        return model.provider == request.provider && model.model == request.model;
                    });
                    if (found == listedModels.end()) throw std::invalid_argument("List models before selecting a context limit");
                    if (found->maxContext && *request.contextLimit > *found->maxContext)
                        throw std::invalid_argument("Context limit exceeds the provider maximum");
                }
                auto reply = co_await selectProvider({request.provider, request.model, selection().reasoning, request.contextLimit});
                reply["type"] = "model.selected";
                co_return reply;
            }
            case ProviderCommand::API_KEY: co_return co_await saveApiKey(request);
            case ProviderCommand::START_OAUTH: co_return co_await startAuthentication(request);
            case ProviderCommand::POLL_OAUTH: co_return pollAuthentication(request.authId);
            case ProviderCommand::CANCEL_OAUTH: co_return co_await cancelAuthentication(request.authId);
            case ProviderCommand::SELECT: co_return co_await selectProvider({request.provider, request.model});
        }
    } catch (...) {
        if (!flow) cancellation.reset();
        throw;
    }
    throw std::invalid_argument("Unknown provider command");
}

nlohmann::json ProviderConnection::listProviders() const {
    return {{"type", "provider.listed"}, {"providers", providers->list(selection())}};
}

asio::awaitable<nlohmann::json> ProviderConnection::listModels() {
    requireNoPendingAuthentication();
    if (!cancellation) cancellation = std::make_shared<std::atomic<bool>>(false);
    auto loading = std::async(std::launch::async, [providers = providers, cancel = cancellation] {
        return providers->listModels([cancel] { return cancel->load(); });
    });
    co_await awaitFuture(loading);
    const auto result = loading.get();
    core::checkCancellation([cancel = cancellation] { return cancel->load(); });
    cancellation.reset();
    listedModels = result.models;
    co_return nlohmann::json{{"type", "model.listed"}, {"models", result.models}, {"errors", result.errors}};
}

asio::awaitable<nlohmann::json> ProviderConnection::createCustom(const ProviderRequest& request) {
    requireNoPendingAuthentication();
    ProviderConnections::validateCustomName(request.name);
    ChatCompletionsProvider::normalizeBaseUrl(request.baseUrl);
    if (!cancellation) cancellation = std::make_shared<std::atomic<bool>>(false);
    auto checking = std::async(std::launch::async, [url = request.baseUrl, key = request.key, cancel = cancellation] {
        return ChatCompletionsProvider::discoverModels(url, key, [cancel] { return cancel->load(); });
    });
    co_await awaitFuture(checking);
    checking.get();
    core::checkCancellation([cancel = cancellation] { return cancel->load(); });
    const auto id = providers->addCustom(request.name, request.baseUrl, request.key);
    cancellation.reset();
    auto result = listProviders();
    result["created_provider"] = id;
    co_return result;
}

asio::awaitable<nlohmann::json> ProviderConnection::saveApiKey(const ProviderRequest& request) {
    requireNoPendingAuthentication();
    if (!cancellation) cancellation = std::make_shared<std::atomic<bool>>(false);
    auto checking = std::async(std::launch::async,
        [providers = providers, id = request.provider, key = request.key, cancel = cancellation] {
            providers->validateApiKey(id, key, [cancel] { return cancel->load(); });
        });
    co_await awaitFuture(checking);
    checking.get();
    core::checkCancellation([cancel = cancellation] { return cancel->load(); });
    providers->saveApiKey(request.provider, request.key);
    cancellation.reset();
    co_return nlohmann::json{{"type", "auth.saved"}};
}

asio::awaitable<nlohmann::json> ProviderConnection::startAuthentication(const ProviderRequest& request) {
    requireNoPendingAuthentication();
    if (!cancellation) cancellation = std::make_shared<std::atomic<bool>>(false);
    authId = std::to_string(++authNumber);
    authProvider = request.provider;
    authMethod = parseAuthMethod(request.method);
    auto starting = std::async(std::launch::async,
        [providers = providers, provider = request.provider, method = request.method, cancel = cancellation] {
            core::checkCancellation([cancel] { return cancel->load(); });
            return providers->startOAuth(provider, method, cancel);
        });
    co_await awaitFuture(starting);
    flow = starting.get();
    co_return nlohmann::json{{"type", "auth.started"}, {"authId", authId}, {"url", flow->url}, {"instructions", flow->instructions}};
}

nlohmann::json ProviderConnection::pollAuthentication(const std::string& id) {
    if (!flow || id != authId) throw std::invalid_argument("Unknown OAuth connection");
    if (flow->completion.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return {{"type", "auth.pending"}, {"authId", authId}};
    auto completed = std::move(flow);
    auto credential = completed->completion.get();
    credential.method = authMethod;
    core::checkCancellation([cancel = cancellation] { return cancel->load(); });
    providers->saveOAuth(authProvider, credential);
    cancellation.reset();
    return {{"type", "auth.saved"}};
}

asio::awaitable<void> ProviderConnection::drain() {
    cancel();
    if (flow) co_await awaitFuture(flow->completion);
    flow.reset();
    cancellation.reset();
}

asio::awaitable<nlohmann::json> ProviderConnection::cancelAuthentication(const std::string& id) {
    if (matches(id)) co_await drain();
    co_return nlohmann::json{{"type", "auth.cancelled"}};
}

asio::awaitable<nlohmann::json> ProviderConnection::selectProvider(ProviderSelection selection) {
    requireNoPendingAuthentication();
    if (selection.model.empty()) {
        if (!cancellation) cancellation = std::make_shared<std::atomic<bool>>(false);
        auto finding = std::async(std::launch::async, [providers = providers, id = selection.provider, cancel = cancellation] {
            return providers->firstCompatibleModel(id, [cancel] { return cancel->load(); });
        });
        co_await awaitFuture(finding);
        selection.model = finding.get();
        core::checkCancellation([cancel = cancellation] { return cancel->load(); });
        cancellation.reset();
    }
    providers->select(selection);
    selected = selection;
    if (conversationSelection) conversationSelection = providers->selection();
    co_return nlohmann::json{{"type", "provider.selected"}};
}
