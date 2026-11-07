#ifndef SOS_GENERATION_RETRY_HPP
#define SOS_GENERATION_RETRY_HPP

#include "../core/cancellation.hpp"
#include <nlohmann/json.hpp>
#include <cpr/error.h>
#include <functional>
#include <stdexcept>
#include <string>

class RetryableGenerationError : public std::runtime_error {
public:
    explicit RetryableGenerationError(const std::string& detail, bool service = false)
        : std::runtime_error(detail), service(service) {}
    bool service;
};

class GenerationRetriesExhausted : public std::runtime_error {
public:
    explicit GenerationRetriesExhausted(const std::string& detail = {}) : std::runtime_error(
        "Generation failed after 10 retries. Your message was kept. Please try again manually." +
        (detail.empty() ? std::string{} : "\nLast error: " + detail)) {}
};

struct GenerationRetryStatus {
    int attempt = 0; // Zero clears the temporary notice after recovery.
    int seconds = 0;
    std::string reason;
    std::string error; // Safe provider diagnostic, not a raw response body.
};

inline constexpr int GENERATION_RETRY_DELAYS[] = {0, 2, 5, 10, 15, 15, 15, 15, 15, 15};
bool retryableHttpStatus(long status);
bool retryableTransportError(cpr::ErrorCode code);
// Generation failures retry by default, except explicit quota/auth/input errors.
bool retryableApiError(const nlohmann::json& error);
bool quotaApiError(const nlohmann::json& error);
std::string describeApiError(const nlohmann::json& error);
void checkGenerationResponse(const nlohmann::json& response, const std::string& provider);
using RetryNotification = std::function<void(const GenerationRetryStatus&)>;
using RetryWait = std::function<void(core::CancellationCheck)>;
void runWithRetry(const std::function<void()>& operation,
    core::CancellationCheck cancelled, const RetryNotification& notify, RetryWait wait = {});
nlohmann::json generateWithRetry(const std::function<nlohmann::json()>& generate,
    core::CancellationCheck cancelled, const RetryNotification& notify, RetryWait wait = {});

#endif // SOS_GENERATION_RETRY_HPP
