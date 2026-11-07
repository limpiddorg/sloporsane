#ifndef SOS_SESSION_JOURNAL_HPP
#define SOS_SESSION_JOURNAL_HPP

#include "session_state.hpp"
#include "btw_conversation.hpp"
#include "session_database.hpp"
#include "session_recovery_index.hpp"
#include "session_history_index.hpp"
#include "../snapshots/turn_snapshot.hpp"
#include <agent-runtime/core/cancellation.hpp>
#include <atomic>
#include <functional>

// SQLite is the authoritative session store.
// The all-summary API is retained for context reconstruction and reference tests;
// constructing a journal and reading an individual message do not load it.
class SessionJournal {
public:
    explicit SessionJournal(std::filesystem::path directory, core::CancellationCheck cancelled = {});
    void append(const std::vector<Message>& messages, std::uint64_t updatedAt);
    void save(const std::vector<Message>& messages, const SessionInfo& info);
    std::optional<SessionInfo> info() const;
    void publishInfo() const;
    const std::vector<Message>& summaries(core::CancellationCheck cancelled = {}) const;
    std::size_t count() const;
    std::uint64_t updatedAt() const;
    std::uintmax_t sizeBytes() const;
    std::uintmax_t bytesRead() const;
    std::size_t summaryRowsRead() const { return readSummaryRows + displayIndex.summaryRowsRead(); }
    std::function<Message(std::size_t)> reader(core::CancellationCheck cancelled = {}, bool pixels = true) const;
    Image readImage(const std::string& id) const;
    bool hasImages() const;
    BtwConversation createBtw(BtwConversation branch);
    void saveBtw(const BtwConversation& branch);
    void saveAcceptedReview(const Message& message, const SessionInfo& info, const BtwConversation& branch);
    BtwConversation readBtw(const std::string& id) const;
    std::vector<BtwConversation> listBtw() const;
    std::vector<Message> readAll(core::CancellationCheck cancelled = {}) const;
    std::vector<Message> readFrom(std::size_t start) const;
    std::string lastUserMessage() const;
    bool objectivePlanReady() const;
    bool implementationPlanReady() const;
    const SessionRecoveryIndex& recovery() const { return recoveryIndex; }
    const SessionHistoryIndex& historyIndex() const { return displayIndex; }
    TurnSnapshot beginTurn(TurnSnapshot turn);
    void saveTurn(const TurnSnapshot& turn);
    std::optional<TurnSnapshot> navigationTurn(bool redo) const;
    bool navigationPending() const;
    void navigate(const TurnSnapshot& turn, const SessionInfo& info);
    void reload() { display.clear(); displayLoaded = false; }

private:
    bool planReady(bool implementation) const;
    static Message summarize(const Message& message);
    void initialize(core::CancellationCheck cancelled);
    void insert(const std::vector<Message>& messages, std::uint64_t time, std::size_t first,
        core::CancellationCheck cancelled = {});
    void storeInfo(const SessionInfo& info);
    void commit(const std::vector<Message>& messages, std::uint64_t time, const SessionInfo* info, const BtwConversation* branch = nullptr);
    std::filesystem::path directory;
    mutable SessionDatabase db;
    SessionRecoveryIndex recoveryIndex{db};
    SessionHistoryIndex displayIndex{db};
    mutable std::vector<Message> display;
    mutable bool displayLoaded = false;
    mutable std::atomic<std::uintmax_t> readBytes{0};
    mutable std::atomic<std::size_t> readSummaryRows{0};
};

#endif // SOS_SESSION_JOURNAL_HPP
