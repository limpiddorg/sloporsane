#ifndef SOS_PROVIDER_HTTP_HPP
#define SOS_PROVIDER_HTTP_HPP

#include "provider_auth.hpp"
#include <cpr/cprtypes.h>

struct ProviderRequestTimeouts {
    std::chrono::milliseconds total{std::chrono::minutes(10)};
    std::chrono::milliseconds idle{std::chrono::minutes(10)};
    void validate() const;
};

std::string formatProviderTimeout(std::chrono::milliseconds duration);

// Shared generation transport. Non-streaming responses may take the full total
// budget before their first body bytes; streaming responses must stay active.
std::string postProviderResponse(const std::string& url, const std::string& bearer, const cpr::Header& headers,
    const nlohmann::json& request, core::CancellationCheck cancelled, const std::string& provider,
    ProviderRequestTimeouts timeouts = {});

// Bounded, cancellable authenticated GET. Never expose the remote body in errors.
nlohmann::json getProviderJson(const std::string& url, const ProviderAuthorization& auth,
    core::CancellationCheck cancelled, const std::string& operation);

#endif // SOS_PROVIDER_HTTP_HPP
