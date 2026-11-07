#ifndef SOS_COMPOSER_HPP
#define SOS_COMPOSER_HPP
#include "draft_buffer.hpp"
#include <ftxui/component/component_base.hpp>
#include <functional>
#include <algorithm>
namespace sos::tui {
class Composer final : public ftxui::ComponentBase {
public:
    Composer(DraftBuffer& buffer, std::function<void()> changed, std::function<void(PasteSpan)> preview,
             std::function<void(std::string)> copied);
    ftxui::Element OnRender() override;
    bool OnEvent(ftxui::Event event) override;
    bool Focusable() const override { return true; }
    void paste(const std::string& text);
    int rows() const { return std::min(6, lines_); }
    bool mouseSelection() const { return dragging_; }
    bool contains(int x, int y) const { return box_.Contain(x, y); }
private:
    class View;
    struct Cell { std::size_t byte; std::string glyph; int x, y, width; bool block; };
    DraftBuffer& buffer_;
    std::function<void()> changed_;
    std::function<void(PasteSpan)> preview_;
    std::function<void(std::string)> copied_;
    std::vector<Cell> cells_;
    ftxui::Box box_;
    int width_ = 80, lines_ = 1, firstLine_ = 0, cursorX_ = 0, cursorY_ = 0;
    std::optional<int> anchor_;
    std::optional<PasteSpan> clicked_;
    bool dragging_ = false;
    bool followCursor_ = true;
    std::string previousText_;
    int mouseX_ = 0, mouseY_ = 0;
    void layout(int width);
    int position(int x, int y) const;
    void removeSelection();
    std::pair<std::size_t, std::size_t> selection() const;
    void copySelection();
};
}
#endif
