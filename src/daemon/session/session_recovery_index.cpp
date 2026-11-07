#include "session_recovery_index.hpp"
#include <agent-runtime/conversation/messages/message_json.hpp>

namespace {
using Statement = SessionDatabase::Statement;
using Json = nlohmann::json;
}

void SessionRecoveryIndex::create() {
    db.execute(
        "CREATE TABLE pending_tool_calls (position INTEGER NOT NULL REFERENCES messages(position),"
        " part INTEGER NOT NULL, call_id TEXT NOT NULL, PRIMARY KEY(position,part));"
        "CREATE INDEX pending_tool_calls_id ON pending_tool_calls(call_id);"
        "CREATE TABLE resolved_tool_calls (call_id TEXT PRIMARY KEY NOT NULL) WITHOUT ROWID;"
        "CREATE TABLE refused_actions (message_id TEXT PRIMARY KEY NOT NULL REFERENCES messages(id),"
        " action TEXT NOT NULL, displayable INTEGER NOT NULL) WITHOUT ROWID;"
        "CREATE TABLE approval_events (position INTEGER PRIMARY KEY REFERENCES messages(position),"
        " action TEXT NOT NULL, approved INTEGER NOT NULL CHECK(approved IN (0,1)));"
        "CREATE TABLE approval_states (action TEXT PRIMARY KEY NOT NULL,"
        " approved INTEGER NOT NULL CHECK(approved IN (0,1))) WITHOUT ROWID;");
}

void SessionRecoveryIndex::recordCall(const ToolCallContent& call, std::size_t position, std::size_t part) {
    // Retain the old recovery semantics even for a result preceding its call.
    Statement insert(db, "INSERT INTO pending_tool_calls SELECT ?,?,? WHERE NOT EXISTS"
        " (SELECT 1 FROM resolved_tool_calls WHERE call_id=?)");
    insert.bind(1, static_cast<std::int64_t>(position));
    insert.bind(2, static_cast<std::int64_t>(part));
    insert.bind(3, call.callId);
    insert.bind(4, call.callId);
    insert.step();
}

void SessionRecoveryIndex::recordResult(const ToolResultContent& result, const std::string& messageId) {
    Statement resolved(db, "INSERT OR IGNORE INTO resolved_tool_calls VALUES(?)");
    resolved.bind(1, result.callId);
    resolved.step();
    Statement remove(db, "DELETE FROM pending_tool_calls WHERE call_id=?");
    remove.bind(1, result.callId);
    remove.step();
    if (!result.metadata.contains("refused_action")) return;
    for (const auto& permission : result.metadata.value("permissions", Json::array())) {
        if (permission.value("decision", "") != "ai_refused") continue;
        // A message can contain multiple results; the first qualified refusal is
        // the exact action historically used by Session::refusedAction().
        Statement insert(db, "INSERT OR IGNORE INTO refused_actions VALUES(?,?,?)");
        insert.bind(1, messageId);
        insert.bind(2, result.metadata.at("refused_action").dump());
        insert.bind(3, result.metadata.contains("refused_message_id") ? 1 : 0);
        insert.step();
        break;
    }
}

void SessionRecoveryIndex::recordApproval(const ManualApprovalChange& change, std::size_t position) {
    const auto action = refusedAction(change.refusedMessageId).dump();
    Statement event(db, "INSERT INTO approval_events VALUES(?,?,?)");
    event.bind(1, static_cast<std::int64_t>(position));
    event.bind(2, action);
    event.bind(3, change.approved ? 1 : 0);
    event.step();
    Statement state(db, "INSERT INTO approval_states VALUES(?,?) ON CONFLICT(action) DO UPDATE SET approved=excluded.approved");
    state.bind(1, action);
    state.bind(2, change.approved ? 1 : 0);
    state.step();
}

void SessionRecoveryIndex::append(const Message& message, std::size_t position) {
    bool displayable = false;
    for (std::size_t i = 0; i < message.content.size(); ++i) {
        const auto& part = message.content[i];
        if (const auto* call = std::get_if<ToolCallContent>(&part)) recordCall(*call, position, i);
        if (const auto* result = std::get_if<ToolResultContent>(&part)) {
            recordResult(*result, message.id);
            displayable |= result->metadata.contains("refused_message_id");
        }
    }
    if (displayable) {
        Statement update(db, "UPDATE refused_actions SET displayable=1 WHERE message_id=?");
        update.bind(1, message.id);
        update.step();
    }
    if (message.manualApproval) recordApproval(*message.manualApproval, position);
}

std::vector<std::string> SessionRecoveryIndex::interruptedCalls(core::CancellationCheck cancelled) const {
    Statement statement(db, "SELECT call_id FROM pending_tool_calls ORDER BY position,part");
    std::vector<std::string> result;
    while (statement.step()) {
        core::checkCancellation(cancelled);
        result.push_back(statement.text(0));
    }
    core::checkCancellation(cancelled);
    return result;
}

Json SessionRecoveryIndex::refusedAction(const std::string& messageId) const {
    Statement statement(db, "SELECT action FROM refused_actions WHERE message_id=?");
    statement.bind(1, messageId);
    if (!statement.step()) throw std::invalid_argument("Action has no recorded AI refusal with exact arguments");
    return Json::parse(statement.text(0));
}

bool SessionRecoveryIndex::approved(const Json& action) const {
    Statement statement(db, "SELECT approved FROM approval_states WHERE action=?");
    statement.bind(1, action.dump());
    return statement.step() && statement.integer(0) != 0;
}

std::vector<Json> SessionRecoveryIndex::approvedActions() const {
    Statement statement(db, "SELECT action FROM approval_states WHERE approved=1 ORDER BY action");
    std::vector<Json> result;
    while (statement.step()) result.push_back(Json::parse(statement.text(0)));
    return result;
}

Json SessionRecoveryIndex::approvalStates() const {
    auto result = Json::object();
    Statement statement(db,
        "SELECT r.message_id,COALESCE(s.approved,0) FROM refused_actions r"
        " LEFT JOIN approval_states s ON s.action=r.action WHERE r.displayable=1"
        " AND EXISTS(SELECT 1 FROM approval_states LIMIT 1)");
    while (statement.step()) result[statement.text(0)] = statement.integer(1) != 0;
    return result;
}

std::vector<Json> SessionRecoveryIndex::approvedReplays() const {
    Statement statement(db, "SELECT MIN(r.message_id),s.action FROM approval_states s"
        " JOIN refused_actions r ON r.action=s.action WHERE s.approved=1 GROUP BY s.action ORDER BY s.action");
    std::vector<Json> result;
    while (statement.step()) result.push_back({{"id", "approval-" + statement.text(0)}, {"action", Json::parse(statement.text(1))}});
    return result;
}

Json SessionRecoveryIndex::approvedReplay(const std::string& id) const {
    if (!id.starts_with("approval-") || id.size() == 9) throw std::invalid_argument("Unknown approval id");
    Statement statement(db, "SELECT r.action FROM refused_actions r JOIN approval_states s ON s.action=r.action"
        " WHERE r.message_id=? AND s.approved=1");
    statement.bind(1, id.substr(9));
    if (!statement.step()) throw std::invalid_argument("Approval is unknown or revoked in this session");
    return Json::parse(statement.text(0));
}

std::optional<double> SessionRecoveryIndex::approvedReplayConfidence(const std::string& id) const {
    if (!id.starts_with("approval-") || id.size() == 9) throw std::invalid_argument("Unknown approval id");
    Statement statement(db, "SELECT m.summary FROM refused_actions r JOIN approval_states s ON s.action=r.action"
        " JOIN messages m ON m.id=r.message_id WHERE r.message_id=? AND s.approved=1");
    statement.bind(1, id.substr(9));
    if (!statement.step()) throw std::invalid_argument("Approval is unknown or revoked in this session");
    const auto message = Json::parse(statement.text(0)).get<Message>();
    for (const auto& part : message.content) {
        const auto* result = std::get_if<ToolResultContent>(&part);
        if (!result) continue;
        const auto permissions = result->metadata.value("permissions", Json::array());
        for (auto record = permissions.rbegin(); record != permissions.rend(); ++record) {
            if (record->value("decision", "") != "ai_refused") continue;
            const auto confidence = record->find("confidence");
            if (confidence != record->end() && confidence->is_number()) {
                const auto value = confidence->get<double>();
                if (value >= 0 && value <= 1) return value;
            }
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::vector<SessionRecoveryIndex::ApprovalChange> SessionRecoveryIndex::changesSinceLastUser() const {
    Statement statement(db,
        "SELECT action,approved FROM approval_events WHERE position >"
        " COALESCE((SELECT position FROM messages WHERE role=? ORDER BY position DESC LIMIT 1),-1) ORDER BY position");
    statement.bind(1, static_cast<std::int64_t>(MessageRole::USER));
    std::vector<ApprovalChange> result;
    while (statement.step()) result.push_back({Json::parse(statement.text(0)), statement.integer(1) != 0});
    return result;
}
