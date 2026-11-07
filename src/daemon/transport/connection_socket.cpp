#include "connection_socket.hpp"
#include "../config.hpp"

#include <utility>
#include <array>
#include <asio/redirect_error.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/read_until.hpp>
#include <asio/write.hpp>

ConnectionSocket::ConnectionSocket(asio::local::stream_protocol::socket socket)
    : socket(std::move(socket)) {}

void ConnectionSocket::close() {
    asio::error_code error;
    socket.close(error);
}

bool ConnectionSocket::isOpen() const { return socket.is_open(); }

asio::awaitable<std::optional<nlohmann::json>> ConnectionSocket::receive() {
    asio::error_code error;
    const auto count = co_await asio::async_read_until(socket, asio::dynamic_buffer(readBuffer, daemon_config::MAX_FRAME_BYTES), '\n',
        asio::redirect_error(asio::use_awaitable, error));
    if (error) {
        close();
        if (error == asio::error::eof || error == asio::error::connection_reset || error == asio::error::operation_aborted)
            co_return std::nullopt;
        throw asio::system_error(error);
    }
    auto message = nlohmann::json::parse(readBuffer.substr(0, count));
    readBuffer.erase(0, count);
    co_return message;
}

asio::awaitable<void> ConnectionSocket::send(nlohmann::json message) {
    const auto data = message.dump() + '\n';
    co_await asio::async_write(socket, asio::buffer(data), asio::use_awaitable);
}

asio::awaitable<void> ConnectionSocket::waitForDisconnect() {
    std::array<char, 4096> buffer;
    while (socket.is_open()) {
        asio::error_code error;
        co_await socket.async_read_some(asio::buffer(buffer), asio::redirect_error(asio::use_awaitable, error));
        if (!error) continue;
        close();
        if (error == asio::error::eof || error == asio::error::connection_reset ||
            error == asio::error::operation_aborted) co_return;
        throw asio::system_error(error);
    }
}
