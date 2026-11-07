#include "api_key_validator.hpp"
#include "provider_http.hpp"

ApiKeyValidator::ApiKeyValidator(ApiKeyValidationEndpoints endpoints) : endpoints(std::move(endpoints)) {}

void ApiKeyValidator::validate(const std::string& provider, const std::string& key, core::CancellationCheck cancelled) const {
    if (provider != "openai" && provider != "openrouter")
        throw std::invalid_argument("API key validation is only available for built-in providers");
    validateProviderId(provider);
    validateApiKeyFormat(key);
    const auto response = getProviderJson(provider == "openai" ? endpoints.openai : endpoints.openrouter,
        ProviderAuthorization{.bearer = key}, std::move(cancelled), "API key validation");
    const auto data = response.find("data");
    if (data == response.end() || (provider == "openai" ? !data->is_array() : !data->is_object()))
        throw std::runtime_error("API key validation returned an unexpected response");
    if (provider == "openrouter" &&
        (data->value("is_management_key", false) || data->value("is_provisioning_key", false)))
        throw std::runtime_error("Use an OpenRouter inference key, not a management key");
}
