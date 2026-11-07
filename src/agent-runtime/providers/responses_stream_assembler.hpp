#ifndef SOS_RESPONSES_STREAM_ASSEMBLER_HPP
#define SOS_RESPONSES_STREAM_ASSEMBLER_HPP

#include <nlohmann/json.hpp>
#include <map>
#include <set>

class ResponsesStreamAssembler {
public:
    void consume(const nlohmann::json& event);
    nlohmann::json finish() const;

private:
    enum class State { READING, COMPLETED, FAILED };
    using Items = std::map<std::size_t, nlohmann::json>;
    void recordItem(const nlohmann::json& event);
    void recordCompletion(const nlohmann::json& event);
    nlohmann::json assembleOutput() const;
    static std::size_t outputIndex(const nlohmann::json& event);
    static std::string itemId(const nlohmann::json& item);
    static void validateItem(const nlohmann::json& item);
    static void mergeFinalItems(Items& items, const nlohmann::json& finalOutput);

    State state = State::READING;
    Items completedItems;
    std::set<std::size_t> expectedIndices;
    nlohmann::json finalResponse;
};

#endif // SOS_RESPONSES_STREAM_ASSEMBLER_HPP
