#ifndef SOS_MESSAGE_HPP
#define SOS_MESSAGE_HPP

#include "reasoning_content.hpp"
#include "text_content.hpp"
#include "tool_call_content.hpp"
#include "tool_result_content.hpp"
#include "generation_metadata.hpp"
#include "implementation_plan.hpp"
#include "../../image.hpp"
#include <optional>

#include <string>
#include <variant>
#include <vector>

enum class MessageRole {
    SYSTEM,
    USER,
    ASSISTANT,
    TOOL
};

using MessageContent = std::variant<
    TextContent,
    ReasoningContent,
    ToolCallContent,
    ToolResultContent
>;

// Harness display metadata, excluded from provider input.
struct UserMessageMetadata {
    std::string mode;
    bool saneRequest = false;
    bool implementationRequest = false;
};

// A persisted checkpoint for rebuilding model context without deleting history.
struct CompactionMetadata {
    std::vector<std::string> retainedMessageIds;
    std::string mode;
    std::vector<Image> imageReferences;
};

struct ManualApprovalChange {
    std::string refusedMessageId;
    bool approved = false;
};

struct Message {
    std::string id;
    MessageRole role = MessageRole::USER;
    std::vector<MessageContent> content;
    std::optional<GenerationMetadata> metadata;
    std::optional<UserMessageMetadata> userMetadata;
    std::optional<CompactionMetadata> compaction;
    // Monotonic elapsed time within this user turn, for activity boundaries only.
    std::optional<std::uint64_t> elapsedMs;
    // Human audit event, excluded from model context until the next user turn's report.
    std::optional<ManualApprovalChange> manualApproval;
    std::optional<ModelCallMetadata> modelCall;
    // A user-facing reading aid, persisted in history but never in model context.
    bool recap = false;
    // Explicit display state, never inferred from assistant prose.
    bool objectivePlan = false;
    // Optional display-only Stack section for an Objective Plan.
    std::optional<std::string> objectiveStack;
    std::vector<Image> images;
    // Display-only origin for a synthetic skill call.
    std::optional<std::string> userSkill;
    // User text presented to the model instead of the original command in content.
    std::optional<std::string> providerText;
    std::optional<ImplementationPlan> implementationPlan;
    // Harness boundary: keep historical plans, but stop carrying the objective forward.
    bool objectiveCompleted = false;
};

#endif // SOS_MESSAGE_HPP
