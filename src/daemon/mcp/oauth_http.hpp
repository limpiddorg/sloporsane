#ifndef SOS_MCP_OAUTH_HTTP_HPP
#define SOS_MCP_OAUTH_HTTP_HPP

#include "transport.hpp"
#include <map>

namespace mcp::oauth {
struct HttpResult {
    long status;
    nlohmann::json body;
    std::vector<std::string> challenges;
};
struct HttpOptions {
    std::string authorization;
    bool allowNonJson = false;
    bool acceptEvents = false; // Unauthenticated MCP GET probe, not metadata/token requests.
};
// Bounded, no redirects, no ambient credentials. HTTPS except loopback.
HttpResult request(const std::string& url, const std::string& body, const std::string& contentType,
    core::CancellationCheck cancelled, HttpOptions options = {});
std::map<std::string, std::string> bearerChallenge(const std::vector<std::string>& headers);
std::string basicAuthorization(const std::string& id, const std::string& secret);
std::string encode(const std::string& value);
std::string form(const std::map<std::string, std::string>& values);
std::string origin(const std::string& url);
std::string path(const std::string& url);
void validateEndpoint(const std::string& url);
}

#endif // SOS_MCP_OAUTH_HTTP_HPP
