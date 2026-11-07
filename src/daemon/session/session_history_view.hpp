#ifndef SOS_SESSION_HISTORY_VIEW_HPP
#define SOS_SESSION_HISTORY_VIEW_HPP

#include <agent-runtime/conversation/messages/message.hpp>
#include <nlohmann/json.hpp>
#include <optional>
#include <functional>
#include <unordered_map>
#include <unordered_set>

// Display index only. Content stays in Session's append-only journal.
class SessionHistoryView {
 public:
  static constexpr std::size_t PAGE_SIZE = 20;
  static constexpr std::size_t PAGE_STEP = 10;
  // Display-only classification; never authorizes execution.
  struct SkillScript {
    std::string name;
    std::string path; // Relative to this skill's scripts/ directory.
  };
  static std::optional<std::string> skillScriptCommand(const std::string& tool, const nlohmann::json& arguments);
  static std::optional<SkillScript> skillScript(const std::string& tool, const nlohmann::json& arguments);
  static std::optional<std::string> skillScriptName(const std::string& tool, const nlohmann::json& arguments);
  static nlohmann::json fileArguments(const std::string& tool, const nlohmann::json& arguments);
  static nlohmann::json fileMetadata(const nlohmann::json& metadata);
  struct Page {
    std::vector<std::size_t> records;
    std::unordered_set<std::size_t> loadedGroups;
    std::string first, last;
    bool hasOlder = false, hasNewer = false;
  };
  void update(const std::vector<Message>& history);
  // Optional disk reader: summaries drive pagination; full bodies are read only
  // for visible texts or an explicitly requested activity detail.
  void setReader(std::function<Message(std::size_t)> reader);
  Page page(const std::string& direction = "latest",
            const std::string& anchor = "", bool clean = false) const;
  nlohmann::json record(const std::vector<Message>& history, std::size_t index,
                        bool includeActivities) const;
  nlohmann::json activities(const std::vector<Message>& history,
                            const std::string& groupId) const;
  nlohmann::json activityDetail(const std::vector<Message>& history,
                               const std::string& groupId, std::size_t index) const;

 private:
   friend class SessionHistoryIndex;
  enum class Kind { MESSAGE, ACTIVITY, COMPACTION };
  struct Reference {
    std::size_t message, part;
  };
  struct Activity {
    Reference source;
    std::optional<Reference> result;
  };
  struct Record {
    Kind kind;
    std::string id;
    std::size_t source;
    std::vector<Activity> activities;
    std::optional<std::size_t> metadata;
    bool terminal = true;
    std::optional<std::size_t> textPart;
    std::optional<std::uint64_t> startedMs;
    std::optional<std::uint64_t> durationMs;
    std::optional<GenerationMetadata> generation;
    std::optional<ModelCallMetadata> modelCall;
    bool missingModelCall = false;
  };
  std::size_t ensureGroup(const Message& message, std::size_t source,
                          std::size_t part);
  nlohmann::json activityData(const std::vector<Message>& history,
                              const Record& group) const;
  nlohmann::json activityItem(const std::vector<Message>& history,
                             const Activity& activity, std::size_t index, bool detail) const;
  std::vector<Record> records;
  std::vector<std::size_t> visible;
  std::unordered_map<std::string, std::size_t> messages, groups;
  std::unordered_map<std::string, std::pair<std::size_t, std::size_t>> calls;
  std::optional<std::size_t> currentGroup;
  std::optional<std::size_t> previousGroup;
  std::vector<std::size_t> turnGroups;
  std::optional<GenerationMetadata> turnGeneration;
  std::size_t indexed = 0;
  std::function<Message(std::size_t)> read;
};

#endif  // SOS_SESSION_HISTORY_VIEW_HPP
