#include <daemon/transport/server_socket.hpp>

#include <asio/co_spawn.hpp>
#include <asio/use_future.hpp>
#include <gtest/gtest.h>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <asio/write.hpp>
#include <chrono>
#include <type_traits>

namespace {
asio::awaitable<void> acceptInto(ServerSocket& server, std::optional<ConnectionSocket>& result) {
    result.emplace(co_await server.accept());
}

class TransportTest : public testing::Test {
protected:
    std::filesystem::path directory;
    std::filesystem::path path;
    void SetUp() override {
        // Keep Unix socket addresses below the platform's sun_path limit.
        std::string pattern = "/tmp/sos-transport-XXXXXX";
        ASSERT_NE(mkdtemp(pattern.data()), nullptr);
        directory = pattern;
        path = directory / "server.sock";
    }
    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }
};

TEST_F(TransportTest, AcceptsMultipleConnectionsWithoutCreatingSessions) {
    asio::io_context io;
    ServerSocket server(io, path);
    server.listen();
    std::optional<ConnectionSocket> accepted;
    auto first = asio::co_spawn(io, acceptInto(server, accepted), asio::use_future);
    asio::local::stream_protocol::socket client(io);
    client.connect(asio::local::stream_protocol::endpoint(path.string()));
    io.run();
    first.get();
    auto connection = std::move(*accepted);
    EXPECT_TRUE(connection.isOpen());
    io.restart();
    auto second = asio::co_spawn(io, acceptInto(server, accepted), asio::use_future);
    asio::local::stream_protocol::socket otherClient(io);
    otherClient.connect(asio::local::stream_protocol::endpoint(path.string()));
    io.run();
    second.get();
    auto otherConnection = std::move(*accepted);
    EXPECT_TRUE(otherConnection.isOpen());
    connection.close();
    connection.close();
    EXPECT_FALSE(connection.isOpen());
    EXPECT_TRUE(otherConnection.isOpen());
    server.close();
    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST_F(TransportTest, CloseCancelsPendingAccept) {
    asio::io_context io;
    ServerSocket server(io, path);
    server.listen();
    std::optional<ConnectionSocket> accepted;
    auto pending = asio::co_spawn(io, acceptInto(server, accepted), asio::use_future);
    io.poll();
    server.close();
    io.run();
    EXPECT_THROW(pending.get(), asio::system_error);
    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST_F(TransportTest, DoesNotRemoveAnotherServersSocket) {
    asio::io_context io;
    ServerSocket first(io, path);
    first.listen();
    {
        ServerSocket second(io, path);
        EXPECT_THROW(second.listen(), asio::system_error);
    }
    EXPECT_TRUE(std::filesystem::is_socket(path));
    asio::local::stream_protocol::socket client(io);
    EXPECT_NO_THROW(client.connect(asio::local::stream_protocol::endpoint(path.string())));
}

TEST_F(TransportTest, PreservesExistingRegularFile) {
    std::ofstream(path) << "keep";
    asio::io_context io;
    {
        ServerSocket server(io, path);
        EXPECT_THROW(server.listen(), asio::system_error);
    }
    EXPECT_TRUE(std::filesystem::is_regular_file(path));
}

TEST_F(TransportTest, DestructorCleansPathAndSocketOwnershipIsMoveOnly) {
    static_assert(!std::is_copy_constructible_v<ConnectionSocket>);
    static_assert(std::is_move_constructible_v<ConnectionSocket>);
    asio::io_context io;
    {
        ServerSocket server(io, path);
        server.listen();
        EXPECT_THROW(server.listen(), std::logic_error);
    }
    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST_F(TransportTest, ReadsDataWithoutDisconnectingThenDetectsPeerClosure) {
    asio::io_context io;
    ServerSocket server(io, path);
    server.listen();
    std::optional<ConnectionSocket> accepted;
    auto pending = asio::co_spawn(io, acceptInto(server, accepted), asio::use_future);
    asio::local::stream_protocol::socket client(io);
    client.connect(asio::local::stream_protocol::endpoint(path.string()));
    io.run();
    pending.get();
    io.restart();
    auto closed = asio::co_spawn(io, accepted->waitForDisconnect(), asio::use_future);
    asio::write(client, asio::buffer("hello\n", 6));
    io.poll();
    EXPECT_TRUE(accepted->isOpen());
    EXPECT_EQ(closed.wait_for(std::chrono::seconds(0)), std::future_status::timeout);
    client.close();
    io.run_for(std::chrono::seconds(1));
    ASSERT_EQ(closed.wait_for(std::chrono::seconds(0)), std::future_status::ready);
    EXPECT_NO_THROW(closed.get());
    EXPECT_FALSE(accepted->isOpen());
}

TEST_F(TransportTest, LocalCloseCompletesDisconnectWait) {
    asio::io_context io;
    ServerSocket server(io, path);
    server.listen();
    std::optional<ConnectionSocket> accepted;
    auto pending = asio::co_spawn(io, acceptInto(server, accepted), asio::use_future);
    asio::local::stream_protocol::socket client(io);
    client.connect(asio::local::stream_protocol::endpoint(path.string()));
    io.run();
    pending.get();
    io.restart();
    auto closed = asio::co_spawn(io, accepted->waitForDisconnect(), asio::use_future);
    io.poll();
    accepted->close();
    io.run_for(std::chrono::seconds(1));
    ASSERT_EQ(closed.wait_for(std::chrono::seconds(0)), std::future_status::ready);
    EXPECT_NO_THROW(closed.get());
    EXPECT_FALSE(accepted->isOpen());
}
}
