#ifndef SOS_CONNECTION_SOCKET_HPP
#define SOS_CONNECTION_SOCKET_HPP

#include <asio/local/stream_protocol.hpp>
#include <asio/awaitable.hpp>
#include <nlohmann/json.hpp>
#include <optional>

class ConnectionSocket {
public:
    explicit ConnectionSocket(asio::local::stream_protocol::socket socket);

    void close();
    bool isOpen() const;

    // Sole reader for now: discards incoming data until EOF or disconnection.
    // The connection must stay alive and must not be moved during this operation.
    asio::awaitable<void> waitForDisconnect();
    // One concurrent reader and one writer maximum. Frames are newline-delimited JSON.
    asio::awaitable<std::optional<nlohmann::json>> receive();
    asio::awaitable<void> send(nlohmann::json message);

private:
    asio::local::stream_protocol::socket socket;
    std::string readBuffer;
};

#endif // SOS_CONNECTION_SOCKET_HPP
