#ifndef SOS_RESPONSES_TYPES_HPP
#define SOS_RESPONSES_TYPES_HPP

#include "../../../file_attachment.hpp"

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// Wire types for the supported subset of the OpenAI Responses API.
namespace openai_responses {

struct TextPart {
    std::string type;
    std::string text;
    nlohmann::json annotations = nlohmann::json::array();
};

struct TextMetadata {
    std::string itemId;
    std::optional<std::string> phase;
    nlohmann::json annotations = nlohmann::json::array();
};

struct TextItem {
    std::string id;
    std::string role;
    std::optional<std::string> phase;
    std::vector<TextPart> content;
};

struct FunctionCallMetadata {
    std::optional<std::string> itemId;
    std::optional<std::string> nameSpace;
};

struct FunctionCallItem {
    FunctionCallMetadata metadata;
    std::string callId;
    std::string name;
    std::string arguments;
};

struct ReasoningPart {
    std::string type;
    std::string text;
};

struct ReasoningItem {
    std::string id;
    std::vector<ReasoningPart> summary;
    std::optional<std::vector<ReasoningPart>> content;
    std::optional<std::string> encryptedContent;
};

using OutputItem = std::variant<TextItem, FunctionCallItem, ReasoningItem>;

struct Response {
    std::string id;
    std::vector<OutputItem> output;
};

struct InputTextItem {
    std::string role;
    std::string text;
};

struct FunctionResultItem {
    std::string callId;
    std::string output;
    std::vector<FileAttachment> attachments;
};

void from_json(const nlohmann::json& json, TextPart& value);
void from_json(const nlohmann::json& json, TextMetadata& value);
void to_json(nlohmann::json& json, const TextMetadata& value);
void from_json(const nlohmann::json& json, TextItem& value);
void to_json(nlohmann::json& json, const TextItem& value);
void from_json(const nlohmann::json& json, FunctionCallMetadata& value);
void to_json(nlohmann::json& json, const FunctionCallMetadata& value);
void from_json(const nlohmann::json& json, FunctionCallItem& value);
void to_json(nlohmann::json& json, const FunctionCallItem& value);
void from_json(const nlohmann::json& json, ReasoningPart& value);
void to_json(nlohmann::json& json, const ReasoningPart& value);
void from_json(const nlohmann::json& json, ReasoningItem& value);
void to_json(nlohmann::json& json, const ReasoningItem& value);
void from_json(const nlohmann::json& json, Response& value);
void to_json(nlohmann::json& json, const InputTextItem& value);
void to_json(nlohmann::json& json, const FunctionResultItem& value);
std::optional<std::string> reasoningText(const ReasoningItem& item);

}

#endif // SOS_RESPONSES_TYPES_HPP
