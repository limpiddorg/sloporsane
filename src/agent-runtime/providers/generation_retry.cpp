#include "generation_retry.hpp"
#include <chrono>
#include <thread>
#include <algorithm>

bool retryableHttpStatus(long status) {
    return status == 408 || status == 429 || (status >= 500 && status < 600);
}

bool retryableTransportError(cpr::ErrorCode code) {
    switch (code) {
        case cpr::ErrorCode::COULDNT_CONNECT:
        case cpr::ErrorCode::COULDNT_RESOLVE_HOST:
        case cpr::ErrorCode::COULDNT_RESOLVE_PROXY:
        case cpr::ErrorCode::RECV_ERROR:
        case cpr::ErrorCode::SEND_ERROR:
        case cpr::ErrorCode::HTTP2:
        case cpr::ErrorCode::HTTP2_STREAM:
        case cpr::ErrorCode::GOT_NOTHING:
        case cpr::ErrorCode::PARTIAL_FILE:
        case cpr::ErrorCode::OPERATION_TIMEDOUT:
            return true;
        default:
            return false;
    }
}

bool retryableApiError(const nlohmann::json& error) {
    if (quotaApiError(error)) return false;
    // Retry unknown failures; classify terminal errors by structured codes, never raw text.
    if (!error.is_object()) return true;
    constexpr const char* PERMANENT_CODES[] = {
        "invalid_api_key", "authentication_error", "unauthorized", "permission_denied",
        "permission_error", "forbidden", "access_denied", "invalid_request_error",
        "invalid_request", "invalid_argument", "bad_request", "model_not_found",
        "unsupported_model", "not_found", "context_length_exceeded", "context_window_exceeded",
        "max_output_tokens", "max_tokens", "length", "content_filter", "content_policy_violation",
        "safety", "unsupported_parameter", "invalid_parameter", "missing_required_parameter"
    };
    for (const auto* field : {"code", "type", "reason"}) {
        const auto value = error.find(field);
        if (value == error.end()) continue;
        if (value->is_number_integer() && value->get<long>() >= 400 &&
            !retryableHttpStatus(value->get<long>())) return false;
        for (const auto* code : PERMANENT_CODES)
            if (*value == code) return false;
    }
    return true;
}

bool quotaApiError(const nlohmann::json& error) {
    if (!error.is_object()) return false;
    for (const auto* field : {"code", "type"}) {
        const auto value = error.find(field);
        if (value != error.end() && (*value == "insufficient_quota" || *value == "usage_limit_reached" ||
            *value == "quota_exceeded" || *value == "billing_hard_limit_reached" || *value == "credits_exhausted"))
            return true;
    }
    return false;
}

std::string describeApiError(const nlohmann::json& error) {
    if (quotaApiError(error)) return "quota exhausted. Check your usage limits or billing before trying again.";
    // Keep structured identifiers, never arbitrary upstream messages or bodies.
    if (error.is_object()) {
        for (const auto* field : {"code", "type", "reason"}) {
            const auto value = error.find(field);
            if (value == error.end()) continue;
            if (value->is_number_integer()) return "API error " + value->dump();
            if (!value->is_string()) continue;
            const auto& code = value->get_ref<const std::string&>();
            if (!code.empty() && code.size() <= 80 && std::all_of(code.begin(), code.end(), [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
            })) return "API error: " + code;
        }
    }
    return "generation failed or was incomplete";
}

void checkGenerationResponse(const nlohmann::json& response, const std::string& provider) {
    if (!response.is_object()) throw RetryableGenerationError(provider + " returned an invalid response");
    const auto error = response.find("error");
    const auto status = response.find("status");
    const bool failed = status != response.end() &&
        (*status == "failed" || *status == "incomplete" || *status == "cancelled");
    if (!failed && (error == response.end() || error->is_null())) return;
    const auto details = response.find("incomplete_details");
    const auto& failure = error != response.end() && !error->is_null()
        ? *error : details != response.end() ? *details : response;
    const auto message = provider + " " + describeApiError(failure);
    if (retryableApiError(failure)) throw RetryableGenerationError(message, true);
    throw std::runtime_error(message);
}

namespace {
void waitOneSecond(core::CancellationCheck cancelled) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline) {
        core::checkCancellation(cancelled);
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    core::checkCancellation(cancelled);
}
}

void runWithRetry(const std::function<void()>& operation,
    core::CancellationCheck cancelled, const RetryNotification& notify, RetryWait wait) {
    if (!wait) wait = waitOneSecond;
    int retries = 0;
    for (;;) {
        core::checkCancellation(cancelled);
        try {
            operation();
            // Record completed responses before observing a racing cancellation.
            if (retries && notify) notify({});
            return;
        } catch (const RetryableGenerationError& error) {
            core::checkCancellation(cancelled);
            if (retries == 10) throw GenerationRetriesExhausted(error.what());
            const auto seconds = GENERATION_RETRY_DELAYS[retries++];
            const std::string reason = error.service ? "Service temporarily unavailable" : "Connection problem";
            for (int remaining = seconds; remaining > 0; --remaining) {
                if (notify) notify({retries, remaining, reason, error.what()});
                wait(cancelled);
                core::checkCancellation(cancelled);
            }
            if (notify) notify({retries, 0, reason, error.what()});
        }
    }
}

nlohmann::json generateWithRetry(const std::function<nlohmann::json()>& generate,
    core::CancellationCheck cancelled, const RetryNotification& notify, RetryWait wait) {
    nlohmann::json response;
    runWithRetry([&] { response = generate(); }, std::move(cancelled), notify, std::move(wait));
    return response;
}
