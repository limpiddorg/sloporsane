#ifndef SOS_IMPLEMENTATION_COMPLETION_HPP
#define SOS_IMPLEMENTATION_COMPLETION_HPP
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

struct ImplementationSubstepResult { std::string explanation; };
struct ImplementationCompletion { std::string explanation; std::vector<ImplementationSubstepResult> substeps; std::string conclusion; };
inline void to_json(nlohmann::json& json, const ImplementationSubstepResult& result) {
    json = {{"explanation", result.explanation}};
}
inline void from_json(const nlohmann::json& json, ImplementationSubstepResult& result) {
    result.explanation = json.value("explanation", ""); // Empty means reuse captured substep progress.
    if (json.contains("changes")) throw std::invalid_argument("Hunk assignments are not supported");
}
inline void to_json(nlohmann::json& json, const ImplementationCompletion& result) {
    json = {{"explanation", result.explanation}, {"substeps", result.substeps}, {"conclusion", result.conclusion}};
}
inline void from_json(const nlohmann::json& json, ImplementationCompletion& result) {
    json.at("explanation").get_to(result.explanation);
    json.at("substeps").get_to(result.substeps);
    result.conclusion = json.value("conclusion", "");
}
#endif // SOS_IMPLEMENTATION_COMPLETION_HPP
