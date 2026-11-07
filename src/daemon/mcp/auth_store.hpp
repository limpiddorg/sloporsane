#ifndef SOS_MCP_AUTH_STORE_HPP
#define SOS_MCP_AUTH_STORE_HPP

#include "config.hpp"
#include <cstdint>
#include <optional>

namespace mcp {
struct Credentials {
    std::string accessToken;
    std::string refreshToken;
    std::int64_t expiresAt = 0; // Unix seconds; zero for a non-expiring token.
    std::string clientId;
    std::string clientSecret;
    std::map<std::string, std::string> environment;
    std::string tokenEndpoint;
    std::string resource;
    std::string tokenAuthMethod = "none";
    std::string revocationEndpoint;
    bool operator==(const Credentials&) const = default;
};

// Local trust is never read from the workspace's shareable configuration.
// Callers must not return Credentials in protocol responses or event metadata.
class AuthStore {
public:
    explicit AuthStore(std::filesystem::path path);
    AuthStore();
    bool trusted(const std::string& identity) const;
    void trust(const std::string& identity, bool accepted) const;
    std::optional<Credentials> get(const std::string& identity) const;
    void set(const std::string& identity, const Credentials& credentials) const;
    void forget(const std::string& identity) const;
    void replaceIfCurrent(const std::string& identity, const Credentials& previous, const Credentials& next) const;
private:
    std::filesystem::path path;
};
}

#endif // SOS_MCP_AUTH_STORE_HPP
