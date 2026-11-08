#include <agent-runtime/providers/chat_completions_provider.hpp>
#include <agent-runtime/providers/openai_oauth.hpp>
#include <agent-runtime/providers/generation_retry.hpp>
#include <agent-runtime/providers/openai_provider.hpp>
#include <agent-runtime/providers/openrouter_provider.hpp>
#include <daemon/providers/provider_connections.hpp>
#include <daemon/providers/provider_connection.hpp>
#include <daemon/session/session_title.hpp>
#include <daemon/permissions/permission_verifier.hpp>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
#include <asio/post.hpp>
#include <gtest/gtest.h>
#include <cpr/cpr.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fstream>
#include <thread>

namespace {
using Json = nlohmann::json;
struct TempAuth {
    std::filesystem::path directory;
    TempAuth() {
        char path[] = "/tmp/sos-auth-XXXXXX";
        if (!mkdtemp(path)) throw std::runtime_error("Cannot create test directory");
        directory = path;
    }
    ~TempAuth() { std::filesystem::remove_all(directory); }
};

struct Environment {
    std::string name;
    std::optional<std::string> previous;
    Environment(std::string name, const std::string& value) : name(std::move(name)) {
        if (const auto* old = std::getenv(this->name.c_str())) previous = old;
        setenv(this->name.c_str(), value.c_str(), 1);
    }
    ~Environment() {
        if (previous) setenv(name.c_str(), previous->c_str(), 1);
        else unsetenv(name.c_str());
    }
};

// Bounded local HTTP server; only fake tokens and no external services in these tests.
class AuthHttpServer {
public:
    using Chunk = std::pair<std::chrono::milliseconds, std::string>;
    explicit AuthHttpServer(std::vector<std::string> replies, std::vector<int> statuses = {}, bool stall = false,
        std::vector<Chunk> chunks = {}, std::string extraHeaders = "") {
        listener = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t size = sizeof(address);
        if (::bind(listener, reinterpret_cast<sockaddr*>(&address), size) || ::listen(listener, 4) ||
            ::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size)) throw std::runtime_error("Test HTTP bind failed");
        port = ntohs(address.sin_port);
        url = "http://127.0.0.1:" + std::to_string(port);
        worker = std::async(std::launch::async, [this, replies, statuses, stall, chunks, extraHeaders] {
            std::vector<std::string> requests;
            for (const auto& body : replies) {
                pollfd ready{listener, POLLIN, 0};
                if (::poll(&ready, 1, 8000) <= 0) throw std::runtime_error("Test HTTP accept timed out");
                const int client = ::accept(listener, nullptr, nullptr);
                if (client < 0) return requests;
                struct Close { int fd; ~Close() { ::close(fd); } } close{client};
#ifdef SO_NOSIGPIPE
                const int noSigpipe = 1;
                ::setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &noSigpipe, sizeof(noSigpipe));
#endif
                timeval timeout{5, 0};
                ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
                std::string request;
                while (true) {
                    char bytes[4096];
                    const auto count = ::recv(client, bytes, sizeof(bytes), 0);
                    if (count <= 0) throw std::runtime_error("Test HTTP receive failed");
                    request.append(bytes, count);
                    const auto end = request.find("\r\n\r\n");
                    if (end == std::string::npos) continue;
                    const auto length = request.find("Content-Length: ");
                    if (length == std::string::npos || request.size() >= end + 4 + std::stoul(request.substr(length + 16))) break;
                }
                requests.push_back(request);
                if (requests.size() == 1) entered.set_value();
                if (stall) {
                    char ignored;
                    ::recv(client, &ignored, 1, 0);
                    return requests;
                }
                const auto status = statuses.empty() ? 200 : statuses.at(requests.size() - 1);
                auto length = body.size();
                if (!chunks.empty()) {
                    length = 0;
                    for (const auto& chunk : chunks) length += chunk.second.size();
                }
                const std::string response = "HTTP/1.1 " + std::to_string(status) + " Test\r\nConnection: close\r\nContent-Length: " +
                    std::to_string(length) + "\r\n" + extraHeaders + "\r\n" + (chunks.empty() ? body : "");
                if (!sendAll(client, response)) return requests;
                for (const auto& [delay, bytes] : chunks) {
                    // Stop promptly when the provider closes the connection after cancellation/timeout.
                    pollfd closed{client, POLLIN, 0};
                    if (::poll(&closed, 1, static_cast<int>(delay.count())) != 0 || !sendAll(client, bytes)) return requests;
                }
            }
            return requests;
        });
    }
    ~AuthHttpServer() { ::shutdown(listener, SHUT_RDWR); ::close(listener); if (worker.valid()) worker.wait(); }
    bool hasPendingRequest() const {
        pollfd ready{listener, POLLIN, 0};
        return ::poll(&ready, 1, 0) > 0;
    }
    unsigned short port;
    std::string url;
    std::future<std::vector<std::string>> worker;
    std::promise<void> entered;
private:
    static bool sendAll(int client, std::string_view bytes) {
        while (!bytes.empty()) {
#ifdef MSG_NOSIGNAL
            const auto count = ::send(client, bytes.data(), bytes.size(), MSG_NOSIGNAL);
#else
            const auto count = ::send(client, bytes.data(), bytes.size(), 0);
#endif
            if (count <= 0) return false;
            bytes.remove_prefix(static_cast<std::size_t>(count));
        }
        return true;
    }
    int listener;
};

TEST(PermissionVerifierHttpTest, SendsDecisionsContractAndFailsClosedOnInvalidResponses) {
    AuthHttpServer server({R"({"answers":{"permission":{"type":"choice","choice":"allow","confidence":0.01,"probabilities":{"allow":0.99,"deny":0.01}}}})",
        R"({"answers":{"permission":{"type":"choice","choice":"deny","confidence":0.01,"probabilities":{"allow":0,"deny":1}}}})",
        "not-json", "private-error-body"}, {200, 200, 200, 503});
    PermissionVerifier verifier("verification-key", server.url + "/api/alpha/decisions");
    ToolPermissionRequest request{"bash", {"cmake --build build-ai"}, {},
        {{"arguments", {{"command", "cmake --build build-ai"}, {"workdir", "/workspace"}}}}, "c", "bash"};
    EXPECT_EQ(verifier.verify(request, "/workspace", "Build the project", {}), 0.99);
    EXPECT_EQ(verifier.verify(request, "/workspace", "Build the project", {}), 0.0);
    EXPECT_FALSE(verifier.verify(request, "/workspace", "Build the project", {}));
    try { verifier.verify(request, "/workspace", "Build the project", {}); FAIL(); }
    catch (const std::runtime_error& error) {
        EXPECT_EQ(std::string(error.what()).find("private-error-body"), std::string::npos);
    }
    const auto requests = server.worker.get();
    ASSERT_EQ(requests.size(), 4);
    EXPECT_NE(requests[0].find("POST /api/alpha/decisions"), std::string::npos);
    EXPECT_NE(requests[0].find("Content-Type: application/json"), std::string::npos);
    const auto body = Json::parse(requests[0].substr(requests[0].find("\r\n\r\n") + 4));
    EXPECT_EQ(body["model"], "typesafe/jev-1.13");
    EXPECT_EQ(body["questions"]["permission"]["type"], "choice");
    const auto& question = body["questions"]["permission"];
    EXPECT_TRUE(question["criteria"].contains("deny"));
    EXPECT_FALSE(question["criteria"].contains("ask"));
    const auto instructions = question["instructions"].get<std::string>();
    EXPECT_NE(instructions.find("Allow ordinary development work"), std::string::npos);
    EXPECT_NE(instructions.find("Do not refuse routine repository work"), std::string::npos);
    EXPECT_NE(instructions.find("production database"), std::string::npos);
    EXPECT_NE(instructions.find("deleting personal/user files"), std::string::npos);
    EXPECT_EQ(body["state"]["details"]["arguments"]["command"], "cmake --build build-ai");
    EXPECT_EQ(body["state"]["recent_user_request"], "Build the project");
    EXPECT_EQ(body.dump().find("verification-key"), std::string::npos);
}

TEST(PermissionVerifierHttpTest, ConfigValidatesEndpointsWithoutExposingTheirContents) {
    for (const auto& endpoint : {"https://api.typesafe.ai/v1/systemone", "https://api.openai.com/v1/decisions",
         "http://localhost:8080/decision", "http://127.0.0.1:8080/decision", "http://[::1]:8080/decision"}) {
        PermissionVerifierConfig config{.endpoint = endpoint};
        EXPECT_NO_THROW(config.validate());
    }
    for (const auto& endpoint : {"http://example.com/decisions", "ftp://localhost/decisions", "not-url",
         "https://secret@example.com/decisions", "https://example.com/#secret", "http://localhost.evil/decisions"}) {
        PermissionVerifierConfig config{.endpoint = endpoint};
        EXPECT_THROW(config.validate(), std::invalid_argument);
    }
    PermissionVerifierConfig config;
    config.model = "";
    EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(PermissionVerifierHttpTest, SendsOpenAIDecisionsInputAndNamedChoice) {
    AuthHttpServer server({R"({"answers":[{"name":"permission","type":"choice","choice":"allow","confidence":0.01,"probabilities":[{"value":"allow","probability":0.9},{"value":"deny","probability":0.1}]}]})"});
    PermissionVerifier verifier("fake-decision-key", PermissionVerifierConfig{
        DecisionProtocol::OPENAI_DECISIONS, server.url + "/v1/decisions", "custom-model"});
    ToolPermissionRequest request{"bash", {}, {}, {{"arguments", {{"command", "true"}}}}, "c", "bash"};
    EXPECT_EQ(verifier.verify(request, "/workspace", "Build", {}), 0.9);
    const auto requests = server.worker.get();
    ASSERT_EQ(requests.size(), 1);
    EXPECT_NE(requests[0].find("POST /v1/decisions"), std::string::npos);
    EXPECT_NE(requests[0].find("Authorization: Bearer fake-decision-key"), std::string::npos);
    const auto body = Json::parse(requests[0].substr(requests[0].find("\r\n\r\n") + 4));
    EXPECT_EQ(body["model"], "custom-model");
    EXPECT_EQ(body["input"][0]["content"][0]["type"], "input_json");
    EXPECT_EQ(body["input"][0]["content"][0]["data"]["recent_user_request"], "Build");
    EXPECT_EQ(body["questions"][0]["name"], "permission");
    EXPECT_EQ(body["questions"][0]["choices"].size(), 2);
    EXPECT_FALSE(body.contains("state"));
}

TEST(PermissionVerifierHttpTest, RedirectNeverForwardsCredentialsAndNetworkFailureDoesNotAuthorize) {
    AuthHttpServer target({});
    AuthHttpServer source({""}, {307}, false, {}, "Location: " + target.url + "/stolen\r\n");
    ToolPermissionRequest request{"bash", {}, {}, {{"arguments", {{"command", "true"}}}}, "c", "bash"};
    PermissionVerifier verifier("fake-key", source.url);
    EXPECT_THROW(verifier.verify(request, "/workspace", "test", {}), std::runtime_error);
    EXPECT_EQ(source.worker.get().size(), 1);
    EXPECT_FALSE(target.hasPendingRequest());
    EXPECT_TRUE(target.worker.get().empty());
    PermissionVerifier unavailable("fake-key", "http://127.0.0.1:1/v1/decisions");
    EXPECT_THROW(unavailable.verify(request, "/workspace", "test", {}), std::runtime_error);
}

TEST(PermissionVerifierHttpTest, CancellationInterruptsAStalledDecision) {
    AuthHttpServer server({""}, {}, true);
    PermissionVerifier verifier("fake-key", server.url);
    ToolPermissionRequest request{"bash", {}, {}, {{"arguments", {{"command", "true"}}}}, "c", "bash"};
    std::atomic<bool> cancelled{false};
    auto entered = server.entered.get_future();
    auto result = std::async(std::launch::async, [&] {
        return verifier.verify(request, "/workspace", "test", [&] { return cancelled.load(); });
    });
    EXPECT_EQ(entered.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    cancelled = true;
    EXPECT_THROW(result.get(), core::Cancelled);
}

TEST(PermissionVerifierHttpTest, SavedModelAndCredentialAreSnapshottedTogetherAndUseProbability) {
    TempAuth temp;
    auto auth = std::make_shared<AuthStore>(temp.directory / "auth.json");
    ProviderConnections connections(auth, temp.directory / "config.json");
    AuthHttpServer server({R"({"answers":[{"name":"permission","type":"choice","choice":"allow","confidence":0.01,"probabilities":[{"value":"allow","probability":0.95},{"value":"deny","probability":0.05}]}]})"});
    PermissionVerifierConfig model{DecisionProtocol::OPENAI_DECISIONS, server.url + "/v1/decisions", "first-model"};
    connections.savePermissionSettings(PermissionMode::AUTO, 95, model, "first-key");
    const auto snapshot = connections.permissionVerifier(temp.directory.string(), "Build");
    model.endpoint = "http://127.0.0.1:1/v1/decisions";
    model.model = "second-model";
    connections.savePermissionSettings(PermissionMode::AUTO, 95, model, "second-key");
    PermissionManager manager([](auto) {});
    manager.configure(temp.directory, ExecutionMode::SLOP);
    manager.setPermissionMode(PermissionMode::AUTO, snapshot, 95);
    ToolPermissionRequest request{"bash", {}, {}, {{"arguments", {{"command", "true"}}}}, "c", "bash"};
    EXPECT_EQ(manager.ask(request, [] {}).decision, PermissionDecision::AI_ACCEPTED);
    const auto requests = server.worker.get();
    ASSERT_EQ(requests.size(), 1);
    EXPECT_NE(requests[0].find("Bearer first-key"), std::string::npos);
    EXPECT_EQ(requests[0].find("second-key"), std::string::npos);
    EXPECT_NE(requests[0].find("first-model"), std::string::npos);
    manager.setMode(ExecutionMode::SANE);
    EXPECT_THROW(manager.ask(request, [] {}), std::runtime_error);
}

TEST(ApiKeyValidationTest, RejectsBadKeysAndServerErrorsBeforeReplacingSavedCredentials) {
    TempAuth temp;
    auto store = std::make_shared<AuthStore>(temp.directory / "auth.json");
    store->set("openai", ProviderCredential{.key = "old-openai"});
    store->set("openrouter", ProviderCredential{.key = "old-openrouter"});
    AuthHttpServer server({R"({"data":{"is_management_key":false}})", R"({"data":[]})",
        R"({"error":{"message":"never-display-this-body"}})", "server body", "not-json",
        R"({"data":{"is_management_key":true}})"}, {200, 200, 401, 503, 200, 200});
    auto providers = std::make_shared<ProviderConnections>(store, temp.directory / "config.json",
        ProviderConnections::OAuthStarter{}, ModelCatalog{}, ApiKeyValidator{{server.url, server.url}});
    ProviderConnection connection(providers);
    asio::io_context io;
    const auto save = [&](const std::string& id, const std::string& key) {
        io.restart();
        auto command = Json{{"type", "auth.api"}, {"provider", id}, {"key", key}}.get<ProviderRequest>();
        connection.prepare(command);
        auto result = asio::co_spawn(io, connection.handle(command), asio::use_future);
        io.run();
        return result.get();
    };
    EXPECT_EQ(save("openrouter", "valid-router").at("type"), "auth.saved");
    EXPECT_EQ(save("openai", "valid-openai").at("type"), "auth.saved");
    try { save("openai", "rejected-key"); FAIL() << "Expected key rejection"; }
    catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("401"), std::string::npos);
        EXPECT_EQ(std::string(error.what()).find("never-display"), std::string::npos);
        EXPECT_EQ(std::string(error.what()).find("rejected-key"), std::string::npos);
    }
    EXPECT_THROW(save("openrouter", "server-error-key"), std::runtime_error);
    EXPECT_THROW(save("openrouter", "malformed-response-key"), std::runtime_error);
    EXPECT_THROW(save("openrouter", "management-key"), std::runtime_error);
    EXPECT_EQ(store->get("openai").key, "valid-openai");
    EXPECT_EQ(store->get("openrouter").key, "valid-router");
    const auto requests = server.worker.get();
    ASSERT_EQ(requests.size(), 6);
    EXPECT_TRUE(requests[0].starts_with("GET "));
    EXPECT_NE(requests[0].find("Authorization: Bearer valid-router"), std::string::npos);
    EXPECT_NE(requests[1].find("Authorization: Bearer valid-openai"), std::string::npos);
}

TEST(ApiKeyValidationTest, CancellationDuringNetworkCheckKeepsThePreviousKey) {
    TempAuth temp;
    auto store = std::make_shared<AuthStore>(temp.directory / "auth.json");
    store->set("openai", ProviderCredential{.key = "old-key"});
    AuthHttpServer server({""}, {}, true);
    auto providers = std::make_shared<ProviderConnections>(store, temp.directory / "config.json",
        ProviderConnections::OAuthStarter{}, ModelCatalog{}, ApiKeyValidator{{server.url, server.url}});
    ProviderConnection connection(providers);
    asio::io_context io;
    auto command = Json{{"type", "auth.api"}, {"provider", "openai"}, {"key", "new-key"}}.get<ProviderRequest>();
    connection.prepare(command);
    auto result = asio::co_spawn(io, connection.handle(command), asio::use_future);
    auto loop = std::async(std::launch::async, [&] { io.run(); });
    ASSERT_EQ(server.entered.get_future().wait_for(std::chrono::seconds(3)), std::future_status::ready);
    asio::post(io, [&] { connection.cancel(); });
    ASSERT_EQ(result.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    EXPECT_THROW(result.get(), core::Cancelled);
    EXPECT_EQ(store->get("openai").key, "old-key");
}

TEST(ProviderConnectionTest, CancellationBeforeStartAndDuringPollingPreservesCredentials) {
    TempAuth temp;
    auto store = std::make_shared<AuthStore>(temp.directory / "auth.json");
    store->set("openai", ProviderCredential{.key = "existing-key"});
    std::atomic<int> started = 0;
    auto providers = std::make_shared<ProviderConnections>(store, temp.directory / "config.json",
        [&](const std::string&, std::shared_ptr<std::atomic<bool>> cancelled) {
            ++started;
            auto flow = std::make_unique<ProviderAuthFlow>();
            flow->cancelled = cancelled;
            flow->url = "https://example.test/auth";
            flow->completion = std::async(std::launch::async, [cancelled]() -> ProviderCredential {
                while (!cancelled->load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                throw core::Cancelled();
            });
            return flow;
        });
    ProviderConnection connection(providers);
    asio::io_context io;
    const auto request = [&](Json json) {
        io.restart();
        auto command = json.get<ProviderRequest>();
        auto response = asio::co_spawn(io, connection.handle(command), asio::use_future);
        io.run();
        return response.get();
    };
    const Json start{{"type", "auth.start"}, {"provider", "openai"}, {"method", "browser"}};
    connection.prepare(start.get<ProviderRequest>());
    connection.prepare(Json{{"type", "auth.cancel"}}.get<ProviderRequest>());
    EXPECT_THROW(request(start), core::Cancelled);
    EXPECT_EQ(started, 0);
    const auto active = request(start);
    const auto id = active.at("authId").get<std::string>();
    EXPECT_EQ(started, 1);
    EXPECT_EQ(request({{"type", "auth.poll"}, {"authId", id}}).at("type"), "auth.pending");
    request({{"type", "auth.cancel"}, {"authId", "stale"}});
    EXPECT_EQ(request({{"type", "auth.poll"}, {"authId", id}}).at("type"), "auth.pending");
    EXPECT_EQ(request({{"type", "auth.cancel"}, {"authId", id}}).at("type"), "auth.cancelled");
    EXPECT_EQ(store->get("openai").key, "existing-key");
    EXPECT_THROW(request({{"type", "auth.poll"}, {"authId", id}}), std::invalid_argument);
}

TEST(ProviderConnectionTest, SavesCompletedOAuthAndRejectsInvalidSelectionWithoutChangingSettings) {
    TempAuth temp;
    auto store = std::make_shared<AuthStore>(temp.directory / "auth.json");
    auto providers = std::make_shared<ProviderConnections>(store, temp.directory / "config.json",
        [](const std::string&, std::shared_ptr<std::atomic<bool>> cancelled) {
            auto flow = std::make_unique<ProviderAuthFlow>();
            flow->cancelled = cancelled;
            std::promise<ProviderCredential> complete;
            flow->completion = complete.get_future();
            complete.set_value(ProviderCredential{.kind = AuthKind::OAUTH, .access = "fake-access", .refresh = "fake-refresh",
                .expires = oauthNow() + 3600000, .method = AuthMethod::BROWSER});
            return flow;
        });
    ProviderConnection connection(providers);
    asio::io_context io;
    const auto request = [&](Json json) {
        io.restart();
        auto command = json.get<ProviderRequest>();
        auto response = asio::co_spawn(io, connection.handle(command), asio::use_future);
        io.run();
        return response.get();
    };
    const auto started = request({{"type", "auth.start"}, {"provider", "openai"}, {"method", "device"}});
    const auto saved = request({{"type", "auth.poll"}, {"authId", started.at("authId")}});
    EXPECT_EQ(saved.at("type"), "auth.saved");
    EXPECT_EQ(saved.dump().find("fake-access"), std::string::npos);
    EXPECT_TRUE(providers->hasAuth("openai"));
    EXPECT_EQ(store->get("openai").refresh, "fake-refresh");
    EXPECT_EQ(store->get("openai").method, AuthMethod::DEVICE);
    EXPECT_THROW(request({{"type", "provider.select"}, {"provider", "openai"}, {"model", "invalid model"}}), std::invalid_argument);
    EXPECT_EQ(connection.selection().provider, "openrouter");
    EXPECT_FALSE(std::filesystem::exists(temp.directory / "config.json"));
    EXPECT_EQ(request({{"type", "provider.select"}, {"provider", "openai"}, {"model", "gpt-5.4"}}).at("type"), "provider.selected");
    EXPECT_EQ(connection.selection().provider, "openai");
}

TEST(ModelCatalogTest, FiltersUnsupportedModelsAndPreservesProviderOrder) {
    const auto openai = Json::parse(R"({"data":[
        {"id":"text-embedding-3-small"},{"id":"gpt-4o-realtime-preview"},{"id":"gpt-5-chat-latest"},
        {"id":"gpt-4.1-mini-2025-04-14"},{"id":"gpt-5.4"}]})");
    EXPECT_EQ(ModelCatalog::compatibleModels(openai, ModelCatalogFormat::OPENAI),
        (std::vector<std::string>{"gpt-4.1-mini-2025-04-14", "gpt-5.4"}));
    const auto router = Json::parse(R"({"data":[
        {"id":"image-only","architecture":{"input_modalities":["text"],"output_modalities":["image"]},"supported_parameters":["tools"]},
        {"id":"no-tools","architecture":{"input_modalities":["text"],"output_modalities":["text"]},"supported_parameters":[]},
        {"id":"vendor/first","architecture":{"input_modalities":["text"],"output_modalities":["text"]},"supported_parameters":["tools"]},
        {"id":"vendor/second","architecture":{"input_modalities":["text"],"output_modalities":["text"]},"supported_parameters":["tools"]}]})");
    EXPECT_EQ(ModelCatalog::compatibleModels(router, ModelCatalogFormat::OPENROUTER),
        (std::vector<std::string>{"vendor/first", "vendor/second"}));
    const auto codex = Json::parse(R"({"models":[{"slug":"hidden","visibility":"hide"},
        {"slug":"codex-first","visibility":"list","supported_in_api":false},{"slug":"codex-second","visibility":"list"}]})");
    EXPECT_EQ(ModelCatalog::compatibleModels(codex, ModelCatalogFormat::CODEX),
        (std::vector<std::string>{"codex-first", "codex-second"}));
    EXPECT_EQ(ModelCatalog::selectFirstCompatible(openai, ModelCatalogFormat::OPENAI), "gpt-4.1-mini-2025-04-14");
    EXPECT_EQ(ModelCatalog::selectFirstCompatible(router, ModelCatalogFormat::OPENROUTER), "vendor/first");
    EXPECT_EQ(ModelCatalog::selectFirstCompatible(codex, ModelCatalogFormat::CODEX), "codex-first");
    EXPECT_THROW(ModelCatalog::selectFirstCompatible(Json{{"data", Json::array()}}, ModelCatalogFormat::OPENAI), std::runtime_error);
}

TEST(ModelCatalogTest, OpenAIOrdersByCreationDateWithStableUndatedFallback) {
    const auto catalog = Json::parse(R"({"data":[
        {"id":"gpt-4o"}, {"id":"gpt-4.1","created":100},
        {"id":"gpt-5.4","created":300}, {"id":"gpt-5.4","created":400},
        {"id":"gpt-5.2","created":300}, {"id":"gpt-5","created":200},
        {"id":"gpt-4o-mini","created":"invalid"}, {"id":"o3","created":-1},
        {"id":"o4-mini","created":null}, {"id":"text-embedding-3-small","created":500}
    ]})");
    EXPECT_EQ(ModelCatalog::compatibleModels(catalog, ModelCatalogFormat::OPENAI),
        (std::vector<std::string>{"gpt-5.4", "gpt-5.2", "gpt-5", "gpt-4.1",
            "gpt-4o", "gpt-4o-mini", "o3", "o4-mini"}));
    EXPECT_EQ(ModelCatalog::selectFirstCompatible(catalog, ModelCatalogFormat::OPENAI), "gpt-5.4");
}

TEST(ModelCatalogTest, CodexOrdersByPriorityWithStableUnrankedFallback) {
    const auto catalog = Json::parse(R"({"models":[
        {"slug":"missing","visibility":"list"},
        {"slug":"astra","visibility":"list","priority":2},
        {"slug":"sol","visibility":"list","priority":1},
        {"slug":"tie","visibility":"list","priority":2},
        {"slug":"invalid","visibility":"list","priority":"first"},
        {"slug":"negative","visibility":"list","priority":-1},
        {"slug":"null","visibility":"list","priority":null},
        {"slug":"fractional","visibility":"list","priority":1.5},
        {"slug":"hidden","visibility":"hide","priority":0}
    ]})");
    EXPECT_EQ(ModelCatalog::compatibleModels(catalog, ModelCatalogFormat::CODEX),
        (std::vector<std::string>{"sol", "astra", "tie", "missing", "invalid", "negative", "null", "fractional"}));
    EXPECT_EQ(ModelCatalog::selectFirstCompatible(catalog, ModelCatalogFormat::CODEX), "sol");
}

TEST(ModelCatalogTest, CodexDiscoveryUsesCurrentVersionAndKeepsVisibleModels) {
    EXPECT_NE(ModelCatalogEndpoints{}.codex.find("client_version=0.155.0"), std::string::npos);
    const auto legacy = Json::parse(R"({"models":[{"slug":"codex-auto-review","visibility":"hide","input_modalities":["text","image"]}]})");
    EXPECT_THROW(ModelCatalog::selectFirstCompatible(legacy, ModelCatalogFormat::CODEX), std::runtime_error);
    const auto current = Json::parse(R"({"models":[
        {"slug":"gpt-6-astra","visibility":"list","input_modalities":["text","image"]},
        {"slug":"gpt-reserve","visibility":"hide","input_modalities":["text","image"]},
        {"slug":"gpt-5.6-sol","visibility":"list","input_modalities":["text","image"]}]})");
    EXPECT_EQ(ModelCatalog::compatibleModels(current, ModelCatalogFormat::CODEX),
        (std::vector<std::string>{"gpt-6-astra", "gpt-5.6-sol"}));
}

TEST(ModelCatalogTest, DiscoversAfterAuthenticationAndKeepsSelectionOnFailureOrCancellation) {
    TempAuth temp;
    auto store = std::make_shared<AuthStore>(temp.directory / "auth.json");
    store->set("openai", ProviderCredential{.key = "fake-openai"});
    store->set("openrouter", ProviderCredential{.key = "fake-openrouter"});
    AuthHttpServer server({R"({"data":[{"id":"gpt-4o","created":100},{"id":"gpt-4.1","created":200}]})",
        R"({"data":[{"id":"vendor/first","architecture":{"input_modalities":["text"],"output_modalities":["text"]},"supported_parameters":["tools"]}]})",
        R"({"models":[{"slug":"gpt-4.1","visibility":"list","priority":2},
            {"slug":"gpt-5.4","visibility":"list","priority":1}]})", R"({"models":[]})"});
    auto providers = std::make_shared<ProviderConnections>(store, temp.directory / "config.json",
        ProviderConnections::OAuthStarter{}, ModelCatalog{ModelCatalogEndpoints{server.url, server.url, server.url, ""}});
    ProviderConnection connection(providers);
    asio::io_context io;
    const auto request = [&](const std::string& provider) {
        io.restart();
        auto command = Json{{"type", "provider.select"}, {"provider", provider}}.get<ProviderRequest>();
        connection.prepare(command);
        auto response = asio::co_spawn(io, connection.handle(command), asio::use_future);
        io.run();
        return response.get();
    };
    request("openai");
    EXPECT_EQ(connection.selection().model, "gpt-4.1");
    request("openrouter");
    EXPECT_EQ(connection.selection().model, "vendor/first");
    store->set("openai", ProviderCredential{.kind = AuthKind::OAUTH, .access = "fake-codex", .refresh = "refresh",
        .accountId = "account", .expires = oauthNow() + 3600000, .method = AuthMethod::BROWSER});
    request("openai");
    EXPECT_EQ(connection.selection().model, "gpt-5.4");
    EXPECT_THROW(request("openai"), std::runtime_error);
    EXPECT_EQ(connection.selection().model, "gpt-5.4");
    connection.prepare(Json{{"type", "provider.select"}, {"provider", "openai"}}.get<ProviderRequest>());
    connection.prepare(Json{{"type", "auth.cancel"}}.get<ProviderRequest>());
    EXPECT_THROW(request("openai"), core::Cancelled);
    EXPECT_EQ(readPrivateJson(temp.directory / "config.json").at("model"), "gpt-5.4");
    const auto requests = server.worker.get();
    ASSERT_EQ(requests.size(), 4);
    EXPECT_NE(requests[0].find("Authorization: Bearer fake-openai"), std::string::npos);
    EXPECT_NE(requests[1].find("Authorization: Bearer fake-openrouter"), std::string::npos);
    EXPECT_TRUE(requests[1].starts_with("GET /?sort=newest "));
    EXPECT_NE(requests[2].find("Authorization: Bearer fake-codex"), std::string::npos);
    EXPECT_NE(requests[2].find("ChatGPT-Account-Id: account"), std::string::npos);
}

TEST(ModelCatalogTest, ListsConnectedProvidersPreservingOrderAndDuplicateModelsAcrossProviders) {
    TempAuth temp;
    auto store = std::make_shared<AuthStore>(temp.directory / "auth.json");
    AuthHttpServer server({R"({"data":[{"id":"gpt-4.1","created":100},{"id":"gpt-5.4","created":200},{"id":"gpt-5.4","created":200}]})",
        R"({"data":[{"id":"gpt-5.4","context_length":128000,"architecture":{"input_modalities":["text"],"output_modalities":["text"]},"supported_parameters":["tools"]}]})"});
    auto providers = std::make_shared<ProviderConnections>(store, temp.directory / "config.json",
        ProviderConnections::OAuthStarter{}, ModelCatalog{ModelCatalogEndpoints{server.url, server.url, server.url, ""}});
    EXPECT_TRUE(providers->listModels({}).models.empty());
    store->set("openai", ProviderCredential{.key = "fake-openai"});
    store->set("openrouter", ProviderCredential{.key = "fake-router"});
    providers->select({"openai", "gpt-4.1", "medium"});
    ProviderConnection connection(providers);
    asio::io_context io;
    const auto request = [&](Json json) {
        io.restart();
        auto command = json.get<ProviderRequest>();
        connection.prepare(command);
        auto response = asio::co_spawn(io, connection.handle(command), asio::use_future);
        io.run();
        return response.get();
    };
    const auto listed = request({{"type", "model.list"}});
    EXPECT_EQ(listed.at("type"), "model.listed");
    EXPECT_TRUE(listed.at("errors").empty());
    EXPECT_EQ(listed.at("models"), Json::parse(R"([
        {"provider":"openai","model":"gpt-5.4","auth_method":"api","max_context":null,"image_input":"supported"},{"provider":"openai","model":"gpt-4.1","auth_method":"api","max_context":null,"image_input":"supported"},
        {"provider":"openrouter","model":"gpt-5.4","auth_method":"api","max_context":128000,"image_input":"unsupported"}])"));
    EXPECT_EQ(connection.selection().model, "gpt-4.1");
    EXPECT_EQ(request({{"type", "model.select"}, {"provider", "openrouter"}, {"model", "gpt-5.4"}}).at("type"), "model.selected");
    ProviderConnections restarted(store, temp.directory / "config.json");
    EXPECT_EQ(restarted.selection().provider, "openrouter");
    EXPECT_EQ(restarted.selection().model, "gpt-5.4");
    EXPECT_EQ(restarted.selection().reasoning, "medium");
    EXPECT_THROW(request({{"type", "model.select"}, {"provider", "openai"}, {"model", ""}}), std::invalid_argument);
    EXPECT_EQ(server.worker.get().size(), 2);
}

TEST(ModelCatalogTest, ConversationSelectionIsIndependentAndExplicitChangesUpdateDefault) {
    TempAuth temp;
    auto store = std::make_shared<AuthStore>(temp.directory / "auth.json");
    store->set("openai", ProviderCredential{.key = "fake"});
    auto providers = std::make_shared<ProviderConnections>(store, temp.directory / "config.json");
    providers->select({"openai", "model-a", "low", 65536});
    ProviderConnection a(providers), b(providers);
    a.restoreSelection(a.selection());
    providers->select({"openai", "model-b", "high", 131072});
    b.restoreSelection(b.selection());
    EXPECT_EQ(a.selection().model, "model-a");
    EXPECT_EQ(a.selection().reasoning, "low");
    EXPECT_EQ(a.selection().contextLimit, 65536);
    a.setReasoning("medium");
    EXPECT_EQ(providers->selection().model, "model-a");
    EXPECT_EQ(providers->selection().reasoning, "medium");
    EXPECT_EQ(providers->selection().contextLimit, 65536);
    EXPECT_EQ(b.selection().model, "model-b");
    EXPECT_EQ(b.selection().reasoning, "high");
    EXPECT_EQ(b.selection().contextLimit, 131072);
    a.restoreSelection(std::nullopt);
    EXPECT_EQ(a.selection().reasoning, "medium");
    b.restoreSelection(ProviderSelection{"openai", "model-b", "high", 131072});
    EXPECT_EQ(providers->selection().model, "model-a");
}

TEST(ModelCatalogTest, ContextBudgetIsValidatedAndSavedWithCurrentGlobalSelection) {
    TempAuth temp;
    auto store = std::make_shared<AuthStore>(temp.directory / "auth.json");
    store->set("openai", ProviderCredential{.kind = AuthKind::OAUTH, .access = "fake", .refresh = "refresh",
        .expires = oauthNow() + 3600000, .method = AuthMethod::BROWSER});
    AuthHttpServer server({R"({"models":[
        {"slug":"gpt-6-astra","visibility":"list","context_window":272000,"max_context_window":872000},
        {"slug":"fallback","visibility":"list","context_window":64000},
        {"slug":"unknown","visibility":"list","context_window":-1,"max_context_window":null}
    ]})"});
    auto providers = std::make_shared<ProviderConnections>(store, temp.directory / "config.json",
        ProviderConnections::OAuthStarter{}, ModelCatalog{ModelCatalogEndpoints{server.url, server.url, server.url, ""}});
    providers->select({"openai", "gpt-6-astra", "high"});
    ProviderConnection connection(providers);
    asio::io_context io;
    const auto request = [&](Json json) {
        io.restart();
        auto command = json.get<ProviderRequest>();
        connection.prepare(command);
        auto response = asio::co_spawn(io, connection.handle(command), asio::use_future);
        io.run();
        return response.get();
    };
    Json choose{{"type", "model.select"}, {"provider", "openai"}, {"model", "gpt-6-astra"}, {"context_limit", 150000}};
    EXPECT_THROW(request(choose), std::invalid_argument);
    const auto listed = request({{"type", "model.list"}});
    EXPECT_EQ(listed["models"][0]["max_context"], 872000);
    EXPECT_EQ(listed["models"][1]["max_context"], 64000);
    EXPECT_TRUE(listed["models"][2]["max_context"].is_null());
    EXPECT_EQ(request(choose)["type"], "model.selected");
    ProviderConnections restarted(store, temp.directory / "config.json");
    EXPECT_EQ(restarted.selection().contextLimit, 150000);
    EXPECT_EQ(restarted.selection().model, "gpt-6-astra");
    restarted.setReasoning("low");
    EXPECT_EQ(restarted.selection().contextLimit, 150000);
    for (const auto& invalid : {Json(32767), Json(872001), Json(-1), Json(1.5), Json("150000"), Json(nullptr)}) {
        choose["context_limit"] = invalid;
        EXPECT_THROW(request(choose), std::invalid_argument);
        EXPECT_EQ(restarted.selection().contextLimit, 150000);
    }
    for (const auto boundary : {32768, 872000}) {
        choose["context_limit"] = boundary;
        EXPECT_EQ(request(choose)["type"], "model.selected");
        EXPECT_EQ(restarted.selection().contextLimit, boundary);
    }
    choose["model"] = "unknown";
    choose["context_limit"] = 65536;
    EXPECT_EQ(request(choose)["type"], "model.selected");
    EXPECT_EQ(restarted.selection().contextLimit, 65536);
    restarted.select({"openai", "fallback"});
    EXPECT_FALSE(restarted.selection().contextLimit);
    EXPECT_EQ(restarted.selection().reasoning, "low");
    EXPECT_EQ(server.worker.get().size(), 1);
}

TEST(ModelCatalogTest, FailedCatalogKeepsOtherProvidersAndCancellationDoesNotChangeSelection) {
    TempAuth temp;
    auto store = std::make_shared<AuthStore>(temp.directory / "auth.json");
    store->set("openai", ProviderCredential{.key = "fake-openai"});
    store->set("openrouter", ProviderCredential{.key = "fake-router"});
    AuthHttpServer server({R"({"error":"do not expose raw body"})",
        R"({"data":[{"id":"vendor/model","architecture":{"input_modalities":["text"],"output_modalities":["text"]},"supported_parameters":["tools"]}]})"}, {503, 200});
    auto providers = std::make_shared<ProviderConnections>(store, temp.directory / "config.json",
        ProviderConnections::OAuthStarter{}, ModelCatalog{ModelCatalogEndpoints{server.url, server.url, server.url, ""}});
    providers->select({"openai", "gpt-4.1", "low"});
    const auto result = providers->listModels({});
    ASSERT_EQ(result.models.size(), 1);
    EXPECT_EQ(result.models[0].provider, "openrouter");
    ASSERT_EQ(result.errors.size(), 1);
    EXPECT_EQ(result.errors[0].provider, "openai");
    EXPECT_EQ(result.errors[0].error.find("do not expose"), std::string::npos);
    EXPECT_EQ(server.worker.get().size(), 2);
    ProviderConnection connection(providers);
    auto command = Json{{"type", "model.list"}}.get<ProviderRequest>();
    connection.prepare(command);
    connection.prepare(Json{{"type", "model.cancel"}}.get<ProviderRequest>());
    asio::io_context io;
    auto response = asio::co_spawn(io, connection.handle(command), asio::use_future);
    io.run();
    EXPECT_THROW(response.get(), core::Cancelled);
    EXPECT_EQ(connection.selection().model, "gpt-4.1");
    EXPECT_EQ(connection.selection().reasoning, "low");
}

TEST(ModelCatalogTest, DoesNotRequestCatalogsOfDisconnectedProviders) {
    TempAuth temp;
    auto store = std::make_shared<AuthStore>(temp.directory / "auth.json");
    store->set("openrouter", ProviderCredential{.key = "fake-router"});
    AuthHttpServer server({R"({"data":[]})"});
    ProviderConnections providers(store, temp.directory / "config.json", ProviderConnections::OAuthStarter{},
        ModelCatalog{ModelCatalogEndpoints{server.url, server.url, server.url, ""}});
    const auto result = providers.listModels({});
    EXPECT_TRUE(result.models.empty());
    EXPECT_TRUE(result.errors.empty());
    const auto requests = server.worker.get();
    ASSERT_EQ(requests.size(), 1);
    EXPECT_NE(requests[0].find("Authorization: Bearer fake-router"), std::string::npos);
}

TEST(ProviderAuthTest, ResolvesXdgPathsAndRejectsRelativeOverrides) {
    Environment home("HOME", "/tmp/sos-home"), data("XDG_DATA_HOME", "/tmp/sos-data"),
        config("XDG_CONFIG_HOME", "relative"), state("XDG_STATE_HOME", ""), cache("XDG_CACHE_HOME", "/tmp/sos-cache");
    EXPECT_EQ(PathResolution::authFile(), "/tmp/sos-data/sos/auth.json");
    EXPECT_EQ(PathResolution::configFile(), "/tmp/sos-home/.config/sos/config.json");
    EXPECT_EQ(PathResolution::skillsDirectory(), "/tmp/sos-home/.config/sos/skills");
    EXPECT_EQ(PathResolution::sessionsDirectory(), "/tmp/sos-home/.local/state/sos/sessions");
    EXPECT_EQ(PathResolution::cacheDirectory(), "/tmp/sos-cache/sos");
    EXPECT_EQ(PathResolution::binDirectory(), "/tmp/sos-home/.local/bin");
}

TEST(ProviderAuthTest, PersistsBothProvidersOwnerOnlyWithoutPuttingKeysInConfig) {
    TempAuth temp;
    auto store = std::make_shared<AuthStore>(temp.directory / "auth.json");
    store->set("openrouter", ProviderCredential{.key = "fake-router-key"});
    store->set("openai", ProviderCredential{.key = "fake-openai-key"});
    AuthStore reloaded(temp.directory / "auth.json");
    EXPECT_EQ(reloaded.get("openrouter").key, "fake-router-key");
    EXPECT_EQ(reloaded.get("openai").key, "fake-openai-key");
    struct stat info{};
    ASSERT_EQ(::stat((temp.directory / "auth.json").c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777, 0600);
    ProviderConnections connections(store, temp.directory / "config.json");
    connections.select({"openai", "gpt-5.4"});
    EXPECT_EQ(connections.selection().provider, "openai");
    EXPECT_EQ(connections.defaultReasoning(connections.selection()), "xhigh");
    connections.select({"openai", "gpt-5"});
    EXPECT_EQ(connections.defaultReasoning(connections.selection()), "high");
    EXPECT_EQ(readPrivateJson(temp.directory / "config.json").dump().find("fake-"), std::string::npos);
    EXPECT_THROW(store->set("openai", ProviderCredential{.key = "bad\nkey"}), std::invalid_argument);
    EXPECT_THROW(store->set("../../escape", ProviderCredential{.key = "key"}), std::invalid_argument);
    EXPECT_EQ(reloaded.get("openai").key, "fake-openai-key");
}

TEST(ProviderAuthTest, ReplacesOneAuthPerProviderAndRemembersMethodAcrossReloads) {
    TempAuth temp;
    auto store = std::make_shared<AuthStore>(temp.directory / "auth.json");
    ProviderConnections providers(store, temp.directory / "config.json");
    providers.saveApiKey("openrouter", "router-key");
    providers.saveApiKey("openai", "old-api-key");
    ProviderCredential credential{.kind = AuthKind::OAUTH, .access = "browser-access", .refresh = "browser-refresh",
        .expires = oauthNow() + 3600000, .method = AuthMethod::BROWSER};
    providers.saveOAuth("openai", credential);
    auto saved = readPrivateJson(temp.directory / "auth.json");
    EXPECT_FALSE(saved.at("openai").contains("key"));
    EXPECT_EQ(saved.at("openrouter").at("key"), "router-key");
    EXPECT_EQ(AuthStore(temp.directory / "auth.json").get("openai").method, AuthMethod::BROWSER);
    credential.method = AuthMethod::DEVICE;
    credential.access = "device-access";
    credential.refresh = "device-refresh";
    providers.saveOAuth("openai", credential);
    EXPECT_EQ(AuthStore(temp.directory / "auth.json").get("openai").method, AuthMethod::DEVICE);
    providers.select({"openrouter", "vendor/model"});
    providers.select({"openai", "gpt-6-astra"});
    EXPECT_EQ(ProviderConnections(store, temp.directory / "config.json").selection().provider, "openai");
    const auto connected = Json(providers.list(providers.selection()));
    EXPECT_EQ(connected[0].at("auth_method"), "device");
    EXPECT_EQ(connected[1].at("auth_method"), "api");
    EXPECT_TRUE(connected[0].at("connected").get<bool>());
    EXPECT_TRUE(connected[1].at("connected").get<bool>());
    providers.saveApiKey("openai", "new-api-key");
    saved = readPrivateJson(temp.directory / "auth.json");
    EXPECT_EQ(saved.at("openai").at("method"), "api");
    EXPECT_FALSE(saved.at("openai").contains("access"));
    EXPECT_FALSE(saved.at("openai").contains("refresh"));
    EXPECT_EQ(saved.at("openrouter").at("key"), "router-key");
    saved["openai"] = {{"type", "oauth"}, {"access", "legacy"}, {"refresh", "legacy-refresh"}, {"expires", 123}};
    writePrivateJson(temp.directory / "auth.json", saved);
    EXPECT_THROW(store->get("openai"), std::runtime_error);
    EXPECT_EQ(readPrivateJson(temp.directory / "auth.json"), saved);
    saved["openai"]["method"] = "oauth";
    writePrivateJson(temp.directory / "auth.json", saved);
    EXPECT_THROW(store->get("openai"), std::runtime_error);
    EXPECT_EQ(readPrivateJson(temp.directory / "auth.json"), saved);
    saved["openai"] = {{"type", "api"}, {"key", "key-without-method"}};
    writePrivateJson(temp.directory / "auth.json", saved);
    EXPECT_THROW(store->get("openai"), std::runtime_error);
    EXPECT_EQ(readPrivateJson(temp.directory / "auth.json"), saved);
}

TEST(ProviderAuthTest, GenerationPreferencesAreSharedAndSurviveProviderChangesAndRestart) {
    TempAuth temp;
    auto store = std::make_shared<AuthStore>(temp.directory / "auth.json");
    store->set("openai", ProviderCredential{.key = "fake-openai"});
    store->set("openrouter", ProviderCredential{.key = "fake-router"});
    auto providers = std::make_shared<ProviderConnections>(store, temp.directory / "config.json");
    providers->select({"openai", "gpt-5.4"});
    ProviderConnection first(providers), second(providers);
    first.setReasoning("low");
    EXPECT_EQ(second.selection().reasoning, "low");
    providers->select({"openrouter", "vendor/model"});
    EXPECT_EQ(first.selection().provider, "openrouter");
    EXPECT_EQ(first.selection().model, "vendor/model");
    EXPECT_EQ(first.selection().reasoning, "low");
    ProviderConnection restarted(std::make_shared<ProviderConnections>(store, temp.directory / "config.json"));
    EXPECT_EQ(restarted.selection().model, "vendor/model");
    EXPECT_EQ(restarted.selection().reasoning, "low");
    EXPECT_EQ(readPrivateJson(temp.directory / "config.json").at("reasoning"), "low");
}

TEST(ProviderAuthTest, EnvironmentKeysAreIgnoredAndCorruptionIsNotOverwritten) {
    TempAuth temp;
    Environment key("OPENAI_API_KEY", "fake-environment-key"), routerKey("OPENROUTER_API_KEY", "fake-router-environment-key");
    AuthStore store(temp.directory / "auth.json");
    EXPECT_THROW(store.get("openai"), std::runtime_error);
    EXPECT_THROW(store.get("openrouter"), std::runtime_error);
    ProviderConnections providers(std::make_shared<AuthStore>(temp.directory / "auth.json"), temp.directory / "config.json");
    EXPECT_FALSE(providers.hasAuth("openai"));
    EXPECT_FALSE(providers.hasAuth("openrouter"));
    store.set("openai", ProviderCredential{.key = "fake-stored-key"});
    EXPECT_EQ(store.get("openai").key, "fake-stored-key");
    EXPECT_THROW(store.get("openrouter"), std::runtime_error);
    std::ofstream(temp.directory / "auth.json") << "broken";
    EXPECT_THROW(store.set("openai", ProviderCredential{.key = "replacement"}), std::runtime_error);
}

TEST(ProviderAuthTest, DeviceLoginExchangesCodeThenRefreshesWithoutLeakingTokens) {
    const std::string tokens = R"({"access_token":"fake-access","refresh_token":"fake-refresh","expires_in":3600})";
    AuthHttpServer server({R"({"device_auth_id":"device","user_code":"CODE-1234","interval":"1"})",
        R"({"authorization_code":"authorization","code_verifier":"verifier"})", tokens,
        R"({"access_token":"new-access","expires_in":3600})"});
    OpenAIOAuthSettings settings{.issuer = server.url, .deadline = std::chrono::seconds(8)};
    auto flow = startOpenAIOAuth("device", std::make_shared<std::atomic<bool>>(false), settings);
    EXPECT_EQ(flow->instructions, "Enter code: CODE-1234");
    ASSERT_EQ(flow->completion.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    const auto credential = flow->completion.get();
    EXPECT_EQ(credential.kind, AuthKind::OAUTH);
    EXPECT_EQ(credential.refresh, "fake-refresh");
    EXPECT_GT(credential.expires, oauthNow());
    const auto refreshed = refreshOpenAICredential(credential, {}, settings);
    EXPECT_EQ(refreshed.access, "new-access");
    EXPECT_EQ(refreshed.refresh, "fake-refresh");
    const auto requests = server.worker.get();
    ASSERT_EQ(requests.size(), 4);
    EXPECT_NE(requests[2].find("code_verifier=verifier"), std::string::npos);
    EXPECT_NE(requests[3].find("grant_type=refresh_token"), std::string::npos);
    EXPECT_EQ(flow->url.find("fake-access"), std::string::npos);
}

TEST(ProviderAuthTest, ConcurrentProvidersRefreshExpiredTokensOnceAndPersistRotation) {
    TempAuth temp;
    auto store = std::make_shared<AuthStore>(temp.directory / "auth.json");
    store->set("openai", ProviderCredential{.kind = AuthKind::OAUTH, .access = "expired", .refresh = "old-refresh",
        .accountId = "account", .expires = oauthNow() - 1000, .method = AuthMethod::DEVICE});
    AuthHttpServer server({R"({"access_token":"renewed","refresh_token":"rotated","expires_in":3600})"});
    OpenAIOAuthSettings settings{.issuer = server.url};
    OpenAIProviderAuth first(store, settings), second(store, settings);
    auto a = std::async(std::launch::async, [&] { return first.authorize(); });
    auto b = std::async(std::launch::async, [&] { return second.authorize(); });
    EXPECT_EQ(a.get().bearer, "renewed");
    EXPECT_EQ(b.get().bearer, "renewed");
    EXPECT_EQ(server.worker.get().size(), 1);
    AuthStore reloaded(temp.directory / "auth.json");
    EXPECT_EQ(reloaded.get("openai").refresh, "rotated");
    EXPECT_EQ(reloaded.get("openai").accountId, "account");
    EXPECT_EQ(reloaded.get("openai").method, AuthMethod::DEVICE);
}

TEST(ProviderAuthTest, BrowserLoginChecksStateAndSupportsCancellation) {
    AuthHttpServer server({R"({"access_token":"fake-access","refresh_token":"fake-refresh"})"});
    // Reserve an ephemeral port, then release it for the callback listener.
    unsigned short callbackPort;
    { AuthHttpServer reserve({}); callbackPort = reserve.port; }
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    OpenAIOAuthSettings settings{.issuer = server.url, .callbackPort = callbackPort, .deadline = std::chrono::seconds(8)};
    auto flow = startOpenAIOAuth("browser", cancel, settings);
    const auto position = flow->url.find("&state=") + 7;
    const auto state = flow->url.substr(position, flow->url.find('&', position) - position);
    const auto callback = "http://127.0.0.1:" + std::to_string(callbackPort) + "/auth/callback?code=fake-code&state=";
    EXPECT_EQ(cpr::Get(cpr::Url{callback + "wrong"}, cpr::Timeout{2000}).status_code, 400);
    EXPECT_EQ(flow->completion.wait_for(std::chrono::milliseconds(10)), std::future_status::timeout);
    EXPECT_EQ(cpr::Get(cpr::Url{callback + state}, cpr::Timeout{2000}).status_code, 200);
    ASSERT_EQ(flow->completion.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    EXPECT_EQ(flow->completion.get().access, "fake-access");
    const auto request = server.worker.get().at(0);
    EXPECT_NE(request.find("code_verifier="), std::string::npos);
    EXPECT_NE(request.find("grant_type=authorization_code"), std::string::npos);
    flow.reset();
    cancel = std::make_shared<std::atomic<bool>>(false);
    flow = startOpenAIOAuth("browser", cancel, settings);
    cancel->store(true);
    ASSERT_EQ(flow->completion.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_THROW(flow->completion.get(), core::Cancelled);
}

TEST(ProviderAuthTest, GeneratesTitleFromCodexItemEventsWhenFinalOutputIsEmpty) {
    TempAuth temp;
    auto store = std::make_shared<AuthStore>(temp.directory / "auth.json");
    store->set("openai", ProviderCredential{.kind = AuthKind::OAUTH, .access = "fake-access", .refresh = "fake-refresh",
        .expires = oauthNow() + 3600000, .method = AuthMethod::BROWSER});
    const Json item{{"id", "title"}, {"type", "message"}, {"status", "completed"}, {"role", "assistant"},
        {"phase", "final_answer"}, {"content", Json::array({{{"type", "output_text"}, {"text", "Changer le thème"},
        {"annotations", Json::array()}}})}};
    const Json done{{"type", "response.output_item.done"}, {"output_index", 0}, {"item", item}};
    const Json complete{{"type", "response.completed"}, {"response", {{"id", "title-response"}, {"status", "completed"},
        {"output", Json::array()}}}};
    AuthHttpServer server({"data: " + done.dump() + "\n\ndata: " + complete.dump() + "\n\n"});
    OpenAIProvider provider(std::make_shared<OpenAIProviderAuth>(store), server.url, server.url);
    EXPECT_EQ(generateSessionTitle(provider, "gpt-6-astra", "Ajouter un bouton pour changer le thème."), "Changer le thème");
    EXPECT_EQ(server.worker.get().size(), 1);
}

TEST(ProviderAuthTest, CodexContextLimitsComeFromCatalogAndAreCachedAcrossModels) {
    TempAuth temp;
    auto store = std::make_shared<AuthStore>(temp.directory / "auth.json");
    store->set("openai", ProviderCredential{.kind = AuthKind::OAUTH, .access = "fake-token", .refresh = "fake-refresh",
        .accountId = "account", .expires = oauthNow() + 3600000, .method = AuthMethod::BROWSER});
    AuthHttpServer server({R"({"models":[
        {"slug":"gpt-6-astra","context_window":128000,"max_context_window":256000},
        {"slug":"fallback","max_context_window":64000},
        {"slug":"missing"},{"slug":"zero","context_window":0},
        {"slug":"negative","context_window":-1},{"slug":"string","context_window":"128000"},
        {"slug":"fraction","context_window":1.5},{"slug":"null","context_window":null}
    ]})"});
    OpenAIProvider provider(std::make_shared<OpenAIProviderAuth>(store), server.url, server.url,
        ModelCatalogEndpoints{.codex = server.url, .codexPublic = ""});
    EXPECT_EQ(provider.getModelInfo("gpt-6-astra").contextLimit, 128000);
    EXPECT_EQ(provider.getModelInfo("gpt-6-astra").maxContextLimit, 256000);
    EXPECT_EQ(provider.getModelInfo("missing").maxContextLimit, std::nullopt);
    EXPECT_EQ(provider.getContextLimit("fallback"), 64000);
    for (const auto* model : {"missing", "zero", "negative", "string", "fraction", "null", "absent"})
        EXPECT_EQ(provider.getContextLimit(model), std::nullopt);
    EXPECT_EQ(provider.getModelInfo("gpt-6-astra").inputLimit, std::nullopt);
    const auto requests = server.worker.get();
    ASSERT_EQ(requests.size(), 1);
    EXPECT_NE(requests.front().find("Authorization: Bearer fake-token"), std::string::npos);
    EXPECT_NE(requests.front().find("ChatGPT-Account-Id: account"), std::string::npos);
    store->set("openai", ProviderCredential{.key = "fake-api"});
    EXPECT_EQ(provider.getContextLimit("gpt-6-astra"), std::nullopt);
}

TEST(ProviderAuthTest, CodexReasoningLevelsUseCatalogIncludingNoneAndFastAliases) {
    TempAuth temp;
    auto store = std::make_shared<AuthStore>(temp.directory / "auth.json");
    store->set("openai", ProviderCredential{.kind = AuthKind::OAUTH, .access = "fake-token",
        .refresh = "fake-refresh", .expires = oauthNow() + 3600000, .method = AuthMethod::BROWSER});
    AuthHttpServer server({R"({"models":[
        {"slug":"gpt-5.4","context_window":128000,"visibility":"list","service_tiers":[{"id":"priority"}],
         "supported_reasoning_levels":[{"effort":"high"},{"effort":"none"},{"effort":"low"}]},
        {"slug":"gpt-5.3-codex","supported_reasoning_levels":[{"effort":"low"},{"effort":"high"}]}
    ]})", "data: {\"type\":\"response.completed\",\"response\":{\"output\":[]}}\n\n"});
    OpenAIProvider provider(std::make_shared<OpenAIProviderAuth>(store), server.url, server.url,
        ModelCatalogEndpoints{.codex = server.url, .codexPublic = ""});
    EXPECT_EQ(provider.getReasoningEfforts("gpt-5.4"), (std::vector<std::string>{"none", "low", "high"}));
    EXPECT_EQ(provider.getReasoningEfforts("gpt-5.4-fast"), provider.getReasoningEfforts("gpt-5.4"));
    EXPECT_EQ(provider.getReasoningEfforts("gpt-5.3-codex"), (std::vector<std::string>{"low", "high"}));
    provider.generate(Json::array(), "gpt-5.4-fast", {}, "none");
    const auto requests = server.worker.get();
    ASSERT_EQ(requests.size(), 2);
    const auto body = Json::parse(requests[1].substr(requests[1].find("\r\n\r\n") + 4));
    EXPECT_EQ(body["reasoning"]["effort"], "none");
    EXPECT_FALSE(body["reasoning"].contains("summary"));
    store->set("openai", ProviderCredential{.key = "fake-api"});
    EXPECT_EQ(provider.getReasoningEfforts("gpt-5.4").front(), "none");
    EXPECT_EQ(provider.getReasoningEfforts("o3").front(), "low");
    EXPECT_EQ(provider.getReasoningEfforts("gpt-5.4-pro").front(), "low");
}

TEST(ModelCatalogTest, SynthesizesOnlyAdvertisedFastModelsWithoutShadowingRealSlugs) {
    const auto catalog = Json::parse(R"({"models":[
        {"slug":"current","visibility":"list","service_tiers":[{"id":"priority"}]},
        {"slug":"legacy","visibility":"list","additional_speed_tiers":["fast"]},
        {"slug":"hidden","visibility":"hide","additional_speed_tiers":["fast"]},
        {"slug":"normal","visibility":"list"},
        {"slug":"malformed","visibility":"list","service_tiers":[null,{},"priority",{"id":42}],"additional_speed_tiers":null},
        {"slug":"collision","visibility":"list","additional_speed_tiers":["fast"]},
        {"slug":"collision-fast","visibility":"list"}
    ]})");
    const std::map<std::string, std::string> expected{{"current-fast", "current"}, {"legacy-fast", "legacy"}};
    EXPECT_EQ(ModelCatalog::codexFastModels(catalog), expected);
}

TEST(ModelCatalogTest, PublicCodexModelsFillMissingSlugsWithoutOverridingAccountDiscovery) {
    TempAuth temp;
    auto store = std::make_shared<AuthStore>(temp.directory / "auth.json");
    store->set("openai", ProviderCredential{.kind = AuthKind::OAUTH, .access = "fake-token",
        .refresh = "fake-refresh", .expires = oauthNow() + 3600000, .method = AuthMethod::BROWSER});
    AuthHttpServer server({R"({"models":[{"slug":"other","visibility":"list","priority":3}]})",
        R"({"models":[{"slug":"gpt-6.1-sol","visibility":"list","context_window":65536,"max_context_window":131072,"input_modalities":["text"]}]})",
        R"({"models":[{"slug":"gpt-6.1-sol","visibility":"hide"}]})"});
    AuthHttpServer publicServer({R"({"models":[
        {"slug":"gpt-6.1-sol","visibility":"list","supported_in_api":true,"context_window":272000,
          "max_context_window":872000,"input_modalities":["text","image"],"priority":2},
        {"slug":"gpt-7-future","visibility":"list","supported_in_api":true,"context_window":128000,"priority":1},
        {"slug":"unavailable","visibility":"list","supported_in_api":false}]})",
        R"({"models":[{"slug":"gpt-6.1-sol","visibility":"list","supported_in_api":true,"context_window":272000}]})",
        R"({"models":[{"slug":"gpt-6.1-sol","visibility":"list","supported_in_api":true}]})"});
    ModelCatalog catalog(ModelCatalogEndpoints{server.url, server.url, server.url, publicServer.url});
    const auto authorization = OpenAIProviderAuth(store).authorize({});
    const auto missing = catalog.listModelInfo("openai", authorization);
    ASSERT_EQ(missing.size(), 3);
    EXPECT_EQ(missing[1].id, "gpt-6.1-sol");
    EXPECT_EQ(missing[1].contextLimit, 272000);
    EXPECT_EQ(missing[1].maxContextLimit, 872000);
    EXPECT_EQ(missing[1].imageInput, ImageSupport::SUPPORTED);
    EXPECT_EQ(missing[0].id, "gpt-7-future");
    EXPECT_EQ(missing[2].id, "other");
    const auto present = catalog.listModelInfo("openai", authorization);
    ASSERT_EQ(present.size(), 1);
    EXPECT_EQ(present.front().id, "gpt-6.1-sol");
    EXPECT_EQ(present.front().contextLimit, 65536);
    EXPECT_EQ(present.front().maxContextLimit, 131072);
    EXPECT_EQ(present.front().imageInput, ImageSupport::UNSUPPORTED);
    EXPECT_TRUE(catalog.listModelInfo("openai", authorization).empty());
    EXPECT_EQ(server.worker.get().size(), 3);
    const auto publicRequests = publicServer.worker.get();
    ASSERT_EQ(publicRequests.size(), 3);
    EXPECT_EQ(publicRequests[0].find("fake-token"), std::string::npos);
    EXPECT_EQ(publicRequests[0].find("Authorization:"), std::string::npos);
}

TEST(ModelCatalogTest, PublicDiscoveryFailureKeepsAuthenticatedModels) {
    TempAuth temp;
    auto store = std::make_shared<AuthStore>(temp.directory / "auth.json");
    store->set("openai", ProviderCredential{.kind = AuthKind::OAUTH, .access = "fake-token",
        .refresh = "fake-refresh", .expires = oauthNow() + 3600000, .method = AuthMethod::BROWSER});
    AuthHttpServer account({R"({"models":[{"slug":"account-model","visibility":"list","context_window":64000}]})"});
    AuthHttpServer publicServer({R"({"error":"temporary"})"}, {503});
    ModelCatalog catalog(ModelCatalogEndpoints{account.url, account.url, account.url, publicServer.url});
    const auto models = catalog.listModelInfo("openai", OpenAIProviderAuth(store).authorize({}));
    ASSERT_EQ(models.size(), 1);
    EXPECT_EQ(models[0].id, "account-model");
    EXPECT_EQ(models[0].contextLimit, 64000);
    EXPECT_EQ(account.worker.get().size(), 1);
    EXPECT_EQ(publicServer.worker.get().size(), 1);
}

TEST(ProviderAuthTest, PublicCodexModelProvidesCapabilitiesAndUsesCodexWireModel) {
    TempAuth temp;
    auto store = std::make_shared<AuthStore>(temp.directory / "auth.json");
    store->set("openai", ProviderCredential{.kind = AuthKind::OAUTH, .access = "fake-token",
        .refresh = "fake-refresh", .accountId = "account", .expires = oauthNow() + 3600000, .method = AuthMethod::BROWSER});
    AuthHttpServer server({R"({"models":[{"slug":"other","visibility":"list"}]})",
        "data: {\"type\":\"response.completed\",\"response\":{\"output\":[]}}\n\n"});
    AuthHttpServer publicServer({R"({"models":[{"slug":"gpt-6.1-sol","visibility":"list",
        "supported_in_api":true,"context_window":272000,"max_context_window":872000,
        "input_modalities":["text","image"],"supported_reasoning_levels":[
        {"effort":"low"},{"effort":"medium"},{"effort":"high"},{"effort":"xhigh"},
        {"effort":"max"},{"effort":"ultra"}]}]})"});
    OpenAIProvider provider(std::make_shared<OpenAIProviderAuth>(store), server.url, server.url,
        ModelCatalogEndpoints{.codex = server.url, .codexPublic = publicServer.url});
    const auto info = provider.getModelInfo("gpt-6.1-sol");
    EXPECT_EQ(info.contextLimit, 272000);
    EXPECT_EQ(info.maxContextLimit, 872000);
    EXPECT_EQ(info.imageInput, ImageSupport::SUPPORTED);
    EXPECT_EQ(provider.getReasoningEfforts("gpt-6.1-sol"),
        (std::vector<std::string>{"low", "medium", "high", "xhigh", "max", "ultra"}));
    provider.generate(Json::array({{{"role", "user"}, {"content", "Hello"}}}), "gpt-6.1-sol", {}, "high");
    const auto requests = server.worker.get();
    ASSERT_EQ(requests.size(), 2);
    const auto body = Json::parse(requests[1].substr(requests[1].find("\r\n\r\n") + 4));
    EXPECT_EQ(body.at("model"), "gpt-6.1-sol");
    EXPECT_EQ(body.at("reasoning").at("effort"), "high");
    EXPECT_NE(requests[1].find("ChatGPT-Account-Id: account"), std::string::npos);
    const auto publicRequests = publicServer.worker.get();
    ASSERT_EQ(publicRequests.size(), 1);
    EXPECT_EQ(publicRequests[0].find("fake-token"), std::string::npos);
    EXPECT_EQ(publicRequests[0].find("Authorization:"), std::string::npos);
}

TEST(ModelCatalogTest, FastEntriesAreListedWithInheritedContextAndPersistAsSelection) {
    TempAuth temp;
    auto store = std::make_shared<AuthStore>(temp.directory / "auth.json");
    store->set("openai", ProviderCredential{.kind = AuthKind::OAUTH, .access = "fake-token",
        .refresh = "fake-refresh", .expires = oauthNow() + 3600000, .method = AuthMethod::BROWSER});
    AuthHttpServer server({R"({"models":[{"slug":"older","visibility":"list","priority":2},
        {"slug":"gpt-5.4","visibility":"list","priority":1,
        "context_window":128000,"max_context_window":256000,"additional_speed_tiers":["fast"]}]})"});
    ProviderConnections connections(store, temp.directory / "config.json", ProviderConnections::OAuthStarter{},
        ModelCatalog{ModelCatalogEndpoints{server.url, server.url, server.url, ""}});
    const auto listed = connections.listModels({});
    ASSERT_EQ(listed.models.size(), 3);
    EXPECT_EQ(listed.models[0].model, "gpt-5.4");
    EXPECT_EQ(listed.models[1].model, "gpt-5.4-fast");
    EXPECT_EQ(listed.models[2].model, "older");
    EXPECT_EQ(listed.models[1].maxContext, 256000);
    connections.select({"openai", "gpt-5.4-fast", "xhigh", 128000});
    EXPECT_EQ(connections.selection().model, "gpt-5.4-fast");
    EXPECT_EQ(connections.selection().reasoning, "xhigh");
    EXPECT_EQ(connections.selection().contextLimit, 128000);
    EXPECT_EQ(server.worker.get().size(), 1);
}

TEST(ProviderAuthTest, FastSelectionResolvesWireModelAndPreservesReasoningAndContext) {
    TempAuth temp;
    auto store = std::make_shared<AuthStore>(temp.directory / "auth.json");
    store->set("openai", ProviderCredential{.kind = AuthKind::OAUTH, .access = "fake-token",
        .refresh = "fake-refresh", .expires = oauthNow() + 3600000, .method = AuthMethod::BROWSER});
    const std::string completed = "data: {\"type\":\"response.completed\",\"response\":{\"output\":[]}}\n\n";
    AuthHttpServer server({R"({"models":[{"slug":"gpt-5.4","visibility":"list",
        "context_window":128000,"max_context_window":256000,"service_tiers":[{"id":"priority"}]},
        {"slug":"native-fast","visibility":"list"}]})", completed, completed, completed});
    OpenAIProvider provider(std::make_shared<OpenAIProviderAuth>(store), server.url, server.url,
        ModelCatalogEndpoints{.codex = server.url, .codexPublic = ""});
    const auto input = Json::array({{{"role", "user"}, {"content", "Hello"}}});
    provider.generate(input, "gpt-5.4-fast", {}, "xhigh");
    const auto info = provider.getModelInfo("gpt-5.4-fast");
    EXPECT_EQ(info.id, "gpt-5.4-fast");
    EXPECT_EQ(info.contextLimit, 128000);
    EXPECT_EQ(info.maxContextLimit, 256000);
    EXPECT_EQ(provider.getReasoningEfforts("gpt-5.4-fast"), provider.getReasoningEfforts("gpt-5.4"));
    provider.generate(input, "gpt-5.4", {}, "high");
    provider.generate(input, "native-fast");
    EXPECT_THROW(provider.generate(input, "unknown-fast"), std::runtime_error);
    const auto requests = server.worker.get();
    ASSERT_EQ(requests.size(), 4);
    const auto body = [&](std::size_t i) { return Json::parse(requests[i].substr(requests[i].find("\r\n\r\n") + 4)); };
    EXPECT_EQ(body(1)["model"], "gpt-5.4");
    EXPECT_EQ(body(1)["service_tier"], "priority");
    EXPECT_EQ(body(1)["reasoning"]["effort"], "xhigh");
    EXPECT_FALSE(body(2).contains("service_tier"));
    EXPECT_EQ(body(3)["model"], "native-fast");
    EXPECT_FALSE(body(3).contains("service_tier"));
    store->set("openai", ProviderCredential{.key = "fake-api"});
    EXPECT_THROW(provider.generate(input, "gpt-5.4-fast"), std::runtime_error);
}

TEST(ProviderAuthTest, InvalidCodexCatalogIsNotAnUnknownCapabilityAndCanBeFetchedAgain) {
    TempAuth temp;
    auto store = std::make_shared<AuthStore>(temp.directory / "auth.json");
    store->set("openai", ProviderCredential{.kind = AuthKind::OAUTH, .access = "fake-token", .refresh = "fake-refresh",
        .expires = oauthNow() + 3600000, .method = AuthMethod::BROWSER});
    AuthHttpServer server({"not-json", R"({"models":[{"slug":"model","context_window":64000}]})"});
    OpenAIProvider provider(std::make_shared<OpenAIProviderAuth>(store), server.url, server.url,
        ModelCatalogEndpoints{.codex = server.url, .codexPublic = ""});
    EXPECT_THROW(provider.getContextLimit("model"), std::runtime_error);
    EXPECT_EQ(provider.getContextLimit("model"), 64000);
    EXPECT_EQ(server.worker.get().size(), 2);
    provider.setCancellationCheck([] { return true; });
    EXPECT_THROW(provider.getContextLimit("model"), core::Cancelled);
}

TEST(ProviderAuthTest, OpenAIUsesApiOrCodexEndpointAndNormalizesCompletedStream) {
    TempAuth temp;
    auto store = std::make_shared<AuthStore>(temp.directory / "auth.json");
    store->set("openai", ProviderCredential{.key = "fake-api"});
    AuthHttpServer api({R"({"id":"api-result","output":[]})"});
    AuthHttpServer codex({"event: response.completed\r\ndata: {\"type\":\"response.completed\",\"response\":{\"id\":\"codex-result\",\"output\":[]}}\r\n\r\n"});
    OpenAIProvider provider(std::make_shared<OpenAIProviderAuth>(store), api.url, codex.url);
    provider.setSessionId("conversation-identity");
    provider.setWebSocketEnabled(false);
    const auto input = Json::array({{{"type", "message"}, {"role", "system"}, {"content", "Instructions"}},
        {{"type", "message"}, {"role", "user"}, {"content", "Hello"}}});
    EXPECT_EQ(provider.generate(input, "gpt-5.4").at("id"), "api-result");
    store->set("openai", ProviderCredential{.kind = AuthKind::OAUTH, .access = "fake-token", .refresh = "fake-refresh",
        .accountId = "account", .expires = oauthNow() + 3600000, .method = AuthMethod::BROWSER});
    EXPECT_EQ(provider.generate(input, "gpt-6-astra", {}, "xhigh").at("id"), "codex-result");
    const auto request = codex.worker.get().at(0);
    EXPECT_NE(request.find("Authorization: Bearer fake-token"), std::string::npos);
    EXPECT_NE(request.find("ChatGPT-Account-Id: account"), std::string::npos);
    const auto payload = Json::parse(request.substr(request.find("\r\n\r\n") + 4));
    EXPECT_EQ(payload.at("stream"), true);
    EXPECT_EQ(payload.at("reasoning").at("effort"), "xhigh");
    EXPECT_EQ(payload.at("instructions"), "Follow the conversation instructions.");
    auto expectedCodexInput = input;
    expectedCodexInput[0].erase("type");
    EXPECT_EQ(payload.at("input"), expectedCodexInput);
    EXPECT_TRUE(input[0].contains("type"));
    EXPECT_EQ(payload.at("prompt_cache_key"), "conversation-identity");
    EXPECT_NE(request.find("session-id: conversation-identity\r\n"), std::string::npos);
    const auto apiRequest = api.worker.get().at(0);
    EXPECT_NE(apiRequest.find("Authorization: Bearer fake-api"), std::string::npos);
    EXPECT_EQ(apiRequest.find("session-id:"), std::string::npos);
    EXPECT_EQ(Json::parse(apiRequest.substr(apiRequest.find("\r\n\r\n") + 4))["input"], input);
    EXPECT_FALSE(Json::parse(apiRequest.substr(apiRequest.find("\r\n\r\n") + 4)).contains("prompt_cache_key"));
    EXPECT_THROW(completedResponsesEvent("data: {\"type\":\"response.failed\"}\n\n"), std::runtime_error);
    EXPECT_THROW(completedResponsesEvent("data: [DONE]\n\n"), std::runtime_error);
}

TEST(ProviderAuthTest, CodexAppendsPhaseAndPermissionInstructionsWithoutRewritingThePrefix) {
    TempAuth temp;
    auto store = std::make_shared<AuthStore>(temp.directory / "auth.json");
    store->set("openai", ProviderCredential{.kind = AuthKind::OAUTH, .access = "fake-token", .refresh = "fake-refresh",
        .expires = oauthNow() + 3600000, .method = AuthMethod::BROWSER});
    const std::string completed = "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"result\",\"output\":[]}}\n\n";
    AuthHttpServer server({completed, completed, completed});
    OpenAIProvider provider(std::make_shared<OpenAIProviderAuth>(store), server.url, server.url);
    provider.setWebSocketEnabled(false);
    provider.setSessionId("stable-session");
    Json input = Json::array({{{"type", "message"}, {"role", "system"}, {"content", "Base rules"}},
        {{"type", "message"}, {"role", "developer"}, {"content", Json::array({{{"type", "input_text"}, {"text", "Project rules"}}})}},
        {{"role", "user"}, {"content", "Build"}}});
    std::vector<Json> histories;
    for (const auto* announcement : {"", "Current implementation phase: execution", "Manual permission changed"}) {
        if (*announcement) {
            input.push_back({{"type", "message"}, {"role", "assistant"}, {"content", "Previous result"}});
            input.push_back({{"type", "message"}, {"role", "system"},
                {"content", Json::array({{{"type", "input_text"}, {"text", announcement}}})}});
            input.push_back({{"role", "user"}, {"content", "Continue"}});
        }
        histories.push_back(input);
        provider.generate(input, "model");
        EXPECT_EQ(input, histories.back());
    }
    const auto requests = server.worker.get();
    ASSERT_EQ(requests.size(), histories.size());
    Json previous;
    for (std::size_t i = 0; i < requests.size(); ++i) {
        const auto payload = Json::parse(requests[i].substr(requests[i].find("\r\n\r\n") + 4));
        auto expected = histories[i];
        for (auto& item : expected) if (item.value("role", "") == "system") item.erase("type");
        EXPECT_EQ(payload["input"], expected);
        EXPECT_EQ(payload["instructions"], "Follow the conversation instructions.");
        EXPECT_EQ(payload["prompt_cache_key"], "stable-session");
        if (i) {
            EXPECT_EQ(payload["instructions"], previous["instructions"]);
            for (std::size_t item = 0; item < previous["input"].size(); ++item)
                EXPECT_EQ(payload["input"][item], previous["input"][item]);
        }
        previous = payload;
    }
}

TEST(ProviderAuthTest, CodexSessionIdentityIsStableAndIsolatedAcrossProviders) {
    TempAuth temp;
    auto store = std::make_shared<AuthStore>(temp.directory / "auth.json");
    store->set("openai", ProviderCredential{.kind = AuthKind::OAUTH, .access = "fake-token", .refresh = "fake-refresh",
        .expires = oauthNow() + 3600000, .method = AuthMethod::BROWSER});
    const std::string reply = "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"result\",\"output\":[]}}\n\n";
    AuthHttpServer server({reply, reply, reply});
    OpenAIProvider first(std::make_shared<OpenAIProviderAuth>(store), server.url, server.url);
    OpenAIProvider second(std::make_shared<OpenAIProviderAuth>(store), server.url, server.url);
    first.setSessionId("first-conversation");
    second.setSessionId("second-conversation");
    first.setWebSocketEnabled(false);
    second.setWebSocketEnabled(false);
    const auto input = Json::array({{{"role", "user"}, {"content", "Hello"}}});
    first.generate(input, "test");
    first.generate(input, "test");
    second.generate(input, "test");
    const auto requests = server.worker.get();
    for (std::size_t i = 0; i < requests.size(); ++i) {
        const auto id = i < 2 ? "first-conversation" : "second-conversation";
        EXPECT_NE(requests[i].find(std::string("session-id: ") + id + "\r\n"), std::string::npos);
        EXPECT_EQ(Json::parse(requests[i].substr(requests[i].find("\r\n\r\n") + 4)).at("prompt_cache_key"), id);
    }
    EXPECT_THROW(first.setSessionId("invalid\r\nheader"), std::invalid_argument);
}

TEST(ProviderAuthTest, UnavailableWebSocketFallsBackBeforeGenerationAndStaysOnHttp) {
    TempAuth temp;
    auto store = std::make_shared<AuthStore>(temp.directory / "auth.json");
    store->set("openai", ProviderCredential{.kind = AuthKind::OAUTH, .access = "fake-token", .refresh = "fake-refresh",
        .expires = oauthNow() + 3600000, .method = AuthMethod::BROWSER});
    const std::string reply = "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"result\",\"output\":[]}}\n\n";
    AuthHttpServer server({"{}", reply, reply});
    OpenAIProvider provider(std::make_shared<OpenAIProviderAuth>(store), server.url, server.url);
    provider.setSessionId("conversation");
    const auto input = Json::array({{{"role", "user"}, {"content", "Hello"}}});
    provider.generate(input, "test");
    provider.generate(input, "test");
    const auto requests = server.worker.get();
    ASSERT_EQ(requests.size(), 3);
    EXPECT_TRUE(requests[0].starts_with("GET "));
    EXPECT_TRUE(requests[1].starts_with("POST "));
    EXPECT_TRUE(requests[2].starts_with("POST "));
    EXPECT_EQ(provider.webSocketStatistics().requests, 0);
}

class ProviderTransportTest : public testing::TestWithParam<std::string> {
protected:
    TempAuth temp;
    std::shared_ptr<AuthStore> store = std::make_shared<AuthStore>(temp.directory / "auth.json");
    void SetUp() override {
        if (streaming())
            store->set("openai", ProviderCredential{.kind = AuthKind::OAUTH, .access = "fake-private-token",
                .refresh = "fake-refresh", .expires = oauthNow() + 3600000, .method = AuthMethod::BROWSER});
        else store->set("openai", ProviderCredential{.key = "fake-private-token"});
    }
    bool streaming() const { return GetParam() == "Codex"; }
    std::string providerName() const { return GetParam() == "OpenRouter" ? "OpenRouter" : "OpenAI"; }
    std::unique_ptr<Provider> providerFor(const std::string& url, ProviderRequestTimeouts timeouts = {}) {
        if (GetParam() == "OpenRouter")
            return std::make_unique<OpenRouterProvider>("fake-private-token", url, url, timeouts);
        return std::make_unique<OpenAIProvider>(std::make_shared<OpenAIProviderAuth>(store), url, url,
            ModelCatalogEndpoints{}, timeouts);
    }
    std::string heartbeat() const { return streaming() ? ": heartbeat\n\n" : " \n"; }
    std::string complete() const {
        if (!streaming()) return R"({"id":"done","output":[]})";
        return "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"done\",\"output\":[]}}\n\n";
    }
    void expectError(Provider& provider, const std::string& expected) const {
        try {
            provider.generate(Json::array(), "gpt-6-astra");
            FAIL() << "Expected transport failure";
        } catch (const std::runtime_error& error) {
            EXPECT_NE(std::string(error.what()).find(providerName() + " " + expected), std::string::npos) << error.what();
            EXPECT_EQ(std::string(error.what()).find("fake-private-token"), std::string::npos);
        }
    }
};

TEST(ProviderTimeoutTest, FormatsHumanReadableDurations) {
    using namespace std::chrono_literals;
    EXPECT_EQ(formatProviderTimeout(10min), "10 minutes");
    EXPECT_EQ(formatProviderTimeout(1min), "1 minute");
    EXPECT_EQ(formatProviderTimeout(15s), "15 seconds");
    EXPECT_EQ(formatProviderTimeout(1s), "1 second");
    EXPECT_EQ(formatProviderTimeout(500ms), "500 milliseconds");
    EXPECT_EQ(formatProviderTimeout(1ms), "1 millisecond");
}

TEST_P(ProviderTransportTest, DefaultsAllowTenMinutesTotalAndIdle) {
    EXPECT_EQ(ProviderRequestTimeouts{}.total, std::chrono::minutes(10));
    EXPECT_EQ(ProviderRequestTimeouts{}.idle, std::chrono::minutes(10));
    EXPECT_EQ(formatProviderTimeout(ProviderRequestTimeouts{}.total), "10 minutes");
    EXPECT_EQ(formatProviderTimeout(ProviderRequestTimeouts{}.idle), "10 minutes");
    EXPECT_THROW(providerFor("", {.total = std::chrono::milliseconds(0)}), std::invalid_argument);
    EXPECT_THROW(providerFor("", {.idle = std::chrono::milliseconds(-1)}), std::invalid_argument);
}

TEST_P(ProviderTransportTest, HttpQuotaErrorsNeverRetry) {
    for (const auto* quota : {"insufficient_quota", "usage_limit_reached"}) {
        AuthHttpServer server({Json({{"error", {{"type", quota}, {"message", "fake-private-token"}}}}).dump()}, {429});
        auto provider = providerFor(server.url);
        int calls = 0;
        try {
            generateWithRetry([&] {
                ++calls;
                return provider->generate(Json::array(), "gpt-6-astra");
            }, {}, [](auto) { FAIL() << "Quota error retried"; });
            FAIL() << "Expected quota error";
        } catch (const RetryableGenerationError&) {
            FAIL() << "Quota classified as transient";
        } catch (const std::runtime_error& error) {
            EXPECT_NE(std::string(error.what()).find(providerName() + " quota exhausted"), std::string::npos);
            EXPECT_EQ(std::string(error.what()).find("fake-private-token"), std::string::npos);
        }
        EXPECT_EQ(calls, 1);
        EXPECT_EQ(server.worker.get().size(), 1);
    }
}

TEST_P(ProviderTransportTest, UnknownGenerationFailureRetriesTheSameRequest) {
    const Json failure{{"status", "failed"}, {"error", {{"message", "fake-private-token"}}}};
    const auto body = streaming()
        ? "data: " + Json({{"type", "response.failed"}, {"response", failure}}).dump() + "\n\n"
        : failure.dump();
    AuthHttpServer server({body, complete()});
    auto provider = providerFor(server.url);
    int calls = 0;
    const auto result = generateWithRetry([&] {
        ++calls;
        return provider->generate(Json::array(), "gpt-6-astra");
    }, {}, [](const auto& status) {
        EXPECT_EQ(status.error.find("fake-private-token"), std::string::npos);
    }, [](auto) { FAIL() << "First retry should be immediate"; });
    EXPECT_EQ(result.at("id"), "done");
    EXPECT_EQ(calls, 2);
    const auto requests = server.worker.get();
    ASSERT_EQ(requests.size(), 2);
    EXPECT_EQ(requests[0], requests[1]);
}

TEST_P(ProviderTransportTest, IncompleteResponseWithoutPermanentCauseRetries) {
    const Json failure{{"status", "incomplete"}, {"error", nullptr}};
    const auto body = streaming()
        ? "data: " + Json({{"type", "response.incomplete"}, {"response", failure}}).dump() + "\n\n"
        : failure.dump();
    AuthHttpServer server({body, complete()});
    auto provider = providerFor(server.url);
    int calls = 0;
    const auto result = generateWithRetry([&] {
        ++calls;
        return provider->generate(Json::array(), "gpt-6-astra");
    }, {}, {}, [](auto) { FAIL() << "First retry should be immediate"; });
    EXPECT_EQ(result.at("id"), "done");
    EXPECT_EQ(calls, 2);
    EXPECT_EQ(server.worker.get().size(), 2);
}

TEST_P(ProviderTransportTest, StructuredFailureCauseIsPreserved) {
    AuthHttpServer server({streaming()
        ? "data: {\"type\":\"response.failed\",\"response\":{\"error\":{\"code\":\"invalid_api_key\",\"message\":\"fake-private-token\"}}}\n\n"
        : R"({"error":{"code":"invalid_api_key","message":"fake-private-token"}})"});
    auto provider = providerFor(server.url);
    expectError(*provider, "API error: invalid_api_key");
    EXPECT_EQ(server.worker.get().size(), 1);
}

TEST_P(ProviderTransportTest, IncomingDataKeepsResponseAliveBeyondIdleLimit) {
    using namespace std::chrono_literals;
    std::vector<AuthHttpServer::Chunk> chunks(10, {200ms, heartbeat()});
    chunks.emplace_back(0ms, complete());
    AuthHttpServer server({""}, {}, false, chunks);
    auto provider = providerFor(server.url, {.total = 5s, .idle = 1s});
    EXPECT_EQ(provider->generate(Json::array(), "gpt-6-astra").at("id"), "done");
    EXPECT_EQ(server.worker.get().size(), 1);
}

TEST_P(ProviderTransportTest, SilentServerTimesOutWithoutWaitingForever) {
    using namespace std::chrono_literals;
    AuthHttpServer server({""}, {}, true);
    auto provider = providerFor(server.url, {.total = 2s, .idle = 500ms});
    expectError(*provider, streaming() ? "inactivity timeout: no response data received for 500 milliseconds"
                                      : "request timeout (total limit: 2 seconds; connection limit: 15 seconds)");
    EXPECT_EQ(server.worker.get().size(), 1);
}

TEST_P(ProviderTransportTest, NonStreamingGenerationCanWaitBeyondIdleLimitForFirstBytes) {
    if (streaming()) GTEST_SKIP() << "Streaming responses are monitored from the start";
    using namespace std::chrono_literals;
    AuthHttpServer server({""}, {}, false, {{2s, complete()}});
    auto provider = providerFor(server.url, {.total = 5s, .idle = 500ms});
    EXPECT_EQ(provider->generate(Json::array(), "gpt-6-astra").at("id"), "done");
    EXPECT_EQ(server.worker.get().size(), 1);
}

TEST_P(ProviderTransportTest, ResponseThatStopsSendingDataHitsIdleTimeout) {
    using namespace std::chrono_literals;
    AuthHttpServer server({""}, {}, false, {{0ms, heartbeat()}, {4s, complete()}});
    auto provider = providerFor(server.url, {.total = 5s, .idle = 500ms});
    expectError(*provider, "inactivity timeout");
    EXPECT_EQ(server.worker.get().size(), 1);
}

TEST_P(ProviderTransportTest, TotalLimitStillAppliesToAnActiveResponse) {
    using namespace std::chrono_literals;
    std::vector<AuthHttpServer::Chunk> chunks(30, {100ms, heartbeat()});
    chunks.emplace_back(0ms, complete());
    AuthHttpServer server({""}, {}, false, chunks);
    auto provider = providerFor(server.url, {.total = 2s, .idle = 1s});
    expectError(*provider, "request timeout (total limit: 2 seconds; connection limit: 15 seconds)");
    EXPECT_EQ(server.worker.get().size(), 1);
}

TEST_P(ProviderTransportTest, CancellationRemainsDistinctFromTimeouts) {
    using namespace std::chrono_literals;
    AuthHttpServer server({""}, {}, true);
    auto provider = providerFor(server.url, {.total = 5s, .idle = 3s});
    std::atomic<bool> cancelled = false;
    provider->setCancellationCheck([&] { return cancelled.load(); });
    auto result = std::async(std::launch::async, [&] { return provider->generate(Json::array(), "gpt-6-astra"); });
    ASSERT_EQ(server.entered.get_future().wait_for(2s), std::future_status::ready);
    cancelled = true;
    ASSERT_EQ(result.wait_for(2s), std::future_status::ready);
    EXPECT_THROW(result.get(), core::Cancelled);
    EXPECT_EQ(server.worker.get().size(), 1);
}

TEST_P(ProviderTransportTest, NetworkFailuresExposeOnlyTheirCategory) {
    auto provider = providerFor("unsupported://fake-private-token");
    expectError(*provider, "transport error (UNSUPPORTED_PROTOCOL)");
}

TEST_P(ProviderTransportTest, TemporaryHttpFailureRetriesTheSameRequest) {
    AuthHttpServer server({"private upstream details", complete()}, {503, 200});
    auto provider = providerFor(server.url);
    std::vector<GenerationRetryStatus> statuses;
    const auto result = generateWithRetry([&] { return provider->generate(Json::array(), "gpt-6-astra"); }, {},
        [&](const auto& status) { statuses.push_back(status); });
    EXPECT_EQ(result.at("id"), "done");
    const auto requests = server.worker.get();
    ASSERT_EQ(requests.size(), 2);
    EXPECT_EQ(requests[0], requests[1]);
    ASSERT_EQ(statuses.size(), 2);
    EXPECT_EQ(statuses[0].reason, "Service temporarily unavailable");
    EXPECT_EQ(statuses[0].error, providerName() + " HTTP error 503");
    EXPECT_EQ(statuses[0].error.find("private upstream details"), std::string::npos);
    EXPECT_EQ(statuses[1].attempt, 0);
}

TEST_P(ProviderTransportTest, TruncatedResponseRetriesWithoutReplayingPartialOutput) {
    AuthHttpServer server({streaming() ? "data: {\"type\":\"response.created\"}\n\n" : "{", complete()});
    auto provider = providerFor(server.url);
    const auto result = generateWithRetry([&] { return provider->generate(Json::array(), "gpt-6-astra"); }, {}, {});
    EXPECT_EQ(result.at("id"), "done");
    EXPECT_EQ(server.worker.get().size(), 2);
}

TEST_P(ProviderTransportTest, InvalidCredentialsDoNotTriggerRetries) {
    AuthHttpServer server({"private credential details"}, {401});
    auto provider = providerFor(server.url);
    EXPECT_THROW(generateWithRetry([&] { return provider->generate(Json::array(), "gpt-6-astra"); }, {},
        [](const auto&) { FAIL() << "Authentication must not retry"; }), std::runtime_error);
    EXPECT_EQ(server.worker.get().size(), 1);
}

TEST_P(ProviderTransportTest, CatalogFailureRetriesInsteadOfReportingUnknownImageSupport) {
    if (GetParam() == "OpenAI") GTEST_SKIP() << "API image capabilities are resolved locally";
    const auto catalog = streaming()
        ? R"({"models":[{"slug":"model","input_modalities":["text","image"]}]})"
        : R"({"data":[{"id":"model","architecture":{"input_modalities":["text","image"]}}]})";
    AuthHttpServer server({"temporary failure", catalog}, {503, 200});
    std::unique_ptr<Provider> provider;
    if (streaming()) provider = std::make_unique<OpenAIProvider>(std::make_shared<OpenAIProviderAuth>(store),
        server.url, server.url, ModelCatalogEndpoints{.codex = server.url, .codexPublic = ""});
    else provider = providerFor(server.url);
    ModelInfo info;
    std::vector<GenerationRetryStatus> events;
    runWithRetry([&] { info = provider->getModelInfo("model"); }, {},
        [&](const auto& status) { events.push_back(status); });
    EXPECT_EQ(info.imageInput, ImageSupport::SUPPORTED);
    ASSERT_EQ(events.size(), 2);
    EXPECT_EQ(events[0].attempt, 1);
    EXPECT_EQ(events[0].reason, "Service temporarily unavailable");
    EXPECT_EQ(events[1].attempt, 0);
    EXPECT_EQ(server.worker.get().size(), 2);
}

TEST_P(ProviderTransportTest, CatalogConnectionFailureIsNotAnImageCapabilityResult) {
    if (GetParam() == "OpenAI") GTEST_SKIP() << "API image capabilities are resolved locally";
    // Keep a bound non-listening socket so the port cannot be reused by another test.
    const int socket = ::socket(AF_INET, SOCK_STREAM, 0);
    struct Close { int fd; ~Close() { ::close(fd); } } close{socket};
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(::bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    socklen_t size = sizeof(address);
    ASSERT_EQ(::getsockname(socket, reinterpret_cast<sockaddr*>(&address), &size), 0);
    const auto url = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port));
    std::unique_ptr<Provider> provider;
    if (streaming()) provider = std::make_unique<OpenAIProvider>(std::make_shared<OpenAIProviderAuth>(store),
        url, url, ModelCatalogEndpoints{.codex = url, .codexPublic = ""});
    else provider = providerFor(url);
    EXPECT_THROW(provider->getModelInfo("model"), RetryableGenerationError);
}

INSTANTIATE_TEST_SUITE_P(AllProviders, ProviderTransportTest, testing::Values("Codex", "OpenAI", "OpenRouter"),
    [](const testing::TestParamInfo<std::string>& info) { return info.param; });
}

namespace {
TEST(CustomProviderTest, CreatesNamedInstancesDiscoversModelsAndPersistsSeparateCredentials) {
    TempAuth temp;
    const std::string catalog = R"({"data":[{"id":"llama"},{"id":"llama"},{"id":"qwen"},{"id":""}]})";
    AuthHttpServer server({catalog, catalog, catalog, catalog});
    auto auth = std::make_shared<AuthStore>(temp.directory / "auth.json");
    auto providers = std::make_shared<ProviderConnections>(auth, temp.directory / "config.json");
    ProviderConnection connection(providers);
    asio::io_context io;
    const auto handle = [&](const Json& json) {
        io.restart();
        const auto command = json.get<ProviderRequest>();
        connection.prepare(command);
        auto result = asio::co_spawn(io, connection.handle(command), asio::use_future);
        io.run();
        return result.get();
    };
    const auto created = handle({{"type", "provider.create"}, {"name", "Local server"}, {"base_url", server.url + "/v1/"}, {"key", "fake-key"}});
    const auto id = created.at("created_provider").get<std::string>();
    ASSERT_TRUE(id.starts_with("custom-"));
    EXPECT_EQ(created["providers"].size(), 3);
    EXPECT_EQ(created["providers"][2]["name"], "Local server");
    EXPECT_EQ(created.dump().find("fake-key"), std::string::npos);
    EXPECT_EQ(providers->selection().provider, "openrouter"); // Creation is not selection.
    const auto second = providers->addCustom("Another server", server.url + "/v1", "");
    EXPECT_NE(second, id);
    EXPECT_THROW(providers->addCustom("Local server", server.url, ""), std::invalid_argument);
    EXPECT_FALSE(auth->contains(second));
    EXPECT_EQ(auth->get(id).key, "fake-key");
    EXPECT_EQ(readPrivateJson(temp.directory / "config.json").dump().find("fake-key"), std::string::npos);
    EXPECT_EQ(handle({{"type", "provider.select"}, {"provider", id}})["type"], "provider.selected");
    EXPECT_EQ(providers->selection().model, "llama");
    const auto models = handle({{"type", "model.list"}});
    ASSERT_EQ(models["models"].size(), 4);
    const auto row = std::find_if(models["models"].begin(), models["models"].end(), [&](const auto& model) {
        return model.at("provider") == id;
    });
    ASSERT_NE(row, models["models"].end());
    EXPECT_EQ((*row)["provider_name"], "Local server");
    EXPECT_TRUE((*row)["max_context"].is_null());
    EXPECT_EQ((*row)["image_input"], "unknown");
    EXPECT_EQ(handle({{"type", "model.select"}, {"provider", second}, {"model", "qwen"}, {"context_limit", 65536}})["type"], "model.selected");
    ProviderConnections restored(auth, temp.directory / "config.json");
    EXPECT_EQ(restored.selection().provider, second);
    EXPECT_EQ(restored.selection().contextLimit, 65536);
    EXPECT_EQ(restored.create(second)->getFormat(), ProviderFormat::OPENAI_RESPONSES);
    EXPECT_THROW(restored.deleteCustom(second), std::invalid_argument);
    EXPECT_EQ(handle({{"type", "provider.delete"}, {"provider", id}})["providers"].size(), 3);
    EXPECT_FALSE(auth->contains(id));
    EXPECT_THROW(providers->create(id), std::runtime_error);
    const auto requests = server.worker.get();
    ASSERT_EQ(requests.size(), 4);
    EXPECT_TRUE(requests[0].starts_with("GET /v1/models "));
    EXPECT_NE(requests[0].find("Authorization: Bearer fake-key"), std::string::npos);
    EXPECT_EQ(std::count_if(requests.begin() + 2, requests.end(), [](const auto& request) {
        return request.find("Authorization:") == std::string::npos;
    }), 1);
    struct stat mode{};
    ASSERT_EQ(::stat((temp.directory / "auth.json").c_str(), &mode), 0);
    EXPECT_EQ(mode.st_mode & 0777, 0600);
}

TEST(CustomProviderTest, DiscoveryErrorsAndCancellationNeverSaveCandidates) {
    TempAuth temp;
    AuthHttpServer server({R"({"data":[]})", "not-json", R"({"error":"private body"})"}, {200, 200, 401});
    auto auth = std::make_shared<AuthStore>(temp.directory / "auth.json");
    auto providers = std::make_shared<ProviderConnections>(auth, temp.directory / "config.json");
    ProviderConnection connection(providers);
    for (int i = 0; i < 3; ++i) {
        asio::io_context io;
        const auto command = Json{{"type", "provider.create"}, {"name", "Bad server"}, {"base_url", server.url}, {"key", "candidate"}}.get<ProviderRequest>();
        connection.prepare(command);
        auto result = asio::co_spawn(io, connection.handle(command), asio::use_future);
        io.run();
        EXPECT_THROW(result.get(), std::runtime_error);
        EXPECT_EQ(providers->list(providers->selection()).size(), 2);
        EXPECT_FALSE(std::filesystem::exists(temp.directory / "auth.json"));
    }
    server.worker.get();
    asio::io_context io;
    const auto command = Json{{"type", "provider.create"}, {"name", "Cancelled"}, {"base_url", server.url}}.get<ProviderRequest>();
    connection.prepare(command);
    connection.cancel();
    auto result = asio::co_spawn(io, connection.handle(command), asio::use_future);
    io.run();
    EXPECT_THROW(result.get(), core::Cancelled);
    EXPECT_FALSE(std::filesystem::exists(temp.directory / "config.json"));
}

TEST(CustomProviderTest, SendsClassicChatWithToolsAndOptionalBearerOnly) {
    const auto reply = R"({"id":"chat-1","choices":[{"finish_reason":"stop","message":{"role":"assistant","content":"Hello"}}],"usage":{"prompt_tokens":12,"completion_tokens":3}})";
    AuthHttpServer server({reply, reply});
    ChatCompletionsProvider anonymous(server.url + "/v1", "", "Local");
    const auto input = Json::array({{{"role", "user"}, {"content", "Hello"}}});
    EXPECT_EQ(anonymous.generate(input, "llama")["output"][0]["content"][0]["text"], "Hello");
    ChatCompletionsProvider authenticated(server.url + "/v1", "fake-key", "Remote");
    EXPECT_EQ(authenticated.generate(input, "qwen", {{"read", "Read file", Json::object()}}, "high")["usage"]["input_tokens"], 12);
    const auto requests = server.worker.get();
    ASSERT_EQ(requests.size(), 2);
    EXPECT_TRUE(requests[0].starts_with("POST /v1/chat/completions "));
    EXPECT_EQ(requests[0].find("Authorization:"), std::string::npos);
    EXPECT_NE(requests[1].find("Authorization: Bearer fake-key"), std::string::npos);
    const auto body = Json::parse(requests[1].substr(requests[1].find("\r\n\r\n") + 4));
    EXPECT_TRUE(body.contains("messages"));
    EXPECT_TRUE(body.contains("tools"));
    EXPECT_FALSE(body.contains("input"));
    EXPECT_FALSE(body.contains("reasoning"));
    EXPECT_FALSE(body.contains("store"));
}
}

namespace {
TEST(CustomProviderTest, CancelsDiscoveryInFlightWithoutSavingDefinitionOrKey) {
    TempAuth temp;
    AuthHttpServer server({R"({"data":[{"id":"model"}]})"}, {}, true);
    auto providers = std::make_shared<ProviderConnections>(std::make_shared<AuthStore>(temp.directory / "auth.json"),
        temp.directory / "config.json");
    ProviderConnection connection(providers);
    asio::io_context io;
    const auto command = Json{{"type", "provider.create"}, {"name", "Cancelled"},
        {"base_url", server.url}, {"key", "candidate"}}.get<ProviderRequest>();
    connection.prepare(command);
    auto result = asio::co_spawn(io, connection.handle(command), asio::use_future);
    auto loop = std::async(std::launch::async, [&] { io.run(); });
    ASSERT_EQ(server.entered.get_future().wait_for(std::chrono::seconds(3)), std::future_status::ready);
    connection.cancel();
    EXPECT_THROW(result.get(), core::Cancelled);
    loop.get();
    EXPECT_FALSE(std::filesystem::exists(temp.directory / "auth.json"));
    EXPECT_FALSE(std::filesystem::exists(temp.directory / "config.json"));
}
}
