#ifndef SOS_SESSION_RECOVERY_INDEX_HPP
#define SOS_SESSION_RECOVERY_INDEX_HPP

#include "session_database.hpp"
#include <agent-runtime/conversation/messages/message.hpp>
#include <agent-runtime/core/cancellation.hpp>

class SessionJournal;

// Auxiliary state is projected in the same transaction as its source messages.
// Reads never construct the display history or fetch full message bodies.
class SessionRecoveryIndex {
public:
    struct ApprovalChange {
        nlohmann::json action;
        bool approved;
    };
    explicit SessionRecoveryIndex(SessionDatabase& db) : db(db) {}
    std::vector<std::string> interruptedCalls(core::CancellationCheck cancelled = {}) const;
    nlohmann::json refusedAction(const std::string& messageId) const;
    bool approved(const nlohmann::json& action) const;
    std::vector<nlohmann::json> approvedActions() const;
    nlohmann::json approvalStates() const;
    std::vector<ApprovalChange> changesSinceLastUser() const;
    std::vector<nlohmann::json> approvedReplays() const;
    nlohmann::json approvedReplay(const std::string& id) const;
    std::optional<double> approvedReplayConfidence(const std::string& id) const;

private:
    friend class SessionJournal;
    void create();
    void append(const Message& message, std::size_t position);
    void recordCall(const ToolCallContent& call, std::size_t position, std::size_t part);
    void recordResult(const ToolResultContent& result, const std::string& messageId);
    void recordApproval(const ManualApprovalChange& change, std::size_t position);
    SessionDatabase& db;
};

#endif // SOS_SESSION_RECOVERY_INDEX_HPP
