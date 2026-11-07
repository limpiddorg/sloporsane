#ifndef SOS_MCP_OAUTH_CREDENTIALS_HPP
#define SOS_MCP_OAUTH_CREDENTIALS_HPP

#include "oauth.hpp"
#include <mutex>

namespace mcp {
// Shared identity lock across transports/workspaces. Refresh before sending a
// request, never replay a tools/call after an authentication failure.
class OAuthCredentials {
public:
    OAuthCredentials(AuthStore store, std::string identity);
    std::string bearer(core::CancellationCheck cancelled);
    void revoke(core::CancellationCheck cancelled);
private:
    std::unique_lock<std::mutex> acquire(core::CancellationCheck cancelled);
    AuthStore store;
    std::string identity;
    std::shared_ptr<std::mutex> mutex;
};
}

#endif // SOS_MCP_OAUTH_CREDENTIALS_HPP
