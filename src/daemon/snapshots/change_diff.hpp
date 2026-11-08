#ifndef SOS_CHANGE_DIFF_HPP
#define SOS_CHANGE_DIFF_HPP
#include <nlohmann/json.hpp>
#include <string>
#include <cstdint>
#include <vector>
struct ImplementationDiff {
    std::string path, patch, before, after;
    std::vector<std::uint64_t> hunkOrder;
    std::vector<std::string> hunkExplanations;
};
inline void to_json(nlohmann::json& json, const ImplementationDiff& value) {
    json = {{"path", value.path}, {"patch", value.patch}, {"before", value.before}, {"after", value.after}};
    if (!value.hunkOrder.empty()) json["hunk_order"] = value.hunkOrder;
    if (!value.hunkExplanations.empty()) json["hunk_explanations"] = value.hunkExplanations;
}
inline void from_json(const nlohmann::json& json, ImplementationDiff& value) {
    json.at("path").get_to(value.path); json.at("patch").get_to(value.patch);
    json.at("before").get_to(value.before); json.at("after").get_to(value.after);
    value.hunkOrder = json.value("hunk_order", std::vector<std::uint64_t>{});
    value.hunkExplanations = json.value("hunk_explanations", std::vector<std::string>{});
}
#endif // SOS_CHANGE_DIFF_HPP
