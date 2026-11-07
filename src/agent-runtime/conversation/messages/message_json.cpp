#include "message_json.hpp"

#include <stdexcept>
#include <type_traits>

namespace {
std::string roleName(MessageRole role) {
    switch (role) {
        case MessageRole::SYSTEM: return "system";
        case MessageRole::USER: return "user";
        case MessageRole::ASSISTANT: return "assistant";
        case MessageRole::TOOL: return "tool";
    }
    throw std::invalid_argument("Unknown message role");
}

template <typename T>
void metadata(nlohmann::json& json, const T& content) {
    if (content.provider) {
        if (*content.provider != ProviderFormat::OPENAI_RESPONSES) throw std::invalid_argument("Unknown provider format");
        json["provider"] = "openai_responses";
    }
    json["providerData"] = content.providerData;
}

template <typename T>
void readMetadata(const nlohmann::json& json, T& content) {
    if (json.contains("provider")) {
        if (json.at("provider") != "openai_responses") throw std::invalid_argument("Unknown provider format");
        content.provider = ProviderFormat::OPENAI_RESPONSES;
    }
    content.providerData = json.value("providerData", nlohmann::json::object());
}
}

void to_json(nlohmann::json& json, const Message& message) {
    json = {{"id", message.id}, {"role", roleName(message.role)}, {"content", nlohmann::json::array()}};
    if (message.metadata) json["metadata"] = *message.metadata;
    if (message.elapsedMs) json["elapsed_ms"] = *message.elapsedMs;
    if (message.modelCall) json["model_call"] = *message.modelCall;
    if (message.recap) json["recap"] = true;
    if (message.objectivePlan) json["objective_plan"] = true;
    if (message.implementationPlan) json["implementation_plan"] = *message.implementationPlan;
    if (message.objectiveStack) json["objective_stack"] = *message.objectiveStack;
    if (message.userSkill) json["user_skill"] = *message.userSkill;
    if (message.providerText) json["provider_text"] = *message.providerText;
    if (!message.images.empty()) json["images"] = message.images;
    if (message.manualApproval) json["manual_approval"] = {
        {"refused_message_id", message.manualApproval->refusedMessageId}, {"approved", message.manualApproval->approved}};
    if (message.compaction) {
        if (message.role != MessageRole::ASSISTANT) throw std::invalid_argument("Compaction requires an assistant summary");
        json["compaction"] = {{"retained_message_ids", message.compaction->retainedMessageIds}, {"mode", message.compaction->mode}};
        if (!message.compaction->imageReferences.empty()) json["compaction"]["image_references"] = message.compaction->imageReferences;
    }
    if (message.userMetadata) {
        if (message.role != MessageRole::USER || message.metadata)
            throw std::invalid_argument("User metadata requires a user message without generation metadata");
        const auto& mode = message.userMetadata->mode;
        if (mode != "sane" && mode != "slop") throw std::invalid_argument("Invalid user message mode");
        json["metadata"] = {{"mode", mode}};
        if (message.userMetadata->saneRequest) json["metadata"]["sane_request"] = true;
        if (message.userMetadata->implementationRequest) json["metadata"]["implementation_request"] = true;
    }
    for (const auto& content : message.content) {
        nlohmann::json part;
        std::visit([&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, TextContent>) {
                part = {{"type", "text"}, {"text", value.text}};
                metadata(part, value);
            } else if constexpr (std::is_same_v<T, ReasoningContent>) {
                part = {{"type", "reasoning"}};
                if (value.text) part["text"] = *value.text;
                metadata(part, value);
            } else if constexpr (std::is_same_v<T, ToolCallContent>) {
                part = {{"type", "tool_call"}, {"callId", value.callId}, {"toolName", value.toolName}, {"arguments", value.arguments}};
                metadata(part, value);
            } else {
                part = {{"type", "tool_result"}, {"callId", value.callId}, {"output", value.output},
                    {"isError", value.isError}, {"metadata", value.metadata}, {"attachments", nlohmann::json::array()}};
                for (const auto& attachment : value.attachments)
                    part["attachments"].push_back({{"mime", attachment.mime}, {"url", attachment.url}});
            }
        }, content);
        json["content"].push_back(std::move(part));
    }
}

void from_json(const nlohmann::json& json, Message& message) {
    Message result;
    result.images = json.value("images", std::vector<Image>{});
    json.at("id").get_to(result.id);
    result.recap = json.value("recap", false);
    result.objectivePlan = json.value("objective_plan", false);
    if (json.contains("implementation_plan")) {
        if (json.at("role") != "assistant" || result.objectivePlan)
            throw std::invalid_argument("An implementation plan requires a distinct assistant message");
        result.implementationPlan = json.at("implementation_plan").get<ImplementationPlan>();
    }
    if (result.objectivePlan && json.at("role") != "assistant")
        throw std::invalid_argument("An objective plan requires an assistant message");
    if (json.contains("objective_stack")) {
        if (!result.objectivePlan) throw std::invalid_argument("A stack requires an objective plan");
        result.objectiveStack = json.at("objective_stack").get<std::string>();
    }
    if (json.contains("user_skill")) result.userSkill = json.at("user_skill").get<std::string>();
    if (json.contains("model_call")) result.modelCall = json.at("model_call").get<ModelCallMetadata>();
    if (json.contains("elapsed_ms")) {
        const auto& elapsed = json.at("elapsed_ms");
        if (!elapsed.is_number_integer() || (!elapsed.is_number_unsigned() && elapsed.get<std::int64_t>() < 0))
            throw std::invalid_argument("Invalid activity elapsed time");
        result.elapsedMs = elapsed.get<std::uint64_t>();
    }
    const auto role = json.at("role").get<std::string>();
    if (role == "system") result.role = MessageRole::SYSTEM;
    else if (role == "user") result.role = MessageRole::USER;
    else if (role == "assistant") result.role = MessageRole::ASSISTANT;
    else if (role == "tool") result.role = MessageRole::TOOL;
    else throw std::invalid_argument("Unknown message role: " + role);
    if (json.contains("provider_text")) {
        if (result.role != MessageRole::USER) throw std::invalid_argument("Provider text requires a user message");
        result.providerText = json.at("provider_text").get<std::string>();
    }
    if (json.contains("manual_approval")) {
        if (result.role != MessageRole::SYSTEM) throw std::invalid_argument("Manual approval requires a system audit event");
        const auto& change = json.at("manual_approval");
        result.manualApproval = ManualApprovalChange{change.at("refused_message_id").get<std::string>(), change.at("approved").get<bool>()};
    }
    if (json.contains("compaction")) {
        if (result.role != MessageRole::ASSISTANT) throw std::invalid_argument("Compaction requires an assistant summary");
        const auto& checkpoint = json.at("compaction");
        const auto mode = checkpoint.at("mode").get<std::string>();
        if (mode != "sane" && mode != "slop") throw std::invalid_argument("Invalid compaction mode");
        result.compaction = CompactionMetadata{checkpoint.at("retained_message_ids").get<std::vector<std::string>>(), mode};
        result.compaction->imageReferences = checkpoint.value("image_references", std::vector<Image>{});
    }
    if (json.contains("metadata") && !json.at("metadata").is_null()) {
        if (result.role == MessageRole::USER) {
            const auto mode = json.at("metadata").at("mode").get<std::string>();
            if (mode != "sane" && mode != "slop") throw std::invalid_argument("Invalid user message mode");
            result.userMetadata = UserMessageMetadata{mode, json.at("metadata").value("sane_request", false),
                json.at("metadata").value("implementation_request", false)};
        } else {
            result.metadata = json.at("metadata").get<GenerationMetadata>();
        }
    }
    if (!json.at("content").is_array()) throw std::invalid_argument("Message content must be an array");
    for (const auto& part : json.at("content")) {
        const auto type = part.at("type").get<std::string>();
        if (type == "text") {
            TextContent value;
            part.at("text").get_to(value.text);
            readMetadata(part, value);
            result.content.emplace_back(std::move(value));
        } else if (type == "reasoning") {
            ReasoningContent value;
            if (part.contains("text")) value.text = part.at("text").get<std::string>();
            readMetadata(part, value);
            result.content.emplace_back(std::move(value));
        } else if (type == "tool_call") {
            ToolCallContent value;
            part.at("callId").get_to(value.callId);
            part.at("toolName").get_to(value.toolName);
            value.arguments = part.at("arguments");
            readMetadata(part, value);
            result.content.emplace_back(std::move(value));
        } else if (type == "tool_result") {
            ToolResultContent value;
            part.at("callId").get_to(value.callId);
            value.output = part.at("output");
            part.at("isError").get_to(value.isError);
            value.metadata = part.value("metadata", nlohmann::json::object());
            for (const auto& attachment : part.at("attachments"))
                value.attachments.push_back({attachment.at("mime").get<std::string>(), attachment.at("url").get<std::string>()});
            result.content.emplace_back(std::move(value));
        } else throw std::invalid_argument("Unknown message content: " + type);
    }
    message = std::move(result);
}
