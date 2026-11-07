#include "responses_types.hpp"

#include <stdexcept>

namespace openai_responses {
namespace {

template <typename T>
std::optional<T> optionalField(const nlohmann::json& json, const char* name) {
    if (!json.contains(name) || json.at(name).is_null()) return std::nullopt;
    return json.at(name).get<T>();
}

void requireType(const nlohmann::json& json, const char* expected) {
    if (json.at("type").get<std::string>() != expected)
        throw std::invalid_argument("Unexpected OpenAI content type");
}

void requireCompleted(const nlohmann::json& json) {
    const auto status = optionalField<std::string>(json, "status");
    if (status && *status != "completed")
        throw std::invalid_argument("Only completed OpenAI items are supported");
}

}

void from_json(const nlohmann::json& json, TextPart& value) {
    requireType(json, "output_text");
    value.type = "output_text";
    json.at("text").get_to(value.text);
    value.annotations = json.value("annotations", nlohmann::json::array());
    if (!value.annotations.is_array()) throw std::invalid_argument("Annotations must be an array");
}

void from_json(const nlohmann::json& json, TextMetadata& value) {
    json.at("item_id").get_to(value.itemId);
    value.phase = optionalField<std::string>(json, "phase");
    value.annotations = json.value("annotations", nlohmann::json::array());
    if (!value.annotations.is_array()) throw std::invalid_argument("Annotations must be an array");
}

void to_json(nlohmann::json& json, const TextMetadata& value) {
    json = {{"item_id", value.itemId}, {"annotations", value.annotations}};
    if (value.phase) json["phase"] = *value.phase;
}

void from_json(const nlohmann::json& json, TextItem& value) {
    requireType(json, "message");
    requireCompleted(json);
    json.at("id").get_to(value.id);
    json.at("role").get_to(value.role);
    if (value.role != "assistant") throw std::invalid_argument("Expected an assistant output message");
    value.phase = optionalField<std::string>(json, "phase");
    json.at("content").get_to(value.content);
    if (value.content.empty()) throw std::invalid_argument("Empty OpenAI output message is unsupported");
}

void to_json(nlohmann::json& json, const TextItem& value) {
    json = {{"type", "message"}, {"id", value.id}, {"role", value.role},
            {"status", "completed"}, {"content", nlohmann::json::array()}};
    if (value.phase) json["phase"] = *value.phase;
    for (const auto& part : value.content)
        json["content"].push_back({{"type", part.type}, {"text", part.text}, {"annotations", part.annotations}});
}

void from_json(const nlohmann::json& json, FunctionCallMetadata& value) {
    value.itemId = optionalField<std::string>(json, "id");
    value.nameSpace = optionalField<std::string>(json, "namespace");
}

void to_json(nlohmann::json& json, const FunctionCallMetadata& value) {
    json = nlohmann::json::object();
    if (value.itemId) json["id"] = *value.itemId;
    if (value.nameSpace) json["namespace"] = *value.nameSpace;
}

void from_json(const nlohmann::json& json, FunctionCallItem& value) {
    requireType(json, "function_call");
    requireCompleted(json);
    if (json.value("async", false) || (json.contains("caller") && !json.at("caller").is_null()))
        throw std::invalid_argument("Asynchronous and programmatic tool calls are not supported yet");
    value.metadata = json.get<FunctionCallMetadata>();
    json.at("call_id").get_to(value.callId);
    json.at("name").get_to(value.name);
    json.at("arguments").get_to(value.arguments);
}

void to_json(nlohmann::json& json, const FunctionCallItem& value) {
    json = value.metadata;
    json["type"] = "function_call";
    json["call_id"] = value.callId;
    json["name"] = value.name;
    json["arguments"] = value.arguments;
}

void from_json(const nlohmann::json& json, ReasoningPart& value) {
    json.at("type").get_to(value.type);
    json.at("text").get_to(value.text);
}

void to_json(nlohmann::json& json, const ReasoningPart& value) {
    json = {{"type", value.type}, {"text", value.text}};
}

void from_json(const nlohmann::json& json, ReasoningItem& value) {
    requireType(json, "reasoning");
    requireCompleted(json);
    json.at("id").get_to(value.id);
    json.at("summary").get_to(value.summary);
    value.content = optionalField<std::vector<ReasoningPart>>(json, "content");
    value.encryptedContent = optionalField<std::string>(json, "encrypted_content");
    for (const auto& part : value.summary)
        if (part.type != "summary_text") throw std::invalid_argument("Unsupported reasoning summary type");
    if (value.content)
        for (const auto& part : *value.content)
            if (part.type != "reasoning_text") throw std::invalid_argument("Unsupported reasoning content type");
}

void to_json(nlohmann::json& json, const ReasoningItem& value) {
    json = {{"type", "reasoning"}, {"id", value.id}, {"summary", value.summary}};
    if (value.content) json["content"] = *value.content;
    if (value.encryptedContent) json["encrypted_content"] = *value.encryptedContent;
}

void from_json(const nlohmann::json& json, Response& value) {
    if (json.at("status").get<std::string>() != "completed")
        throw std::invalid_argument("Expected a completed OpenAI response");
    json.at("id").get_to(value.id);
    const auto& output = json.at("output");
    if (!output.is_array()) throw std::invalid_argument("OpenAI output must be an array");
    value.output.clear();
    for (const auto& item : output) {
        const auto type = item.at("type").get<std::string>();
        if (type == "message") value.output.emplace_back(item.get<TextItem>());
        else if (type == "function_call") value.output.emplace_back(item.get<FunctionCallItem>());
        else if (type == "reasoning") value.output.emplace_back(item.get<ReasoningItem>());
        else throw std::invalid_argument("Unsupported OpenAI output type: " + type);
    }
}

void to_json(nlohmann::json& json, const InputTextItem& value) {
    json = {{"type", "message"}, {"role", value.role}, {"content", value.text}};
}

void to_json(nlohmann::json& json, const FunctionResultItem& value) {
    json = {{"type", "function_call_output"}, {"call_id", value.callId}, {"output", value.output}};
    if (value.attachments.empty()) return;
    json["output"] = nlohmann::json::array({{{"type", "input_text"}, {"text", value.output}}});
    for (const auto& attachment : value.attachments) {
        if (attachment.mime == "image/png" || attachment.mime == "image/jpeg" ||
            attachment.mime == "image/gif" || attachment.mime == "image/webp") {
            json["output"].push_back({{"type", "input_image"}, {"image_url", attachment.url}});
        } else if (attachment.mime == "application/pdf") {
            json["output"].push_back({{"type", "input_file"}, {"filename", "attachment.pdf"},
                {"file_data", attachment.url}});
        } else {
            throw std::invalid_argument("Unsupported tool attachment type: " + attachment.mime);
        }
    }
}

std::optional<std::string> reasoningText(const ReasoningItem& item) {
    const auto& parts = item.summary.empty() && item.content ? *item.content : item.summary;
    if (parts.empty()) return std::nullopt;
    std::string text;
    for (std::size_t index = 0; index < parts.size(); ++index) {
        if (index != 0) text += '\n';
        text += parts[index].text;
    }
    if (!item.summary.empty() && item.content && !item.content->empty()) {
        std::string content;
        for (const auto& part : *item.content) {
            if (!content.empty()) content += '\n';
            content += part.text;
        }
        if (!content.empty())
            return text.empty() ? content : "Summary\n" + text + "\n\nReasoning\n" + content;
    }
    return text;
}

}
