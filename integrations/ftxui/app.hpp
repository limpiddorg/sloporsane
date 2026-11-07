#ifndef SOS_TUI_APP_HPP
#define SOS_TUI_APP_HPP
#include "model.hpp"
#include "draft_buffer.hpp"
#include "composer.hpp"
#include "history_viewport.hpp"
#include <deque>
#include <future>
#include <chrono>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
namespace sos::tui {
class App {
public:
    App(Client& client, Json hello, std::string workspace);
    ~App();
    void run();
private:
    enum class Dialog { None, Sessions, Providers, Methods, Key, OAuth, Models, Budget, Permissions, PermissionKey, Images, Paste };
    struct Choice { std::string label, id, provider, method; std::optional<long long> maximum; };
    using Clock = std::chrono::steady_clock;
    Client& client_;
    Model model_;
    DraftBuffer draft_{model_.draft, &model_.images};
    std::optional<PasteSpan> previewPaste_;
    ftxui::ScreenInteractive screen_ = ftxui::ScreenInteractive::Fullscreen();
    ftxui::Component input_, field_, menu_, modal_, root_;
    std::shared_ptr<Composer> composer_;
    Dialog dialog_ = Dialog::None;
    std::vector<Choice> choices_, catalog_;
    std::vector<std::string> labels_;
    std::vector<ftxui::Box> boxes_, approvalBoxes_, refusedBoxes_;
    std::map<std::string, bool> refusedOpen_;
    ftxui::Box permissionBoxes_[3], modeBox_, chatBox_;
    std::string fieldValue_, dialogTitle_, dialogNote_, provider_, authId_, preview_, selectedSession_;
    std::string connectedMethod_;
    HistoryViewport dialogViewport_;
    void backDialog();
    Choice selectedModel_;
    std::string dialogRequest_, authRequest_;
    std::map<std::string, std::string> pendingBtw_;
    std::string latestRequest_, latestDraft_;
    std::vector<::Image> latestImages_;
    Json permission_;
    std::string paste_, authUrl_;
    bool pasting_ = false, pasteRejected_ = false, pollInFlight_ = false;
    int selected_ = 0, permissionChoice_ = 0, activeTab_ = 0, suggestion_ = -1;
    bool permissionSent_ = false, password_ = false, recapAttempted_ = false;
    bool recapping_ = false, compacting_ = false, cancelling_ = false;
    HistoryViewport viewport_;
    struct RenderedBody { std::string text; bool loaded; ftxui::Element element; };
    std::map<std::string, RenderedBody> renderedBodies_;
    ftxui::Element body(const Entry& entry);
    unsigned long sequence_ = 0;
    Clock::time_point exitDeadline_{}, cancelDeadline_{}, idleSince_ = Clock::now(), oauthPoll_{}, copiedUntil_{};
    std::thread ticker_;
    std::atomic<bool> stopping_{false};
    std::mutex incomingMutex_;
    std::deque<Json> incoming_;
    bool editing_ = false;
    std::future<void> clipboardWorker_;
    std::string pendingCopy_;
    Clock::time_point runStarted_{};
    std::string runModel_, runReasoning_;
    void drainIncoming();
    std::string request(Json value, const std::string& prefix = "ui-");
    void receive(Json event);
    void submit();
    void sendMessage(std::string text, std::vector<Image> images = {});
    void acceptPaste(std::string text);
    void addImage(Image image);
    void command(const std::string& text);
    void open(Dialog dialog, std::string title);
    void closeDialog();
    void choose();
    void choices(std::vector<Choice> values);
    void filterModels();
    std::vector<std::string> suggestions() const;
    void selectionChanged();
    void page(const std::string& direction);
    void toggle(std::size_t index);
    void permissionReply();
    void tick();
    void externalEditor();
    bool event(ftxui::Event event);
    ftxui::Element render();
    ftxui::Element renderDialog();
};
}
#endif
