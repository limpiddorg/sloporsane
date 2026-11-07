#ifndef SOS_MCP_OAUTH_HPP
#define SOS_MCP_OAUTH_HPP

#include "auth_store.hpp"
#include "transport.hpp"
#include <atomic>
#include <future>

namespace mcp {
// Owns an ephemeral loopback listener and one authorization attempt. Cancelling
// never saves credentials. The owner must consume completion before committing.
struct OAuthFlow {
    ~OAuthFlow();
    std::string url;
    std::shared_ptr<std::atomic<bool>> cancelled;
    std::future<Credentials> completion;
};
std::unique_ptr<OAuthFlow> startOAuth(const Remote& remote, core::CancellationCheck cancelled = {},
    std::shared_ptr<std::atomic<bool>> stop = {}, const Credentials& registration = {},
    const AuthorizationChallenge& challenge = {});
// Refreshes only at the persisted token endpoint, never a rediscovered endpoint.
Credentials refreshOAuth(const Credentials& previous, core::CancellationCheck cancelled);
void revokeOAuth(const Credentials& credentials, core::CancellationCheck cancelled);
}

#endif // SOS_MCP_OAUTH_HPP
