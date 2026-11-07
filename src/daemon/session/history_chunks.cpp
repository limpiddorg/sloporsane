#include "history_chunks.hpp"
#include <algorithm>
#include <stdexcept>

HistoryChunks::HistoryChunks(std::string requestId, std::string sessionId, std::size_t messageIndex, std::string message)
    : envelope{{"type", "session.history_chunk"}, {"requestId", std::move(requestId)},
        {"sessionId", std::move(sessionId)}, {"messageIndex", messageIndex}}, message(std::move(message)) {}

std::optional<nlohmann::json> HistoryChunks::next() {
    if (offset == message.size()) return std::nullopt;
    auto size = std::min(message.size() - offset, MAX_FRAME_BYTES / 2);
    for (;;) {
        // JSON strings must remain valid UTF-8, even at fragment boundaries.
        while (size && offset + size < message.size() &&
            (static_cast<unsigned char>(message[offset + size]) & 0xc0) == 0x80) --size;
        if (!size) throw std::invalid_argument("History frame envelope exceeds the size limit");
        auto frame = envelope;
        frame.update({{"partIndex", part}, {"last", offset + size == message.size()},
            {"data", message.substr(offset, size)}});
        if (frame.dump().size() + 1 <= MAX_FRAME_BYTES) {
            offset += size;
            ++part;
            return frame;
        }
        size /= 2;
    }
}
