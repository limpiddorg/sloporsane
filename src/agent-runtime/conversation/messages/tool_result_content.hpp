#ifndef SOS_TOOL_RESULT_CONTENT_HPP
#define SOS_TOOL_RESULT_CONTENT_HPP

#include "../../file_attachment.hpp"
#include <vector>

#include <nlohmann/json.hpp>

#include <string>

struct ToolResultContent {
    std::string callId;
    nlohmann::json output;
    bool isError = false;
    std::vector<FileAttachment> attachments;
    // Tool display details, persisted and sent to integrations, not to the model.
    nlohmann::json metadata = nlohmann::json::object();
};

#endif // SOS_TOOL_RESULT_CONTENT_HPP
