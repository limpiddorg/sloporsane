#ifndef SOS_MCP_HTTP_TRANSPORT_HPP
#define SOS_MCP_HTTP_TRANSPORT_HPP

#include "transport.hpp"
#include "config.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <thread>

namespace mcp {
// Streamable HTTP only. A POST response may stream server requests while the
// original request is pending; the consumer can answer via another POST.
// No redirect following, legacy SSE endpoint, automatic reconnect or replay.
class HttpTransport final : public Transport {
public:
    using Authorize = std::function<std::string(core::CancellationCheck)>;
    explicit HttpTransport(Remote remote, std::string bearer = {}, Authorize authorize = {});
    ~HttpTransport() override;
    void send(const nlohmann::json& message, Deadline deadline, core::CancellationCheck cancelled) override;
    nlohmann::json receive(Deadline deadline, core::CancellationCheck cancelled) override;
    void setProtocolVersion(const std::string& version) override;
    void close() noexcept override;
    void terminate(Deadline deadline, core::CancellationCheck cancelled) override;
private:
    void post(const nlohmann::json& message, Deadline deadline, core::CancellationCheck cancelled, bool terminating = false);
    void publish(nlohmann::json message);
    Remote remote;
    std::string bearer;
    Authorize authorize;
    std::string sessionId;
    std::string protocolVersion;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<nlohmann::json> messages;
    std::exception_ptr failure;
    std::atomic<bool> closed{false};
    std::thread worker;
    bool receiving = false;
};
}

#endif // SOS_MCP_HTTP_TRANSPORT_HPP
