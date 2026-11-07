#ifndef SOS_PROJECT_INSTRUCTIONS_HPP
#define SOS_PROJECT_INSTRUCTIONS_HPP
#include "../conversation/messages/message.hpp"
#include <filesystem>

// Stable key for persisted, replaceable project context; empty for ordinary messages.
std::string projectContextKey(const Message& message);
std::vector<Message> projectInstructions(const std::filesystem::path& workspace,
    const std::filesystem::path& target, const std::vector<Message>& history);
std::optional<Message> changedProjectContext(const std::string& key, const std::string& text,
    const std::vector<Message>& history);
#endif
