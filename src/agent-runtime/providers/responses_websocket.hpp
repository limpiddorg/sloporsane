#ifndef SOS_RESPONSES_WEBSOCKET_HPP
#define SOS_RESPONSES_WEBSOCKET_HPP

#include "provider_http.hpp"
#include <memory>

// One sequential Responses connection per provider/conversation. Never persists server state.
class ResponsesWebSocket {
public:
    struct Statistics {
        std::size_t connections = 0, requests = 0, continuations = 0, inputItems = 0, requestBytes = 0;
    };
    const Statistics& statistics() const { return counters; }
    ResponsesWebSocket();
    ~ResponsesWebSocket();
    ResponsesWebSocket(const ResponsesWebSocket&) = delete;
    ResponsesWebSocket& operator=(const ResponsesWebSocket&) = delete;
    // An unavailable handshake returns nullopt, before any generation is submitted.
    std::optional<nlohmann::json> generate(const std::string& url, const cpr::Header& headers,
        const nlohmann::json& request, core::CancellationCheck cancelled, ProviderRequestTimeouts timeouts);
    void reset();
private:
    struct State;
    std::unique_ptr<State> state;
    Statistics counters;
};

#endif // SOS_RESPONSES_WEBSOCKET_HPP
