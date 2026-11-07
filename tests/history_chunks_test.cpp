#include <daemon/session/history_chunks.hpp>
#include <gtest/gtest.h>

using nlohmann::json;

TEST(HistoryChunksTest, BoundsEscapedUtf8FramesAndReconstructsAnOversizedMessage) {
    std::string text;
    while (text.size() <= 17 * 1024 * 1024) text += "é\"\\\n\t日本語";
    const json message{{"id", "large"}, {"role", "assistant"},
        {"content", json::array({{{"type", "text"}, {"text", text}}})}};
    const auto serialized = message.dump();
    HistoryChunks chunks("request-1", "session-1", 4, serialized);
    std::string restored;
    std::size_t parts = 0;
    bool finished = false;
    while (auto frame = chunks.next()) {
        ASSERT_FALSE(finished);
        EXPECT_LE(frame->dump().size() + 1, HistoryChunks::MAX_FRAME_BYTES);
        EXPECT_EQ(json::parse(frame->dump()), *frame); // Every frame is independently valid UTF-8 JSON.
        EXPECT_EQ(frame->at("requestId"), "request-1");
        EXPECT_EQ(frame->at("sessionId"), "session-1");
        EXPECT_EQ(frame->at("messageIndex"), 4);
        EXPECT_EQ(frame->at("partIndex"), parts++);
        restored += frame->at("data").get<std::string>();
        finished = frame->at("last");
    }
    EXPECT_GT(parts, 1);
    EXPECT_TRUE(finished);
    EXPECT_EQ(restored, serialized);
    EXPECT_EQ(json::parse(restored), message);
    EXPECT_FALSE(chunks.next());
}

TEST(HistoryChunksTest, SmallMessagesHaveOneFinalFrameAndOversizedEnvelopesAreRejected) {
    HistoryChunks small("r", "s", 0, "{}");
    const auto frame = small.next();
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->at("data"), "{}");
    EXPECT_EQ(frame->at("partIndex"), 0);
    EXPECT_EQ(frame->at("last"), true);
    EXPECT_FALSE(small.next());
    HistoryChunks large(std::string(HistoryChunks::MAX_FRAME_BYTES, 'x'), "s", 0, "{}");
    EXPECT_THROW(large.next(), std::invalid_argument);
}
