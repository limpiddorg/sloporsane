#ifndef SOS_TUI_MODEL_HPP
#define SOS_TUI_MODEL_HPP
#include "client.hpp"
#include "attachments.hpp"
#include <map>
#include <optional>
#include <vector>
namespace sos::tui {
std::string stringField(const Json& value, const char* key, std::string fallback = "");
struct Info {
    std::string model, provider, reasoning, permissionMode = "ask";
    std::optional<long long> used, total, contextLimit;
    bool hasKey = false, lastRecap = false;
    void update(const Json& value);
};
struct Entry {
    std::string id, title, body, group, refusedId;
    std::string callId;
    std::string toolName, output;
    Json arguments = Json::object(), resultMetadata = Json::object();
    bool resultReceived = false, error = false;
    unsigned activityKinds = 0;
    std::optional<long long> durationMs, modelCallMs, outputTokens;
    unsigned modelCalls = 0;
    std::string mode = "sane";
    int activityIndex = -1;
    bool folded = false, expandable = false, loaded = true, approved = false;
    enum class Kind { User, Text, Activity, Tool, Reasoning, Recap, Compaction, Btw, Error, Footer } kind = Kind::Text;
};
struct Page {
    std::string first, last;
    bool older = false, newer = false;
};
struct Branch {
    std::string anchor, pageAnchor;
    bool standalone = false;
    Entry entry;
};
class Transfer {
public:
    explicit Transfer(const Json& header);
    bool accept(const Json& event);
    std::string request, sessionId, purpose, group;
    int activityIndex = -1;
    Json session, branch;
    Page page;
    std::vector<Json> messages;
private:
    std::size_t count_ = 0, part_ = 0;
    std::string fragment_;
};
class Model {
public:
    std::string workspace, launchWorkspace, sessionId, title = "-", mode = "sane";
    std::string draft, queued, provisional, pendingRequest, status;
    std::vector<Image> images, queuedImages, provisionalImages;
    Info info;
    Page page;
    bool busy = false, loading = false, connected = true;
    std::vector<Entry> entries;
    std::map<std::string, Branch> branches;
    std::map<std::string, Transfer> transfers;
    void apply(const Json& event);
    void append(const Json& message, bool projected = false);
    void begin(const std::string& text, const std::string& request);
    void restore();
    void restoreQueue();
private:
    std::string liveGroup_;
    void ensureGroup(const std::string& id, unsigned part);
    std::string openBranch_;
    void placeBranches();
    void finishTransfer(Transfer& transfer);
};
}
#endif
