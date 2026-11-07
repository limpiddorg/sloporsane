#ifndef SOS_PROVIDER_CONNECTION_HPP
#define SOS_PROVIDER_CONNECTION_HPP

#include "provider_connections.hpp"
#include <asio/awaitable.hpp>

enum class ProviderCommand { LIST, API_KEY, START_OAUTH, POLL_OAUTH, CANCEL_OAUTH, SELECT,
    LIST_MODELS, SELECT_MODEL, CANCEL_MODELS, CREATE_CUSTOM, DELETE_CUSTOM };

struct ProviderRequest {
    ProviderCommand command;
    std::string provider;
    std::string model;
    std::string method;
    std::string key;
    std::string authId;
    std::string name;
    std::string baseUrl;
    std::optional<std::uint64_t> contextLimit;
};
void from_json(const nlohmann::json& json, ProviderRequest& request);

// Connection-local lifecycle. ProviderConnections owns shared credentials/settings.
class ProviderConnection {
public:
    explicit ProviderConnection(std::shared_ptr<ProviderConnections> providers);
    ProviderSelection selection() const;
    void restoreSelection(std::optional<ProviderSelection> selection);
    void setReasoning(const std::string& reasoning);
    void setModel(const std::string& model);
    void prepare(const ProviderRequest& request);
    asio::awaitable<nlohmann::json> handle(const ProviderRequest& request);
    void cancel();
    asio::awaitable<void> drain();
private:
    nlohmann::json listProviders() const;
    asio::awaitable<nlohmann::json> listModels();
    asio::awaitable<nlohmann::json> createCustom(const ProviderRequest& request);
    asio::awaitable<nlohmann::json> saveApiKey(const ProviderRequest& request);
    asio::awaitable<nlohmann::json> startAuthentication(const ProviderRequest& request);
    nlohmann::json pollAuthentication(const std::string& id);
    asio::awaitable<nlohmann::json> cancelAuthentication(const std::string& id);
    asio::awaitable<nlohmann::json> selectProvider(ProviderSelection selection);
    void requireNoPendingAuthentication() const;
    bool matches(const std::string& id) const;

    std::shared_ptr<ProviderConnections> providers;
    ProviderSelection selected;
    std::optional<ProviderSelection> conversationSelection;
    std::vector<ModelDescriptor> listedModels;
    std::unique_ptr<ProviderAuthFlow> flow;
    std::shared_ptr<std::atomic<bool>> cancellation;
    std::string authProvider;
    AuthMethod authMethod = AuthMethod::BROWSER;
    std::string authId;
    std::uint64_t authNumber = 0;
};

#endif // SOS_PROVIDER_CONNECTION_HPP
