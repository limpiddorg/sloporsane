#include "history_viewport.hpp"
#include <algorithm>
#include <ftxui/dom/node.hpp>
namespace sos::tui {
void HistoryViewport::scroll(int lines) {
    const int previous = offset;
    offset = std::clamp(offset + lines, 0, maximum);
    for (auto& [id, position] : positions) position -= offset - previous;
    following = atBottom(); snapBottom = false; alignId.clear();
}
void HistoryViewport::bottom() { following = snapBottom = true; alignId.clear(); }
void HistoryViewport::align(std::string id) { alignId = std::move(id); following = true; snapBottom = false; }
namespace {
class ViewportNode final : public ftxui::Node {
public:
    ViewportNode(std::vector<ViewportRow> rows, HistoryViewport& state) : rows_(std::move(rows)), state_(state) {
        for (const auto& row : rows_) children_.push_back(row.element);
    }
    void ComputeRequirement() override {
        Node::ComputeRequirement();
        requirement_.min_x = requirement_.min_y = 0;
        requirement_.flex_grow_x = requirement_.flex_grow_y = 1;
        requirement_.flex_shrink_x = requirement_.flex_shrink_y = 1;
    }
    void SetBox(ftxui::Box box) override {
        box_ = box;
        state_.height = std::max(1, box.y_max - box.y_min + 1);
        int total = 0, lastUser = -1;
        std::map<std::string, int> positions;
        for (const auto& row : rows_) {
            positions[row.id] = total;
            if (row.user) lastUser = total;
            total += row.element->requirement().min_y;
        }
        // Reserve at least one viewport for the latest user turn.
        if (lastUser >= 0) total = std::max(total, lastUser + state_.height);
        state_.maximum = std::max(0, total - state_.height);
        if (state_.snapBottom) state_.offset = state_.maximum;
        else if (!state_.alignId.empty() && positions.contains(state_.alignId)) state_.offset = positions.at(state_.alignId);
        else {
            // Retain the first shared visible row and its screen position across reflow/page swaps.
            std::string anchor;
            int relative = 0;
            for (const auto& [id, oldPosition] : state_.positions) {
                if (!positions.contains(id)) continue;
                if (anchor.empty() || (oldPosition <= 0 && (relative > 0 || oldPosition > relative)) ||
                    (oldPosition > 0 && relative > oldPosition)) { anchor = id; relative = oldPosition; }
            }
            if (!anchor.empty()) state_.offset = positions.at(anchor) - relative;
        }
        state_.offset = std::clamp(state_.offset, 0, state_.maximum);
        int y = box.y_min - state_.offset;
        for (const auto& row : rows_) {
            const int height = row.element->requirement().min_y;
            row.element->SetBox({box.x_min, box.x_max, y, y + height - 1}); y += height;
        }
        for (auto& [id, position] : positions) position -= state_.offset;
        state_.positions = std::move(positions);
        // Consume scroll requests after rendering, not during repeated layout passes.
    }
    void Render(ftxui::Screen& screen) override {
        const auto previous = screen.stencil;
        screen.stencil = ftxui::Box::Intersection(box_, previous);
        Node::Render(screen);
        screen.stencil = previous;
        state_.snapBottom = false; state_.alignId.clear();
    }
private:
    std::vector<ViewportRow> rows_;
    HistoryViewport& state_;
};
}
ftxui::Element historyViewport(std::vector<ViewportRow> rows, HistoryViewport& state) {
    return std::make_shared<ViewportNode>(std::move(rows), state);
}
}
