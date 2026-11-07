#include "generation_metadata.hpp"
#include <stdexcept>
#include <limits>

void to_json(nlohmann::json& json, const GenerationMetadata& metadata) {
    json = {{"model", metadata.model}, {"reasoning", metadata.reasoning}, {"duration_ms", metadata.durationMs}};
}

void from_json(const nlohmann::json& json, GenerationMetadata& metadata) {
    json.at("model").get_to(metadata.model);
    json.at("reasoning").get_to(metadata.reasoning);
    const auto& duration = json.at("duration_ms");
    if (!duration.is_number_integer() || (!duration.is_number_unsigned() && duration.get<std::int64_t>() < 0))
        throw std::invalid_argument("Invalid generation duration");
    duration.get_to(metadata.durationMs);
}

void ModelCallMetadata::accumulate(const ModelCallMetadata& call) {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (durationMs > maximum - call.durationMs) {
        outputTokens.reset();
        return;
    }
    durationMs += call.durationMs;
    if (!outputTokens || !call.outputTokens || *outputTokens > maximum - *call.outputTokens)
        outputTokens.reset();
    else *outputTokens += *call.outputTokens;
}

void to_json(nlohmann::json& json, const ModelCallMetadata& metadata) {
    json = {{"model", metadata.model}, {"reasoning", metadata.reasoning}, {"duration_ms", metadata.durationMs}};
    if (metadata.outputTokens) json["output_tokens"] = *metadata.outputTokens;
}

void from_json(const nlohmann::json& json, ModelCallMetadata& metadata) {
    GenerationMetadata base;
    from_json(json, base);
    metadata = {base.model, base.reasoning, base.durationMs, {}};
    if (const auto tokens = json.find("output_tokens"); tokens != json.end()) {
        if (!tokens->is_number_integer() || (!tokens->is_number_unsigned() && tokens->get<std::int64_t>() < 0))
            throw std::invalid_argument("Invalid output token count");
        metadata.outputTokens = tokens->get<std::uint64_t>();
    }
}
