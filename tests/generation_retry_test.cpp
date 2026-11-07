#include <agent-runtime/providers/generation_retry.hpp>
#include <agent-runtime/providers/openai_provider.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <future>
#include <vector>

namespace {
using nlohmann::json;

TEST(GenerationRetryTest, ExhaustsExactlyTenRetriesWithTheRequestedCountdown) {
    int calls = 0;
    int seconds = 0;
    std::vector<GenerationRetryStatus> events;
    EXPECT_THROW(generateWithRetry([&]() -> json {
        ++calls;
        throw RetryableGenerationError("private diagnostic");
    }, {}, [&](const auto& status) { events.push_back(status); }, [&](auto) { ++seconds; }),
        GenerationRetriesExhausted);
    EXPECT_EQ(calls, 11);
    EXPECT_EQ(seconds, 107);
    std::size_t index = 0;
    for (int retry = 1; retry <= 10; ++retry) {
        for (int remaining = GENERATION_RETRY_DELAYS[retry - 1]; remaining >= 0; --remaining) {
            ASSERT_LT(index, events.size());
            EXPECT_EQ(events[index].attempt, retry);
            EXPECT_EQ(events[index].seconds, remaining);
            EXPECT_EQ(events[index].error, "private diagnostic");
            EXPECT_EQ(events[index++].reason, "Connection problem");
        }
    }
    EXPECT_EQ(index, events.size());
}

TEST(GenerationRetryTest, RecoversImmediatelyAndClearsTheNotice) {
    int calls = 0;
    std::vector<GenerationRetryStatus> events;
    const auto result = generateWithRetry([&]() -> json {
        if (++calls == 1) throw RetryableGenerationError("HTTP 503", true);
        return {{"ok", true}};
    }, {}, [&](const auto& status) { events.push_back(status); }, [](auto) { FAIL() << "Unexpected wait"; });
    EXPECT_EQ(result, json({{"ok", true}}));
    ASSERT_EQ(events.size(), 2);
    EXPECT_EQ(events[0].attempt, 1);
    EXPECT_EQ(events[0].reason, "Service temporarily unavailable");
    EXPECT_EQ(events[1].attempt, 0);
}

TEST(GenerationRetryTest, PermanentErrorsNeverRetry) {
    int calls = 0;
    EXPECT_THROW(generateWithRetry([&]() -> json {
        ++calls;
        throw std::runtime_error("invalid credentials");
    }, {}, [](auto) { FAIL() << "Permanent error retried"; }), std::runtime_error);
    EXPECT_EQ(calls, 1);
    for (const auto status : {400, 401, 403, 404, 413, 422}) EXPECT_FALSE(retryableHttpStatus(status));
    for (const auto status : {408, 429, 500, 502, 503, 504}) EXPECT_TRUE(retryableHttpStatus(status));
    EXPECT_FALSE(retryableApiError({{"code", "insufficient_quota"}}));
    EXPECT_FALSE(retryableApiError({{"code", "context_length_exceeded"}}));
    EXPECT_TRUE(retryableApiError({{"code", "server_error"}}));
}

TEST(GenerationRetryTest, UnknownApiFailuresRetryButExplicitPermanentErrorsDoNot) {
    for (const auto& failure : std::vector<json>{nullptr, "upstream failure", json::object(),
        {{"message", "private diagnostic"}}, {{"type", "server_error"}}, {{"code", "generation_error"}},
        {{"code", 529}}}) EXPECT_TRUE(retryableApiError(failure));
    for (const auto* code : {"invalid_api_key", "authentication_error", "permission_error", "invalid_request_error",
        "model_not_found", "context_length_exceeded", "max_output_tokens", "content_filter"}) {
        for (const auto* field : {"code", "type", "reason"})
            EXPECT_FALSE(retryableApiError({{field, code}})) << field << ": " << code;
    }
    EXPECT_FALSE(retryableApiError({{"code", "server_error"}, {"type", "invalid_request_error"}}));
    EXPECT_TRUE(retryableHttpStatus(529));
    EXPECT_FALSE(retryableHttpStatus(600));
}

TEST(GenerationRetryTest, RecoversFromUnknownFailuresInJsonAndStreamingResponses) {
    const json success{{"id", "response-test"}, {"status", "completed"}, {"error", nullptr}, {"output", json::array()}};
    for (const auto& failure : std::vector<json>{
        {{"status", "failed"}, {"error", {{"message", "private diagnostic"}}}},
        {{"status", "failed"}, {"error", {{"code", "generation_error"}}}},
        {{"status", "incomplete"}, {"error", nullptr}},
        {{"status", "cancelled"}}}) {
        for (const bool streaming : {false, true}) {
            int calls = 0;
            const auto result = generateWithRetry([&] {
                const auto& response = ++calls == 1 ? failure : success;
                if (streaming) {
                    const auto type = calls == 1 ? "response." + failure.at("status").get<std::string>() : "response.completed";
                    return completedResponsesEvent("data: " + json({{"type", type}, {"response", response}}).dump() + "\n\n");
                }
                checkGenerationResponse(response, "Test");
                return response;
            }, {}, {}, [](auto) { FAIL() << "First retry should be immediate"; });
            EXPECT_EQ(calls, 2);
            EXPECT_EQ(result.at("id"), success.at("id"));
        }
    }
}

TEST(GenerationRetryTest, PermanentJsonAndStreamFailuresNeverRetry) {
    for (const auto* code : {"insufficient_quota", "invalid_api_key", "invalid_request_error",
        "context_length_exceeded", "max_output_tokens", "content_filter"}) {
        for (const bool streaming : {false, true}) {
            int calls = 0;
            const json failure{{"status", "incomplete"}, {"error", nullptr}, {"incomplete_details", {{"reason", code}}}};
            // Quota is reported via error.code/type, not incomplete_details.reason.
            const json response = std::string(code) == "insufficient_quota"
                ? json{{"status", "failed"}, {"error", {{"code", code}}}} : failure;
            EXPECT_THROW(generateWithRetry([&] {
                ++calls;
                if (streaming) return completedResponsesEvent("data: " + json({{"type", "response.incomplete"},
                    {"response", response}}).dump() + "\n\n");
                checkGenerationResponse(response, "Test");
                return response;
            }, {}, [](auto) { FAIL() << "Permanent failure retried"; }), std::runtime_error);
            EXPECT_EQ(calls, 1);
        }
    }
}

TEST(GenerationRetryTest, ExhaustionReportsTheLastFailure) {
    int calls = 0;
    try {
        generateWithRetry([&]() -> json {
            ++calls;
            throw RetryableGenerationError(calls == 11 ? "OpenAI HTTP error 503" : "OpenAI connection interrupted", true);
        }, {}, {}, [](auto) {});
        FAIL() << "Expected exhaustion";
    } catch (const GenerationRetriesExhausted& error) {
        EXPECT_NE(std::string(error.what()).find("Last error: OpenAI HTTP error 503"), std::string::npos);
    }
    EXPECT_EQ(calls, 11);
}

TEST(GenerationRetryTest, SuccessOnTheLastRetryStillRecovers) {
    int calls = 0;
    int lastAttempt = -1;
    const auto result = generateWithRetry([&]() -> json {
        if (++calls <= 10) throw RetryableGenerationError("offline");
        return {{"ok", true}};
    }, {}, [&](const auto& status) { lastAttempt = status.attempt; }, [](auto) {});
    EXPECT_EQ(calls, 11);
    EXPECT_EQ(lastAttempt, 0);
    EXPECT_EQ(result, json({{"ok", true}}));
}

TEST(GenerationRetryTest, QuotaOverridesTransientCodesAndPreservesSafeDetails) {
    EXPECT_FALSE(retryableApiError({{"code", 429}, {"type", "insufficient_quota"}}));
    EXPECT_FALSE(retryableApiError({{"code", "rate_limit_exceeded"}, {"type", "usage_limit_reached"}}));
    EXPECT_TRUE(retryableApiError({{"code", "rate_limit_exceeded"}}));
    EXPECT_EQ(describeApiError({{"code", "invalid_api_key"}, {"message", "private details"}}),
        "API error: invalid_api_key");
    EXPECT_EQ(describeApiError({{"code", "private details\nsecret"}}), "generation failed or was incomplete");
    for (const auto* field : {"code", "type"}) {
        const json event = {{"type", "response.failed"}, {"response", {{"error", {{field, "usage_limit_reached"}}}}}};
        int calls = 0;
        try {
            generateWithRetry([&] {
                ++calls;
                return completedResponsesEvent("data: " + event.dump() + "\n\n");
            }, {}, [](auto) { FAIL() << "Quota error retried"; });
            FAIL() << "Expected quota error";
        } catch (const RetryableGenerationError&) {
            FAIL() << "Quota classified as transient";
        } catch (const std::runtime_error& error) {
            EXPECT_NE(std::string(error.what()).find("quota exhausted"), std::string::npos);
        }
        EXPECT_EQ(calls, 1);
    }
}

TEST(GenerationRetryTest, CancellationTakesPriorityOverATransientFailure) {
    bool cancelled = false;
    EXPECT_THROW(generateWithRetry([&]() -> json {
        cancelled = true;
        throw RetryableGenerationError("offline");
    }, [&] { return cancelled; }, [](const auto&) { FAIL() << "Cancelled request retried"; }), core::Cancelled);
}

TEST(GenerationRetryTest, CancellationInterruptsTheRealBackoff) {
    std::atomic<bool> cancelled = false;
    std::promise<void> waiting;
    auto started = waiting.get_future();
    int calls = 0;
    auto result = std::async(std::launch::async, [&] {
        return generateWithRetry([&]() -> json {
            ++calls;
            throw RetryableGenerationError("offline");
        }, [&] { return cancelled.load(); }, [&](const auto& status) {
            if (status.seconds == 2) waiting.set_value();
        });
    });
    EXPECT_EQ(started.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    cancelled = true;
    EXPECT_EQ(result.wait_for(std::chrono::milliseconds(500)), std::future_status::ready);
    EXPECT_THROW(result.get(), core::Cancelled);
    EXPECT_EQ(calls, 2);
}

TEST(GenerationRetryTest, StreamTruncationIsRetryableButPermanentApiErrorsAreNot) {
    EXPECT_THROW(completedResponsesEvent("data: {\"type\":\"response.created\"}\n\n"), RetryableGenerationError);
    EXPECT_THROW(completedResponsesEvent("data: broken\n\n"), RetryableGenerationError);
    EXPECT_THROW(completedResponsesEvent("data: {\"type\":\"response.failed\",\"response\":{\"error\":{\"code\":\"server_error\"}}}\n\n"), RetryableGenerationError);
    int calls = 0;
    EXPECT_THROW(generateWithRetry([&] {
        ++calls;
        return completedResponsesEvent("data: {\"type\":\"response.failed\",\"response\":{\"error\":{\"code\":\"invalid_api_key\"}}}\n\n");
    }, {}, {}), std::runtime_error);
    EXPECT_EQ(calls, 1);
}
}
