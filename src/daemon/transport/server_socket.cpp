#include "server_socket.hpp"

#include <asio/use_awaitable.hpp>
#include <stdexcept>
#include <utility>

ServerSocket::ServerSocket(asio::io_context& io, std::filesystem::path socketPath)
    : acceptor(io), socketPath(std::move(socketPath)) {}

ServerSocket::~ServerSocket() { close(); }

void ServerSocket::listen() {
    if (acceptor.is_open()) throw std::logic_error("Server is already listening");
    if (socketPath.empty() || socketPath.string().find('\0') != std::string::npos)
        throw std::invalid_argument("Invalid socket path");
    const asio::local::stream_protocol::endpoint endpoint(socketPath.string());
    try {
        acceptor.open(endpoint.protocol());
        // bind refuses an existing path: never remove another server's socket on startup.
        acceptor.bind(endpoint);
        ownsPath = true;
        std::filesystem::permissions(socketPath, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
        acceptor.listen();
    } catch (...) {
        close();
        throw;
    }
}

asio::awaitable<ConnectionSocket> ServerSocket::accept() {
    auto socket = co_await acceptor.async_accept(asio::use_awaitable);
    co_return ConnectionSocket(std::move(socket));
}

void ServerSocket::close() {
    asio::error_code error;
    acceptor.close(error);
    if (ownsPath) {
        std::error_code filesystemError;
        std::filesystem::remove(socketPath, filesystemError);
        ownsPath = false;
    }
}
