#include "session_history_index.hpp"
#include <agent-runtime/conversation/messages/message_json.hpp>
#include <algorithm>
#include <charconv>

namespace {
using Json = nlohmann::json;
using Statement = SessionDatabase::Statement;
constexpr unsigned TOOLS = 1, READS = 2, REASONING = 4, REFUSED = 8, SKILLS = 16;
unsigned contentKind(const MessageContent& content) {
    if (const auto* call = std::get_if<ToolCallContent>(&content))
        return call->toolName == "read" ? READS : (call->toolName == "skill" || SessionHistoryView::skillScriptName(call->toolName, call->arguments)) ? SKILLS : TOOLS;
    return std::holds_alternative<ReasoningContent>(content) ? REASONING : TOOLS;
}
template<class T> Json optionalJson(const std::optional<T>& value) { return value ? Json(*value) : Json(nullptr); }
template<class T> std::optional<T> optionalValue(const Json& json, const char* key) {
    return json.at(key).is_null() ? std::nullopt : std::optional<T>{json.at(key).get<T>()};
}
bool refused(const ToolResultContent& result) {
    for (const auto& permission : result.metadata.value("permissions", Json::array()))
        if (permission.value("decision", "") == "ai_refused") return true;
    return false;
}
}

void SessionHistoryIndex::create() {
    db.execute(
        "CREATE TABLE history_records (position INTEGER PRIMARY KEY, id TEXT NOT NULL, kind INTEGER NOT NULL,"
        " source INTEGER NOT NULL, part INTEGER NOT NULL);"
        "CREATE TABLE history_visible (position INTEGER PRIMARY KEY, id TEXT NOT NULL UNIQUE, record INTEGER NOT NULL);"
        "CREATE TABLE history_groups (position INTEGER PRIMARY KEY, id TEXT NOT NULL UNIQUE, data TEXT NOT NULL);"
        "CREATE TABLE history_items (group_id INTEGER NOT NULL, position INTEGER NOT NULL, source INTEGER NOT NULL,"
        " part INTEGER NOT NULL, result_source INTEGER NOT NULL DEFAULT -1, result_part INTEGER NOT NULL DEFAULT -1,"
        " PRIMARY KEY(group_id,position)) WITHOUT ROWID;"
        "CREATE TABLE history_calls (id TEXT PRIMARY KEY NOT NULL, group_id INTEGER NOT NULL, item INTEGER NOT NULL) WITHOUT ROWID;"
        "CREATE TABLE history_turns (position INTEGER PRIMARY KEY, generation TEXT NOT NULL);"
        "CREATE TABLE history_state (singleton INTEGER PRIMARY KEY CHECK(singleton=1), data TEXT NOT NULL);");
    saveState({});
}

SessionHistoryIndex::State SessionHistoryIndex::state() const {
    Statement query(db, "SELECT data FROM history_state WHERE singleton=1");
    if (!query.step()) throw std::runtime_error("Missing history projection state");
    const auto data = Json::parse(query.text(0));
    return {data.at("records"), data.at("visible"), data.at("current"), data.at("previous"), data.at("turn"),
        optionalValue<GenerationMetadata>(data, "generation")};
}
void SessionHistoryIndex::saveState(const State& value) {
    Statement query(db, "INSERT INTO history_state VALUES(1,?) ON CONFLICT(singleton) DO UPDATE SET data=excluded.data");
    query.bind(1, Json{{"records", value.records}, {"visible", value.visible}, {"current", value.current},
        {"previous", value.previous}, {"turn", value.turn}, {"generation", optionalJson(value.generation)}}.dump());
    query.step();
}

SessionHistoryIndex::Group SessionHistoryIndex::group(std::int64_t id) const {
    Statement query(db, "SELECT id,data FROM history_groups WHERE position=?");
    query.bind(1, id);
    if (!query.step()) throw std::invalid_argument("Unknown activity group");
    const auto data = Json::parse(query.text(1));
    Group result{{Kind::ACTIVITY, query.text(0), 0, {}, {}, data.at("terminal")}};
    auto& record = result.record;
    record.startedMs = optionalValue<std::uint64_t>(data, "started");
    record.durationMs = optionalValue<std::uint64_t>(data, "duration");
    record.generation = optionalValue<GenerationMetadata>(data, "generation");
    record.modelCall = optionalValue<ModelCallMetadata>(data, "model_call");
    record.missingModelCall = data.at("missing_call");
    result.count = data.at("count");
    result.kinds = data.at("kinds");
    result.turn = data.at("turn");
    result.metadata = optionalValue<GenerationMetadata>(data, "metadata");
    return result;
}
void SessionHistoryIndex::saveGroup(std::int64_t id, const Group& value) const {
    const auto& record = value.record;
    Statement query(db, "INSERT INTO history_groups VALUES(?,?,?) ON CONFLICT(position) DO UPDATE SET data=excluded.data");
    query.bind(1, id);
    query.bind(2, record.id);
    query.bind(3, Json{{"terminal", record.terminal}, {"started", optionalJson(record.startedMs)},
        {"duration", optionalJson(record.durationMs)}, {"generation", optionalJson(record.generation)},
        {"model_call", optionalJson(record.modelCall)}, {"missing_call", record.missingModelCall},
        {"count", value.count}, {"kinds", value.kinds}, {"turn", value.turn}, {"metadata", optionalJson(value.metadata)}}.dump());
    query.step();
}

void SessionHistoryIndex::insertRecord(State& state, const Message& message, std::size_t source,
    Kind kind, std::int64_t part, bool visible) {
    Statement query(db, "INSERT INTO history_records VALUES(?,?,?,?,?)");
    query.bind(1, state.records);
    query.bind(2, message.id);
    query.bind(3, static_cast<std::int64_t>(kind));
    query.bind(4, static_cast<std::int64_t>(source));
    query.bind(5, part);
    query.step();
    if (visible) {
        Statement item(db, "INSERT INTO history_visible VALUES(?,?,?)");
        item.bind(1, state.visible++);
        item.bind(2, message.id);
        item.bind(3, state.records);
        item.step();
    }
    ++state.records;
}

std::int64_t SessionHistoryIndex::ensureGroup(State& state, const Message& message,
    std::size_t source, std::size_t part) {
    if (state.current >= 0) return state.current;
    if (state.previous >= 0) {
        auto previous = group(state.previous);
        previous.record.terminal = false;
        if (message.elapsedMs && previous.record.startedMs && *message.elapsedMs >= *previous.record.startedMs)
            previous.record.durationMs = *message.elapsedMs - *previous.record.startedMs;
        saveGroup(state.previous, previous);
    }
    const auto id = message.id + "/activity/" + std::to_string(part);
    state.current = state.previous = state.records;
    Message identity{id, MessageRole::SYSTEM, {}};
    insertRecord(state, identity, source, Kind::ACTIVITY, -1, false);
    Group value{{Kind::ACTIVITY, id, source, {}, {}, true}};
    value.record.startedMs = message.role == MessageRole::USER ? std::optional<std::uint64_t>{0} : message.elapsedMs;
    value.record.generation = state.generation;
    value.turn = state.turn;
    saveGroup(state.current, value);
    return state.current;
}

void SessionHistoryIndex::updateGeneration(State& state, const Message& message, std::size_t source) {
    const auto active = [&] { return state.current >= 0 ? state.current : state.previous >= 0 ? state.previous : ensureGroup(state, message, source, 0); };
    if (message.role == MessageRole::ASSISTANT && !message.userSkill && (state.current >= 0 || state.previous >= 0 || message.modelCall)) {
        const auto id = active();
        auto value = group(id);
        if (message.modelCall) {
            const auto& call = *message.modelCall;
            state.generation = GenerationMetadata{call.model, call.reasoning, 0};
            value.record.generation = state.generation;
            if (value.record.modelCall) value.record.modelCall->accumulate(call);
            else value.record.modelCall = call;
            if (value.record.missingModelCall) value.record.modelCall->outputTokens.reset();
        } else {
            value.record.missingModelCall = true;
            if (value.record.modelCall) value.record.modelCall->outputTokens.reset();
        }
        saveGroup(id, value);
    }
    if (!message.metadata) return;
    const auto id = active();
    auto value = group(id);
    value.metadata = message.metadata;
    state.generation = message.metadata;
    saveGroup(id, value);
    // Store final turn metadata once; earlier groups inherit it without rewrites.
    Statement turn(db, "INSERT OR IGNORE INTO history_turns VALUES(?,?)");
    turn.bind(1, state.turn);
    turn.bind(2, Json(*message.metadata).dump());
    turn.step();
}

void SessionHistoryIndex::appendItem(State& state, const Message& message, std::size_t source, std::size_t part) {
    const auto& content = message.content[part];
    const auto* result = std::get_if<ToolResultContent>(&content);
    if (result) {
        Statement call(db, "SELECT group_id,item FROM history_calls WHERE id=?");
        call.bind(1, result->callId);
        if (call.step()) {
            const auto id = call.integer(0);
            Statement update(db, "UPDATE history_items SET result_source=?,result_part=? WHERE group_id=? AND position=?");
            update.bind(1, static_cast<std::int64_t>(source));
            update.bind(2, static_cast<std::int64_t>(part));
            update.bind(3, id);
            update.bind(4, call.integer(1));
            update.step();
            Statement remove(db, "DELETE FROM history_calls WHERE id=?");
            remove.bind(1, result->callId);
            remove.step();
            if (refused(*result)) { auto value = group(id); value.kinds |= REFUSED; saveGroup(id, value); }
            return;
        }
    }
    const auto id = ensureGroup(state, message, source, part);
    auto value = group(id);
    Statement insert(db, "INSERT INTO history_items(group_id,position,source,part) VALUES(?,?,?,?)");
    insert.bind(1, id);
    insert.bind(2, static_cast<std::int64_t>(value.count));
    insert.bind(3, static_cast<std::int64_t>(source));
    insert.bind(4, static_cast<std::int64_t>(part));
    insert.step();
    if (const auto* call = std::get_if<ToolCallContent>(&content)) {
        value.kinds |= contentKind(content);
        Statement pending(db, "INSERT INTO history_calls VALUES(?,?,?) ON CONFLICT(id) DO UPDATE SET group_id=excluded.group_id,item=excluded.item");
        pending.bind(1, call->callId);
        pending.bind(2, id);
        pending.bind(3, static_cast<std::int64_t>(value.count));
        pending.step();
    } else value.kinds |= message.userSkill ? SKILLS : std::holds_alternative<ReasoningContent>(content) ? REASONING : TOOLS;
    if (result && refused(*result)) value.kinds |= REFUSED;
    ++value.count;
    saveGroup(id, value);
}

void SessionHistoryIndex::append(const Message& message, std::size_t source) {
    if (message.role == MessageRole::SYSTEM && !message.compaction) return;
    auto value = state();
    if (message.recap) {
        insertRecord(value, message, source, Kind::MESSAGE, -1, true);
    } else if (message.compaction) {
        insertRecord(value, message, source, Kind::COMPACTION, -1, false);
        value.current = value.previous = -1;
    } else if (message.role == MessageRole::USER) {
        value.turn = static_cast<std::int64_t>(source);
        value.generation.reset();
        insertRecord(value, message, source, Kind::MESSAGE, -1, true);
        value.current = value.previous = -1;
        ensureGroup(value, message, source, 0);
    } else {
        updateGeneration(value, message, source);
        bool displayed = false;
        for (std::size_t part = 0; part < message.content.size(); ++part) {
            const auto& content = message.content[part];
            if (const auto* text = std::get_if<TextContent>(&content)) {
                if (message.role == MessageRole::ASSISTANT && text->text.find_first_not_of(" \r\n\t") != std::string::npos) {
                    insertRecord(value, message, source, Kind::MESSAGE, static_cast<std::int64_t>(part), !displayed);
                    displayed = true;
                    value.current = -1;
                }
            } else if (const auto* reasoning = std::get_if<ReasoningContent>(&content)) {
                if (reasoning->text && !reasoning->text->empty()) appendItem(value, message, source, part);
            } else appendItem(value, message, source, part);
        }
    }
    saveState(value);
}

SessionHistoryIndex::Page SessionHistoryIndex::page(const std::string& direction, const std::string& anchor,
    std::size_t minimumSource, std::optional<std::size_t> maximumSource) const {
    auto value = state();
    if (maximumSource) {
        Statement limit(db, "SELECT COALESCE(MIN(position), ?) FROM history_records WHERE source>=?");
        limit.bind(1, value.records);
        limit.bind(2, static_cast<std::int64_t>(*maximumSource));
        limit.step();
        value.records = limit.integer(0);
        Statement visibleLimit(db, "SELECT COUNT(*) FROM history_visible WHERE record<?");
        visibleLimit.bind(1, value.records);
        visibleLimit.step();
        value.visible = visibleLimit.integer(0);
    }
    Statement boundary(db, "SELECT COALESCE(MIN(v.position), (SELECT COUNT(*) FROM history_visible)) FROM history_visible v JOIN history_records r ON r.position=v.record WHERE r.source>=?");
    boundary.bind(1, static_cast<std::int64_t>(minimumSource));
    boundary.step();
    // No visible branch message yet: do not expose inherited activity either.
    Statement first(db, "SELECT position FROM history_records WHERE source>=? ORDER BY position LIMIT 1");
    first.bind(1, static_cast<std::int64_t>(minimumSource));
    if (!first.step() || first.integer(0) >= value.records) return {};
    const auto minimumRecord = first.integer(0);
    const auto minimumVisible = boundary.integer(0);
    constexpr auto SIZE = static_cast<std::int64_t>(SessionHistoryView::PAGE_SIZE);
    constexpr auto STEP = static_cast<std::int64_t>(SessionHistoryView::PAGE_STEP);
    auto end = value.visible, start = std::max(std::int64_t{0}, end - SIZE);
    if (direction == "at") {
        std::int64_t offset = 0;
        const auto parsed = std::from_chars(anchor.data(), anchor.data() + anchor.size(), offset);
        if (parsed.ec != std::errc{} || parsed.ptr != anchor.data() + anchor.size() || offset < 0 || offset >= value.records - minimumRecord)
            throw std::invalid_argument("Invalid history position");
        Statement target(db, "SELECT position FROM history_visible WHERE record<=? ORDER BY record DESC LIMIT 1");
        target.bind(1, minimumRecord + offset);
        const auto position = target.step() ? target.integer(0) : minimumVisible;
        start = std::max(minimumVisible, position - SIZE / 2);
        end = std::min(value.visible, start + SIZE);
    } else if (direction != "latest") {
        Statement find(db, "SELECT position FROM history_visible WHERE id=?");
        find.bind(1, anchor);
        if (!find.step() || find.integer(0) >= value.visible) throw std::invalid_argument("Unknown history anchor");
        if (direction == "before") { start = std::max(std::int64_t{0}, find.integer(0) - STEP); end = std::min(value.visible, start + SIZE); }
        else if (direction == "after") { end = std::min(value.visible, find.integer(0) + 1 + STEP); start = std::max(std::int64_t{0}, end - SIZE); }
        else throw std::invalid_argument("Unknown history direction");
    }
    start = std::max(start, minimumVisible);
    if (end < start) throw std::invalid_argument("History anchor precedes Implement");
    Page result;
    result.hasOlder = start > minimumVisible;
    result.hasNewer = end < value.visible;
    auto firstRecord = minimumRecord, lastRecord = value.records;
    Statement visible(db, "SELECT position,id,record FROM history_visible WHERE position>=? AND position<=? ORDER BY position");
    visible.bind(1, start);
    visible.bind(2, end);
    while (visible.step()) {
        const auto index = visible.integer(0);
        if (index == start && start < end) { result.first = visible.text(1); if (start > 0) firstRecord = visible.integer(2); }
        if (index == end - 1) result.last = visible.text(1);
        if (index == end) lastRecord = std::min(lastRecord, visible.integer(2));
    }
    Statement records(db, "SELECT position FROM history_records WHERE position>=? AND position<? ORDER BY position");
    records.bind(1, firstRecord);
    records.bind(2, lastRecord);
    while (records.step()) result.records.push_back(records.integer(0));
    result.start = static_cast<std::size_t>(firstRecord - minimumRecord);
    result.end = static_cast<std::size_t>(lastRecord - minimumRecord);
    result.total = static_cast<std::size_t>(value.records - minimumRecord);
    Statement last(db, "SELECT position FROM history_groups WHERE position<? ORDER BY position DESC LIMIT 1");
    last.bind(1, value.records);
    if (last.step()) result.lastGroup = last.integer(0);
    return result;
}

std::string SessionHistoryIndex::lastRecordId() const {
    Statement last(db, "SELECT id FROM history_records ORDER BY position DESC LIMIT 1");
    return last.step() ? last.text(0) : "";
}

bool SessionHistoryIndex::lastIsRecap() const {
    // Read the actual tail of the display projection, not the current page or
    // the raw journal tail (which may contain invisible system/audit messages).
    Statement last(db, "SELECT kind,source FROM history_records ORDER BY position DESC LIMIT 1");
    return last.step() && static_cast<Kind>(last.integer(0)) == Kind::MESSAGE
        && summary(static_cast<std::size_t>(last.integer(1))).recap;
}

Json SessionHistoryIndex::groupSummary(std::int64_t id) const {
    auto value = group(id);
    if (!value.record.generation) {
        Statement turn(db, "SELECT generation FROM history_turns WHERE position=?");
        turn.bind(1, value.turn);
        if (turn.step()) value.record.generation = Json::parse(turn.text(0)).get<GenerationMetadata>();
    }
    SessionHistoryView renderer;
    renderer.records.push_back(value.record);
    auto result = renderer.record({}, 0, false);
    result["count"] = value.count;
    if (value.metadata) result["metadata"] = *value.metadata;
    for (const auto& [flag, name] : {std::pair{TOOLS, "tools"}, {READS, "read"}, {SKILLS, "skill"}, {REASONING, "reasoning"}, {REFUSED, "refused"}})
        if (value.kinds & flag) result["kinds"].push_back(name);
    return result;
}

Message SessionHistoryIndex::summary(std::size_t source) const {
    Statement query(db, "SELECT summary FROM messages WHERE position=?");
    query.bind(1, static_cast<std::int64_t>(source));
    if (!query.step()) throw std::runtime_error("Missing activity source");
    ++readSummaryRows;
    return Json::parse(query.text(0)).get<Message>();
}

Json SessionHistoryIndex::item(std::int64_t group, std::size_t index, bool detail, const Reader& reader) const {
    Statement query(db, "SELECT source,part,result_source,result_part FROM history_items WHERE group_id=? AND position=?");
    query.bind(1, group);
    query.bind(2, static_cast<std::int64_t>(index));
    if (!query.step()) throw std::invalid_argument("Unknown activity detail");
    std::vector<std::size_t> sources{static_cast<std::size_t>(query.integer(0))};
    std::vector<Message> summaries{summary(sources.front())};
    SessionHistoryView::Activity activity{{0, static_cast<std::size_t>(query.integer(1))}, {}};
    if (query.integer(2) >= 0) {
        sources.push_back(static_cast<std::size_t>(query.integer(2)));
        summaries.push_back(summary(sources.back()));
        activity.result = SessionHistoryView::Reference{1, static_cast<std::size_t>(query.integer(3))};
    }
    SessionHistoryView renderer;
    renderer.setReader([&](std::size_t index) { return reader(sources.at(index)); });
    return renderer.activityItem(summaries, activity, index, detail);
}

Json SessionHistoryIndex::record(std::int64_t index, bool expanded, const Reader& reader) const {
    Statement query(db, "SELECT id,kind,source,part FROM history_records WHERE position=?");
    query.bind(1, index);
    if (!query.step()) throw std::invalid_argument("Unknown history record");
    const auto kind = static_cast<Kind>(query.integer(1));
    if (kind == Kind::ACTIVITY) {
        auto result = groupSummary(index);
        if (expanded) {
            result["activities"] = Json::array();
            const auto count = result.at("count").get<std::size_t>();
            for (std::size_t i = 0; i < count; ++i) result["activities"].push_back(item(index, i, false, reader));
        }
        return result;
    }
    Record value{kind, query.text(0), static_cast<std::size_t>(query.integer(2)), {}, {}, true};
    if (query.integer(3) >= 0) value.textPart = static_cast<std::size_t>(query.integer(3));
    SessionHistoryView renderer;
    renderer.records.push_back(value);
    renderer.setReader(reader);
    return renderer.record({}, 0, false);
}

std::int64_t SessionHistoryIndex::findGroup(const std::string& id) const {
    Statement query(db, "SELECT position FROM history_groups WHERE id=?");
    query.bind(1, id);
    if (!query.step()) throw std::invalid_argument("Unknown activity group");
    return query.integer(0);
}
Json SessionHistoryIndex::activity(const std::string& id, const Reader& reader) const { return record(findGroup(id), true, reader); }
Json SessionHistoryIndex::detail(const std::string& id, std::size_t index, const Reader& reader) const {
    return {{"id", id}, {"role", "activity"}, {"content", Json::array()},
        {"activities", Json::array({item(findGroup(id), index, true, reader)})}};
}
