#include "draft_buffer.hpp"
#include <algorithm>
#include <sstream>
#include <ftxui/screen/string.hpp>
namespace sos::tui {
DraftBuffer::DraftBuffer(std::string& text, std::vector<Image>* images) : text_(text), images_(images) { state_.text = text; state_.images = captureImages(); }
std::vector<std::shared_ptr<const Image>> DraftBuffer::captureImages() const {
    std::vector<std::shared_ptr<const Image>> result;
    if (!images_) return result;
    for (const auto& image : *images_) {
        auto existing = std::find_if(state_.images.begin(), state_.images.end(), [&](const auto& saved) { return saved->id == image.id && saved->name == image.name && saved->origin == image.origin; });
        result.push_back(existing == state_.images.end() ? std::make_shared<const Image>(image) : *existing);
    }
    return result;
}
bool DraftBuffer::compact(const std::string& value) {
    const auto characters = std::count_if(value.begin(), value.end(), [](unsigned char c) { return (c & 0xc0) != 0x80; });
    auto lines = std::count(value.begin(), value.end(), '\n') + (value.ends_with('\n') ? 0 : 1);
    return characters >= 200 || lines >= 3;
}
std::string DraftBuffer::label(const std::string& value) {
    std::istringstream stream(value);
    std::string preview, word;
    for (int i = 0; i < 3 && stream >> word; ++i) { if (!preview.empty()) preview += ' '; preview += word; }
    if (preview.empty()) preview = "Pasted text";
    std::string shortened;
    int width = 0;
    for (const auto& glyph : ftxui::Utf8ToGlyphs(preview)) {
        const auto columns = ftxui::string_width(glyph);
        if (width + columns > 23) break;
        shortened += glyph; width += columns;
    }
    return "[Pasted text • " + shortened + "…]";
}
void DraftBuffer::checkpoint() {
    state_.cursor = cursor;
    undo_.push_back(state_); redo_.clear();
    // Bound undo snapshots; paste bodies are shared, not copied on every keystroke.
    std::size_t bytes = 0;
    for (const auto& saved : undo_) bytes += saved.text.size();
    while (undo_.size() > 1 && (undo_.size() > 128 || bytes > 16 * 1024 * 1024)) {
        bytes -= undo_.front().text.size(); undo_.erase(undo_.begin());
    }
}
void DraftBuffer::sync() {
    auto images = captureImages();
    if (text_ == state_.text && images == state_.images) return;
    // Reconcile rejection prefixes by changed interval, not potentially duplicate block labels.
    std::size_t start = 0;
    while (start < text_.size() && start < state_.text.size() && text_[start] == state_.text[start]) ++start;
    std::size_t suffix = 0;
    while (suffix < text_.size() - start && suffix < state_.text.size() - start &&
           text_[text_.size() - suffix - 1] == state_.text[state_.text.size() - suffix - 1]) ++suffix;
    const auto oldEnd = state_.text.size() - suffix;
    const auto newEnd = text_.size() - suffix;
    checkpoint();
    if (static_cast<std::size_t>(cursor) >= oldEnd) cursor = static_cast<int>(static_cast<std::size_t>(cursor) - oldEnd + newEnd);
    else if (static_cast<std::size_t>(cursor) > start) cursor = static_cast<int>(newEnd);
    std::vector<PasteSpan> spans;
    for (auto span : state_.spans) {
        if (span.end <= start) spans.push_back(span);
        else if (span.start >= oldEnd) {
            span.start = span.start - oldEnd + newEnd;
            span.end = span.end - oldEnd + newEnd;
            spans.push_back(span);
        }
    }
    state_.spans = std::move(spans); state_.text = text_;
    state_.images = std::move(images);
    cursor = std::clamp(cursor, 0, static_cast<int>(text_.size()));
    if (auto span = at(cursor); span && static_cast<std::size_t>(cursor) > span->start) cursor = static_cast<int>(span->end);
}
void DraftBuffer::replace(std::size_t start, std::size_t end, const std::string& text, std::shared_ptr<const std::string> paste) {
    sync(); start = std::min(start, text_.size()); end = std::clamp(end, start, text_.size());
    for (const auto& span : state_.spans) {
        if (span.start < start && start < span.end) start = span.start;
        if (span.start < end && end < span.end) end = span.end;
    }
    checkpoint();
    std::vector<PasteSpan> spans;
    for (auto span : state_.spans) {
        if (span.end <= start) spans.push_back(span);
        else if (span.start >= end) {
            span.start = span.start - end + start + text.size();
            span.end = span.end - end + start + text.size(); spans.push_back(span);
        }
    }
    if (paste) spans.push_back({start, start + text.size(), std::move(paste)});
    std::sort(spans.begin(), spans.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
    text_.replace(start, end - start, text);
    cursor = static_cast<int>(start + text.size());
    state_ = {text_, std::move(spans), cursor, captureImages()};
}
void DraftBuffer::insert(const std::string& text, bool pasted) {
    sync();
    if (pasted && compact(text)) replace(cursor, cursor, label(text), std::make_shared<const std::string>(text));
    else replace(cursor, cursor, text);
}
void DraftBuffer::erase(std::size_t start, std::size_t end) { replace(start, end, ""); }
std::optional<PasteSpan> DraftBuffer::at(std::size_t position) const {
    for (const auto& span : state_.spans) if (span.start <= position && position < span.end) return span;
    return {};
}
bool DraftBuffer::move(bool right) {
    sync();
    for (const auto& span : state_.spans) {
        if ((right && span.start <= static_cast<std::size_t>(cursor) && static_cast<std::size_t>(cursor) < span.end) ||
            (!right && span.start < static_cast<std::size_t>(cursor) && static_cast<std::size_t>(cursor) <= span.end)) {
            cursor = static_cast<int>(right ? span.end : span.start); return true;
        }
    }
    return false;
}
bool DraftBuffer::remove(bool backwards) {
    sync();
    auto span = at(backwards && cursor > 0 ? cursor - 1 : cursor);
    if (!span) return false;
    erase(span->start, span->end); return true;
}
std::string DraftBuffer::expandRange(std::size_t start, std::size_t end) const {
    start = std::min(start, text_.size()); end = std::clamp(end, start, text_.size());
    std::string result;
    for (const auto& span : state_.spans) {
        if (span.start >= start && span.end <= end) {
            result += text_.substr(start, span.start - start); result += *span.content; start = span.end;
        }
    }
    return result + text_.substr(start, end - start);
}
std::string DraftBuffer::expanded() const { return expandRange(0, text_.size()); }
void DraftBuffer::restore(const State& state) {
    state_ = state; text_ = state.text; cursor = state.cursor;
    if (images_) { images_->clear(); for (const auto& image : state.images) images_->push_back(*image); }
}
void DraftBuffer::undo() {
    sync(); if (undo_.empty()) return;
    state_.cursor = cursor; redo_.push_back(state_); restore(undo_.back()); undo_.pop_back();
}
void DraftBuffer::redo() {
    sync(); if (redo_.empty()) return;
    state_.cursor = cursor; undo_.push_back(state_); restore(redo_.back()); redo_.pop_back();
}
void DraftBuffer::clearHistory() { sync(); undo_.clear(); redo_.clear(); }
}
