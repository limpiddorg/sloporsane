#ifndef SOS_SESSION_HISTORY_INDEX_HPP
#define SOS_SESSION_HISTORY_INDEX_HPP

#include "session_database.hpp"
#include "session_history_view.hpp"

class SessionJournal;

// Persistent display projection. Mutations share the source message transaction;
// no process-local projector state survives a rollback or needs replay on open.
class SessionHistoryIndex {
public:
    using Reader = std::function<Message(std::size_t)>;
    struct Page {
        std::string first, last;
        bool hasOlder = false, hasNewer = false;
        std::vector<std::int64_t> records;
        std::int64_t lastGroup = -1;
        std::size_t start = 0, end = 0, total = 0;
    };
    explicit SessionHistoryIndex(SessionDatabase& db) : db(db) {}
    Page page(const std::string& direction, const std::string& anchor, std::size_t minimumSource = 0,
        std::optional<std::size_t> maximumSource = {}) const;
    bool lastIsRecap() const;
    std::string lastRecordId() const;
    nlohmann::json record(std::int64_t index, bool expanded, const Reader& reader) const;
    nlohmann::json activity(const std::string& id, const Reader& reader) const;
    nlohmann::json detail(const std::string& id, std::size_t item, const Reader& reader) const;
    std::size_t summaryRowsRead() const { return readSummaryRows; }

private:
    friend class SessionJournal;
    using Record = SessionHistoryView::Record;
    using Kind = SessionHistoryView::Kind;
    struct State {
        std::int64_t records = 0, visible = 0, current = -1, previous = -1, turn = -1;
        std::optional<GenerationMetadata> generation;
    };
    struct Group {
        Record record;
        std::size_t count = 0;
        unsigned kinds = 0;
        std::int64_t turn = -1;
        std::optional<GenerationMetadata> metadata;
    };
    void create();
    void append(const Message& message, std::size_t source);
    State state() const;
    void saveState(const State& state);
    Group group(std::int64_t id) const;
    void saveGroup(std::int64_t id, const Group& group) const;
    std::int64_t ensureGroup(State& state, const Message& message, std::size_t source, std::size_t part);
    void insertRecord(State& state, const Message& message, std::size_t source, Kind kind,
        std::int64_t part, bool visible);
    void updateGeneration(State& state, const Message& message, std::size_t source);
    void appendItem(State& state, const Message& message, std::size_t source, std::size_t part);
    nlohmann::json groupSummary(std::int64_t id) const;
    nlohmann::json item(std::int64_t group, std::size_t index, bool detail, const Reader& reader) const;
    std::int64_t findGroup(const std::string& id) const;
    Message summary(std::size_t source) const;
    SessionDatabase& db;
    mutable std::size_t readSummaryRows = 0;
};

#endif // SOS_SESSION_HISTORY_INDEX_HPP
