#ifndef SOS_GENERATION_METADATA_HPP
#define SOS_GENERATION_METADATA_HPP

#include <cstdint>
#include <string>
#include <optional>
#include <nlohmann/json.hpp>

struct GenerationMetadata {
    std::string model;
    std::string reasoning;
    std::uint64_t durationMs = 0;
};

void to_json(nlohmann::json& json, const GenerationMetadata& metadata);
void from_json(const nlohmann::json& json, GenerationMetadata& metadata);

// One conversation-model request, excluding tools, permission checks and compaction.
struct ModelCallMetadata {
    std::string model;
    std::string reasoning;
    std::uint64_t durationMs = 0;
    std::optional<std::uint64_t> outputTokens;
    void accumulate(const ModelCallMetadata& call);
};
void to_json(nlohmann::json& json, const ModelCallMetadata& metadata);
void from_json(const nlohmann::json& json, ModelCallMetadata& metadata);

#endif // SOS_GENERATION_METADATA_HPP
