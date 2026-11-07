#ifndef SOS_HISTORY_CHUNKS_HPP
#define SOS_HISTORY_CHUNKS_HPP

#include <nlohmann/json.hpp>
#include <optional>
#include <string>

// One serialized message at a time; frames include their JSONL newline in the limit.
class HistoryChunks {
public:
    static constexpr std::size_t MAX_FRAME_BYTES = 256 * 1024;
    HistoryChunks(std::string requestId, std::string sessionId, std::size_t messageIndex, std::string message);
    std::optional<nlohmann::json> next();
private:
    nlohmann::json envelope;
    std::string message;
    std::size_t offset = 0;
    std::size_t part = 0;
};

#endif // SOS_HISTORY_CHUNKS_HPP
