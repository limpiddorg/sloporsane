#ifndef SOS_CONTEXT_USAGE_HPP
#define SOS_CONTEXT_USAGE_HPP

#include <cstdint>
#include <optional>
#include <nlohmann/json.hpp>

struct ContextUsage {
    std::optional<std::uint64_t> lastTurnUsedContext;
    std::optional<std::uint64_t> totalContext;
};

void to_json(nlohmann::json& json, const ContextUsage& usage);
void from_json(const nlohmann::json& json, ContextUsage& usage);

#endif // SOS_CONTEXT_USAGE_HPP
