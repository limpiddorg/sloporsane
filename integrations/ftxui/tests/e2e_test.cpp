#include "client.hpp"
#include <gtest/gtest.h>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif
using namespace sos::tui;
using namespace std::chrono_literals;
namespace {
class Terminal {
public:
    explicit Terminal(bool fake = true, const char* daemon = nullptr, bool editor = false) {
        std::string pattern = (std::filesystem::current_path() / "ftxui-e2e-XXXXXX").string();
        if (!::mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp failed");
        directory = pattern;
        if (editor) {
            std::ofstream script(directory / "editor.sh");
            script << "echo editor-entered\nwhile [ ! -f '" << (directory / "release").string()
                   << "' ]; do sleep 0.02; done\nprintf 'edited draft' > \"$1\"\n";
        }
        // Unix socket paths are short on macOS. Test resources remain under build-ai.
        socketPath = (directory / "s").string();
        if (fake) {
            listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
            sockaddr_un address{}; address.sun_family = AF_UNIX;
            if (socketPath.size() >= sizeof(address.sun_path)) throw std::runtime_error("Test socket path too long");
            std::strcpy(address.sun_path, socketPath.c_str());
            if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || ::listen(listener, 1) != 0) throw std::runtime_error("Cannot bind test socket");
            ::fcntl(listener, F_SETFD, FD_CLOEXEC);
        }
        winsize size{40, 120, 0, 0};
        const auto binary = std::filesystem::path(SOS_FTXUI_BINARY);
        child = ::forkpty(&terminal, nullptr, nullptr, &size);
        if (child == 0) {
            ::setenv("TERM", "xterm-256color", 1);
            // OSC 52 stays in this PTY; never modify the developer's desktop clipboard.
            ::setenv("SSH_CONNECTION", "native-e2e", 1);
            if (editor) ::setenv("EDITOR", ("/bin/sh '" + (directory / "editor.sh").string() + "'").c_str(), 1);
            for (const char* name : {"XDG_STATE_HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) ::setenv(name, directory.c_str(), 1);
            if (daemon) ::execl(binary.c_str(), binary.c_str(), "--socket", socketPath.c_str(), "--daemon", daemon, nullptr);
            else ::execl(binary.c_str(), binary.c_str(), "--socket", socketPath.c_str(), nullptr);
            ::_exit(127);
        }
        if (child < 0) throw std::runtime_error("forkpty failed");
        ::fcntl(terminal, F_SETFL, O_NONBLOCK);
        if (fake) {
            pollfd event{listener, POLLIN, 0};
            if (::poll(&event, 1, 5000) <= 0) throw std::runtime_error("TUI did not connect");
            peer = ::accept(listener, nullptr, nullptr);
            send({{"type", "daemon.ready"}, {"protocolVersion", 7}, {"info", {{"model", "fixture-model"}, {"reasoning", "high"}}}});
            auto configured = receive();
            if (configured.at("type") != "integration.configure") throw std::runtime_error("TUI did not configure workspace");
            send({{"type", "integration.configured"}, {"mode", "sane"}});
        }
        waitText("Discussion");
    }
    ~Terminal() {
        if (child > 0) { ::kill(child, SIGKILL); ::waitpid(child, nullptr, 0); }
        for (int descriptor : {terminal, peer, listener}) if (descriptor >= 0) ::close(descriptor);
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
    void send(const Json& value) {
        const auto bytes = value.dump() + '\n';
        if (::write(peer, bytes.data(), bytes.size()) != static_cast<ssize_t>(bytes.size())) throw std::runtime_error("Test write failed");
    }
    void keys(const std::string& value) {
        if (::write(terminal, value.data(), value.size()) != static_cast<ssize_t>(value.size())) throw std::runtime_error("PTY write failed");
    }
    void drain() {
        char data[65536];
        ssize_t count;
        while ((count = ::read(terminal, data, sizeof(data))) > 0) output.append(data, count);
    }
    bool waitText(const std::string& text, int milliseconds = 5000) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
        while (std::chrono::steady_clock::now() < deadline) {
            drain(); if (output.find(text) != std::string::npos) return true;
            std::this_thread::sleep_for(10ms);
        }
        return false;
    }
    Json receive(int milliseconds = 5000) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
        while (std::chrono::steady_clock::now() < deadline) {
            drain();
            auto newline = buffer.find('\n');
            if (newline != std::string::npos) {
                auto value = Json::parse(buffer.substr(0, newline)); buffer.erase(0, newline + 1); return value;
            }
            pollfd event{peer, POLLIN, 0};
            if (::poll(&event, 1, 20) > 0) {
                char data[8192]; auto count = ::read(peer, data, sizeof(data));
                if (count <= 0) throw std::runtime_error("TUI disconnected unexpectedly");
                buffer.append(data, count);
            }
        }
        throw std::runtime_error("Timed out waiting for TUI request. Terminal output: " + output.substr(output.size() > 4000 ? output.size() - 4000 : 0));
    }
    Json next(const std::string& type) {
        for (int i = 0; i < 20; ++i) { auto value = receive(); if (value.at("type") == type) return value; }
        throw std::runtime_error("Expected request not received: " + type);
    }
    void finish() {
        keys("\x11");
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            drain(); int status = 0;
            if (::waitpid(child, &status, WNOHANG) == child) { child = -1; EXPECT_TRUE(WIFEXITED(status)); EXPECT_EQ(WEXITSTATUS(status), 0); return; }
            std::this_thread::sleep_for(10ms);
        }
        FAIL() << "TUI did not terminate";
    }
    std::filesystem::path directory;
    std::string output, socketPath;
private:
    int listener = -1, peer = -1, terminal = -1;
    pid_t child = -1;
    std::string buffer;
};
Json user(std::string text, std::string id = "m1") {
    return {{"id", id}, {"role", "user"}, {"content", Json::array({{{"type", "text"}, {"text", text}}})}};
}
}
TEST(FtxuiE2E, ChatQueuePermissionHeartbeatAndShutdown) {
    Terminal tui;
    tui.keys("\t");
    auto mode = tui.next("mode.set"); EXPECT_EQ(mode["mode"], "slop");
    tui.send({{"type", "mode.updated"}, {"mode", "slop"}});
    tui.keys("hello\r");
    auto message = tui.next("message.send"); EXPECT_EQ(message["text"], "hello"); EXPECT_EQ(message["mode"], "slop");
    tui.send({{"type", "session.created"}, {"session", {{"id", "session"}, {"title", "Fixture conversation"}}}});
    tui.send({{"type", "harness.message_arrived"}, {"messages", Json::array({user("hello")})}});
    tui.send({{"type", "permission.requested"}, {"permissionId", "p"}, {"tool", "write"}, {"details", {{"filepath", "sample.cpp"}}}});
    ASSERT_TRUE(tui.waitText("Allow write"));
    tui.keys("\r");
    auto reply = tui.next("permission.reply"); EXPECT_EQ(reply["decision"], "yes");
    tui.send({{"type", "permission.resolved"}, {"permissionId", "p"}, {"decision", "yes"}});
    tui.send({{"type", "integration.ping"}, {"pingId", "health"}});
    EXPECT_EQ(tui.next("integration.pong")["pingId"], "health");
    tui.keys("queued\r"); ASSERT_TRUE(tui.waitText("[Pending]"));
    tui.send({{"type", "execution.completed"}});
    EXPECT_EQ(tui.next("message.send")["text"], "queued");
    tui.finish();
}
TEST(FtxuiE2E, ConnectMasksCredentialsAndSelectsProvider) {
    Terminal tui;
    tui.keys("/connect\r"); auto list = tui.next("provider.list");
    tui.send({{"type", "provider.listed"}, {"requestId", list["requestId"]}, {"providers", Json::array({{{"id", "openrouter"}, {"connected", false}}})}});
    ASSERT_TRUE(tui.waitText("openrouter")); tui.keys("\r");
    ASSERT_TRUE(tui.waitText("OpenRouter API key"));
    tui.keys("secret-fixture-key"); std::this_thread::sleep_for(150ms); tui.drain();
    EXPECT_EQ(tui.output.find("secret-fixture-key"), std::string::npos);
    tui.keys("\r"); auto auth = tui.next("auth.api"); EXPECT_EQ(auth["key"], "secret-fixture-key");
    tui.send({{"type", "auth.saved"}, {"requestId", auth["requestId"]}});
    auto select = tui.next("provider.select"); EXPECT_EQ(select["provider"], "openrouter");
    tui.send({{"type", "provider.selected"}, {"requestId", select["requestId"]}});
    tui.finish();
}
TEST(FtxuiE2E, ResumeAcceptsNullableActivityIndexAndRestoresText) {
    Terminal tui;
    tui.keys("/resume\r"); auto list = tui.next("session.list");
    tui.send({{"type", "session.listed"}, {"requestId", list["requestId"]}, {"sessions", Json::array({{{"id", "s"}, {"title", "Saved conversation"}}})}});
    auto preview = tui.next("session.preview");
    tui.send({{"type", "session.previewed"}, {"requestId", preview["requestId"]}, {"sessionId", "s"}, {"text", "Old question"}});
    ASSERT_TRUE(tui.waitText("Saved conversation")); tui.keys("\r");
    auto attach = tui.next("session.attach");
    tui.send({{"type", "session.attach_started"}, {"requestId", attach["requestId"]}, {"sessionId", "s"}, {"session", {{"id", "s"}, {"title", "Saved conversation"}}}, {"purpose", "page"}, {"activityIndex", nullptr}, {"messageCount", 1}});
    tui.send({{"type", "session.history_chunk"}, {"requestId", attach["requestId"]}, {"sessionId", "s"}, {"messageIndex", 0}, {"partIndex", 0}, {"last", true}, {"data", user("Restored question").dump()}});
    tui.send({{"type", "session.attached"}, {"requestId", attach["requestId"]}, {"sessionId", "s"}, {"messageCount", 1}});
    EXPECT_TRUE(tui.waitText("Restored question")); EXPECT_EQ(tui.next("btw.list")["sessionId"], "s");
    tui.finish();
}
TEST(FtxuiE2E, RealDaemonStartupProviderListAndShutdown) {
    Terminal tui(false);
    tui.keys("/connect\r");
    EXPECT_TRUE(tui.waitText("openrouter"));
    tui.keys("\x1b"); std::this_thread::sleep_for(100ms); tui.finish();
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::filesystem::exists(tui.socketPath) && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(20ms);
    EXPECT_FALSE(std::filesystem::exists(tui.socketPath));
}
TEST(FtxuiE2E, MultilinePasteIsOneMessageAndNotAnImplicitSubmission) {
    Terminal tui;
    tui.keys("\x1b[200~first line\nsecond line\nthird line\x1b[201~");
    ASSERT_TRUE(tui.waitText("Pasted text"));
    tui.send({{"type", "integration.ping"}, {"pingId", "paste"}});
    // Any accidental submission would precede the pong and fail this assertion.
    EXPECT_EQ(tui.receive()["type"], "integration.pong");
    tui.keys("\r");
    EXPECT_EQ(tui.next("message.send")["text"], "first line\nsecond line\nthird line");
    tui.finish();
}
TEST(FtxuiE2E, ModelFilterAndContextBudgetPreserveConversation) {
    Terminal tui;
    tui.keys("/model\r"); auto list = tui.next("model.list");
    tui.send({{"type", "model.listed"}, {"requestId", list["requestId"]}, {"models", Json::array({
        {{"provider", "openai"}, {"model", "alpha"}, {"max_context", 128000}},
        {{"provider", "openrouter"}, {"model", "beta"}, {"max_context", 64000}}})}});
    ASSERT_TRUE(tui.waitText("alpha"));
    tui.keys("beta"); std::this_thread::sleep_for(100ms); tui.keys("\r");
    ASSERT_TRUE(tui.waitText("Maximum context tokens")); tui.keys("\r");
    auto selected = tui.next("model.select");
    EXPECT_EQ(selected["model"], "beta"); EXPECT_EQ(selected["provider"], "openrouter"); EXPECT_EQ(selected["context_limit"], 64000);
    tui.send({{"type", "model.selected"}, {"requestId", selected["requestId"]}});
    tui.finish();
}
TEST(FtxuiE2E, ImagePixelsSurviveFileRemovalAndRejectedSend) {
    Terminal tui;
    // The runtime deliberately validates headers/dimensions, not full image decoding.
    std::string png("\x89PNG\r\n\x1a\n\0\0\0\x0dIHDR\0\0\0\x01\0\0\0\x01", 24);
    auto path = tui.directory / "fixture.png";
    { std::ofstream image(path, std::ios::binary); image.write(png.data(), png.size()); }
    tui.keys("\x1b[200~" + path.string() + "\x1b[201~");
    ASSERT_TRUE(tui.waitText("fixture.png"));
    std::filesystem::remove(path);
    tui.keys("\r"); auto message = tui.next("message.send");
    ASSERT_EQ(message["images"].size(), 1); EXPECT_EQ(message["images"][0]["width"], 1);
    tui.send({{"type", "request.failed"}, {"requestId", message["requestId"]}, {"error", "Images unsupported"}});
    ASSERT_TRUE(tui.waitText("Images unsupported"));
    tui.keys("\r"); auto retry = tui.next("message.send");
    EXPECT_EQ(retry["images"], message["images"]);
    tui.finish();
}
TEST(FtxuiE2E, EscapeRestoresPendingBeforeCancellingExecution) {
    Terminal tui;
    tui.keys("first\r"); auto sent = tui.next("message.send");
    tui.send({{"type", "harness.message_arrived"}, {"messages", Json::array({user("first")})}});
    tui.keys("pending\r"); ASSERT_TRUE(tui.waitText("[Pending]"));
    tui.keys("\x1b"); std::this_thread::sleep_for(100ms);
    tui.send({{"type", "integration.ping"}, {"pingId", "not-cancelled"}});
    EXPECT_EQ(tui.receive()["type"], "integration.pong");
    tui.keys("\x1b"); ASSERT_TRUE(tui.waitText("Cancel Execution?"));
    tui.keys("\x1b"); EXPECT_EQ(tui.next("execution.cancel")["type"], "execution.cancel");
    tui.send({{"type", "execution.cancelled"}});
    tui.finish();
}
TEST(FtxuiE2E, RealIntegrationGeneratesPersistsAndResumesConversation) {
    Terminal tui(false, SOS_FTXUI_FIXTURE_DAEMON);
    tui.keys("A complete native conversation\r");
    ASSERT_TRUE(tui.waitText("Native E2E answer"));
    // Wait for the terminal execution event before opening idle-only dialogs.
    std::this_thread::sleep_for(100ms);
    tui.keys("/new\r"); std::this_thread::sleep_for(150ms);
    tui.drain(); tui.output.clear();
    tui.keys("/resume\r"); ASSERT_TRUE(tui.waitText("Native E2E session"));
    tui.keys("\r"); ASSERT_TRUE(tui.waitText("Native E2E answer"));
    EXPECT_TRUE(tui.waitText("A complete native conversation"));
    bool journal = false;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(tui.directory))
        if (entry.path().filename() == "session.sqlite") journal = true;
    EXPECT_TRUE(journal);
    tui.finish();
}
TEST(FtxuiE2E, OAuthPollingContinuesHeartbeatsAndSelectsProvider) {
    Terminal tui;
    tui.keys("/connect\r"); auto list = tui.next("provider.list");
    tui.send({{"type", "provider.listed"}, {"requestId", list["requestId"]}, {"providers", Json::array({{{"id", "openai"}, {"connected", false}}})}});
    ASSERT_TRUE(tui.waitText("openai")); tui.keys("\r"); ASSERT_TRUE(tui.waitText("OpenAI authentication"));
    tui.keys("\x1b[B\r"); auto start = tui.next("auth.start"); EXPECT_EQ(start["method"], "browser");
    tui.send({{"type", "auth.started"}, {"requestId", start["requestId"]}, {"authId", "auth"}, {"url", "https://example.invalid/login"}, {"instructions", "Fixture authorization instructions"}});
    auto poll = tui.next("auth.poll"); EXPECT_EQ(poll["authId"], "auth");
    tui.send({{"type", "integration.ping"}, {"pingId", "oauth"}}); EXPECT_EQ(tui.next("integration.pong")["pingId"], "oauth");
    tui.send({{"type", "auth.saved"}, {"requestId", poll["requestId"]}});
    auto selected = tui.next("provider.select"); EXPECT_EQ(selected["provider"], "openai");
    tui.send({{"type", "provider.selected"}, {"requestId", selected["requestId"]}}); tui.finish();
}
TEST(FtxuiE2E, AutoPermissionsCanReuseSavedKeyWithoutLeakingIntoChat) {
    Terminal tui;
    tui.keys("/permissions\r"); auto current = tui.next("permissions.get");
    tui.send({{"type", "permissions.current"}, {"requestId", current["requestId"]}, {"mode", "ask"}, {"has_key", true}});
    ASSERT_TRUE(tui.waitText("AI Verified"));
    std::this_thread::sleep_for(50ms);
    tui.keys("\x1b[B\r"); ASSERT_TRUE(tui.waitText("Jev OpenRouter key"));
    tui.keys("\r"); auto selected = tui.next("permissions.select");
    EXPECT_EQ(selected["mode"], "auto"); EXPECT_EQ(selected["key"], "");
    tui.send({{"type", "permissions.selected"}, {"requestId", selected["requestId"]}, {"mode", "auto"}});
    tui.finish();
}
TEST(FtxuiE2E, CompactPasteUndoRedoAndAtomicDeletionPreserveOriginalText) {
    Terminal tui;
    tui.keys("\x1b[200~first\nsecond\nthird\x1b[201~"); ASSERT_TRUE(tui.waitText("Pasted text"));
    tui.keys("\x7f\x1a\x19\x1a\r");
    EXPECT_EQ(tui.next("message.send")["text"], "first\nsecond\nthird");
    tui.finish();
}
TEST(FtxuiE2E, QueuedMessageDispatchesWhileExternalEditorIsStillOpen) {
    Terminal tui(true, nullptr, true);
    tui.keys("first\r"); tui.next("message.send");
    tui.send({{"type", "harness.message_arrived"}, {"messages", Json::array({user("first")})}});
    tui.keys("queued\r"); ASSERT_TRUE(tui.waitText("[Pending]"));
    tui.keys("draft\x07"); ASSERT_TRUE(tui.waitText("editor-entered"));
    tui.send({{"type", "execution.completed"}});
    EXPECT_EQ(tui.next("message.send")["text"], "queued");
    tui.send({{"type", "integration.ping"}, {"pingId", "editor"}});
    EXPECT_EQ(tui.next("integration.pong")["pingId"], "editor");
    tui.send({{"type", "harness.message_arrived"}, {"messages", Json::array({user("queued", "m2")})}});
    tui.send({{"type", "execution.completed"}});
    { std::ofstream release(tui.directory / "release"); release << "done"; }
    ASSERT_TRUE(tui.waitText("edited draft")); tui.keys("\r");
    EXPECT_EQ(tui.next("message.send")["text"], "edited draft"); tui.finish();
}
TEST(FtxuiE2E, RejectionDuringEditorPreservesRestoredPrefixAndEditedDraft) {
    Terminal tui(true, nullptr, true);
    tui.keys("rejected message\r"); auto original = tui.next("message.send");
    tui.keys("new draft\x07"); ASSERT_TRUE(tui.waitText("editor-entered"));
    tui.send({{"type", "request.failed"}, {"requestId", original["requestId"]}, {"error", "Fixture rejection"}});
    tui.send({{"type", "integration.ping"}, {"pingId", "rejection"}}); tui.next("integration.pong");
    { std::ofstream release(tui.directory / "release"); release << "done"; }
    ASSERT_TRUE(tui.waitText("edited draft")); tui.keys("\r");
    EXPECT_EQ(tui.next("message.send")["text"], "rejected message\nedited draft"); tui.finish();
}
TEST(FtxuiE2E, UnchangedEditorKeepsPasteAtomicAndReceivesExpandedText) {
    Terminal tui(true, nullptr, true);
    { std::ofstream script(tui.directory / "editor.sh"); script << "echo editor-entered\ncat \"$1\"\n"; }
    tui.keys("\x1b[200~first\nsecond\nthird\x1b[201~"); ASSERT_TRUE(tui.waitText("Pasted text"));
    tui.keys("\x07"); ASSERT_TRUE(tui.waitText("editor-entered")); ASSERT_TRUE(tui.waitText("third"));
    std::this_thread::sleep_for(100ms);
    tui.keys("\x7f\x1a\r");
    EXPECT_EQ(tui.next("message.send")["text"], "first\nsecond\nthird"); tui.finish();
}
TEST(FtxuiE2E, RefusedToolApprovalUsesRecordedIdWithoutExecutingAnything) {
    Terminal tui;
    tui.keys("hello\r"); tui.next("message.send");
    tui.send({{"type", "session.created"}, {"session", {{"id", "s"}, {"title", "Tools"}}}});
    tui.send({{"type", "harness.message_arrived"}, {"messages", Json::array({user("hello")})}});
    Json call = {{"id", "call-message"}, {"role", "assistant"}, {"content", Json::array({{{"type", "tool_call"}, {"callId", "call"}, {"toolName", "write"}, {"arguments", Json::object()}}})}};
    Json result = {{"type", "tool_result"}, {"callId", "call"}, {"isError", true}, {"output", "blocked"},
        {"metadata", {{"refused_action", Json::object()}, {"refused_message_id", "refused-id"}, {"permissions", Json::array({{{"decision", "ai_refused"}}})}}}};
    tui.send({{"type", "provider.message_arrived"}, {"messages", Json::array({call})}});
    tui.send({{"type", "harness.message_arrived"}, {"messages", Json::array({{{"id", "result-message"}, {"role", "tool"}, {"content", Json::array({result})}}})}});
    tui.send({{"type", "execution.completed"}});
    tui.send({{"type", "reasoning.updated"}, {"info", {{"model", "tools-ready"}, {"reasoning", "high"}}}});
    ASSERT_TRUE(tui.waitText("tools-ready"));
    tui.keys("\x1b[<0;3;5M\x1b[<0;3;5m"); ASSERT_TRUE(tui.waitText("Refused tools"));
    tui.keys("\x1b[<0;6;7M\x1b[<0;6;7m"); ASSERT_TRUE(tui.waitText("[Approve]"));
    tui.keys("\x1b[<0;6;13M\x1b[<0;6;13m");
    auto approval = tui.next("permission.manual");
    EXPECT_EQ(approval["refusedMessageId"], "refused-id"); EXPECT_EQ(approval["approved"], true);
    EXPECT_FALSE(approval.contains("arguments"));
    tui.send({{"type", "permission.manual_updated"}, {"requestId", approval["requestId"]}, {"states", {{"refused-id", true}}}});
    ASSERT_TRUE(tui.waitText("Revoke approval"));
    tui.finish();
}
