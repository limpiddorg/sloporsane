#ifndef SOS_BTW_CONVERSATION_HPP
#define SOS_BTW_CONVERSATION_HPP

#include "session_state.hpp"

// Separate from the principal journal/context. The snapshot never changes.
struct BtwConversation {
    std::string id;
    std::string anchor;
    std::string pageAnchor;
    bool standalone = false;
    bool review = false;
    bool revisionPlanning = false;
    std::vector<Message> snapshot;
    std::vector<Message> messages;
    std::vector<Message> activeContext;
    std::size_t contextMessageCount = 0;
    std::size_t reviewSegmentStart = 0;
    std::string contextConstructionKey;
    std::string status = "idle";
    std::string error;
    std::string model;
    std::string reasoning;
    std::string provider;
    std::string reviewSummary;
    std::optional<std::size_t> reviewStep;
};
void to_json(nlohmann::json& json, const BtwConversation& branch);
void from_json(const nlohmann::json& json, BtwConversation& branch);
std::vector<Message> freezeBtwContext(std::vector<Message> messages);
std::vector<Message> currentReviewMessages(const BtwConversation& branch);
std::unique_ptr<Agent> makeBtwAgent(const BtwConversation& branch, std::unique_ptr<Provider> provider,
    const ProviderSelection& selection, const std::filesystem::path& workspace,
    std::function<Image(const std::string&)> readImage, const std::filesystem::path& authorizedRoot,
    bool allowRework = false, std::vector<std::filesystem::path> historicalRoots = {},
    std::function<void(const std::function<void()>&)> prepareFiles = {});

#endif // SOS_BTW_CONVERSATION_HPP
