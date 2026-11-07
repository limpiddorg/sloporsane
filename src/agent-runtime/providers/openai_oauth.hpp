#ifndef SOS_OPENAI_OAUTH_HPP
#define SOS_OPENAI_OAUTH_HPP

#include "provider_auth.hpp"
#include <atomic>
#include <future>

std::int64_t oauthNow();
std::unique_ptr<ProviderAuthFlow> startOpenAIOAuth(const std::string& method,
    std::shared_ptr<std::atomic<bool>> cancelled, OpenAIOAuthSettings settings = {});
ProviderCredential refreshOpenAICredential(const ProviderCredential& previous,
    core::CancellationCheck cancelled, OpenAIOAuthSettings settings = {});

#endif // SOS_OPENAI_OAUTH_HPP
