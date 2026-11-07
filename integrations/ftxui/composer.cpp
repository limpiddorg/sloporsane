#include "composer.hpp"
#include <algorithm>
#include <cctype>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/string.hpp>
namespace sos::tui {
using namespace ftxui;
namespace {
std::size_t previousGlyph(const std::string& text, std::size_t cursor) {
    std::size_t offset = 0, previous = 0;
    for (const auto& glyph : Utf8ToGlyphs(text)) {
        if (offset >= cursor) break;
        previous = offset; offset += glyph.size();
    }
    return previous;
}
std::size_t nextGlyph(const std::string& text, std::size_t cursor) {
    std::size_t offset = 0;
    for (const auto& glyph : Utf8ToGlyphs(text)) { offset += glyph.size(); if (offset > cursor) return offset; }
    return text.size();
}
}
Composer::Composer(DraftBuffer& buffer, std::function<void()> changed, std::function<void(PasteSpan)> preview,
                   std::function<void(std::string)> copied)
    : buffer_(buffer), changed_(std::move(changed)), preview_(std::move(preview)), copied_(std::move(copied)) {}
void Composer::layout(int width) {
    buffer_.sync();
    const auto newWidth = std::max(1, width);
    const auto& current = buffer_.visible();
    if (newWidth == width_ && current == previousText_ && !cells_.empty()) {
        const auto cursor = std::lower_bound(cells_.begin(), cells_.end(), static_cast<std::size_t>(buffer_.cursor), [](const Cell& cell, std::size_t byte) { return cell.byte < byte; });
        if (cursor != cells_.end()) { cursorX_ = cursor->x; cursorY_ = cursor->y; }
        const auto visible = std::min(6, lines_);
        if (followCursor_) firstLine_ = std::clamp(firstLine_, std::max(0, cursorY_ - visible + 1), cursorY_);
        firstLine_ = std::min(firstLine_, std::max(0, lines_ - visible));
        return;
    }
    width_ = newWidth; cells_.clear();
    if (anchor_ && current != previousText_) {
        std::size_t prefix = 0, suffix = 0;
        while (prefix < current.size() && prefix < previousText_.size() && current[prefix] == previousText_[prefix]) ++prefix;
        while (suffix < current.size() - prefix && suffix < previousText_.size() - prefix && current[current.size() - suffix - 1] == previousText_[previousText_.size() - suffix - 1]) ++suffix;
        const auto oldEnd = previousText_.size() - suffix, newEnd = current.size() - suffix;
        if (static_cast<std::size_t>(*anchor_) >= oldEnd) *anchor_ = static_cast<int>(static_cast<std::size_t>(*anchor_) - oldEnd + newEnd);
        else if (static_cast<std::size_t>(*anchor_) > prefix) *anchor_ = static_cast<int>(newEnd);
    }
    previousText_ = current;
    buffer_.cursor = std::clamp(buffer_.cursor, 0, static_cast<int>(buffer_.visible().size()));
    int x = 0, y = 0; std::size_t byte = 0;
    cursorX_ = cursorY_ = 0;
    for (const auto& glyph : Utf8ToGlyphs(buffer_.visible())) {
        int columns = std::max(0, string_width(glyph));
        if (glyph == "\t") columns = std::min(4 - x % 4, width_);
        if (glyph != "\n" && x + columns > width_) { x = 0; ++y; }
        if (byte == static_cast<std::size_t>(buffer_.cursor)) { cursorX_ = x; cursorY_ = y; }
        cells_.push_back({byte, glyph, x, y, columns, buffer_.at(byte).has_value()});
        byte += glyph.size();
        if (glyph == "\n") { x = 0; ++y; } else x += columns;
    }
    if (x >= width_) { x = 0; ++y; }
    if (static_cast<std::size_t>(buffer_.cursor) == byte) { cursorX_ = x; cursorY_ = y; }
    cells_.push_back({byte, " ", x, y, 1, false}); lines_ = y + 1;
    const auto visible = std::min(6, lines_);
    if (followCursor_ && cursorY_ < firstLine_) firstLine_ = cursorY_;
    if (followCursor_ && cursorY_ >= firstLine_ + visible) firstLine_ = cursorY_ - visible + 1;
    firstLine_ = std::clamp(firstLine_, 0, std::max(0, lines_ - visible));
}
std::pair<std::size_t, std::size_t> Composer::selection() const {
    auto a = static_cast<std::size_t>(std::clamp(anchor_.value_or(buffer_.cursor), 0, static_cast<int>(buffer_.visible().size())));
    auto b = static_cast<std::size_t>(std::clamp(buffer_.cursor, 0, static_cast<int>(buffer_.visible().size())));
    if (a > b) std::swap(a, b);
    if (a != b) {
        if (auto span = buffer_.at(a)) a = span->start;
        if (b > 0) if (auto span = buffer_.at(b - 1)) b = span->end;
    }
    return {a, b};
}
void Composer::removeSelection() {
    const auto [a, b] = selection();
    if (a != b) buffer_.erase(a, b);
    anchor_.reset();
}
void Composer::copySelection() {
    const auto [a, b] = selection();
    if (a != b) copied_(buffer_.expandRange(a, b));
}
void Composer::paste(const std::string& value) { layout(width_); followCursor_ = true; removeSelection(); buffer_.insert(value, true); changed_(); }
int Composer::position(int x, int y) const {
    int result = 0;
    for (const auto& cell : cells_) {
        if (cell.y > y || (cell.y == y && cell.x > x)) break;
        result = static_cast<int>(cell.byte);
    }
    return result;
}
class Composer::View final : public Node {
public:
    explicit View(Composer& owner) : owner_(owner) {}
    void ComputeRequirement() override {
        owner_.layout(owner_.width_);
        requirement_.min_x = 1; requirement_.min_y = std::min(6, owner_.lines_);
        requirement_.flex_grow_x = 1;
    }
    void SetBox(Box box) override {
        box_ = owner_.box_ = box;
        const auto width = box.x_max - box.x_min + 1;
        retry_ = owner_.width_ != width;
        owner_.layout(width);
    }
    void Check(Status* status) override { if (retry_) { status->need_iteration = true; retry_ = false; } }
    void Render(Screen& screen) override {
        const auto clip = Box::Intersection(box_, screen.stencil);
        const auto [start, end] = owner_.selection();
        auto begin = std::lower_bound(owner_.cells_.begin(), owner_.cells_.end(), owner_.firstLine_, [](const Cell& cell, int line) { return cell.y < line; });
        for (auto it = begin; it != owner_.cells_.end() && it->y < owner_.firstLine_ + 6; ++it) {
            const auto& cell = *it;
            const auto x = box_.x_min + cell.x, y = box_.y_min + cell.y - owner_.firstLine_;
            if (!clip.Contain(x, y) || cell.glyph == "\n") continue;
            auto& pixel = screen.PixelAt(x, y);
            pixel.character = cell.glyph == "\t" ? " " : cell.glyph;
            pixel.foreground_color = cell.block ? Color::RGB(233, 196, 106) : Color::RGB(230, 237, 243);
            pixel.inverted = cell.byte >= start && cell.byte < end;
            for (int i = 1; i < cell.width && clip.Contain(x + i, y); ++i) {
                auto& next = screen.PixelAt(x + i, y); next.character = cell.glyph == "\t" ? " " : ""; next.inverted = pixel.inverted;
            }
        }
        if (owner_.buffer_.visible().empty()) {
            const std::string placeholder = "Message or /command";
            for (std::size_t i = 0; i < placeholder.size() && clip.Contain(box_.x_min + i, box_.y_min); ++i) {
                auto& pixel = screen.PixelAt(box_.x_min + i, box_.y_min); pixel.character = placeholder.substr(i, 1); pixel.foreground_color = Color::RGB(113, 128, 142);
            }
        }
        if (owner_.Focused()) {
            const auto x = box_.x_min + owner_.cursorX_, y = box_.y_min + owner_.cursorY_ - owner_.firstLine_;
            if (clip.Contain(x, y)) screen.SetCursor({x, y, Screen::Cursor::Bar});
        }
    }
private:
    Composer& owner_; bool retry_ = false;
};
Element Composer::OnRender() { return std::make_shared<View>(*this); }
bool Composer::OnEvent(Event event) {
    layout(width_);
    if (event.is_mouse()) {
        const auto mouse = event.mouse();
        if ((mouse.button == Mouse::WheelUp || mouse.button == Mouse::WheelDown) && box_.Contain(mouse.x, mouse.y)) {
            followCursor_ = false;
            firstLine_ = std::clamp(firstLine_ + (mouse.button == Mouse::WheelUp ? -3 : 3), 0, std::max(0, lines_ - 6)); return true;
        }
        if (mouse.button != Mouse::Left) return false;
        if (mouse.motion == Mouse::Pressed && box_.Contain(mouse.x, mouse.y)) {
            TakeFocus();
            followCursor_ = true;
            const auto selected = position(mouse.x - box_.x_min, mouse.y - box_.y_min + firstLine_);
            if (!dragging_) { anchor_ = selected; clicked_ = buffer_.at(selected); mouseX_ = mouse.x; mouseY_ = mouse.y; }
            dragging_ = true; buffer_.cursor = selected;
            return true;
        }
        if (dragging_ && mouse.motion == Mouse::Moved) {
            buffer_.cursor = position(mouse.x - box_.x_min, mouse.y - box_.y_min + firstLine_); clicked_.reset(); return true;
        }
        if (dragging_ && mouse.motion == Mouse::Released) {
            dragging_ = false;
            if (mouse.x != mouseX_ || mouse.y != mouseY_) {
                buffer_.cursor = position(mouse.x - box_.x_min, mouse.y - box_.y_min + firstLine_); copySelection(); clicked_.reset();
            } else {
                anchor_.reset(); if (clicked_) { auto span = *clicked_; buffer_.cursor = static_cast<int>(span.end); clicked_.reset(); preview_(span); }
            }
            return true;
        }
        return false;
    }
    followCursor_ = true;
    if (event == Event::CtrlZ || event == Event::CtrlY) {
        anchor_.reset(); if (event == Event::CtrlZ) buffer_.undo(); else buffer_.redo(); changed_(); return true;
    }
    if (event == Event::CtrlA) { anchor_ = 0; buffer_.cursor = static_cast<int>(buffer_.visible().size()); return true; }
    if (event == Event::CtrlX) { copySelection(); removeSelection(); changed_(); return true; }
    if (event.is_character()) { removeSelection(); buffer_.insert(event.character()); changed_(); return true; }
    if (event == Event::Backspace || event == Event::Delete) {
        auto [a, b] = selection();
        if (a != b) removeSelection();
        else if (!buffer_.remove(event == Event::Backspace)) {
            const auto& text = buffer_.visible();
            if (event == Event::Backspace && buffer_.cursor > 0) buffer_.erase(previousGlyph(text, buffer_.cursor), buffer_.cursor);
            else if (event == Event::Delete && buffer_.cursor < static_cast<int>(text.size())) buffer_.erase(buffer_.cursor, nextGlyph(text, buffer_.cursor));
        }
        changed_(); return true;
    }
    const bool shift = event == Event::Special("\x1b[1;2D") || event == Event::Special("\x1b[1;2C") || event == Event::Special("\x1b[1;2A") || event == Event::Special("\x1b[1;2B");
    if (shift && !anchor_) anchor_ = buffer_.cursor;
    if (!shift) anchor_.reset();
    if (event == Event::ArrowLeft || event == Event::Special("\x1b[1;2D")) {
        if (!buffer_.move(false)) buffer_.cursor = static_cast<int>(previousGlyph(buffer_.visible(), buffer_.cursor)); return true;
    }
    if (event == Event::ArrowRight || event == Event::Special("\x1b[1;2C")) {
        if (!buffer_.move(true)) buffer_.cursor = static_cast<int>(nextGlyph(buffer_.visible(), buffer_.cursor)); return true;
    }
    if (event == Event::ArrowLeftCtrl || event == Event::ArrowRightCtrl) {
        const auto& text = buffer_.visible();
        const bool right = event == Event::ArrowRightCtrl;
        bool seenWord = false;
        while (right ? buffer_.cursor < static_cast<int>(text.size()) : buffer_.cursor > 0) {
            const auto next = right ? nextGlyph(text, buffer_.cursor) : previousGlyph(text, buffer_.cursor);
            const auto character = static_cast<unsigned char>(text[right ? buffer_.cursor : next]);
            if (std::isspace(character) && seenWord) break;
            if (!std::isspace(character)) seenWord = true;
            buffer_.cursor = static_cast<int>(next);
        }
        if (auto span = buffer_.at(buffer_.cursor); span && span->start != static_cast<std::size_t>(buffer_.cursor)) buffer_.cursor = static_cast<int>(right ? span->end : span->start);
        return true;
    }
    if (event == Event::ArrowUp || event == Event::ArrowDown || event == Event::Special("\x1b[1;2A") || event == Event::Special("\x1b[1;2B")) {
        layout(width_); const bool up = event == Event::ArrowUp || event == Event::Special("\x1b[1;2A");
        buffer_.cursor = position(cursorX_, std::max(0, cursorY_ + (up ? -1 : 1)));
        if (auto span = buffer_.at(buffer_.cursor); span && span->start != static_cast<std::size_t>(buffer_.cursor)) buffer_.cursor = static_cast<int>(span->end);
        return true;
    }
    if (event == Event::Home || event == Event::End) { buffer_.cursor = event == Event::Home ? 0 : static_cast<int>(buffer_.visible().size()); return true; }
    return false;
}
}
