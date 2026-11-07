#ifndef SOS_API_KEY_VALIDATOR_HPP
#define SOS_API_KEY_VALIDATOR_HPP

#include "provider_auth.hpp"

struct ApiKeyValidationEndpoints {
    std::string openai = "https://api.openai.com/v1/models";
    std::string openrouter = "https://openrouter.ai/api/v1/key";
};

class ApiKeyValidator {
public:
    explicit ApiKeyValidator(ApiKeyValidationEndpoints endpoints = {});
    void validate(const std::string& provider, const std::string& key, core::CancellationCheck cancelled = {}) const;
private:
    ApiKeyValidationEndpoints endpoints;
};

#endif // SOS_API_KEY_VALIDATOR_HPP
