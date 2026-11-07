#ifndef SOS_IMPLEMENTATION_PLAN_HPP
#define SOS_IMPLEMENTATION_PLAN_HPP

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

struct ImplementationSubstep {
    std::string title, intention, changes;
};
struct ImplementationStep {
    std::string title, objective;
    std::vector<ImplementationSubstep> substeps;
    std::vector<std::string> validation;
};
struct ImplementationPlan {
    std::vector<ImplementationStep> steps;
    std::string markdown() const;
};
void to_json(nlohmann::json& json, const ImplementationSubstep& value);
void from_json(const nlohmann::json& json, ImplementationSubstep& value);
void to_json(nlohmann::json& json, const ImplementationStep& value);
void from_json(const nlohmann::json& json, ImplementationStep& value);
void to_json(nlohmann::json& json, const ImplementationPlan& value);
void from_json(const nlohmann::json& json, ImplementationPlan& value);
nlohmann::json implementationPlanSchema();

#endif // SOS_IMPLEMENTATION_PLAN_HPP
