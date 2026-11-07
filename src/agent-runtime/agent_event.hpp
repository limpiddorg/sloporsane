#ifndef SOS_AGENT_EVENT_HPP
#define SOS_AGENT_EVENT_HPP

#include "conversation/messages/message.hpp"
#include "context_usage.hpp"
#include "providers/generation_retry.hpp"
#include <functional>
#include <future>

enum class AgentEventType {
    PROVIDER_MESSAGE_ARRIVED,
    HARNESS_MESSAGE_ARRIVED,
    COMPACTION_STARTED,
    COMPACTION_PROGRESS,
    COMPACTION_COMPLETED,
    COMPACTION_FAILED,
    COMPACTION_CANCELLED,
    EXECUTION_COMPLETED,
    EXECUTION_RETRY,
    EXECUTION_FAILED,
    EXECUTION_CANCELLED
};

struct AgentEvent {
    AgentEventType type;
    std::vector<Message> messages;
    std::string error;
    ContextUsage context;
    std::optional<GenerationMetadata> metadata;
    // Checkpoint activation waits for the session consumer to persist it.
    std::shared_ptr<std::promise<void>> persisted;
    std::string errorCode;
    std::optional<GenerationRetryStatus> retry;
    std::size_t compactionAttempt = 0;

    bool isTerminal() const;
};

// Called on the thread appending a message, or on the worker during run().
// Consumers should enqueue the value and return promptly. No Asio dependency.
using AgentEventHandler = std::function<void(AgentEvent)>;

#endif // SOS_AGENT_EVENT_HPP
