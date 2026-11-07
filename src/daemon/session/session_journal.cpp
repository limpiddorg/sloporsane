#include "session_journal.hpp"
#include <agent-runtime/conversation/messages/message_json.hpp>
#include <fstream>

namespace {
using Json = nlohmann::json;
using Statement = SessionDatabase::Statement;
constexpr int SCHEMA_VERSION = 6;
constexpr int APPLICATION_ID = 0x534f5353;
nlohmann::json btwBody(const BtwConversation& branch) {
    auto result = Json{{"id", branch.id}, {"anchor", branch.anchor}, {"pageAnchor", branch.pageAnchor}, {"standalone", branch.standalone}, {"messages", branch.messages},
        {"status", branch.status}, {"error", branch.error}, {"model", branch.model},
        {"reasoning", branch.reasoning}, {"provider", branch.provider}, {"review", branch.review},
        {"revision_planning", branch.revisionPlanning}, {"review_summary", branch.reviewSummary},
        {"active_context", branch.activeContext}, {"context_message_count", branch.contextMessageCount}, {"review_segment_start", branch.reviewSegmentStart},
        {"context_construction_key", branch.contextConstructionKey}};
    if (branch.reviewStep) result["review_step"] = *branch.reviewStep;
    return result;
}
}

SessionJournal::SessionJournal(std::filesystem::path directory, core::CancellationCheck cancelled)
    : directory(std::move(directory)), db(this->directory / "session.sqlite") {
    core::checkCancellation(cancelled);
    initialize(cancelled);
}

void SessionJournal::initialize(core::CancellationCheck cancelled) {
    Statement version(db, "PRAGMA user_version");
    version.step();
    const auto current = version.integer(0);
    if (current != 0 && current != SCHEMA_VERSION) throw std::runtime_error("Unsupported session database version");
    Statement application(db, "PRAGMA application_id");
    application.step();
    if (application.integer(0) != 0 && application.integer(0) != APPLICATION_ID)
        throw std::runtime_error("Not a session database");
    if (current != 0) {
        if (application.integer(0) != APPLICATION_ID) throw std::runtime_error("Not a session database");
        return;
    }
    Statement tables(db, "SELECT name FROM sqlite_master WHERE type='table' LIMIT 1");
    if (tables.step()) throw std::runtime_error("Unrecognized session database schema");
    SessionDatabase::Transaction transaction(db);
    db.execute(
        "CREATE TABLE messages (position INTEGER PRIMARY KEY, id TEXT NOT NULL UNIQUE, role INTEGER NOT NULL,"
        " summary TEXT NOT NULL, body TEXT NOT NULL, updated_at INTEGER NOT NULL);"
        "CREATE INDEX messages_role_position ON messages(role, position);"
        "CREATE TABLE session_info (singleton INTEGER PRIMARY KEY CHECK(singleton=1), data TEXT NOT NULL);"
        "CREATE TABLE images (id TEXT PRIMARY KEY NOT NULL, body TEXT NOT NULL) WITHOUT ROWID;"
        "CREATE TABLE btw (number INTEGER PRIMARY KEY, id TEXT NOT NULL UNIQUE, snapshot TEXT NOT NULL, body TEXT NOT NULL);"
        "PRAGMA application_id=1397707603;");
    recoveryIndex.create();
    displayIndex.create();
    core::checkCancellation(cancelled);
    db.execute("CREATE TABLE undo_turns (id INTEGER PRIMARY KEY AUTOINCREMENT, undone INTEGER NOT NULL, operation TEXT NOT NULL, body TEXT NOT NULL); PRAGMA user_version=6;");
    transaction.commit();
}

bool SessionJournal::navigationPending() const {
    Statement query(db, "SELECT 1 FROM undo_turns WHERE operation<>'' LIMIT 1");
    return query.step();
}

TurnSnapshot SessionJournal::beginTurn(TurnSnapshot turn) {
    if (navigationPending()) throw std::runtime_error("Finish the pending /undo or /redo before sending a message");
    SessionDatabase::Transaction transaction(db);
    db.execute("DELETE FROM undo_turns WHERE undone=1");
    db.execute("INSERT INTO undo_turns(undone,operation,body) VALUES(0,'','{}')");
    Statement next(db, "SELECT last_insert_rowid()");
    next.step();
    turn.id = next.integer(0);
    saveTurn(turn);
    transaction.commit();
    return turn;
}

void SessionJournal::saveTurn(const TurnSnapshot& turn) {
    Statement put(db, "INSERT INTO undo_turns VALUES(?,?,?,?) ON CONFLICT(id) DO UPDATE SET undone=excluded.undone,operation=excluded.operation,body=excluded.body");
    put.bind(1, turn.id);
    put.bind(2, turn.undone ? 1 : 0);
    put.bind(3, turn.operation);
    put.bind(4, Json(turn).dump());
    put.step();
}

std::optional<TurnSnapshot> SessionJournal::navigationTurn(bool redo) const {
    Statement pending(db, "SELECT body FROM undo_turns WHERE operation<>'' ORDER BY id LIMIT 1");
    if (pending.step()) {
        auto turn = Json::parse(pending.text(0)).get<TurnSnapshot>();
        if (turn.operation != (redo ? "redo" : "undo"))
            throw std::runtime_error("Finish the pending /" + turn.operation + " first");
        return turn;
    }
    Statement query(db, redo ? "SELECT body FROM undo_turns WHERE undone=1 ORDER BY id LIMIT 1" :
                              "SELECT body FROM undo_turns WHERE undone=0 ORDER BY id DESC LIMIT 1");
    if (!query.step()) return std::nullopt;
    return Json::parse(query.text(0)).get<TurnSnapshot>();
}

void SessionJournal::navigate(const TurnSnapshot& turn, const SessionInfo& info) {
    if (turn.start > count()) throw std::runtime_error("Invalid undo history boundary");
    SessionDatabase::Transaction transaction(db);
    // Rebuild the projections in the same transaction as the active message tail.
    db.execute("DROP TABLE pending_tool_calls; DROP TABLE resolved_tool_calls; DROP TABLE refused_actions;"
        "DROP TABLE approval_events; DROP TABLE approval_states;"
        "DROP TABLE history_records; DROP TABLE history_visible; DROP TABLE history_groups;"
        "DROP TABLE history_items; DROP TABLE history_calls; DROP TABLE history_turns; DROP TABLE history_state;");
    Statement remove(db, "DELETE FROM messages WHERE position>=?");
    remove.bind(1, static_cast<std::int64_t>(turn.start));
    remove.step();
    recoveryIndex.create();
    displayIndex.create();
    Statement retained(db, "SELECT position,body,summary FROM messages ORDER BY position");
    while (retained.step()) {
        const auto position = static_cast<std::size_t>(retained.integer(0));
        recoveryIndex.append(Json::parse(retained.text(1)).get<Message>(), position);
        displayIndex.append(Json::parse(retained.text(2)).get<Message>(), position);
    }
    if (!turn.undone) insert(turn.messages, info.updatedAt, turn.start);
    Statement touched(db, "UPDATE messages SET updated_at=? WHERE position=(SELECT MAX(position) FROM messages)");
    touched.bind(1, static_cast<std::int64_t>(info.updatedAt));
    touched.step();
    storeInfo(info);
    saveTurn(turn);
    transaction.commit();
    reload();
}

BtwConversation SessionJournal::createBtw(BtwConversation branch) {
    SessionDatabase::Transaction transaction(db);
    Statement next(db, "SELECT COALESCE(MAX(number),0)+1 FROM btw");
    next.step();
    const auto number = next.integer(0);
    branch.id = "btw-" + std::to_string(number);
    Statement insert(db, "INSERT INTO btw(number,id,snapshot,body) VALUES(?,?,?,?)");
    insert.bind(1, number);
    insert.bind(2, branch.id);
    insert.bind(3, Json(branch.snapshot).dump());
    insert.bind(4, btwBody(branch).dump());
    insert.step();
    transaction.commit();
    return branch;
}

void SessionJournal::saveBtw(const BtwConversation& branch) {
    Statement update(db, "UPDATE btw SET body=? WHERE id=?");
    update.bind(1, btwBody(branch).dump());
    update.bind(2, branch.id);
    update.step();
}
void SessionJournal::saveAcceptedReview(const Message& message, const SessionInfo& info, const BtwConversation& branch) {
    commit({message}, info.updatedAt, &info, &branch);
}

BtwConversation SessionJournal::readBtw(const std::string& id) const {
    Statement read(db, "SELECT body,snapshot FROM btw WHERE id=?");
    read.bind(1, id);
    if (!read.step()) throw std::invalid_argument("Unknown BTW conversation: " + id);
    auto branch = Json::parse(read.text(0)).get<BtwConversation>();
    branch.snapshot = Json::parse(read.text(1)).get<std::vector<Message>>();
    return branch;
}

std::vector<BtwConversation> SessionJournal::listBtw() const {
    Statement read(db, "SELECT body FROM btw ORDER BY number");
    std::vector<BtwConversation> branches;
    while (read.step()) branches.push_back(Json::parse(read.text(0)).get<BtwConversation>());
    return branches;
}

Image SessionJournal::readImage(const std::string& id) const {
    Statement statement(db, "SELECT body FROM images WHERE id=?");
    statement.bind(1, id);
    if (!statement.step()) throw std::invalid_argument("Image does not belong to this session");
    auto body = statement.text(0);
    readBytes += body.size();
    return Json::parse(body).get<Image>();
}

bool SessionJournal::hasImages() const {
    Statement statement(db, "SELECT 1 FROM messages WHERE json_array_length(summary,'$.images')>0 LIMIT 1");
    return statement.step();
}

void SessionJournal::insert(const std::vector<Message>& messages, std::uint64_t time, std::size_t first,
    core::CancellationCheck cancelled) {
    for (const auto& message : messages) {
        core::checkCancellation(cancelled);
        auto stored = message;
        for (auto& image : stored.images) {
            if (image.data.empty()) {
                (void)readImage(image.id);
            } else {
                image = validateImage(std::move(image));
                Statement put(db, "INSERT INTO images(id,body) VALUES(?,?) ON CONFLICT(id) DO NOTHING");
                put.bind(1, image.id);
                put.bind(2, Json(image).dump());
                put.step();
            }
            image.data.clear();
        }
        Statement insert(db, "INSERT INTO messages(position,id,role,summary,body,updated_at) VALUES(?,?,?,?,?,?)");
        insert.bind(1, static_cast<std::int64_t>(first++));
        insert.bind(2, message.id);
        insert.bind(3, static_cast<std::int64_t>(message.role));
        insert.bind(4, Json(summarize(message)).dump());
        insert.bind(5, Json(stored).dump());
        insert.bind(6, static_cast<std::int64_t>(time));
        insert.step();
        recoveryIndex.append(message, first - 1);
        displayIndex.append(message, first - 1);
    }
}

void SessionJournal::storeInfo(const SessionInfo& info) {
    Statement statement(db, "INSERT INTO session_info VALUES(1,?) ON CONFLICT(singleton) DO UPDATE SET data=excluded.data");
    statement.bind(1, Json(info).dump());
    statement.step();
}

void SessionJournal::commit(const std::vector<Message>& messages, std::uint64_t time, const SessionInfo* info, const BtwConversation* branch) {
    // Reserve and build the in-memory delta before COMMIT; no throwing allocations
    // after a successful commit may make the caller retry already persisted messages.
    std::vector<Message> delta;
    if (displayLoaded) {
        delta.reserve(messages.size());
        for (const auto& message : messages) delta.push_back(summarize(message));
        display.reserve(display.size() + delta.size());
    }
    SessionDatabase::Transaction transaction(db);
    insert(messages, time, count());
    if (info) storeInfo(*info);
    if (branch) saveBtw(*branch);
    transaction.commit();
    if (displayLoaded) for (auto& message : delta) display.push_back(std::move(message));
}

void SessionJournal::append(const std::vector<Message>& messages, std::uint64_t time) {
    if (messages.empty()) return;
    auto metadata = info();
    if (metadata) metadata->updatedAt = time;
    commit(messages, time, metadata ? &*metadata : nullptr);
}
void SessionJournal::save(const std::vector<Message>& messages, const SessionInfo& info) { commit(messages, info.updatedAt, &info); }
std::optional<SessionInfo> SessionJournal::info() const {
    Statement statement(db, "SELECT data FROM session_info WHERE singleton=1");
    if (!statement.step()) return std::nullopt;
    return Json::parse(statement.text(0)).get<SessionInfo>();
}
void SessionJournal::publishInfo() const {
    const auto metadata = info();
    if (!metadata) return;
    const auto target = directory / "info.json";
    const auto temporary = directory / "info.json.tmp";
    std::ofstream stream;
    stream.exceptions(std::ios::failbit | std::ios::badbit);
    stream.open(temporary, std::ios::binary | std::ios::trunc);
    stream << Json(*metadata).dump(2) << '\n';
    stream.close();
    std::filesystem::rename(temporary, target);
}

const std::vector<Message>& SessionJournal::summaries(core::CancellationCheck cancelled) const {
    core::checkCancellation(cancelled);
    if (!displayLoaded) {
        std::vector<Message> loaded;
        Statement statement(db, "SELECT summary FROM messages ORDER BY position");
        while (statement.step()) {
            core::checkCancellation(cancelled);
            loaded.push_back(Json::parse(statement.text(0)).get<Message>());
            ++readSummaryRows;
        }
        display = std::move(loaded);
        displayLoaded = true;
    }
    return display;
}
std::size_t SessionJournal::count() const {
    Statement statement(db, "SELECT COALESCE(MAX(position)+1,0) FROM messages");
    statement.step();
    return static_cast<std::size_t>(statement.integer(0));
}
std::uint64_t SessionJournal::updatedAt() const {
    Statement statement(db, "SELECT updated_at FROM messages ORDER BY position DESC LIMIT 1");
    return statement.step() ? static_cast<std::uint64_t>(statement.integer(0)) : 0;
}
std::uintmax_t SessionJournal::sizeBytes() const {
    Statement statement(db, "SELECT COALESCE(SUM(length(CAST(body AS BLOB))),0) FROM messages");
    statement.step();
    return static_cast<std::uintmax_t>(statement.integer(0));
}
std::uintmax_t SessionJournal::bytesRead() const { return readBytes; }
std::function<Message(std::size_t)> SessionJournal::reader(core::CancellationCheck cancelled, bool pixels) const {
    return [this, cancelled = std::move(cancelled), pixels](std::size_t index) {
        core::checkCancellation(cancelled);
        Statement statement(db, "SELECT body FROM messages WHERE position=?");
        statement.bind(1, static_cast<std::int64_t>(index));
        if (!statement.step()) throw std::out_of_range("Unknown session message");
        auto body = statement.text(0);
        readBytes += body.size();
        core::checkCancellation(cancelled);
        auto message = Json::parse(body).get<Message>();
        if (pixels) for (auto& image : message.images) image.data = readImage(image.id).data;
        return message;
    };
}
std::vector<Message> SessionJournal::readAll(core::CancellationCheck cancelled) const {
    const auto total = count();
    auto read = reader(std::move(cancelled));
    std::vector<Message> result;
    for (std::size_t i = 0; i < total; ++i) result.push_back(read(i));
    return result;
}
std::vector<Message> SessionJournal::readFrom(std::size_t start) const {
    const auto total = count();
    auto read = reader();
    std::vector<Message> result;
    for (auto i = start; i < total; ++i) result.push_back(read(i));
    return result;
}
std::string SessionJournal::lastUserMessage() const {
    Statement statement(db, "SELECT position FROM messages WHERE role=? ORDER BY position DESC LIMIT 1");
    statement.bind(1, static_cast<std::int64_t>(MessageRole::USER));
    if (!statement.step()) return {};
    const auto message = reader()(static_cast<std::size_t>(statement.integer(0)));
    std::string result;
    for (const auto& part : message.content) if (const auto* text = std::get_if<TextContent>(&part)) {
        if (!result.empty()) result += '\n';
        result += text->text;
    }
    return result;
}
bool SessionJournal::objectivePlanReady() const {
    const auto& messages = summaries();
    return std::any_of(messages.begin(), messages.end(), [](const auto& message) { return message.objectivePlan; });
}
bool SessionJournal::implementationPlanReady() const { return planReady(true); }
bool SessionJournal::planReady(bool implementation) const {
    Statement user(db, "SELECT position,summary FROM messages WHERE role=? ORDER BY position DESC LIMIT 1");
    user.bind(1, static_cast<std::int64_t>(MessageRole::USER));
    if (!user.step()) return false;
    const auto metadata = Json::parse(user.text(1)).get<Message>().userMetadata.value_or(UserMessageMetadata{});
    // Any planning exchange can publish a structured implementation plan, not
    // only the dedicated generation request. A later exchange still invalidates it.
    if (!implementation && !metadata.saneRequest) return false;
    const auto userPosition = user.integer(0);
    Statement assistant(db, "SELECT position,summary FROM messages WHERE role=? ORDER BY position DESC");
    assistant.bind(1, static_cast<std::int64_t>(MessageRole::ASSISTANT));
    while (assistant.step()) {
        if (assistant.integer(0) <= userPosition) return false;
        const auto message = Json::parse(assistant.text(1)).get<Message>();
        if (message.compaction) continue;
        if (message.recap || std::any_of(message.content.begin(), message.content.end(), [](const auto& part) {
            return std::holds_alternative<TextContent>(part) && !std::get<TextContent>(part).text.empty();
        })) return implementation ? message.implementationPlan.has_value() : message.objectivePlan;
    }
    return false;
}
Message SessionJournal::summarize(const Message& message) {
    Message result{message.id, message.role, {}, message.metadata, message.userMetadata, message.compaction};
    result.elapsedMs = message.elapsedMs;
    result.manualApproval = message.manualApproval;
    result.modelCall = message.modelCall;
    result.recap = message.recap;
    result.objectivePlan = message.objectivePlan;
    result.implementationPlan = message.implementationPlan;
    result.userSkill = message.userSkill;
    result.images = message.images;
    for (auto& image : result.images) image.data.clear();
    for (const auto& part : message.content) {
        if (const auto* text = std::get_if<TextContent>(&part))
            result.content.push_back(TextContent{text->text.find_first_not_of(" \r\n\t") == std::string::npos ? "" : "x"});
        else if (const auto* reasoning = std::get_if<ReasoningContent>(&part))
            result.content.push_back(ReasoningContent{reasoning->text && !reasoning->text->empty() ? std::optional<std::string>{"x"} : std::nullopt});
        else if (const auto* call = std::get_if<ToolCallContent>(&part)) {
            ToolCallContent value{call->callId, call->toolName};
            value.arguments.update(SessionHistoryView::fileArguments(call->toolName, call->arguments));
            if (const auto command = SessionHistoryView::skillScriptCommand(call->toolName, call->arguments))
                value.arguments["command"] = *command;
            if (call->toolName == "read" && call->arguments.contains("filePath"))
                value.arguments["filePath"] = call->arguments["filePath"];
            if (call->toolName == "skill" && call->arguments.contains("name"))
                value.arguments["name"] = call->arguments["name"];
            result.content.push_back(std::move(value));
        } else if (const auto* tool = std::get_if<ToolResultContent>(&part)) {
            ToolResultContent value{tool->callId, "", tool->isError};
            if (tool->metadata.contains("approved_action")) {
                const auto& replay = tool->metadata.at("approved_action");
                value.metadata["approved_action"] = {{"id", replay.at("id")}, {"tool", replay.at("tool")}};
                const auto fileArguments = SessionHistoryView::fileArguments(replay.at("tool"), replay.value("arguments", Json::object()));
                if (!fileArguments.empty()) value.metadata["approved_action"]["arguments"] = fileArguments;
                if (const auto command = SessionHistoryView::skillScriptCommand(replay.at("tool"), replay.value("arguments", Json::object())))
                    value.metadata["approved_action"]["arguments"]["command"] = *command;
                if (replay.at("tool") == "read" && replay.at("arguments").contains("filePath"))
                    value.metadata["approved_action"]["arguments"] = {{"filePath", replay.at("arguments").at("filePath")}};
            }
            for (const auto* key : {"exit", "timedOut", "cancelled", "started", "refused_message_id", "mcp_server", "mcp_tool"})
                if (tool->metadata.contains(key)) value.metadata[key] = tool->metadata[key];
            value.metadata.update(SessionHistoryView::fileMetadata(tool->metadata));
            if (const auto permissions = tool->metadata.find("permissions"); permissions != tool->metadata.end() && permissions->is_array()) {
                value.metadata["permissions"] = Json::array();
                for (const auto& permission : *permissions) {
                    auto brief = Json::object();
                    for (const auto* key : {"id", "decision", "confidence"})
                        if (permission.contains(key)) brief[key] = permission[key];
                    value.metadata["permissions"].push_back(std::move(brief));
                }
            }
            result.content.push_back(std::move(value));
        }
    }
    return result;
}
