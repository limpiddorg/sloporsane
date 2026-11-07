#ifndef SOS_SESSION_HISTORY_SERVICE_HPP
#define SOS_SESSION_HISTORY_SERVICE_HPP

#include "session_journal.hpp"
#include "session_history_view.hpp"

// Session-owned display service. Calls are serialized by the session's event
// consumer. Page readers must not outlive the service. Journal bodies remain lazy.
class SessionHistoryService {
public:
    struct Page {
        std::string first, last;
        bool hasOlder = false, hasNewer = false;
        std::size_t count = 0;
        std::function<nlohmann::json(std::size_t)> record;
        std::size_t start = 0, end = 0, total = 0;
    };

    explicit SessionHistoryService(std::shared_ptr<SessionJournal> journal);
    Page page(const std::string& direction = "latest", const std::string& anchor = "", bool clean = false,
        std::size_t minimumSource = 0, std::optional<std::size_t> maximumSource = {});
    nlohmann::json activity(const std::string& groupId);
    nlohmann::json detail(const std::string& groupId, std::size_t index);

private:
    std::shared_ptr<SessionJournal> journal;
};

#endif // SOS_SESSION_HISTORY_SERVICE_HPP
