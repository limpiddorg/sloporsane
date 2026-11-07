#ifndef SOS_COMPACTION_CONTEXT_HPP
#define SOS_COMPACTION_CONTEXT_HPP

#include "conversation.hpp"
#include "../providers/model_info.hpp"
#include "../tools/tool.hpp"
#include <span>

// A borrowed view, valid only during the check/preparation of the next request.
struct CompactionContext {
    const Conversation& conversation;
    ModelInfo model;
    std::span<const ToolDefinition> tools = {};
    // Provider input + output usage, measured after measuredMessages were appended.
    std::optional<std::uint64_t> measuredTokens;
    std::size_t measuredMessages = 0;
    std::string reasoning;
};

#endif // SOS_COMPACTION_CONTEXT_HPP
