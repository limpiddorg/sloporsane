#ifndef SOS_SESSION_STATE_HPP
#define SOS_SESSION_STATE_HPP

#include <agent-runtime/agent.hpp>
#include "../config.hpp"
#include "../permissions/execution_mode.hpp"
#include "../providers/provider_selection.hpp"
#include <cstdint>
#include <filesystem>

struct TerminalOpening {
    std::string terminalSessionId;
    std::uint64_t lastOpenedAt = 0;
};
void to_json(nlohmann::json& json, const TerminalOpening& opening);
void from_json(const nlohmann::json& json, TerminalOpening& opening);

struct SessionInfo {
    std::string id;
    std::string title;
    std::filesystem::path workspace;
    std::string model = daemon_config::DEFAULT_MODEL;
    std::uint64_t createdAt = 0;
    std::uint64_t updatedAt = 0;
    std::string reasoning = daemon_config::DEFAULT_REASONING_EFFORT;
    ContextUsage context;
    ExecutionMode mode = ExecutionMode::SANE; // Last mode announced in the conversation, not the live TUI selection.
    std::string provider = "openrouter";
    std::size_t providerContextStart = 0;
    std::vector<TerminalOpening> terminalOpenings;
    // Implement conversations are independent journals rooted at an accepted objective.
    std::string discussionId;
    std::string objectiveId;
    std::size_t inheritedMessages = 0;
    bool frozen = false;
    std::vector<std::string> implementationIds;
    std::string activeImplementationId;
    std::string autoPlanPace;
    std::string autoPlanError;
    std::optional<std::uint64_t> contextLimit;
    std::filesystem::path executionWorkspace;
    std::filesystem::path configurationWorkspace;
    std::filesystem::path configurationDirectory() const { return configurationWorkspace.empty() ? workspace : configurationWorkspace; }
    std::filesystem::path executionDirectory() const { return executionWorkspace.empty() ? workspace : executionWorkspace; }
};

void to_json(nlohmann::json& json, const SessionInfo& info);
void from_json(const nlohmann::json& json, SessionInfo& info);

struct SessionState {
    SessionInfo info;
    std::unique_ptr<Agent> discussionAgent;
};

using SessionAgentFactory = std::function<std::unique_ptr<Agent>(const SessionInfo&)>;

#endif // SOS_SESSION_STATE_HPP
