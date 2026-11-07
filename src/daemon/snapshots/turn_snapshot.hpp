#ifndef SOS_TURN_SNAPSHOT_HPP
#define SOS_TURN_SNAPSHOT_HPP

#include <agent-runtime/conversation/messages/message_json.hpp>
#include <filesystem>

struct SnapshotFileState {
    bool exists = false;
    std::string blob;
    unsigned mode = 0;
    std::string kind;
    bool operator==(const SnapshotFileState&) const = default;
};
struct SnapshotEdit {
    std::string path;
    SnapshotFileState before, after;
    bool complete = false;
};
struct WorkspaceSnapshot {
    std::string root, before, after;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WorkspaceSnapshot, root, before, after)
struct TurnSnapshot {
    std::int64_t id = 0;
    std::size_t start = 0;
    std::string root, before, after;
    std::vector<SnapshotEdit> edits;
    bool complete = false;
    bool undone = false;
    std::string operation; // Durable navigation intent, cleared with history publication.
    std::string modeBefore = "sane", modeAfter = "sane";
    std::vector<Message> messages; // Archived tail, including interrupted/error turns.
    std::vector<WorkspaceSnapshot> workspaces; // Other roots visited by this turn; never rebase their paths.
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SnapshotFileState, exists, blob, mode, kind)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SnapshotEdit, path, before, after, complete)
inline void to_json(nlohmann::json& json, const TurnSnapshot& turn) {
    json = {{"id", turn.id}, {"start", turn.start}, {"root", turn.root}, {"before", turn.before}, {"after", turn.after},
        {"edits", turn.edits}, {"complete", turn.complete}, {"undone", turn.undone}, {"operation", turn.operation},
        {"modeBefore", turn.modeBefore}, {"modeAfter", turn.modeAfter}, {"messages", turn.messages}, {"workspaces", turn.workspaces}};
}
inline void from_json(const nlohmann::json& json, TurnSnapshot& turn) {
    json.at("id").get_to(turn.id); json.at("start").get_to(turn.start);
    json.at("root").get_to(turn.root); json.at("before").get_to(turn.before); json.at("after").get_to(turn.after);
    json.at("edits").get_to(turn.edits); json.at("complete").get_to(turn.complete); json.at("undone").get_to(turn.undone);
    json.at("operation").get_to(turn.operation); json.at("modeBefore").get_to(turn.modeBefore);
    json.at("modeAfter").get_to(turn.modeAfter); json.at("messages").get_to(turn.messages);
    json.at("workspaces").get_to(turn.workspaces);
}

#endif // SOS_TURN_SNAPSHOT_HPP
