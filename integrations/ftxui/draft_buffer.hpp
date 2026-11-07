#ifndef SOS_DRAFT_BUFFER_HPP
#define SOS_DRAFT_BUFFER_HPP
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <agent-runtime/image.hpp>
namespace sos::tui {
struct PasteSpan {
    std::size_t start, end;
    std::shared_ptr<const std::string> content;
};
class DraftBuffer {
public:
    explicit DraftBuffer(std::string& text, std::vector<Image>* images = nullptr);
    const std::string& visible() const { return text_; }
    int cursor = 0;
    void sync();
    void insert(const std::string& text, bool pasted = false);
    void erase(std::size_t start, std::size_t end);
    bool move(bool right);
    bool remove(bool backwards);
    void undo();
    void redo();
    std::string expanded() const;
    std::string expandRange(std::size_t start, std::size_t end) const;
    std::optional<PasteSpan> at(std::size_t position) const;
    const std::vector<PasteSpan>& spans() const { return state_.spans; }
    void clearHistory();
    static bool compact(const std::string& value);
    static std::string label(const std::string& value);
private:
    struct State { std::string text; std::vector<PasteSpan> spans; int cursor = 0; std::vector<std::shared_ptr<const Image>> images; };
    std::string& text_;
    std::vector<Image>* images_;
    State state_;
    std::vector<State> undo_, redo_;
    void replace(std::size_t start, std::size_t end, const std::string& text, std::shared_ptr<const std::string> paste = {});
    void restore(const State& state);
    void checkpoint();
    std::vector<std::shared_ptr<const Image>> captureImages() const;
};
}
#endif
