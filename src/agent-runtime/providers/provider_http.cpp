#include "provider_http.hpp"
#include "generation_retry.hpp"
#include <cpr/cpr.h>

std::string formatProviderTimeout(std::chrono::milliseconds duration) {
    auto value = duration.count();
    std::string unit = "millisecond";
    if (value >= 60000 && value % 60000 == 0) {
        value /= 60000;
        unit = "minute";
    } else if (value >= 1000 && value % 1000 == 0) {
        value /= 1000;
        unit = "second";
    }
    return std::to_string(value) + " " + unit + (value == 1 ? "" : "s");
}

void ProviderRequestTimeouts::validate() const {
    if (total.count() <= 0 || idle.count() <= 0)
        throw std::invalid_argument("Provider request timeouts must be positive");
}

std::string postProviderResponse(const std::string& url, const std::string& bearer, const cpr::Header& headers,
    const nlohmann::json& request, core::CancellationCheck cancelled, const std::string& provider,
    ProviderRequestTimeouts timeouts) {
    timeouts.validate();
    core::checkCancellation(cancelled);
    constexpr auto CONNECT_TIMEOUT = std::chrono::seconds(15);
    constexpr std::size_t MAX_RESPONSE_BYTES = 64 * 1024 * 1024;
    std::string output;
    auto lastReceived = std::chrono::steady_clock::now();
    bool monitorIdle = request.value("stream", false);
    bool idleTimeout = false;
    bool oversized = false;
    const auto post = [&](auto... credentials) {
        return cpr::Post(cpr::Url{url}, credentials..., headers, cpr::Body{request.dump()},
        cpr::ConnectTimeout{CONNECT_TIMEOUT}, cpr::Timeout{timeouts.total}, cpr::Redirect{false},
        cpr::ProgressCallback{[&](auto, auto, auto, auto, intptr_t) {
            if (cancelled && cancelled()) return false;
            idleTimeout = monitorIdle && std::chrono::steady_clock::now() - lastReceived >= timeouts.idle;
            return !idleTimeout;
        }},
        cpr::WriteCallback{[&](std::string_view bytes, intptr_t) {
            if (!bytes.empty()) {
                lastReceived = std::chrono::steady_clock::now();
                monitorIdle = true;
            }
            if (output.size() + bytes.size() > MAX_RESPONSE_BYTES) { oversized = true; return false; }
            output.append(bytes);
            return true;
        }});
    };
    const auto response = bearer.empty() ? post() : post(cpr::Bearer{bearer});
    core::checkCancellation(cancelled);
    if (oversized) throw std::runtime_error(provider + " response exceeds the size limit");
    if (idleTimeout)
        throw RetryableGenerationError(provider + " inactivity timeout: no response data received for " +
            formatProviderTimeout(timeouts.idle));
    if (response.error.code == cpr::ErrorCode::OPERATION_TIMEDOUT)
        throw RetryableGenerationError(provider + " request timeout (total limit: " + formatProviderTimeout(timeouts.total) +
            "; connection limit: " + formatProviderTimeout(CONNECT_TIMEOUT) + ")");
    // Never expose raw diagnostics, headers or bodies that may contain private data.
    if (retryableTransportError(response.error.code))
        throw RetryableGenerationError(provider + " connection interrupted");
    if (response.error.code != cpr::ErrorCode::OK)
        throw std::runtime_error(provider + " transport error (" + std::to_string(response.error.code) + ")");
    const auto body = nlohmann::json::parse(output, nullptr, false);
    if (body.is_object() && body.contains("error") && !body.at("error").is_null() &&
        !retryableApiError(body.at("error")))
        throw std::runtime_error(provider + " " + describeApiError(body.at("error")));
    if (retryableHttpStatus(response.status_code))
        throw RetryableGenerationError(provider + " HTTP error " + std::to_string(response.status_code), true);
    if (response.status_code < 200 || response.status_code >= 300) {
        const auto detail = body.is_object() && body.contains("error")
            ? ": " + describeApiError(body.at("error")) : "";
        throw std::runtime_error(provider + " HTTP error " + std::to_string(response.status_code) + detail);
    }
    return output;
}

nlohmann::json getProviderJson(const std::string& url, const ProviderAuthorization& auth,
    core::CancellationCheck cancelled, const std::string& operation) {
    core::checkCancellation(cancelled);
    cpr::Header headers{{"Accept", "application/json"}, {"User-Agent", "sos"}};
    if (auth.codex) {
        headers["originator"] = "sos";
        if (!auth.accountId.empty()) headers["ChatGPT-Account-Id"] = auth.accountId;
    }
    std::string output;
    const auto request = [&](auto... credentials) {
        return cpr::Get(cpr::Url{url}, credentials..., headers,
            cpr::ConnectTimeout{5000}, cpr::Timeout{15000}, cpr::Redirect{false},
            cpr::ProgressCallback{[cancelled](auto, auto, auto, auto, intptr_t) { return !cancelled || !cancelled(); }},
            cpr::WriteCallback{[&output](std::string_view bytes, intptr_t) {
                if (output.size() + bytes.size() > 16 * 1024 * 1024) return false;
                output.append(bytes);
                return true;
            }});
    };
    const auto response = auth.bearer.empty() ? request() : request(cpr::Bearer{auth.bearer});
    core::checkCancellation(cancelled);
    if (retryableTransportError(response.error.code))
        throw RetryableGenerationError(operation + " failed: network error or timeout");
    if (response.error.code != cpr::ErrorCode::OK) throw std::runtime_error(operation + " failed: transport error");
    const auto result = nlohmann::json::parse(output, nullptr, false);
    if (result.is_object() && result.contains("error") && quotaApiError(result.at("error")))
        throw std::runtime_error(operation + " " + describeApiError(result.at("error")));
    if (retryableHttpStatus(response.status_code))
        throw RetryableGenerationError(operation + " HTTP error " + std::to_string(response.status_code), true);
    if (response.status_code < 200 || response.status_code >= 300)
        throw std::runtime_error(operation + " HTTP error " + std::to_string(response.status_code));
    if (!result.is_object()) throw std::runtime_error(operation + " returned invalid JSON");
    return result;
}
