#ifndef SOS_RESPONSES_REQUEST_HPP
#define SOS_RESPONSES_REQUEST_HPP

#include <nlohmann/json.hpp>
#include <string>
#include "../tools/tool.hpp"

struct ResponsesRequest {
    std::string model;
    nlohmann::json input;
    std::vector<ToolDefinition> tools;
    std::string reasoningEffort = "medium";
};

void to_json(nlohmann::json& json, const ResponsesRequest& request);

#endif // SOS_RESPONSES_REQUEST_HPP
