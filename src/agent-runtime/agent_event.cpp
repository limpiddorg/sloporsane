#include "agent_event.hpp"

bool AgentEvent::isTerminal() const {
    return type == AgentEventType::EXECUTION_COMPLETED || type == AgentEventType::EXECUTION_FAILED ||
        type == AgentEventType::EXECUTION_CANCELLED;
}
