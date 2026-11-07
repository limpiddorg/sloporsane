#ifndef SOS_SERVER_SOCKET_HPP
#define SOS_SERVER_SOCKET_HPP

#include "connection_socket.hpp"

#include <asio/awaitable.hpp>
#include <asio/io_context.hpp>
#include <filesystem>

// Used on the io_context thread; must outlive pending accept operations.
class ServerSocket {
public:
    ServerSocket(asio::io_context& io, std::filesystem::path socketPath);
    ~ServerSocket();

    void listen();
    asio::awaitable<ConnectionSocket> accept();
    void close();

private:
    asio::local::stream_protocol::acceptor acceptor;
    std::filesystem::path socketPath;
    bool ownsPath = false;
};

#endif // SOS_SERVER_SOCKET_HPP
