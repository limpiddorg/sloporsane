#include "context_usage.hpp"

void to_json(nlohmann::json& json, const ContextUsage& usage) {
    json = {{"last_turn_used_context", usage.lastTurnUsedContext ? nlohmann::json(*usage.lastTurnUsedContext) : nlohmann::json(nullptr)},
        {"total_context", usage.totalContext ? nlohmann::json(*usage.totalContext) : nlohmann::json(nullptr)}};
}

void from_json(const nlohmann::json& json, ContextUsage& usage) {
    const auto read = [&](const char* key) -> std::optional<std::uint64_t> {
        const auto it = json.find(key);
        if (it == json.end() || it->is_null()) return std::nullopt;
        if (!it->is_number_integer() || (!it->is_number_unsigned() && it->get<std::int64_t>() < 0))
            throw std::invalid_argument("Invalid context token count");
        return it->get<std::uint64_t>();
    };
    usage.lastTurnUsedContext = read("last_turn_used_context");
    usage.totalContext = read("total_context");
}
