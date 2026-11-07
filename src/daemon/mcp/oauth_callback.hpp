#ifndef SOS_MCP_OAUTH_CALLBACK_HPP
#define SOS_MCP_OAUTH_CALLBACK_HPP

#include "transport.hpp"

namespace mcp::oauth {
// Ephemeral IPv4 loopback listener. Exact path/state, bounded requests and wait.
class Callback {
public:
    explicit Callback(unsigned short port = 0);
    ~Callback();
    Callback(const Callback&) = delete;
    Callback& operator=(const Callback&) = delete;
    const std::string& redirect() const { return uri; }
    std::string wait(const std::string& state, core::CancellationCheck cancelled);
private:
    int socket = -1;
    std::string uri;
};
std::string randomCode();
std::string challenge(const std::string& verifier);
}

#endif // SOS_MCP_OAUTH_CALLBACK_HPP
