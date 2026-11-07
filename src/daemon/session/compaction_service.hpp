#ifndef SOS_COMPACTION_SERVICE_HPP
#define SOS_COMPACTION_SERVICE_HPP

#include "session_state.hpp"
#include <agent-runtime/conversation/compaction_context.hpp>

struct CompactionResult {
    Conversation conversation;
    Message summary;
};

class CompactionService {
public:
    static constexpr std::size_t DEFAULT_RECENT_TOKENS = 8000;
    static constexpr std::size_t MAX_SUMMARY_ATTEMPTS = 3;
    using ProviderFactory = std::function<std::unique_ptr<Provider>(const SessionInfo&)>;
    bool needCompaction(const CompactionContext& context, std::span<const Message> pendingMessages = {}) const;
    static std::uint64_t estimateTokens(const CompactionContext& context, std::span<const Message> pendingMessages = {});
    static std::optional<std::uint64_t> triggerBudget(const ModelInfo& model);
    CompactionResult compact(const Conversation& conversation, Provider& provider, const SessionInfo& info,
        const std::string& summaryId, core::CancellationCheck cancelled = {}, std::size_t recentTokenBudget = DEFAULT_RECENT_TOKENS,
        bool preserveCurrentTurn = false, std::function<void(std::size_t)> progress = {}) const;
    static std::size_t recentBudget(std::optional<std::uint64_t> contextLimit);
    // Rebuild a checkpoint from the original messages referenced by its IDs.
    static Conversation restore(const std::vector<Message>& history, const Message& summary);
    static std::string summarizeReview(const std::vector<Message>& messages, Provider& provider,
        const ProviderSelection& selection, core::CancellationCheck cancelled);
};

#endif // SOS_COMPACTION_SERVICE_HPP
