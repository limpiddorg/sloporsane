#include "session_history_service.hpp"
#include <stdexcept>

SessionHistoryService::SessionHistoryService(std::shared_ptr<SessionJournal> journal)
    : journal(std::move(journal)) {
    if (!this->journal) throw std::invalid_argument("History service requires a journal");
}

SessionHistoryService::Page SessionHistoryService::page(const std::string& direction,
    const std::string& anchor, bool /*clean*/, std::size_t minimumSource, std::optional<std::size_t> maximumSource) {
    auto selection = journal->historyIndex().page(direction, anchor, minimumSource, maximumSource);
    Page result{selection.first, selection.last, selection.hasOlder, selection.hasNewer, selection.records.size(), {}};
    result.start = selection.start;
    result.end = selection.end;
    result.total = selection.total;
    result.record = [this, selection = std::move(selection)](std::size_t index) {
        const auto record = selection.records.at(index);
        return journal->historyIndex().record(record, record == selection.lastGroup, journal->reader({}, false));
    };
    return result;
}

nlohmann::json SessionHistoryService::activity(const std::string& groupId) {
    return journal->historyIndex().activity(groupId, journal->reader({}, false));
}

nlohmann::json SessionHistoryService::detail(const std::string& groupId, std::size_t index) {
    return journal->historyIndex().detail(groupId, index, journal->reader({}, false));
}
