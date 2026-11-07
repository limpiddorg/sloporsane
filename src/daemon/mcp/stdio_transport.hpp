#ifndef SOS_MCP_STDIO_TRANSPORT_HPP
#define SOS_MCP_STDIO_TRANSPORT_HPP

#include "transport.hpp"
#include "auth_store.hpp"
#include <sys/types.h>

namespace mcp {
class StdioTransport final : public Transport {
public:
    StdioTransport(const Local& local, const std::filesystem::path& workspace, const Credentials& credentials = {});
    ~StdioTransport() override;
    void send(const nlohmann::json& message, Deadline deadline, core::CancellationCheck cancelled) override;
    nlohmann::json receive(Deadline deadline, core::CancellationCheck cancelled) override;
    void close() noexcept override;
private:
    void launch(const Local& local, const std::filesystem::path& workspace, const Credentials& credentials);
    void wait(short events, Deadline deadline, core::CancellationCheck cancelled);
    int socket = -1;
    pid_t pid = -1;
    std::string pending;
};
}

#endif // SOS_MCP_STDIO_TRANSPORT_HPP
