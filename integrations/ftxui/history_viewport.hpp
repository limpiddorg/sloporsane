#ifndef SOS_HISTORY_VIEWPORT_HPP
#define SOS_HISTORY_VIEWPORT_HPP
#include <ftxui/dom/elements.hpp>
#include <map>
#include <string>
#include <vector>
namespace sos::tui {
struct ViewportRow { std::string id; ftxui::Element element; bool user = false; };
struct HistoryViewport {
    int offset = 0, maximum = 0, height = 1;
    bool following = true, snapBottom = true;
    std::string alignId;
    std::map<std::string, int> positions;
    void scroll(int lines);
    void bottom();
    void align(std::string id);
    bool atTop() const { return offset == 0; }
    bool atBottom() const { return offset >= maximum; }
};
ftxui::Element historyViewport(std::vector<ViewportRow> rows, HistoryViewport& state);
}
#endif
