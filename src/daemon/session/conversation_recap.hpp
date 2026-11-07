#ifndef SOS_CONVERSATION_RECAP_HPP
#define SOS_CONVERSATION_RECAP_HPP

#include "session_state.hpp"

// Isolated, tool-free generation from recent visible discussion text.
Message generateConversationRecap(const Conversation& source, Provider& provider,
    const SessionInfo& info, const std::string& id);

#endif // SOS_CONVERSATION_RECAP_HPP
