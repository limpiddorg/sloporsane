// PTY fixture: real daemon stack with a fake provider. Never installed.
#include <daemon/integration/integration.hpp>
#include <daemon/session/session_registry.hpp>
#include <daemon/transport/server_socket.hpp>
#include <agent-runtime/common/path_resolution.hpp>
#include <asio/co_spawn.hpp>
#include <asio/use_future.hpp>
#include <iostream>
namespace {
class FixtureProvider final : public Provider {
public:
    ProviderFormat getFormat() const override { return ProviderFormat::OPENAI_RESPONSES; }
    std::optional<std::uint64_t> getContextLimit(const std::string&) override { return 128000; }
    std::optional<std::uint64_t> getUsedContext(const nlohmann::json&) const override { return 100; }
    nlohmann::json generate(const nlohmann::json&, const std::string&, const std::vector<ToolDefinition>&,
                           const std::string&) override {
        using Json = nlohmann::json;
        const auto id = "fixture-" + std::to_string(++sequence_);
        return {{"id", id}, {"status", "completed"}, {"output", Json::array({{
            {"type", "message"}, {"id", "msg-" + id}, {"role", "assistant"}, {"phase", "final_answer"},
            {"content", Json::array({{{"type", "output_text"}, {"text", "Native E2E answer"}, {"annotations", Json::array()}}})}
        }})}};
    }
private:
    unsigned sequence_ = 0;
};
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) return 2;
        asio::io_context io;
        SessionRegistry registry(PathResolution::sessionsDirectory(), [](const SessionInfo& info) {
            return std::make_unique<Agent>(std::make_unique<FixtureProvider>(), info.model,
                ToolRegistry{}, ToolContext{}, info.reasoning);
        });
        ServerSocket server(io, argv[1]);
        server.listen();
        auto completed = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
            auto integration = std::make_shared<Integration>(io, co_await server.accept(), registry,
                [](const std::string&, const std::string&, core::CancellationCheck) { return "Native E2E session"; });
            co_await integration->run();
        }, asio::use_future);
        io.run();
        completed.get();
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n'; return 1;
    }
}
