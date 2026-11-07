#include "responses_request.hpp"

void to_json(nlohmann::json& json, const ResponsesRequest& request) {
    json = {{"model", request.model}, {"input", request.input}, {"stream", false}, {"store", false},
            {"include", {"reasoning.encrypted_content"}},
            {"reasoning", {{"effort", request.reasoningEffort}, {"summary", "auto"}}}};
    if (request.reasoningEffort == "none") {
        json["reasoning"].erase("summary");
        json.erase("include");
    }
    if (!request.tools.empty()) {
        json["tools"] = nlohmann::json::array();
        for (const auto& tool : request.tools)
            json["tools"].push_back({{"type", "function"}, {"name", tool.id},
                {"description", tool.description}, {"parameters", tool.parameters}, {"strict", false}});
        json["tool_choice"] = "auto";
    }
}
