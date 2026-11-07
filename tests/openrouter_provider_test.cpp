#include <agent-runtime/providers/openrouter_provider.hpp>
#include <agent-runtime/providers/generation_retry.hpp>
#include <agent-runtime/tools/fs/read.hpp>

#include <gtest/gtest.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <future>
#include <atomic>
#include <chrono>
#include <stdexcept>

namespace {
using nlohmann::json;

// One HTTP exchange on loopback, using only fake credentials.
class LocalServer {
public:
    explicit LocalServer(std::string responseBody, int status = 200, bool stallFirst = false) {
        listener = socket(AF_INET, SOCK_STREAM, 0);
        if (listener < 0) throw std::runtime_error("socket failed");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t size = sizeof(address);
        if (bind(listener, reinterpret_cast<sockaddr*>(&address), size) != 0 || listen(listener, 1) != 0 ||
            getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
            close(listener);
            throw std::runtime_error("Could not bind local test server");
        }
        endpoint = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "/api/v1/responses";
        request = std::async(std::launch::async, [this, responseBody, status, stallFirst] {
          for (int exchange = 0;; ++exchange) {
            const int client = accept(listener, nullptr, nullptr);
            if (client < 0) return std::string{};
            struct Connection {
                int fd;
                ~Connection() { close(fd); }
            } connection{client};
            timeval timeout{5, 0};
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            std::string received;
            char buffer[4096];
            for (;;) {
                const auto count = recv(client, buffer, sizeof(buffer), 0);
                if (count <= 0) throw std::runtime_error("Incomplete HTTP request");
                received.append(buffer, static_cast<std::size_t>(count));
                const auto end = received.find("\r\n\r\n");
                if (end == std::string::npos) continue;
                auto length = received.find("Content-Length: ");
                if (length == std::string::npos && received.starts_with("GET ")) break;
                if (length == std::string::npos) throw std::runtime_error("Missing Content-Length");
                const auto bytes = std::stoul(received.substr(length + 16));
                if (received.size() >= end + 4 + bytes) break;
            }
            if (exchange == 0) entered.set_value();
            if (stallFirst && exchange == 0) {
                // Do not answer: cancellation must close this socket before the receive timeout.
                char ignored;
                recv(client, &ignored, 1, 0);
                continue;
            }
            const auto reply = "HTTP/1.1 " + std::to_string(status) + " Test\r\nContent-Type: application/json\r\n"
                "Connection: close\r\nContent-Length: " + std::to_string(responseBody.size()) + "\r\n\r\n" + responseBody;
            std::size_t offset = 0;
            while (offset < reply.size()) {
                const auto sent = send(client, reply.data() + offset, reply.size() - offset, 0);
                if (sent <= 0) throw std::runtime_error("HTTP test response failed");
                offset += static_cast<std::size_t>(sent);
            }
            return received;
          }
        });
    }
    ~LocalServer() {
        shutdown(listener, SHUT_RDWR);
        close(listener);
        if (request.valid()) request.wait();
    }
    std::string endpoint;
    std::future<std::string> request;
    std::promise<void> entered;
private:
    int listener;
};

TEST(OpenRouterProviderTest, ImageCapabilitiesAreCachedAndMissingDeclarationsStayUnknown) {
    LocalServer server(R"({"data":[
        {"id":"vision","architecture":{"input_modalities":["text","image"]}},
        {"id":"text","architecture":{"input_modalities":["text"]}},
        {"id":"missing"},
        {"id":"malformed","architecture":{"input_modalities":["image",42]}}
    ]})");
    OpenRouterProvider provider("fake-key", server.endpoint, server.endpoint);
    EXPECT_EQ(provider.getModelInfo("vision").imageInput, ImageSupport::SUPPORTED);
    EXPECT_EQ(provider.getModelInfo("text").imageInput, ImageSupport::UNSUPPORTED);
    EXPECT_EQ(provider.getModelInfo("missing").imageInput, ImageSupport::UNKNOWN);
    EXPECT_EQ(provider.getModelInfo("malformed").imageInput, ImageSupport::UNKNOWN);
    EXPECT_EQ(provider.getModelInfo("absent").imageInput, ImageSupport::UNKNOWN);
    EXPECT_TRUE(server.request.get().starts_with("GET "));
}

TEST(OpenRouterProviderTest, SendsResponsesRequestWithBearerAuthentication) {
    LocalServer server(R"({"id":"resp_1","status":"completed","output":[]})");
    OpenRouterProvider provider("fake-key", server.endpoint);
    const json input = json::array({{{"role", "user"}, {"content", "Hello"}}});
    const auto definition = ReadTool{}.getDefinition();
    const auto response = provider.generate(input, "openai/gpt-5.6-luna", {definition});
    EXPECT_EQ(provider.getFormat(), ProviderFormat::OPENAI_RESPONSES);
    EXPECT_EQ(response.at("id"), "resp_1");
    const auto request = server.request.get();
    EXPECT_EQ(request.find("POST /api/v1/responses HTTP/1.1\r\n"), 0);
    EXPECT_NE(request.find("Authorization: Bearer fake-key\r\n"), std::string::npos);
    const auto body = json::parse(request.substr(request.find("\r\n\r\n") + 4));
    EXPECT_EQ(body.at("model"), "openai/gpt-5.6-luna");
    EXPECT_EQ(body.at("input"), input);
    EXPECT_EQ(body.at("store"), false);
    EXPECT_EQ(body.at("stream"), false);
    EXPECT_EQ(body.at("reasoning").at("effort"), "medium");
    EXPECT_EQ(body.at("reasoning").at("summary"), "auto");
    EXPECT_EQ(body.at("include"), json::array({"reasoning.encrypted_content"}));
    EXPECT_EQ(body.at("tool_choice"), "auto");
    ASSERT_EQ(body.at("tools").size(), 1);
    EXPECT_EQ(body["tools"][0]["name"], "read");
    EXPECT_EQ(body["tools"][0]["type"], "function");
    EXPECT_EQ(body["tools"][0]["parameters"], definition.parameters);
    EXPECT_EQ(body["tools"][0]["strict"], false);
    EXPECT_EQ(body.dump().find("fake-key"), std::string::npos);
}

TEST(OpenRouterProviderTest, DiscoversReasoningEffortsAndSharesTheCachedCatalogWithContext) {
    LocalServer server(R"({"data":[
        {"id":"subset","context_length":8192,"reasoning":{"supported_efforts":["xhigh","high","low","none","minimal"]}},
        {"id":"all","reasoning":{"supported_efforts":null}},
        {"id":"missing","reasoning":{}},
        {"id":"plain"},
        {"id":"empty","reasoning":{"supported_efforts":[]}},
        {"id":"high-only","reasoning":{"supported_efforts":["high"]}}
    ]})");
    OpenRouterProvider provider("fake-key", server.endpoint, server.endpoint);
    EXPECT_EQ(provider.getReasoningEfforts("subset"), (std::vector<std::string>{"none", "low", "high", "xhigh"}));
    EXPECT_EQ(provider.getContextLimit("subset"), 8192);
    EXPECT_EQ(provider.getReasoningEfforts("all"), (std::vector<std::string>{"low", "medium", "high", "xhigh", "max"}));
    EXPECT_TRUE(provider.getReasoningEfforts("missing").empty());
    EXPECT_TRUE(provider.getReasoningEfforts("plain").empty());
    EXPECT_TRUE(provider.getReasoningEfforts("empty").empty());
    EXPECT_TRUE(provider.getReasoningEfforts("unknown").empty());
    EXPECT_EQ(provider.getReasoningEfforts("high-only"), (std::vector<std::string>{"high"}));
    EXPECT_TRUE(server.request.get().starts_with("GET "));
}

TEST(OpenRouterProviderTest, ReportsHttpErrorsWithoutResponseBody) {
    LocalServer server("private-body", 401);
    OpenRouterProvider provider("fake-key", server.endpoint);
    try {
        provider.generate(json::array(), "test-model");
        FAIL() << "Expected HTTP error";
    } catch (const std::runtime_error& error) {
        EXPECT_EQ(std::string(error.what()), "OpenRouter HTTP error 401");
    }
}

TEST(OpenRouterProviderTest, RejectsMalformedAndApiErrorResponses) {
    for (const auto& body : {"not-json", "[]", "{\"error\":{\"message\":\"private\"}}"}) {
        LocalServer server(body);
        OpenRouterProvider provider("fake-key", server.endpoint);
        EXPECT_THROW(provider.generate(json::array(), "test-model"), std::runtime_error);
    }
}

TEST(OpenRouterProviderTest, RetriesUnknownGenerationFailuresAndAdditionalServerStatuses) {
    for (const auto status : {200, 529}) {
        LocalServer server(R"({"error":{"message":"private diagnostic"}})", status);
        OpenRouterProvider provider("fake-key", server.endpoint);
        EXPECT_THROW(provider.generate(json::array(), "test-model"), RetryableGenerationError);
    }
    LocalServer server(R"({"status":"incomplete","error":null})");
    OpenRouterProvider provider("fake-key", server.endpoint);
    EXPECT_THROW(provider.generate(json::array(), "test-model"), RetryableGenerationError);
}

TEST(OpenRouterProviderTest, ExplicitPermanentErrorsOverrideHttpRetryStatuses) {
    for (const auto status : {200, 429, 503}) {
        for (const auto* code : {"insufficient_quota", "invalid_api_key", "context_length_exceeded"}) {
            LocalServer server(json({{"error", {{"code", code}}}}).dump(), status);
            OpenRouterProvider provider("fake-key", server.endpoint);
            int calls = 0;
            EXPECT_THROW(generateWithRetry([&] {
                ++calls;
                return provider.generate(json::array(), "test-model");
            }, {}, [](auto) { FAIL() << "Permanent HTTP error retried"; }), std::runtime_error);
            EXPECT_EQ(calls, 1);
        }
    }
}

TEST(OpenRouterProviderTest, ValidatesConfigurationBeforeSending) {
    EXPECT_THROW(OpenRouterProvider{""}, std::invalid_argument);
    EXPECT_THROW(OpenRouterProvider{"key\r\nheader"}, std::invalid_argument);
    OpenRouterProvider provider("fake-key");
    EXPECT_THROW(provider.generate(json::object(), "model"), std::invalid_argument);
    EXPECT_THROW(provider.generate(json::array(), ""), std::invalid_argument);
}

TEST(OpenRouterProviderTest, ReadsAndCachesModelContextLimits) {
    LocalServer server(R"({"data":[{"id":"test-model","context_length":128000,"top_provider":{"max_completion_tokens":4096}},
        {"id":"other-model","context_length":32000},{"id":"unknown","context_length":null}]})");
    OpenRouterProvider provider("fake-key", server.endpoint, server.endpoint);
    EXPECT_EQ(provider.getContextLimit("test-model"), 128000);
    const auto model = provider.getModelInfo("test-model");
    EXPECT_EQ(model.id, "test-model");
    EXPECT_EQ(model.contextLimit, 128000);
    EXPECT_EQ(model.outputLimit, 4096);
    EXPECT_FALSE(model.inputLimit);
    EXPECT_FALSE(provider.getModelInfo("missing").outputLimit);
    EXPECT_EQ(provider.getContextLimit("other-model"), 32000);
    EXPECT_FALSE(provider.getContextLimit("unknown"));
    EXPECT_FALSE(provider.getContextLimit("missing"));
    EXPECT_EQ(server.request.get().find("GET /api/v1/responses HTTP/1.1\r\n"), 0);
}

TEST(OpenRouterProviderTest, InvalidCatalogFailsButMissingLimitsAndUsageRemainUnknown) {
    for (const auto& body : {"not-json", "{}", R"({"data":[{"id":"model","context_length":-5}]})"}) {
        LocalServer server(body);
        OpenRouterProvider provider("fake-key", server.endpoint, server.endpoint);
        if (std::string(body).find("data") == std::string::npos)
            EXPECT_THROW(provider.getContextLimit("model"), std::runtime_error);
        else EXPECT_FALSE(provider.getContextLimit("model"));
    }
    OpenRouterProvider provider("fake-key");
    const auto response = json::parse(R"({"usage":{"input_tokens":100,"output_tokens":25,
        "input_tokens_details":{"cached_tokens":40},"output_tokens_details":{"reasoning_tokens":10}}})");
    EXPECT_EQ(provider.getUsedContext(response), 125);
    EXPECT_FALSE(provider.getUsedContext(json::object()));
    EXPECT_FALSE(provider.getUsedContext({{"usage", nullptr}}));
    EXPECT_FALSE(provider.getUsedContext({{"usage", {{"input_tokens", 100}}}}));
    EXPECT_FALSE(provider.getUsedContext({{"usage", {{"input_tokens", -1}, {"output_tokens", 25}}}}));
    EXPECT_FALSE(provider.getUsedContext({{"usage", {{"input_tokens", "100"}, {"output_tokens", 25}}}}));
    EXPECT_EQ(provider.getUsedContext({{"usage", {{"input_tokens", 0}, {"output_tokens", 0}}}}), 0);
}

TEST(OpenRouterProviderTest, ForwardsSelectedReasoningEffort) {
    LocalServer server(R"({"id":"resp_1","status":"completed","output":[]})");
    OpenRouterProvider provider("fake-key", server.endpoint);
    provider.generate(json::array(), "model", {}, "high");
    const auto request = server.request.get();
    const auto body = json::parse(request.substr(request.find("\r\n\r\n") + 4));
    EXPECT_EQ(body["reasoning"]["effort"], "high");
}

TEST(OpenRouterProviderTest, NoReasoningDisablesEffortWithoutRequestingReasoningContent) {
    LocalServer server(R"({"id":"resp_1","status":"completed","output":[]})");
    OpenRouterProvider provider("fake-key", server.endpoint);
    provider.generate(json::array(), "model", {}, "none");
    const auto request = server.request.get();
    const auto body = json::parse(request.substr(request.find("\r\n\r\n") + 4));
    EXPECT_EQ(body["reasoning"]["effort"], "none");
    EXPECT_FALSE(body["reasoning"].contains("summary"));
    EXPECT_FALSE(body.contains("include"));
}

TEST(OpenRouterProviderTest, CancelsInFlightGenerationAndCatalogThenReusesConnectionClient) {
    for (const bool catalog : {false, true}) {
        LocalServer server(R"({"id":"resp","status":"completed","output":[],"data":[{"id":"model","context_length":8192}]})", 200, true);
        OpenRouterProvider provider("fake-key", server.endpoint, server.endpoint);
        std::atomic<bool> cancelled = false;
        provider.setCancellationCheck([&] { return cancelled.load(); });
        auto entered = server.entered.get_future();
        auto pending = std::async(std::launch::async, [&] {
            if (catalog) provider.getContextLimit("model");
            else provider.generate(json::array(), "model");
        });
        EXPECT_EQ(entered.wait_for(std::chrono::seconds(2)), std::future_status::ready);
        cancelled = true;
        EXPECT_EQ(pending.wait_for(std::chrono::seconds(4)), std::future_status::ready);
        EXPECT_THROW(pending.get(), core::Cancelled);
        cancelled = false;
        if (catalog) EXPECT_EQ(provider.getContextLimit("model"), 8192);
        else EXPECT_EQ(provider.generate(json::array(), "model")["id"], "resp");
        server.request.get();
    }
}
}
