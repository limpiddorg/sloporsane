#include "implementation_plan.hpp"
#include <stdexcept>

namespace {
using Json = nlohmann::json;
std::string text(const Json& json, const char* key) {
    auto value = json.at(key).get<std::string>();
    if (value.find_first_not_of(" \t\r\n") == std::string::npos || value.size() > 8000)
        throw std::invalid_argument("Implementation plan fields require 1-8000 characters");
    return value;
}
void fields(const Json& json, std::initializer_list<const char*> names) {
    if (!json.is_object() || json.size() != names.size())
        throw std::invalid_argument("Invalid implementation plan fields");
    for (const auto* name : names) if (!json.contains(name))
        throw std::invalid_argument("Missing implementation plan field");
}
}

void to_json(Json& json, const ImplementationSubstep& value) {
    json = {{"title", value.title}, {"intention", value.intention}, {"changes", value.changes}};
}
void from_json(const Json& json, ImplementationSubstep& value) {
    fields(json, {"title", "intention", "changes"});
    value = {text(json, "title"), text(json, "intention"), text(json, "changes")};
}
void to_json(Json& json, const ImplementationStep& value) {
    json = {{"title", value.title}, {"objective", value.objective}, {"substeps", value.substeps}, {"validation", value.validation}};
}
void from_json(const Json& json, ImplementationStep& value) {
    fields(json, {"title", "objective", "substeps", "validation"});
    value = {text(json, "title"), text(json, "objective"), json.at("substeps").get<std::vector<ImplementationSubstep>>(),
        json.at("validation").get<std::vector<std::string>>()};
    if (value.substeps.empty() || value.substeps.size() > 30 || value.validation.empty() || value.validation.size() > 20)
        throw std::invalid_argument("Each step requires 1-30 substeps and 1-20 validations");
    for (const auto& check : value.validation) text(Json{{"check", check}}, "check");
}
void to_json(Json& json, const ImplementationPlan& value) { json = {{"steps", value.steps}}; }
void from_json(const Json& json, ImplementationPlan& value) {
    fields(json, {"steps"});
    if (json.dump().size() > 96000) throw std::invalid_argument("Implementation plan is too large");
    value.steps = json.at("steps").get<std::vector<ImplementationStep>>();
    if (value.steps.empty() || value.steps.size() > 30)
        throw std::invalid_argument("Implementation plan requires 1-30 steps");
}

std::string ImplementationPlan::markdown() const {
    std::string result;
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const auto& step = steps[i];
        result += "## Step " + std::to_string(i + 1) + ": " + step.title + "\n\n" + step.objective + "\n\n";
        for (std::size_t j = 0; j < step.substeps.size(); ++j) {
            const auto& substep = step.substeps[j];
            result += "### " + std::to_string(i + 1) + "." + std::to_string(j + 1) + " " + substep.title +
                "\n\n" + substep.intention + "\n\n" + substep.changes + "\n\n";
        }
        result += "**Validation**\n\n";
        for (const auto& check : step.validation) result += "- " + check + "\n";
        result += '\n';
    }
    return result;
}

Json implementationPlanSchema() {
    const Json string = {{"type", "string"}, {"minLength", 1}, {"maxLength", 8000}};
    const Json substep = {{"type", "object"}, {"properties", {{"title", string}, {"intention", string}, {"changes", string}}},
        {"required", {"title", "intention", "changes"}}, {"additionalProperties", false}};
    const Json step = {{"type", "object"}, {"properties", {{"title", string}, {"objective", string},
        {"substeps", {{"type", "array"}, {"items", substep}, {"minItems", 1}, {"maxItems", 30}}},
        {"validation", {{"type", "array"}, {"items", string}, {"minItems", 1}, {"maxItems", 20}}}}},
        {"required", {"title", "objective", "substeps", "validation"}}, {"additionalProperties", false}};
    return {{"type", "object"}, {"properties", {{"steps", {{"type", "array"}, {"items", step}, {"minItems", 1}, {"maxItems", 30}}}}},
        {"required", {"steps"}}, {"additionalProperties", false}};
}
