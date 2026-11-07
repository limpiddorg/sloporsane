#include "responses_websocket.hpp"
#include "responses_stream_assembler.hpp"
#include "generation_retry.hpp"
#include "../conversation/transformers/openai/openai_responses_api_message_provider_transformer.hpp"
#include <ixwebsocket/IXWebSocket.h>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>

namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
constexpr std::size_t MAX_RESPONSE_BYTES = 64 * 1024 * 1024;
constexpr auto MAX_CONNECTION_AGE = std::chrono::minutes(55);
constexpr auto MAX_IDLE = std::chrono::minutes(5);
}

struct ResponsesWebSocket::State {
    // Socket callbacks only touch the synchronized inbox, never conversation state.
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::string> inbox;
    bool opened = false, closed = false, oversized = false;
    std::size_t received = 0;
    Clock::time_point lastReceived, connectedAt, lastUsed;
    std::unique_ptr<ix::WebSocket> socket;
    std::string url;
    cpr::Header headers;
    Json previousFields, expectedInput;
    std::string responseId;

    ~State() { if (socket) socket->stop(); }

    void wait(std::unique_lock<std::mutex>& lock, Clock::time_point deadline,
        const core::CancellationCheck& cancelled) {
        core::checkCancellation(cancelled);
        if (Clock::now() >= deadline) throw RetryableGenerationError("OpenAI WebSocket request timeout");
        changed.wait_for(lock, std::chrono::milliseconds(25));
    }

    bool connect(const std::string& endpoint, const cpr::Header& requestHeaders,
        const core::CancellationCheck& cancelled, std::chrono::milliseconds budget) {
        url = endpoint;
        headers = requestHeaders;
        socket = std::make_unique<ix::WebSocket>();
        auto wsUrl = endpoint;
        if (wsUrl.starts_with("https://")) wsUrl.replace(0, 5, "wss");
        else if (wsUrl.starts_with("http://")) wsUrl.replace(0, 4, "ws");
        socket->setUrl(wsUrl);
        ix::WebSocketHttpHeaders extra;
        for (const auto& [key, value] : headers) extra[key] = value;
        extra["OpenAI-Beta"] = "responses_websockets=2026-02-06";
        socket->setExtraHeaders(extra);
        socket->disableAutomaticReconnection();
        socket->disablePerMessageDeflate();
        socket->setHandshakeTimeout(15);
        socket->setOnMessageCallback([this](const ix::WebSocketMessagePtr& message) {
            std::lock_guard lock(mutex);
            if (message->type == ix::WebSocketMessageType::Open) opened = true;
            else if (message->type == ix::WebSocketMessageType::Close || message->type == ix::WebSocketMessageType::Error)
                closed = true;
            else if (message->type == ix::WebSocketMessageType::Message) {
                lastReceived = Clock::now();
                received += message->str.size();
                if (received > MAX_RESPONSE_BYTES || message->binary) oversized = true;
                else inbox.push_back(message->str);
            }
            changed.notify_all();
        });
        socket->start();
        const auto deadline = Clock::now() + std::min(budget, std::chrono::milliseconds(15000));
        std::unique_lock lock(mutex);
        while (!opened && !closed) {
            if (Clock::now() >= deadline) return false;
            wait(lock, deadline, cancelled);
        }
        connectedAt = lastUsed = Clock::now();
        return opened && !closed;
    }

    Json exchange(const Json& payload, const core::CancellationCheck& cancelled, ProviderRequestTimeouts timeouts) {
        const auto deadline = Clock::now() + timeouts.total;
        {
            std::lock_guard lock(mutex);
            received = 0;
            lastReceived = Clock::now();
        }
        core::checkCancellation(cancelled);
        const auto sent = socket->sendText(payload.dump(), [&](int, int) {
            return (!cancelled || !cancelled()) && Clock::now() < deadline;
        });
        core::checkCancellation(cancelled);
        if (!sent.success)
            throw RetryableGenerationError("OpenAI WebSocket send interrupted");
        ResponsesStreamAssembler assembler;
        for (;;) {
            core::checkCancellation(cancelled);
            if (Clock::now() >= deadline) throw RetryableGenerationError("OpenAI WebSocket request timeout");
            std::string data;
            {
                std::unique_lock lock(mutex);
                while (inbox.empty() && !closed && !oversized) {
                    if (Clock::now() - lastReceived >= timeouts.idle)
                        throw RetryableGenerationError("OpenAI WebSocket inactivity timeout");
                    wait(lock, deadline, cancelled);
                }
                core::checkCancellation(cancelled);
                if (oversized) throw std::runtime_error("OpenAI WebSocket response exceeds the size limit or is binary");
                if (inbox.empty()) throw RetryableGenerationError("OpenAI WebSocket connection interrupted");
                data = std::move(inbox.front());
                inbox.pop_front();
            }
            const auto event = Json::parse(data, nullptr, false);
            // A rejected continuation has no output to replay. Retry once with full input.
            if (event.is_object() && event.value("type", "") == "error" &&
                event.contains("error") && event["error"].is_object() &&
                event["error"].value("code", "") == "previous_response_not_found") return {};
            assembler.consume(event);
            if (event.value("type", "") == "response.completed") return assembler.finish();
        }
    }
};

ResponsesWebSocket::ResponsesWebSocket() = default;
ResponsesWebSocket::~ResponsesWebSocket() = default;
void ResponsesWebSocket::reset() { state.reset(); }

std::optional<Json> ResponsesWebSocket::generate(const std::string& url, const cpr::Header& headers,
    const Json& request, core::CancellationCheck cancelled, ProviderRequestTimeouts timeouts) {
    timeouts.validate();
    const auto deadline = Clock::now() + timeouts.total;
    try {
        bool reconnect = !state;
        if (state) {
            std::lock_guard lock(state->mutex);
            reconnect = state->closed || Clock::now() - state->connectedAt >= MAX_CONNECTION_AGE ||
                Clock::now() - state->lastUsed >= MAX_IDLE || state->url != url || state->headers != headers;
        }
        if (reconnect) {
            reset();
            state = std::make_unique<State>();
            if (!state->connect(url, headers, cancelled, timeouts.total)) { reset(); return std::nullopt; }
            ++counters.connections;
        }
        auto fields = request;
        fields.erase("input");
        fields.erase("stream");
        auto payload = fields;
        payload["type"] = "response.create";
        const auto& input = request.at("input");
        const bool continuation = !state->responseId.empty() && fields == state->previousFields &&
            input.size() >= state->expectedInput.size() &&
            std::equal(state->expectedInput.begin(), state->expectedInput.end(), input.begin());
        payload["input"] = input;
        if (continuation) {
            payload["previous_response_id"] = state->responseId;
            payload["input"] = Json::array();
            for (std::size_t i = state->expectedInput.size(); i < input.size(); ++i) payload["input"].push_back(input[i]);
        }
        const auto exchange = [&] {
            auto remaining = timeouts;
            remaining.total = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
            if (remaining.total.count() <= 0) throw RetryableGenerationError("OpenAI WebSocket request timeout");
            ++counters.requests;
            counters.continuations += payload.contains("previous_response_id");
            counters.inputItems += payload.at("input").size();
            counters.requestBytes += payload.dump().size();
            return state->exchange(payload, cancelled, remaining);
        };
        auto response = exchange();
        if (response.is_null() && continuation) {
            payload.erase("previous_response_id");
            payload["input"] = input;
            response = exchange();
        }
        if (response.is_null()) throw RetryableGenerationError("OpenAI WebSocket continuation rejected");
        checkGenerationResponse(response, "OpenAI");
        state->previousFields = fields;
        state->expectedInput = input;
        // Compare against the exact representation the runtime will send next, not
        // response-only fields (status, usage, etc.) removed by message conversion.
        OpenAiResponsesApiMessageProviderTransformer transformer;
        for (const auto& item : transformer.toProviderFormat(transformer.fromProviderFormat(response)))
            state->expectedInput.push_back(item);
        state->responseId = response.value("id", "");
        state->lastUsed = Clock::now();
        return response;
    } catch (...) {
        // Never silently fall back after a submitted generation or reuse partial output.
        reset();
        throw;
    }
}
