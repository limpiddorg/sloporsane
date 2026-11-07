#ifndef SOS_CONFIG_HPP
#define SOS_CONFIG_HPP

#include <chrono>
#include <cstddef>

namespace daemon_config {
inline constexpr int PROTOCOL_VERSION = 7;
inline constexpr const char* DEFAULT_SOCKET_PATH = "/tmp/sosd.sock";
inline constexpr const char* DEFAULT_MODEL = "openai/gpt-5.6-luna";
inline constexpr const char* DEFAULT_REASONING_EFFORT = "medium";
inline constexpr std::size_t MAX_FRAME_BYTES = 16 * 1024 * 1024;
inline constexpr std::size_t MAX_SESSION_TITLE_WORDS = 6;
inline constexpr auto INTEGRATION_PING_INTERVAL = std::chrono::seconds(15);
inline constexpr auto INTEGRATION_PONG_TIMEOUT = std::chrono::seconds(30);
inline constexpr auto INITIAL_CONNECTION_TIMEOUT = std::chrono::seconds(30);
}  // namespace daemon_config

#endif  // SOS_CONFIG_HPP
