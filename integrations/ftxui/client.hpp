#ifndef SOS_TUI_CLIENT_HPP
#define SOS_TUI_CLIENT_HPP
#include <filesystem>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <nlohmann/json.hpp>
namespace sos::tui {
using Json = nlohmann::json;
inline constexpr std::size_t MAX_FRAME_BYTES = 16 * 1024 * 1024;
class Client {
public:
    Client() = default;
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Json connect(const std::string& socket, const std::filesystem::path& daemon);
    void start(std::function<void(Json)> receive);
    void send(const Json& request);
    void close();
private:
    bool tryConnect(const std::string& path);
    Json receive();
    int descriptor_ = -1;
    std::string buffer_;
    std::mutex writer_;
    std::thread reader_;
    std::atomic<bool> stopping_{false};
    bool ready_ = false;
};
}
#endif
