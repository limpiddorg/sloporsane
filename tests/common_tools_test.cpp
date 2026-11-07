#include <agent-runtime/agent.hpp>
#include <agent-runtime/tools/common/bash.hpp>
#include <agent-runtime/tools/common/webfetch.hpp>
#include <agent-runtime/tools/fs/common.hpp>

#include <gtest/gtest.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <fstream>
#include <future>
#include <thread>

namespace {
using nlohmann::json;
namespace fs = std::filesystem;

struct HttpResponse {
    std::string body;
    std::string contentType = "text/plain";
    int status = 200;
    std::string location;
    std::chrono::milliseconds delay{0};
};

class HttpServer {
public:
    explicit HttpServer(std::vector<HttpResponse> responses) {
        listener = socket(AF_INET, SOCK_STREAM, 0);
        if (listener < 0) throw std::runtime_error("socket failed");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t size = sizeof(address);
        if (bind(listener, reinterpret_cast<sockaddr*>(&address), size) || listen(listener, 5) ||
            getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size)) {
            close(listener);
            throw std::runtime_error("Could not bind HTTP fixture");
        }
        url = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port));
        worker = std::async(std::launch::async, [this, responses = std::move(responses)] {
            for (const auto& response : responses) {
                const int client = accept(listener, nullptr, nullptr);
                if (client < 0) return;
                struct Cleanup { int fd; ~Cleanup() { close(fd); } } cleanup{client};
#ifdef __APPLE__
                const int one = 1;
                setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
                constexpr int SEND_FLAGS = 0;
#else
                constexpr int SEND_FLAGS = MSG_NOSIGNAL;
#endif
                timeval timeout{2, 0};
                setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
                setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
                std::string request;
                char buffer[4096];
                while (request.find("\r\n\r\n") == std::string::npos) {
                    const auto count = recv(client, buffer, sizeof(buffer), 0);
                    if (count <= 0) return;
                    request.append(buffer, static_cast<std::size_t>(count));
                }
                std::this_thread::sleep_for(response.delay);
                std::string reply = "HTTP/1.1 " + std::to_string(response.status) + " Test\r\nContent-Type: " +
                    response.contentType + "\r\nContent-Length: " + std::to_string(response.body.size()) + "\r\nConnection: close\r\n";
                if (!response.location.empty()) reply += "Location: " + response.location + "\r\n";
                reply += "\r\n" + response.body;
                for (std::size_t offset = 0; offset < reply.size();) {
                    const auto count = send(client, reply.data() + offset, reply.size() - offset, SEND_FLAGS);
                    if (count <= 0) break;
                    offset += static_cast<std::size_t>(count);
                }
            }
        });
    }
    ~HttpServer() { shutdown(listener, SHUT_RDWR); close(listener); worker.wait(); }
    std::string url;
private:
    int listener;
    std::future<void> worker;
};

class CommonToolsTest : public testing::Test {
protected:
    fs::path directory;
    ToolContext context;
    std::vector<ToolPermissionRequest> permissions;
    void SetUp() override {
        auto pattern = (fs::temp_directory_path() / "sos-common-XXXXXX").string();
        ASSERT_NE(mkdtemp(pattern.data()), nullptr);
        directory = fs::canonical(pattern);
        context = {directory, directory};
        context.ask = [this](const auto& request) { permissions.push_back(request); };
    }
    void TearDown() override { std::error_code error; fs::remove_all(directory, error); }
};

TEST_F(CommonToolsTest, BashRunsPipelinesInWorkdirAndReportsExitCode) {
    fs::create_directory(directory / "with spaces");
    const auto result = BashTool{}.execute({{"command", "printf hello | tr a-z A-Z; printf error >&2; pwd; exit 7"},
        {"workdir", "with spaces"}, {"mainCommand", "printf"}}, context);
    EXPECT_NE(result.output.find("HELLOerror"), std::string::npos);
    EXPECT_NE(result.output.find((directory / "with spaces").string()), std::string::npos);
    EXPECT_EQ(result.metadata["exit"], 7);
    EXPECT_FALSE(result.metadata["timedOut"]);
    EXPECT_EQ(permissions.back().permission, "bash");
}

TEST_F(CommonToolsTest, BashTimeoutRetainsOutputAndHandlesClosedPipes) {
    auto start = std::chrono::steady_clock::now();
    const auto result = BashTool{}.execute({{"command", "printf ready; sleep 10"}, {"mainCommand", "sleep"}, {"timeout", 50}}, context);
    EXPECT_TRUE(result.metadata["timedOut"]);
    EXPECT_TRUE(result.metadata["exit"].is_null());
    EXPECT_NE(result.output.find("ready"), std::string::npos);
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(3));
    const auto closed = BashTool{}.execute({{"command", "exec 1>&- 2>&-; sleep 10"}, {"mainCommand", "sleep"}, {"timeout", 50}}, context);
    EXPECT_TRUE(closed.metadata["timedOut"]);
}

TEST_F(CommonToolsTest, BashHonorsCancellationAndDenial) {
    int checks = 0;
    context.checkCancelled = [&] { if (++checks > 3) throw std::runtime_error("Cancelled"); };
    EXPECT_THROW(BashTool{}.execute({{"command", "sleep 10"}, {"mainCommand", "sleep"}}, context), std::runtime_error);
    context.checkCancelled = {};
    context.ask = [](const auto&) { throw std::runtime_error("Denied"); };
    EXPECT_THROW(BashTool{}.execute({{"command", "touch should-not-exist"}, {"mainCommand", "touch"}}, context), std::runtime_error);
    EXPECT_FALSE(fs::exists(directory / "should-not-exist"));
}

TEST_F(CommonToolsTest, BashSpoolsLargeOutputAndReplacesInvalidUtf8) {
    const auto result = BashTool{}.execute({{"command", "for ((i=0;i<2100;i++)); do printf 'line-%s\\n' \"$i\"; done"}, {"mainCommand", "printf"}}, context);
    EXPECT_TRUE(result.metadata["truncated"]);
    const auto path = result.metadata["outputPath"].get<std::string>();
    const auto complete = fs_tools::readText(path);
    EXPECT_TRUE(complete.starts_with("line-0\n"));
    EXPECT_TRUE(complete.ends_with("line-2099\n"));
    EXPECT_EQ(result.output.find("line-0\n"), std::string::npos);
    EXPECT_NE(result.output.find("line-2099"), std::string::npos);
    const auto binary = BashTool{}.execute({{"command", "printf '\\377'"}, {"mainCommand", "printf"}}, context);
    EXPECT_NO_THROW(json(binary.output).dump());
}

TEST_F(CommonToolsTest, BashValidatesArgumentsAndDoesNotPersistShellVariables) {
    EXPECT_THROW(BashTool{}.execute({{"command", "true"}, {"mainCommand", "true"}, {"timeout", 0}}, context), std::invalid_argument);
    EXPECT_THROW(BashTool{}.execute({{"command", "true"}, {"mainCommand", "true"}, {"timeout", "100"}}, context), std::invalid_argument);
    BashTool tool;
    tool.execute({{"command", "export SOS_TEST_PRIVATE=value"}, {"mainCommand", "export"}}, context);
    const auto result = tool.execute({{"command", "printf '%s' \"${SOS_TEST_PRIVATE-unset}\""}, {"mainCommand", "printf"}}, context);
    EXPECT_TRUE(result.output.starts_with("unset"));
}

TEST_F(CommonToolsTest, BashRequiresMainCommandButNeverUsesItForExecutionOrPermissions) {
    const auto schema = BashTool{}.getDefinition().parameters;
    EXPECT_NE(std::find(schema["required"].begin(), schema["required"].end(), "mainCommand"), schema["required"].end());
    EXPECT_EQ(schema["properties"]["mainCommand"]["minLength"], 1);
    EXPECT_THROW(BashTool{}.execute({{"command", "touch should-not-run"}}, context), json::exception);
    for (const auto& label : {json(""), json(" \t"), json("bad\nlabel"), json(std::string(65, 'x')), json(42), json(nullptr)}) {
        EXPECT_THROW(BashTool{}.execute({{"command", "touch should-not-run"}, {"mainCommand", label}}, context), std::exception);
    }
    EXPECT_FALSE(fs::exists(directory / "should-not-run"));
    EXPECT_TRUE(permissions.empty());
    const auto first = BashTool{}.execute({{"command", "printf hello"}, {"mainCommand", "pytest"}}, context);
    const auto expected = permissions.back();
    const auto second = BashTool{}.execute({{"command", "printf hello"}, {"mainCommand", "git"}}, context);
    EXPECT_EQ(first.output, second.output);
    EXPECT_EQ(first.title, "printf hello");
    EXPECT_EQ(permissions.back().patterns, expected.patterns);
    EXPECT_EQ(permissions.back().metadata, expected.metadata);
    EXPECT_EQ(permissions.back().readOnly, expected.readOnly);
}

TEST_F(CommonToolsTest, WebfetchConvertsHtmlToMarkdownTextOrRawHtml) {
    const std::string html = "<html><head><style>HIDDEN</style></head><body><h1>Title &amp; stuff</h1>"
        "<p>Hello <strong>world</strong> <a href='/docs'>docs</a></p><script>SECRET</script>"
        "<ul><li>one</li><li>two</li></ul><pre><code>hello();</code></pre></body></html>";
    HttpServer server({{html, "text/html"}, {html, "text/html"}, {html, "text/html"}});
    WebFetchTool tool;
    const auto markdown = tool.execute({{"url", server.url}}, context);
    EXPECT_NE(markdown.output.find("# Title & stuff"), std::string::npos);
    EXPECT_NE(markdown.output.find("**world**"), std::string::npos);
    EXPECT_NE(markdown.output.find("[docs](" + server.url + "/docs)"), std::string::npos);
    EXPECT_NE(markdown.output.find("- one"), std::string::npos);
    EXPECT_NE(markdown.output.find("```\nhello();"), std::string::npos);
    EXPECT_EQ(markdown.output.find("SECRET"), std::string::npos);
    const auto text = tool.execute({{"url", server.url}, {"format", "text"}}, context);
    EXPECT_NE(text.output.find("Hello world docs"), std::string::npos);
    EXPECT_EQ(text.output.find("**world**"), std::string::npos);
    EXPECT_EQ(text.output.find("HIDDEN"), std::string::npos);
    EXPECT_EQ(tool.execute({{"url", server.url}, {"format", "html"}}, context).output, html);
}

TEST_F(CommonToolsTest, WebfetchHandlesRedirectsImagesAndErrors) {
    HttpServer server({{"", "text/plain", 302, "/image"}, {"GIF89a", "image/gif"}});
    const auto result = WebFetchTool{}.execute({{"url", server.url}}, context);
    ASSERT_EQ(result.attachments.size(), 1);
    EXPECT_EQ(result.attachments[0].url, "data:image/gif;base64,R0lGODlh");
    EXPECT_EQ(permissions.size(), 2);
    EXPECT_EQ(permissions[1].patterns[0], server.url + "/image");
    HttpServer failed({{"not found", "text/plain", 404}});
    EXPECT_THROW(WebFetchTool{}.execute({{"url", failed.url}}, context), std::runtime_error);
    HttpServer invalidRedirect({{"", "text/plain", 302, "file:///etc/passwd"}});
    EXPECT_THROW(WebFetchTool{}.execute({{"url", invalidRedirect.url}}, context), std::invalid_argument);
}

TEST_F(CommonToolsTest, WebfetchEnforcesSizeTimeoutAndPermissions) {
    HttpServer big({{std::string(5 * 1024 * 1024 + 1, 'x')}});
    EXPECT_THROW(WebFetchTool{}.execute({{"url", big.url}}, context), std::runtime_error);
    HttpServer slow({{"late", "text/plain", 200, {}, std::chrono::milliseconds(150)}});
    EXPECT_THROW(WebFetchTool{}.execute({{"url", slow.url}, {"timeout", 0.02}}, context), std::runtime_error);
    context.ask = [](const auto&) { throw std::runtime_error("Denied"); };
    EXPECT_THROW(WebFetchTool{}.execute({{"url", "http://127.0.0.1:1/"}}, context), std::runtime_error);
    EXPECT_THROW(WebFetchTool{}.execute({{"url", "file:///etc/passwd"}}, context), std::invalid_argument);
}

TEST_F(CommonToolsTest, WebfetchCapsTimeoutAndSpoolsLongText) {
    const auto arguments = json{{"url", "https://example.com"}, {"timeout", 1000}}.get<WebFetchArguments>();
    EXPECT_EQ(arguments.timeout, 120);
    HttpServer server({{std::string(60000, 'x')}});
    const auto result = WebFetchTool{}.execute({{"url", server.url}}, context);
    EXPECT_TRUE(result.metadata["truncated"]);
    EXPECT_EQ(fs_tools::readText(result.metadata["outputPath"].get<std::string>()).size(), 60000);
    EXPECT_LT(result.output.size(), 53000);
}
}
