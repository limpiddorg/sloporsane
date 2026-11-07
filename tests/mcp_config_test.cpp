#include <daemon/mcp/config.hpp>
#include <daemon/mcp/auth_store.hpp>
#include <daemon/mcp/json_file.hpp>
#include <agent-runtime/common/path_resolution.hpp>
#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <thread>
#include <sys/stat.h>

namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
class McpConfigTest : public testing::Test {
protected:
    fs::path root, work;
    std::map<std::string, std::optional<std::string>> environment;
    void SetUp() override {
        root = fs::temp_directory_path() / ("sos-mcp-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        work = root / "work";
        fs::create_directories(work);
        root = fs::canonical(root);
        work = root / "work";
        for (const auto* name : {"XDG_CONFIG_HOME", "XDG_DATA_HOME"}) {
            if (const auto* value = std::getenv(name)) environment[name] = value;
            else environment[name] = std::nullopt;
            setenv(name, (root / name).c_str(), 1);
        }
    }
    void TearDown() override {
        for (const auto& [name, value] : environment)
            if (value) setenv(name.c_str(), value->c_str(), 1); else unsetenv(name.c_str());
        fs::remove_all(root);
    }
    mcp::Server remote(std::string name = "docs") {
        mcp::Server result;
        result.name = std::move(name);
        result.connection = mcp::Remote{"https://example.test/mcp", mcp::Authentication::TOKEN};
        return result;
    }
};

TEST_F(McpConfigTest, NotionExamplesLoadWithoutSosFieldsAndSaveInPortableFormat) {
    mcp::ConfigStore store(work);
    mcp::JsonFile file(store.path(mcp::Scope::WORKSPACE));
    for (const auto* example : {
        R"({"mcpServers":{"notion":{"url":"https://mcp.notion.com/mcp"}}})",
        R"({"mcpServers":{"notion":{"command":"npx","args":["-y","mcp-remote","https://mcp.notion.com/mcp"]}}})"}) {
        const auto document = Json::parse(example);
        file.update([&](Json& json) { json = document; });
        const auto loaded = store.load();
        ASSERT_TRUE(loaded.issues.empty());
        const auto& server = loaded.servers.at("notion");
        EXPECT_TRUE(server.enabled);
        EXPECT_FALSE(mcp::AuthStore().trusted(store.identity(server)));
        if (const auto* remote = std::get_if<mcp::Remote>(&server.connection))
            EXPECT_EQ(remote->authentication, mcp::Authentication::AUTO);
        else EXPECT_EQ(std::get<mcp::Local>(server.connection).command,
            (std::vector<std::string>{"npx", "-y", "mcp-remote", "https://mcp.notion.com/mcp"}));
        store.save(server);
        EXPECT_EQ(file.read(), document);
        EXPECT_EQ(store.identity(store.load().servers.at("notion")), store.identity(server));
    }
}

TEST_F(McpConfigTest, PreviousDocumentFormatIsRejectedWithoutRewritingIt) {
    mcp::ConfigStore store(work);
    mcp::JsonFile file(store.path(mcp::Scope::WORKSPACE));
    file.update([](Json& json) {
        json = Json::parse(R"({"version":1,"servers":{"old":{"type":"http","url":"https://example.test/mcp"}}})");
    });
    const auto original = file.read();
    const auto loaded = store.load();
    EXPECT_TRUE(loaded.servers.empty());
    EXPECT_FALSE(loaded.issues.empty());
    EXPECT_EQ(file.read(), original);
}

TEST_F(McpConfigTest, PortableEnvironmentAndConflictingFieldsAreValidated) {
    auto entry = Json::parse(R"({"command":"server","env":{"PUBLIC":"value"}})");
    auto server = entry.get<mcp::Server>();
    EXPECT_EQ(std::get<mcp::Local>(server.connection).environment.at("PUBLIC"), "value");
    for (const auto* invalid : {
        R"({"command":"server","url":"https://example.test/mcp"})",
        R"({"command":"server","args":"bad"})",
        R"({"command":["server"],"args":[]})",
        R"({"command":["server"]})",
        R"({"command":"server","environment":{}})",
        R"({"command":"server","env":{},"environment":{}})",
        R"({"type":"sse","url":"https://example.test/mcp"})"})
        EXPECT_THROW(Json::parse(invalid).get<mcp::Server>(), std::invalid_argument);
    mcp::ConfigStore store(work);
    mcp::JsonFile(store.path(mcp::Scope::WORKSPACE)).update([](Json& json) {
        json = {{"mcpServers", Json::object()}, {"servers", Json::object()}};
    });
    EXPECT_FALSE(store.load().issues.empty());
}

TEST_F(McpConfigTest, WorkspaceReplacesRatherThanMergesGlobalEntry) {
    mcp::ConfigStore store(work);
    EXPECT_TRUE(store.load().servers.empty());
    auto server = remote();
    server.enabled = true;
    store.save(server);
    server.scope = mcp::Scope::WORKSPACE;
    server.connection = mcp::Local{{"local-server", "--arg"}, {}, {}};
    server.enabled = false;
    store.save(server);
    auto loaded = store.load();
    ASSERT_TRUE(loaded.issues.empty());
    ASSERT_EQ(loaded.servers.size(), 1);
    EXPECT_FALSE(loaded.servers.at("docs").enabled);
    EXPECT_TRUE(std::holds_alternative<mcp::Local>(loaded.servers.at("docs").connection));
    EXPECT_EQ(store.path(mcp::Scope::WORKSPACE), work / ".sos/mcps.json");
    EXPECT_EQ(store.path(mcp::Scope::GLOBAL), PathResolution::configDirectory() / "mcps.json");
    store.remove(mcp::Scope::WORKSPACE, "docs");
    EXPECT_TRUE(store.load().servers.at("docs").enabled);
}

TEST_F(McpConfigTest, InvalidWorkspaceEntryNeverRevealsGlobalServer) {
    mcp::ConfigStore store(work);
    store.save(remote());
    mcp::JsonFile file(store.path(mcp::Scope::WORKSPACE));
    file.update([](Json& json) { json = {{"version", 1}, {"servers", {{"docs", {{"type", "sse"}}}}}}; });
    auto loaded = store.load();
    EXPECT_TRUE(loaded.servers.empty());
    ASSERT_EQ(loaded.issues.size(), 1);
    EXPECT_EQ(loaded.issues.front().scope, mcp::Scope::WORKSPACE);
    std::ofstream(store.path(mcp::Scope::WORKSPACE)) << "not json";
    EXPECT_TRUE(store.load().servers.empty());
    EXPECT_THROW(store.save(mcp::Server{"local", mcp::Scope::WORKSPACE, mcp::Local{{"x"}, {}, {}}}), std::exception);
}

TEST_F(McpConfigTest, RejectsUnsupportedOrUnsafeSettings) {
    auto server = remote();
    for (const auto& url : {"http://example.test/mcp", "file:///tmp/file", "https://user:secret@example.test", "https://example.test/#fragment"}) {
        std::get<mcp::Remote>(server.connection).url = url;
        EXPECT_THROW(mcp::validate(server), std::invalid_argument) << url;
    }
    for (const auto& url : {"https://example.test/mcp", "http://localhost:8000/mcp", "http://127.0.0.1/mcp", "http://[::1]/mcp"}) {
        std::get<mcp::Remote>(server.connection).url = url;
        EXPECT_NO_THROW(mcp::validate(server));
    }
    for (const auto& invalid : {Json("3000"), Json(3000.5), Json(-1), Json(UINT64_MAX)}) {
        Json entry = remote();
        entry["request_timeout_ms"] = invalid;
        EXPECT_THROW(entry.get<mcp::Server>(), std::invalid_argument);
    }
    Json entry = remote();
    entry["headers"] = {{"Authorization", "secret"}};
    EXPECT_THROW(entry.get<mcp::Server>(), std::invalid_argument);
    server = remote("../bad");
    EXPECT_THROW(mcp::validate(server), std::invalid_argument);
    server = remote();
    server.connection = mcp::Local{};
    EXPECT_THROW(mcp::validate(server), std::invalid_argument);
}

TEST_F(McpConfigTest, CredentialsAndTrustAreBoundToScopeDestinationAndWorkspace) {
    mcp::ConfigStore config(work);
    auto server = remote();
    const auto global = config.identity(server);
    server.enabled = true;
    server.requestTimeoutMs = 5000;
    EXPECT_EQ(config.identity(server), global);
    server.scope = mcp::Scope::WORKSPACE;
    const auto local = config.identity(server);
    EXPECT_NE(local, global);
    fs::create_directory(root / "other");
    mcp::ConfigStore other(root / "other");
    EXPECT_NE(other.identity(server), local);
    std::get<mcp::Remote>(server.connection).url += "/changed";
    EXPECT_NE(config.identity(server), local);
    server = remote();
    EXPECT_EQ(other.identity(server), global);
    server.connection = mcp::Local{{"server"}, {}, {}};
    EXPECT_NE(config.identity(server), other.identity(server));

    mcp::AuthStore auth;
    mcp::Credentials credentials;
    credentials.accessToken = "test-secret-not-real";
    auth.set(global, credentials);
    EXPECT_FALSE(auth.trusted(global)); // Storing secrets never grants trust.
    auth.trust(global, true);
    EXPECT_TRUE(auth.trusted(global));
    EXPECT_FALSE(auth.trusted(local));
    EXPECT_FALSE(auth.get(local));
    ASSERT_TRUE(auth.get(global));
    EXPECT_EQ(auth.get(global)->accessToken, credentials.accessToken);
    struct stat status{};
    ASSERT_EQ(::stat(PathResolution::mcpAuthFile().c_str(), &status), 0);
    EXPECT_EQ(status.st_mode & 0777, 0600);
    auth.forget(global);
    EXPECT_FALSE(auth.get(global));
    EXPECT_FALSE(auth.trusted(global));
    EXPECT_FALSE(fs::exists(PathResolution::authFile()));
}

TEST_F(McpConfigTest, PreviousCredentialFormatsAreRejectedWithoutChangingSecretsOrTrust) {
    mcp::ConfigStore config(work);
    const auto identity = config.identity(remote());
    mcp::AuthStore auth;
    mcp::Credentials credentials;
    credentials.accessToken = "test-token";
    auth.trust(identity, true);
    auth.set(identity, credentials);
    mcp::JsonFile file(PathResolution::mcpAuthFile());
    const auto current = file.read();
    EXPECT_EQ(auth.get(identity), credentials); // Empty OAuth fields are valid for bearer tokens.
    for (const auto fields : {6, 9}) {
        auto old = current;
        auto& entry = old["servers"][identity]["credentials"];
        entry.erase("revocation_endpoint");
        if (fields == 6) {
            entry.erase("token_endpoint");
            entry.erase("resource");
            entry.erase("token_auth_method");
        }
        file.update([&](Json& json) { json = old; });
        EXPECT_THROW(auth.get(identity), std::runtime_error);
        EXPECT_TRUE(auth.trusted(identity));
        EXPECT_EQ(file.read(), old);
    }
}

TEST_F(McpConfigTest, LockedUpdatesPreserveOtherEntriesAndRejectSymlinks) {
    const auto path = root / "settings.json";
    std::vector<std::thread> writers;
    for (int index = 0; index < 12; ++index) writers.emplace_back([=] {
        mcp::JsonFile(path).update([=](Json& json) { json[std::to_string(index)] = index; });
    });
    for (auto& thread : writers) thread.join();
    EXPECT_EQ(mcp::JsonFile(path).read().size(), 12);
    const auto link = root / "link";
    fs::create_symlink(path, link);
    EXPECT_THROW(mcp::JsonFile(link).read(), std::runtime_error);
    EXPECT_THROW(mcp::JsonFile(link).update([](Json&) {}), std::runtime_error);
    EXPECT_THROW(mcp::JsonFile(path).update([](Json& json) { json["large"] = std::string(1024 * 1024, 'x'); }), std::runtime_error);
    EXPECT_EQ(mcp::JsonFile(path).read().size(), 12);
}
}
