#include "draft_buffer.hpp"
#include "history_viewport.hpp"
#include "composer.hpp"
#include "status_bar.hpp"
#include <ftxui/component/event.hpp>
#include <gtest/gtest.h>
#include <ftxui/screen/screen.hpp>
using namespace sos::tui;
TEST(FtxuiDraft, IdenticalLabelsKeepDistinctPayloadsAndUndo) {
    std::string text;
    DraftBuffer draft(text);
    const std::string a = "same three words\nfirst\nlast", b = "same three words\nsecond\nlast";
    draft.insert(a, true); draft.insert(" "); draft.insert(b, true);
    ASSERT_EQ(draft.spans().size(), 2);
    EXPECT_EQ(DraftBuffer::label(a), DraftBuffer::label(b));
    EXPECT_EQ(draft.expanded(), a + " " + b);
    ASSERT_TRUE(draft.remove(true)); EXPECT_EQ(draft.expanded(), a + " ");
    draft.undo(); EXPECT_EQ(draft.expanded(), a + " " + b);
    draft.redo(); EXPECT_EQ(draft.expanded(), a + " ");
}
TEST(FtxuiDraft, MotionAndDeletionAreAtomic) {
    std::string text;
    DraftBuffer draft(text); draft.insert("one\ntwo\nthree", true);
    const auto end = draft.cursor;
    EXPECT_TRUE(draft.move(false)); EXPECT_EQ(draft.cursor, 0);
    EXPECT_TRUE(draft.move(true)); EXPECT_EQ(draft.cursor, end);
    draft.cursor = 0; EXPECT_TRUE(draft.remove(false)); EXPECT_TRUE(text.empty());
    draft.undo(); EXPECT_EQ(draft.expanded(), "one\ntwo\nthree");
}
TEST(FtxuiDraft, RestoredPrefixPreservesBlocksAndTypedLabelsStayLiteral) {
    std::string text;
    DraftBuffer draft(text); draft.insert("one\ntwo\nthree", true);
    text = "rejected\n" + text; draft.sync();
    EXPECT_EQ(draft.expanded(), "rejected\none\ntwo\nthree");
    draft.cursor = static_cast<int>(text.size());
    const auto literal = DraftBuffer::label("one\ntwo\nthree");
    draft.insert(literal); EXPECT_TRUE(draft.expanded().ends_with(literal));
}
TEST(FtxuiDraft, ThresholdsCountUnicodeCharactersAndLines) {
    EXPECT_FALSE(DraftBuffer::compact("a\nb\n")); EXPECT_TRUE(DraftBuffer::compact("a\nb\nc"));
    std::string utf8; for (int i = 0; i < 100; ++i) utf8 += "é";
    EXPECT_FALSE(DraftBuffer::compact(utf8)); EXPECT_TRUE(DraftBuffer::compact(utf8 + utf8));
}
TEST(FtxuiDraft, ImageSnapshotsShareBytesAndParticipateInUndoRedo) {
    std::string text;
    std::vector<Image> images;
    DraftBuffer draft(text, &images);
    images.push_back(Image{.id = "image", .data = "captured pixels"}); draft.sync();
    draft.insert("caption"); draft.undo(); EXPECT_TRUE(text.empty()); ASSERT_EQ(images.size(), 1);
    draft.undo(); EXPECT_TRUE(images.empty()); draft.redo(); ASSERT_EQ(images.size(), 1);
    EXPECT_EQ(images[0].data, "captured pixels");
}
TEST(FtxuiComposer, SelectionReplacementAndUnicodeBackspaceKeepWholeGraphemes) {
    using namespace ftxui;
    std::string text; DraftBuffer draft(text);
    auto input = std::make_shared<Composer>(draft, [] {}, [](PasteSpan) {}, [](std::string) {});
    input->OnEvent(Event::Character("été")); input->OnEvent(Event::Backspace); EXPECT_EQ(text, "ét");
    input->OnEvent(Event::CtrlA); input->OnEvent(Event::Character("replacement")); EXPECT_EQ(text, "replacement");
    input->paste("one\ntwo\nthree"); input->OnEvent(Event::Backspace); EXPECT_EQ(text, "replacement");
    input->OnEvent(Event::CtrlZ); EXPECT_EQ(draft.expanded(), "replacementone\ntwo\nthree");
}
TEST(FtxuiComposer, WrapsAtTerminalWidthAndRendersPasteInGold) {
    using namespace ftxui;
    std::string text; DraftBuffer draft(text);
    auto input = std::make_shared<Composer>(draft, [] {}, [](PasteSpan) {}, [](std::string) {});
    input->paste("one\ntwo\nthree");
    auto element = input->Render();
    auto screen = Screen::Create(Dimension::Fixed(12), Dimension::Fixed(6)); Render(screen, element);
    EXPECT_EQ(screen.PixelAt(0, 0).character, "[");
    EXPECT_EQ(screen.PixelAt(0, 0).foreground_color, Color::RGB(233, 196, 106));
    EXPECT_NE(screen.PixelAt(0, 1).character, "");
}
TEST(FtxuiStatusBar, NarrowTerminalKeepsModeButton) {
    using namespace ftxui;
    Info info; info.model = "very-long-provider-model-identifier"; info.reasoning = "high"; info.used = 125; info.total = 1000;
    Box button;
    auto element = statusBar(info, "sane", false, false, 24, {}, {}, button);
    auto screen = Screen::Create(Dimension::Fixed(24), Dimension::Fixed(1)); Render(screen, element);
    EXPECT_NE(screen.ToString().find("Sane implementation"), std::string::npos);
    EXPECT_EQ(screen.ToString().find("very-long"), std::string::npos);
}
namespace {
void paint(HistoryViewport& state, const std::vector<std::string>& ids, int height = 5, int extra = 0) {
    using namespace ftxui;
    std::vector<ViewportRow> rows;
    for (const auto& id : ids) rows.push_back({id, text(id) | size(HEIGHT, EQUAL, id == "a" ? 2 + extra : 2)});
    auto screen = Screen::Create(Dimension::Fixed(30), Dimension::Fixed(height));
    Render(screen, historyViewport(std::move(rows), state));
}
}
TEST(FtxuiViewport, KeepsSharedAnchorAcrossPageSwap) {
    HistoryViewport state;
    paint(state, {"c", "d", "e", "f"}, 4); state.scroll(-100); paint(state, {"c", "d", "e", "f"}, 4);
    EXPECT_EQ(state.positions.at("c"), 0);
    paint(state, {"a", "b", "c", "d"}, 4);
    EXPECT_EQ(state.positions.at("c"), 0);
}
TEST(FtxuiViewport, ReflowAndTechnicalGrowthDoNotForceFollowing) {
    HistoryViewport state;
    paint(state, {"a", "b", "c", "d", "e"}); state.scroll(-2); paint(state, {"a", "b", "c", "d", "e"});
    const auto position = state.positions.at("c");
    paint(state, {"a", "b", "c", "d", "e"}, 5, 4);
    EXPECT_EQ(state.positions.at("c"), position); EXPECT_FALSE(state.following);
    state.bottom(); paint(state, {"a", "b", "c", "d", "e"}, 5, 4);
    EXPECT_TRUE(state.atBottom()); EXPECT_TRUE(state.following);
}
