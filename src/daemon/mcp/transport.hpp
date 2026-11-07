#ifndef SOS_MCP_TRANSPORT_HPP
#define SOS_MCP_TRANSPORT_HPP

#include <agent-runtime/core/cancellation.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <string_view>

namespace mcp {
inline constexpr const char* LATEST_PROTOCOL_VERSION = "2025-11-25";
inline bool supportedProtocolVersion(std::string_view version) {
    return version == LATEST_PROTOCOL_VERSION;
}
using Deadline = std::chrono::steady_clock::time_point;
struct AuthorizationChallenge {
    std::string metadata, scope;
};
class AuthenticationRequired : public std::runtime_error {
public:
    explicit AuthenticationRequired(AuthorizationChallenge challenge = {})
        : std::runtime_error("MCP authentication required or refused"), challenge(std::move(challenge)) {}
    const AuthorizationChallenge challenge;
};

// Only locally generated diagnostics: never wrap a server body, URL or token.
class TransportError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// One caller serializes send/receive/version/close. close is terminal and
// idempotent. Cancellation is cooperative through the supplied checkpoint;
// concurrent close() is NOT part of the interface contract.
class Transport {
public:
    virtual ~Transport() = default;
    virtual void send(const nlohmann::json& message, Deadline deadline, core::CancellationCheck cancelled) = 0;
    virtual nlohmann::json receive(Deadline deadline, core::CancellationCheck cancelled) = 0;
    virtual void setProtocolVersion(const std::string&) {}
    virtual void terminate(Deadline, core::CancellationCheck) { close(); }
    virtual void close() noexcept = 0;
};
}

#endif // SOS_MCP_TRANSPORT_HPP
