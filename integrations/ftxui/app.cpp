#include "app.hpp"
#include "markdown.hpp"
#include "tool_view.hpp"
#include "status_bar.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <future>
#include <ctime>
#include <iostream>
#include <sstream>
#include <set>
#include <sys/wait.h>
#include <unistd.h>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
namespace sos::tui {
using namespace ftxui;
namespace {
const Color BACKGROUND = Color::RGB(24, 27, 27);
const Color FOREGROUND = Color::RGB(230, 237, 243);
const Color MUTED = Color::RGB(113, 128, 142);
const Color INPUT = Color::RGB(48, 58, 72);
const Color SANE = Color::RGB(5, 176, 125);
const Color SLOP = Color::RGB(245, 29, 83);
const Color GOLD = Color::RGB(233, 196, 106);
const Color CORAL = Color::RGB(244, 163, 138);
const Color MISC = Color::RGB(129, 216, 208);
std::string lower(std::string value) {
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}
std::string tokenCount(std::optional<long long> count) { return count ? std::to_string(*count) : "?"; }
std::string lastActivity(const Json& session) {
    if (!session.contains("updatedAt") || !session["updatedAt"].is_number_integer()) return "-";
    const auto timestamp = static_cast<std::time_t>(session["updatedAt"].get<long long>() / 1000);
    std::tm local{}; if (!::localtime_r(&timestamp, &local)) return "-";
    char value[32]; std::strftime(value, sizeof(value), "%H:%M  %d/%m/%Y", &local); return value;
}
}
App::App(Client& client, Json hello, std::string workspace) : client_(client) {
    model_.workspace = model_.launchWorkspace = std::move(workspace);
    model_.apply(hello);
    composer_ = std::make_shared<Composer>(draft_, [this] { suggestion_ = -1; }, [this](PasteSpan span) {
        previewPaste_ = span; open(Dialog::Paste, "Pasted text");
        dialogNote_ = *span.content; choices({{"Close", "close"}, {"Remove", "remove"}}); menu_->TakeFocus();
    }, [this](std::string text) {
        std::cout << "\033]52;c;" << base64(text) << '\a' << std::flush;
        pendingCopy_ = std::move(text); copiedUntil_ = Clock::now() + std::chrono::milliseconds(1400);
    });
    input_ = composer_;
    InputOption fieldOptions;
    fieldOptions.password = &password_;
    fieldOptions.multiline = false;
    fieldOptions.on_enter = [this] { choose(); };
    fieldOptions.on_change = [this] { if (dialog_ == Dialog::Models) filterModels(); };
    field_ = Input(&fieldValue_, "", fieldOptions);
    MenuOption menuOptions;
    menuOptions.on_enter = [this] { choose(); };
    menuOptions.on_change = [this] { selectionChanged(); };
    menu_ = Menu(&labels_, &selected_, menuOptions);
    modal_ = Container::Vertical({Maybe(field_, [this] {
        return dialog_ == Dialog::Models || dialog_ == Dialog::Key || dialog_ == Dialog::PermissionKey || dialog_ == Dialog::Budget;
    }), Maybe(menu_, [this] { return !labels_.empty(); })});
    auto children = Container::Tab({input_, modal_}, &activeTab_);
    root_ = Renderer(children, [this] { return render(); });
    root_ = CatchEvent(root_, [this, children](Event value) {
        children->SetActiveChild(dialog_ == Dialog::None ? input_ : modal_);
        return event(value);
    });
}
App::~App() {
    stopping_ = true;
    if (ticker_.joinable()) ticker_.join();
    client_.close();
}
std::string App::request(Json value, const std::string& prefix) {
    auto id = prefix + std::to_string(++sequence_);
    value["requestId"] = id;
    client_.send(value);
    return id;
}
void App::run() {
    request({{"type", "integration.configure"}, {"workspace", model_.launchWorkspace}, {"mode", "sane"}});
    client_.start([this](Json value) {
        { std::lock_guard lock(incomingMutex_); incoming_.push_back(std::move(value)); }
        screen_.Post([this] { drainIncoming(); });
    });
    screen_.Post([] { std::cout << "\033[?2004h" << std::flush; });
    ticker_ = std::thread([this] {
        while (!stopping_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!stopping_) screen_.Post([this] { tick(); });
        }
    });
    screen_.Loop(root_);
    std::cout << "\033[?2004l" << std::flush;
    stopping_ = true;
    if (ticker_.joinable()) ticker_.join();
    client_.close();
}
void App::drainIncoming() {
    std::deque<Json> events;
    { std::lock_guard lock(incomingMutex_); events.swap(incoming_); }
    for (auto& value : events) receive(std::move(value));
    draft_.sync();
}
void App::choices(std::vector<Choice> values) {
    choices_ = std::move(values); labels_.clear(); selected_ = 0;
    for (const auto& value : choices_) labels_.push_back(value.label);
}
void App::open(Dialog dialog, std::string title) {
    activeTab_ = 1;
    password_ = dialog == Dialog::Key || dialog == Dialog::PermissionKey;
    dialog_ = dialog; dialogTitle_ = std::move(title); fieldValue_.clear(); dialogNote_.clear(); preview_.clear();
    pollInFlight_ = false;
    choices({});
    dialogViewport_ = {}; dialogViewport_.snapBottom = false; dialogViewport_.following = false;
    field_->TakeFocus();
    if (dialog == Dialog::Sessions || dialog == Dialog::Providers || dialog == Dialog::Methods || dialog == Dialog::Permissions)
        menu_->TakeFocus();
}
void App::closeDialog() {
    try {
        if (dialog_ == Dialog::Models || dialog_ == Dialog::Budget) request({{"type", "model.cancel"}});
        if (dialog_ == Dialog::OAuth || dialog_ == Dialog::Key || dialog_ == Dialog::Methods) {
            Json cancel = {{"type", "auth.cancel"}};
            if (!authId_.empty()) cancel["authId"] = authId_;
            request(cancel);
        }
    } catch (const std::exception& error) { model_.status = error.what(); }
    dialog_ = Dialog::None; activeTab_ = 0; password_ = false; fieldValue_.clear(); authId_.clear(); dialogRequest_.clear(); authRequest_.clear();
    input_->TakeFocus(); idleSince_ = Clock::now();
}
void App::backDialog() {
    const auto previous = dialog_;
    if (previous == Dialog::Budget) {
        open(Dialog::Models, "Select model | Search"); filterModels(); return;
    }
    closeDialog();
    if (previous == Dialog::Key || previous == Dialog::OAuth || previous == Dialog::Methods) {
        open(Dialog::Providers, "Connect a provider"); dialogRequest_ = request({{"type", "provider.list"}});
    } else if (previous == Dialog::PermissionKey) {
        open(Dialog::Permissions, "Permissions"); choices({{"Ask before", "ask"}, {"AI Verified with Jev", "auto"}, {"Danger", "danger"}}); menu_->TakeFocus();
    }
}
void App::receive(Json value) {
    try {
        const auto type = stringField(value, "type");
        if (type == "recap.started") recapping_ = true;
        if (type == "compaction.started") compacting_ = true;
        if (type == "recap.completed" || type == "recap.cancelled") recapping_ = false;
        if (type == "compaction.completed" || type == "compaction.cancelled" || type == "compaction.failed") compacting_ = false;
        const auto requestId = stringField(value, "requestId");
        if (requestId == authRequest_ && (type == "auth.pending" || type == "request.failed")) pollInFlight_ = false;
        if (type == "btw.accepted") pendingBtw_.erase(requestId);
        if (type == "request.failed" && pendingBtw_.contains(requestId)) {
            model_.draft = pendingBtw_.at(requestId) + (model_.draft.empty() ? "" : "\n" + model_.draft);
            pendingBtw_.erase(requestId);
        }
        model_.apply(value);
        if (!model_.busy) recapping_ = compacting_ = cancelling_ = false;
        bool visibleReply = type == "recap.completed" || type == "compaction.completed";
        if (type == "provider.message_arrived") for (const auto& message : value.value("messages", Json::array()))
            for (const auto& part : message.value("content", Json::array()))
                if (stringField(part, "type") == "text" && !stringField(part, "text").empty()) visibleReply = true;
        if (viewport_.following && visibleReply) viewport_.bottom();
        if (type == "session.reset" || type == "provider.selected") viewport_ = {};
        if (type == "session.attached" && requestId.starts_with("attach-")) viewport_.bottom();
        if (!latestRequest_.empty() && requestId == latestRequest_) {
            if (type == "session.attached") {
                latestRequest_.clear();
                auto text = std::exchange(latestDraft_, {});
                auto images = std::exchange(latestImages_, {});
                try { sendMessage(text, images); }
                catch (...) {
                    model_.draft = text + (model_.draft.empty() ? "" : "\n" + model_.draft);
                    model_.images.insert(model_.images.begin(), images.begin(), images.end());
                    throw;
                }
            } else if (type == "request.failed") {
                model_.draft = latestDraft_ + (model_.draft.empty() ? "" : "\n" + model_.draft);
                model_.images.insert(model_.images.begin(), latestImages_.begin(), latestImages_.end());
                latestRequest_.clear(); latestDraft_.clear(); latestImages_.clear();
            }
        }
        if (type == "permission.requested") {
            permission_ = value; permissionChoice_ = 0; permissionSent_ = false; viewport_.bottom();
        } else if (type == "permission.resolved" || type == "permission.dismissed") {
            if (stringField(value, "permissionId") == stringField(permission_, "permissionId")) { permission_ = nullptr; permissionSent_ = false; }
        }
        if (type == "provider.listed" && dialog_ == Dialog::Providers && stringField(value, "requestId") == dialogRequest_) {
            std::vector<Choice> values;
            for (const auto& provider : value.at("providers")) {
                auto id = stringField(provider, "id");
                Choice choice{id + (provider.value("connected", false) ? "   Connected" : ""), id};
                choice.method = stringField(provider, "auth_method"); values.push_back(std::move(choice));
            }
            choices(std::move(values)); menu_->TakeFocus();
        } else if (type == "session.listed" && dialog_ == Dialog::Sessions && stringField(value, "requestId") == dialogRequest_) {
            std::vector<Choice> values;
            for (const auto& session : value.at("sessions")) values.push_back({stringField(session, "title", "Untitled") + "  |  " + lastActivity(session), stringField(session, "id")});
            choices(std::move(values)); menu_->TakeFocus(); selectionChanged();
        } else if (type == "session.previewed" && dialog_ == Dialog::Sessions && stringField(value, "sessionId") == selectedSession_) preview_ = stringField(value, "text");
        else if (type == "model.listed" && dialog_ == Dialog::Models && stringField(value, "requestId") == dialogRequest_) {
            catalog_.clear();
            for (const auto& model : value.at("models")) {
                Choice choice;
                choice.id = stringField(model, "model"); choice.provider = stringField(model, "provider");
                auto providerLabel = choice.provider; if (!providerLabel.empty()) providerLabel[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(providerLabel[0])));
                choice.label = choice.id + "   |   " + providerLabel + " (" + (stringField(model, "auth_method") == "api" ? "Api key" : "Codex") + ")";
                if (choice.id == model_.info.model && choice.provider == model_.info.provider) choice.label += "  Current";
                if (model.contains("max_context") && model["max_context"].is_number_integer()) choice.maximum = model["max_context"].get<long long>();
                catalog_.push_back(std::move(choice));
            }
            filterModels();
            dialogNote_.clear();
            for (const auto& error : value.value("errors", Json::array())) dialogNote_ += stringField(error, "provider") + ": " + stringField(error, "error") + "\n";
        } else if (type == "auth.started" && dialog_ == Dialog::OAuth && stringField(value, "requestId") == authRequest_) {
            authId_ = stringField(value, "authId");
            authUrl_ = stringField(value, "url");
            dialogNote_ = stringField(value, "url") + "\n" + stringField(value, "instructions");
            oauthPoll_ = Clock::now();
        } else if (type == "auth.saved" && stringField(value, "requestId") == authRequest_) {
            authId_.clear();
            fieldValue_.clear(); dialogNote_ = "Selecting the first compatible model...";
            authRequest_ = request({{"type", "provider.select"}, {"provider", provider_}});
        } else if (type == "provider.selected" || type == "model.selected" || type == "permissions.selected") {
            closeDialog();
        } else if (type == "permissions.current" && dialog_ == Dialog::Permissions) {
            model_.info.hasKey = value.value("has_key", false);
            model_.info.permissionMode = stringField(value, "mode", "ask");
        } else if (type == "request.failed" && dialog_ != Dialog::None) dialogNote_ = stringField(value, "error");
        else if (type == "btw.accepted" && model_.draft.empty() && !editing_) model_.draft = "/btw " + stringField(value, "branchId") + " ";
        if (type == "session.attached" && value.value("requestId", "").starts_with("attach-")) {
            request({{"type", "btw.list"}, {"sessionId", model_.sessionId}});
        }
        const bool success = type == "execution.completed" || type == "recap.completed" || (type == "compaction.completed" && !value.value("automatic", false));
        if (success && (!model_.queued.empty() || !model_.queuedImages.empty())) {
            auto queued = std::exchange(model_.queued, {});
            auto images = std::exchange(model_.queuedImages, {});
            try { sendMessage(queued, images); }
            catch (...) { model_.queued = queued; model_.queuedImages = std::move(images); model_.restoreQueue(); throw; }
        } else if ((success || type == "provider.message_arrived" || type == "harness.message_arrived") &&
                   !model_.page.newer && !model_.loading && model_.pendingRequest.empty()) {
            std::set<std::string> visibleMessages;
            for (const auto& entry : model_.entries) {
                if (entry.kind == Entry::Kind::User || entry.kind == Entry::Kind::Recap) visibleMessages.insert(entry.id);
                else if (entry.kind == Entry::Kind::Text && !entry.id.empty()) visibleMessages.insert(entry.id.substr(0, entry.id.find("/text/")));
            }
            if (visibleMessages.size() > 20) page("latest");
        }
        if (type == "client.disconnected") {
            for (const auto& [id, text] : pendingBtw_) model_.draft = text + (model_.draft.empty() ? "" : "\n" + model_.draft);
            pendingBtw_.clear();
            if (!latestRequest_.empty()) {
                model_.draft = latestDraft_ + (model_.draft.empty() ? "" : "\n" + model_.draft);
                model_.images.insert(model_.images.begin(), latestImages_.begin(), latestImages_.end());
                latestRequest_.clear(); latestDraft_.clear(); latestImages_.clear();
            }
            permission_ = nullptr; permissionSent_ = false; closeDialog();
        }
        screen_.PostEvent(Event::Custom);
    } catch (const std::exception& error) {
        // Invalid transfers must fail closed: the daemon may already have switched sessions.
        model_.apply({{"type", "client.disconnected"}, {"error", std::string("Protocol error: ") + error.what()}});
        client_.close();
    }
}
void App::selectionChanged() {
    if (dialog_ == Dialog::Sessions && selected_ >= 0 && selected_ < static_cast<int>(choices_.size())) {
        selectedSession_ = choices_[selected_].id;
        request({{"type", "session.preview"}, {"sessionId", selectedSession_}});
    }
}
void App::filterModels() {
    std::vector<Choice> filtered;
    const auto query = lower(fieldValue_);
    for (const auto& choice : catalog_) if (lower(choice.label).find(query) != std::string::npos) filtered.push_back(choice);
    choices(std::move(filtered));
    if (fieldValue_.empty()) for (std::size_t i = 0; i < choices_.size(); ++i)
        if (choices_[i].id == model_.info.model && choices_[i].provider == model_.info.provider) selected_ = static_cast<int>(i);
}
std::vector<std::string> App::suggestions() const {
    std::vector<std::string> values;
    if (!model_.draft.starts_with('/')) return values;
    for (const auto* command : {"/btw", "/compact", "/connect", "/model", "/new", "/permissions", "/recap", "/resume"})
        if (std::string(command).starts_with(model_.draft)) values.emplace_back(command);
    return values;
}
void App::choose() {
    try {
        if (dialog_ == Dialog::Paste) {
            if (selected_ == 1 && previewPaste_) draft_.erase(previewPaste_->start, previewPaste_->end);
            previewPaste_.reset(); closeDialog(); return;
        }
        if (dialog_ == Dialog::Images && selected_ >= 0 && selected_ < static_cast<int>(model_.images.size())) {
            model_.images.erase(model_.images.begin() + selected_);
            closeDialog(); return;
        }
        if (dialog_ == Dialog::Key || dialog_ == Dialog::PermissionKey) {
            if (fieldValue_.empty() && (dialog_ == Dialog::Key || !model_.info.hasKey)) { dialogNote_ = "Enter an API key"; return; }
            Json value = dialog_ == Dialog::Key ? Json{{"type", "auth.api"}, {"provider", provider_}} : Json{{"type", "permissions.select"}, {"mode", "auto"}};
            value["key"] = fieldValue_;
            authRequest_ = request(value); fieldValue_.clear(); dialogNote_ = "Checking API key...";
            return;
        }
        if (dialog_ == Dialog::Budget) {
            std::size_t end = 0;
            long long limit = 0;
            try { limit = std::stoll(fieldValue_, &end); } catch (...) {}
            if (end != fieldValue_.size() || limit < 32768 || (selectedModel_.maximum && limit > *selectedModel_.maximum)) {
                dialogNote_ = "Enter an integer >= 32768, not exceeding the advertised maximum."; return;
            }
            dialogRequest_ = request({{"type", "model.select"}, {"provider", selectedModel_.provider}, {"model", selectedModel_.id}, {"context_limit", limit}});
            dialogNote_ = "Saving model..."; return;
        }
        if (selected_ < 0 || selected_ >= static_cast<int>(choices_.size())) return;
        const auto choice = choices_[selected_];
        switch (dialog_) {
        case Dialog::Sessions:
            request({{"type", "session.attach"}, {"sessionId", choice.id}}, "attach-");
            model_.loading = true; closeDialog(); break;
        case Dialog::Providers:
            provider_ = choice.id;
            connectedMethod_ = choice.method;
            if (provider_ == "openrouter") open(Dialog::Key, "OpenRouter API key");
            else {
                open(Dialog::Methods, "OpenAI authentication");
                std::vector<Choice> methods{{"API key", "api"}, {"ChatGPT / Codex browser", "browser"}, {"ChatGPT / Codex device code", "device"}};
                int current = 0;
                for (std::size_t i = 0; i < methods.size(); ++i) if (methods[i].id == connectedMethod_) { methods[i].label += "   Connected"; current = i; }
                choices(std::move(methods)); selected_ = current; menu_->TakeFocus();
            }
            break;
        case Dialog::Methods:
            if (choice.id == "api") open(Dialog::Key, "OpenAI API key");
            else {
                open(Dialog::OAuth, "OpenAI authorization"); dialogNote_ = "Starting authorization...";
                authRequest_ = request({{"type", "auth.start"}, {"provider", "openai"}, {"method", choice.id}});
            }
            break;
        case Dialog::Models:
            selectedModel_ = choice; open(Dialog::Budget, "Maximum context tokens");
            fieldValue_ = tokenCount(choice.id == model_.info.model && choice.provider == model_.info.provider && model_.info.contextLimit ? model_.info.contextLimit : choice.maximum);
            if (fieldValue_ == "?") fieldValue_.clear();
            dialogNote_ = "Minimum: 32768. Provider maximum: " + tokenCount(choice.maximum); break;
        case Dialog::Permissions:
            if (choice.id == "auto") { open(Dialog::PermissionKey, "Jev OpenRouter key"); dialogNote_ = model_.info.hasKey ? "Enter a replacement key, or press Enter with an empty field to use the saved key." : "A dedicated OpenRouter key is required."; }
            else request({{"type", "permissions.select"}, {"mode", choice.id}});
            break;
        default: break;
        }
    } catch (const std::exception& error) { dialogNote_ = error.what(); }
}
void App::sendMessage(std::string value, std::vector<::Image> images) {
    if (!model_.connected) { model_.status = "Disconnected. Restart the integration to reconnect."; return; }
    Json message = {{"type", "message.send"}, {"text", value}, {"mode", model_.mode}};
    validateImages(images);
    if (!images.empty()) message["images"] = images;
    if (model_.sessionId.empty()) message["workspace"] = model_.launchWorkspace;
    auto id = request(message, "work-");
    model_.begin(value, id); viewport_.align("local-pending"); idleSince_ = Clock::now(); recapAttempted_ = false;
    runStarted_ = Clock::now(); runModel_ = model_.info.model; runReasoning_ = model_.info.reasoning;
    model_.provisionalImages = std::move(images);
    for (const auto& image : model_.provisionalImages) model_.entries.back().body += "\n[Image: " + image.name + "]";
}
void App::addImage(::Image image) {
    auto images = model_.images; images.push_back(std::move(image));
    validateImages(images); model_.images = std::move(images);
}
void App::acceptPaste(std::string value) {
    try {
        if (dialog_ == Dialog::None) {
            if (auto path = imagePath(value)) { addImage(loadImage(*path)); return; }
            composer_->paste(value);
        } else field_->OnEvent(Event::Character(value));
    } catch (const std::exception& error) { model_.status = error.what(); }
}
void App::submit() {
    try {
        draft_.sync();
        if ((model_.draft.empty() && model_.images.empty()) || permissionSent_ || model_.loading) return;
        if (!model_.draft.empty() && model_.draft.front() == '/') { command(draft_.expanded()); return; }
        if (auto path = imagePath(model_.draft)) { addImage(loadImage(*path)); model_.draft.clear(); return; }
        if (model_.busy) {
            auto images = model_.queuedImages; images.insert(images.end(), model_.images.begin(), model_.images.end());
            validateImages(images); model_.queuedImages = std::move(images); model_.images.clear();
            if (!model_.queued.empty()) model_.queued += '\n';
            model_.queued += draft_.expanded(); model_.draft.clear(); draft_.clearHistory(); return;
        }
        if (model_.page.newer) {
            latestRequest_ = request({{"type", "session.history"}, {"sessionId", model_.sessionId}, {"direction", "latest"}});
            latestDraft_ = draft_.expanded(); model_.draft.clear(); draft_.clearHistory(); latestImages_ = std::exchange(model_.images, {});
            model_.loading = true; model_.status = "Loading latest history before sending..."; return;
        }
        auto value = draft_.expanded();
        sendMessage(value, model_.images);
        if (model_.busy) { model_.draft.clear(); model_.images.clear(); draft_.clearHistory(); }
    } catch (const std::exception& error) { model_.status = error.what(); }
}
void App::command(const std::string& value) {
    const auto space = value.find_first_of(" \n\t");
    const auto name = value.substr(0, space);
    const auto args = space == std::string::npos ? "" : value.substr(space + 1);
    if (name == "/btw") {
        if (!model_.images.empty()) { model_.status = "BTW conversations accept text only"; return; }
        if (args.empty() || model_.sessionId.empty()) { model_.status = "Usage: /btw [btw-N] question, in a nonempty conversation"; return; }
        Json message = {{"type", "btw.send"}, {"sessionId", model_.sessionId}, {"text", args}};
        if (args.starts_with("btw-")) {
            auto end = args.find(' ');
            if (end == std::string::npos) { model_.status = "Enter a question after the branch ID"; return; }
            message["branchId"] = args.substr(0, end); message["text"] = args.substr(end + 1);
        }
        pendingBtw_[request(message)] = model_.draft; model_.draft.clear(); return;
    }
    if (!args.empty()) { model_.status = "This command takes no arguments"; return; }
    if (model_.busy) { model_.status = "This command is unavailable during execution"; return; }
    if (name == "/connect") { open(Dialog::Providers, "Connect a provider"); dialogRequest_ = request({{"type", "provider.list"}}); }
    else if (name == "/model") { open(Dialog::Models, "Select model | Search"); dialogRequest_ = request({{"type", "model.list"}}); }
    else if (name == "/resume") { open(Dialog::Sessions, "Resume session"); dialogRequest_ = request({{"type", "session.list"}}); }
    else if (name == "/permissions") {
        open(Dialog::Permissions, "Permissions"); choices({{"Ask before", "ask"}, {"AI Verified with Jev", "auto"}, {"Danger", "danger"}});
        request({{"type", "permissions.get"}}); menu_->TakeFocus();
    } else if (name == "/new") request({{"type", "session.new"}});
    else if (name == "/compact" || name == "/recap") {
        if (model_.sessionId.empty()) { model_.status = "Open a conversation first"; return; }
        model_.pendingRequest = request({{"type", name == "/compact" ? "session.compact" : "session.recap"}}, "work-");
        model_.busy = true;
        if (name == "/recap") { recapAttempted_ = true; recapping_ = true; }
        else compacting_ = true;
    } else { model_.status = "Unknown command: " + name; return; }
    model_.draft.clear();
}
void App::page(const std::string& direction) {
    if (model_.loading || model_.sessionId.empty()) return;
    Json value = {{"type", "session.history"}, {"sessionId", model_.sessionId}, {"direction", direction}};
    if (direction != "latest") value["anchor"] = direction == "before" ? model_.page.first : model_.page.last;
    request(value); model_.loading = true;
}
void App::toggle(std::size_t index) {
    if (index >= model_.entries.size()) return;
    auto& entry = model_.entries[index];
    if (!entry.expandable) return;
    entry.folded = !entry.folded;
    if (entry.kind == Entry::Kind::Btw && model_.branches.contains(entry.id)) model_.branches.at(entry.id).entry.folded = entry.folded;
    if (entry.kind == Entry::Kind::Activity && !entry.folded && !entry.loaded && !entry.group.empty()) {
        request({{"type", "session.activities"}, {"sessionId", model_.sessionId}, {"groupId", entry.group}});
    } else if (!entry.folded && !entry.loaded && entry.activityIndex >= 0) {
        request({{"type", "session.activity_detail"}, {"sessionId", model_.sessionId}, {"groupId", entry.group}, {"activityIndex", entry.activityIndex}});
    }
}
void App::permissionReply() {
    if (permission_.is_null() || permissionSent_) return;
    const auto& box = permissionBoxes_[permissionChoice_];
    if (box.y_min < chatBox_.y_min || box.y_max > chatBox_.y_max) { viewport_.bottom(); return; }
    const char* decisions[] = {"yes", "always", "no"};
    request({{"type", "permission.reply"}, {"permissionId", permission_.at("permissionId")}, {"decision", decisions[permissionChoice_]}});
    permissionSent_ = true;
}
void App::tick() {
    const auto now = Clock::now();
    if (!pendingCopy_.empty() && (!clipboardWorker_.valid() || clipboardWorker_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)) {
        auto text = std::exchange(pendingCopy_, {});
        clipboardWorker_ = std::async(std::launch::async, [text = std::move(text)] { try { copyNativeClipboard(text); } catch (...) {} });
    }
    if (model_.busy || model_.loading || dialog_ != Dialog::None) idleSince_ = now;
    if (model_.connected && !model_.busy && !model_.loading && dialog_ == Dialog::None && !model_.sessionId.empty() && !recapAttempted_ && !model_.info.lastRecap && now - idleSince_ >= std::chrono::minutes(5)) {
        recapAttempted_ = true;
        try { model_.pendingRequest = request({{"type", "session.recap"}}, "work-"); model_.busy = true; recapping_ = true; }
        catch (const std::exception& error) { model_.status = error.what(); }
    }
    if (dialog_ == Dialog::OAuth && !authId_.empty() && !pollInFlight_ && now >= oauthPoll_) {
        try { authRequest_ = request({{"type", "auth.poll"}, {"authId", authId_}}); pollInFlight_ = true; }
        catch (const std::exception& error) { dialogNote_ = error.what(); }
        oauthPoll_ = now + std::chrono::seconds(2);
    }
    screen_.PostEvent(Event::Custom);
}
void App::externalEditor() {
    const char* editor = std::getenv("EDITOR");
    if (!editor || !*editor) { model_.status = "Set EDITOR before using Ctrl+G"; return; }
    char path[] = "/tmp/sos-ftxui-editor-XXXXXX";
    int fd = ::mkstemp(path);
    if (fd < 0) { model_.status = "Cannot create editor file"; return; }
    ::close(fd);
    draft_.sync();
    const auto visible = model_.draft;
    const auto original = draft_.expanded();
    { std::ofstream file(path); file << original; if (!file) { ::unlink(path); model_.status = "Cannot write editor file"; return; } }
    std::string command = std::string(editor) + " '" + path + "'";
    int result = -1;
    editing_ = true;
    screen_.WithRestoredIO([&] {
        std::cout << "\033[?2004l" << std::flush;
        auto editor = std::async(std::launch::async, [&] { return std::system(command.c_str()); });
        while (editor.wait_for(std::chrono::milliseconds(20)) != std::future_status::ready) drainIncoming();
        result = editor.get();
        drainIncoming();
        std::cout << "\033[?2004h" << std::flush;
    })();
    editing_ = false;
    if (result == 0) {
        std::ifstream file(path);
        const std::string edited{std::istreambuf_iterator<char>(file), {}};
        if (file.bad()) model_.status = "Cannot read editor file; draft preserved";
        else if (edited != original) {
            // Rejections received during editing prepend restored text. Keep that prefix.
            if (model_.draft.ends_with(visible)) model_.draft.resize(model_.draft.size() - visible.size());
            model_.draft += edited;
            draft_.sync(); draft_.cursor = static_cast<int>(model_.draft.size());
        }
    }
    else model_.status = "Editor failed; draft preserved";
    ::unlink(path); idleSince_ = Clock::now();
}
bool App::event(Event value) {
    try {
        const auto now = Clock::now();
        if (value == Event::Special("\x1b[200~")) { pasting_ = true; pasteRejected_ = false; paste_.clear(); return true; }
        if (value == Event::Special("\x1b[201~")) {
            pasting_ = false;
            if (!pasteRejected_) {
                std::string normalized;
                for (std::size_t i = 0; i < paste_.size(); ++i) {
                    if (paste_[i] == '\r') { normalized += '\n'; if (i + 1 < paste_.size() && paste_[i + 1] == '\n') ++i; }
                    else normalized += paste_[i];
                }
                acceptPaste(normalized);
            }
            paste_.clear(); return true;
        }
        if (pasting_) {
            if (pasteRejected_) return true;
            if (paste_.size() + value.input().size() > MAX_FRAME_BYTES) { pasteRejected_ = true; paste_.clear(); model_.status = "Paste exceeds 16 MiB"; return true; }
            paste_ += value.input(); return true;
        }
        if (value == Event::CtrlQ) { screen_.ExitLoopClosure()(); return true; }
        if (value == Event::CtrlC) {
            if (dialog_ != Dialog::None) { closeDialog(); return true; }
            if (!model_.draft.empty() || !model_.images.empty()) { model_.draft.clear(); model_.images.clear(); exitDeadline_ = {}; }
            else if (now < exitDeadline_) screen_.ExitLoopClosure()();
            else exitDeadline_ = now + std::chrono::seconds(3);
            return true;
        }
        if (value == Event::Escape) {
            if (now < exitDeadline_) return true;
            if (dialog_ != Dialog::None) { backDialog(); return true; }
            if (!model_.queued.empty() || !model_.queuedImages.empty()) { model_.restoreQueue(); cancelDeadline_ = {}; return true; }
            if (model_.busy) {
                if (cancelling_) return true;
                if (now < cancelDeadline_) { request({{"type", "execution.cancel"}}); cancelling_ = true; model_.status = "Cancelling..."; cancelDeadline_ = {}; }
                else cancelDeadline_ = now + std::chrono::seconds(3);
            }
            return true;
        }
        if (value.is_mouse() && value.mouse().button == Mouse::Left && value.mouse().motion == Mouse::Released && !password_ && !composer_->mouseSelection()) {
            screen_.Post([this] {
                auto selection = screen_.GetSelection();
                if (selection == model_.draft) selection = draft_.expanded();
                if (!selection.empty()) {
                    std::cout << "\033]52;c;" << base64(selection) << '\a' << std::flush;
                    pendingCopy_ = selection; copiedUntil_ = Clock::now() + std::chrono::milliseconds(1400);
                }
            });
        }
        if (dialog_ != Dialog::None) {
            if (dialog_ == Dialog::Methods && (value == Event::ArrowLeft || value == Event::TabReverse)) { backDialog(); return true; }
            if (dialog_ == Dialog::Paste && (value == Event::PageUp || value == Event::PageDown)) {
                dialogViewport_.scroll(value == Event::PageUp ? -10 : 10); return true;
            }
            if (dialog_ == Dialog::Paste && value.is_mouse() && (value.mouse().button == Mouse::WheelUp || value.mouse().button == Mouse::WheelDown)) {
                dialogViewport_.scroll(value.mouse().button == Mouse::WheelUp ? -3 : 3); return true;
            }
            if (dialog_ == Dialog::OAuth && value == Event::Return && !authUrl_.empty()) { openBrowser(authUrl_); return true; }
            if ((dialog_ == Dialog::Models) && (value == Event::ArrowDown || value == Event::ArrowUp)) return menu_->OnEvent(value);
            if (value == Event::Return) { choose(); return true; }
            return false;
        }
        if (value == Event::CtrlT) { request({{"type", "reasoning.next"}}); return true; }
        if (value == Event::CtrlV) { addImage(clipboardImage()); return true; }
        if (value == Event::CtrlO) {
            open(Dialog::Images, "Attachments | Enter removes the selected image");
            std::vector<Choice> images;
            for (const auto& image : model_.images) images.push_back({image.name + " | " + image.mime + " | " + std::to_string(image.width) + "x" + std::to_string(image.height) + " | " + image.origin, image.id});
            choices(std::move(images)); menu_->TakeFocus(); return true;
        }
        if (value == Event::CtrlG) { externalEditor(); return true; }
        if (value == Event::Tab) {
            const auto matches = suggestions();
            if (!matches.empty()) {
                suggestion_ = (suggestion_ + 1) % static_cast<int>(matches.size()); return true;
            }
            const auto mode = model_.mode == "sane" ? "slop" : "sane";
            request({{"type", "mode.set"}, {"mode", mode}});
            model_.mode = mode;
            return true;
        }
        if (!permission_.is_null()) {
            if (value == Event::ArrowLeft || value == Event::ArrowRight) {
                permissionChoice_ = (permissionChoice_ + (value == Event::ArrowRight ? 1 : 2)) % 3; return true;
            }
            if (value == Event::Return) { permissionReply(); return true; }
        }
        if (value == Event::PageUp) {
            if (model_.loading) return true;
            if (viewport_.atTop() && model_.page.older) page("before");
            else viewport_.scroll(-std::max(1, viewport_.height - 2));
            return true;
        }
        if (value == Event::PageDown) {
            if (model_.loading) return true;
            if (viewport_.atBottom() && model_.page.newer) page("after");
            else viewport_.scroll(std::max(1, viewport_.height - 2));
            return true;
        }
        if (value.is_mouse()) {
            auto mouse = value.mouse();
            if (mouse.button == Mouse::WheelUp || mouse.button == Mouse::WheelDown) {
                if (composer_->contains(mouse.x, mouse.y)) return composer_->OnEvent(value);
                if (model_.loading) return true;
                if (mouse.button == Mouse::WheelUp && viewport_.atTop() && model_.page.older) page("before");
                else if (mouse.button == Mouse::WheelDown && viewport_.atBottom() && model_.page.newer) page("after");
                else viewport_.scroll(mouse.button == Mouse::WheelUp ? -3 : 3);
                return true;
            }
            if (mouse.button == Mouse::Left && mouse.motion == Mouse::Pressed) {
                if (modeBox_.Contain(mouse.x, mouse.y)) {
                    if (model_.busy && !recapping_) return event(Event::Escape);
                    return true;
                }
                if (!permission_.is_null()) for (int i = 0; i < 3; ++i) if (permissionBoxes_[i].Contain(mouse.x, mouse.y)) {
                    permissionChoice_ = i; permissionReply(); return true;
                }
                for (std::size_t i = 0; i < approvalBoxes_.size(); ++i) if (chatBox_.Contain(mouse.x, mouse.y) && approvalBoxes_[i].Contain(mouse.x, mouse.y)) {
                    const auto& entry = model_.entries[i];
                    request({{"type", "permission.manual"}, {"sessionId", model_.sessionId}, {"refusedMessageId", entry.refusedId}, {"approved", !entry.approved}});
                    return true;
                }
                for (std::size_t i = 0; i < refusedBoxes_.size(); ++i) if (chatBox_.Contain(mouse.x, mouse.y) && refusedBoxes_[i].Contain(mouse.x, mouse.y)) {
                    const auto& entry = model_.entries[i];
                    auto& open = refusedOpen_[entry.id]; open = !open;
                    if (open && !entry.loaded) request({{"type", "session.activity_detail"}, {"sessionId", model_.sessionId}, {"groupId", entry.group}, {"activityIndex", entry.activityIndex}});
                    return true;
                }
                for (std::size_t i = 0; i < boxes_.size(); ++i) if (chatBox_.Contain(mouse.x, mouse.y) && boxes_[i].Contain(mouse.x, mouse.y)) { toggle(i); return true; }
            }
        }
        if (value == Event::ArrowUp || value == Event::ArrowDown) {
            const auto matches = suggestions();
            if (!matches.empty()) {
                const auto count = static_cast<int>(matches.size());
                suggestion_ = suggestion_ < 0 ? (value == Event::ArrowDown ? 0 : count - 1) : (suggestion_ + (value == Event::ArrowDown ? 1 : count - 1)) % count;
                return true;
            }
        }
        if (value == Event::Return) {
            exitDeadline_ = {};
            const auto matches = suggestions();
            if (suggestion_ >= 0 && suggestion_ < static_cast<int>(matches.size())) model_.draft = matches[suggestion_];
            suggestion_ = -1; submit(); return true;
        }
        if (value == Event::CtrlJ || value == Event::Special("\x1b[13;2u") || value == Event::Special("\x1b[27;2;13~")) {
            return input_->OnEvent(Event::Character('\n'));
        }
        if (value.is_character()) { exitDeadline_ = {}; model_.status.clear(); }
    } catch (const std::exception& error) { model_.status = error.what(); }
    return false;
}
Element App::renderDialog() {
    Elements content{text(dialogTitle_) | bold | color(CORAL), separator()};
    const bool hasField = dialog_ == Dialog::Models || dialog_ == Dialog::Key || dialog_ == Dialog::PermissionKey || dialog_ == Dialog::Budget;
    if (hasField) {
        // Password rendering is delegated to the FTXUI input component, never to chat.
        content.push_back(field_->Render() | bgcolor(INPUT));
    }
    if (!labels_.empty()) content.push_back(menu_->Render() | vscroll_indicator | frame | size(HEIGHT, LESS_THAN, 14));
    if (dialog_ == Dialog::Permissions && selected_ < static_cast<int>(choices_.size())) {
        const auto id = choices_[selected_].id;
        const std::string description = id == "danger" ? "WARNING: tools may delete accessible files, destroy databases or expose private data. Choose AI Verified if unsure. Sane restrictions still apply." : id == "auto" ? "Jev checks commands and target paths using OpenRouter. This shares command context and adds API usage. It is not a sandbox." : "Ask for approval before mutations and external access. Sane restrictions always apply.";
        content.push_back(richText(description, false) | color(id == "danger" ? SLOP : GOLD));
        content.push_back(text("Current: " + model_.info.permissionMode) | color(CORAL));
    }
    if (!preview_.empty()) content.push_back(vbox({text("Last user request") | color(GOLD), richText(preview_, false) | size(HEIGHT, LESS_THAN, 4)}));
    if (!dialogNote_.empty()) {
        auto note = richText(dialogNote_, false) | color(GOLD);
        if (dialog_ == Dialog::Paste) note = historyViewport({{"preview", note}}, dialogViewport_) | size(HEIGHT, EQUAL, 12);
        content.push_back(note);
    }
    content.push_back(separator());
    content.push_back(text("Enter: select/save   Tab: focus   Esc: cancel") | color(MUTED));
    return vbox(std::move(content)) | border | color(FOREGROUND) | bgcolor(BACKGROUND) | size(WIDTH, LESS_THAN, 90) | size(HEIGHT, LESS_THAN, 30) | clear_under | center;
}
Element App::body(const Entry& entry) {
    const auto cached = renderedBodies_.find(entry.id);
    if (cached != renderedBodies_.end() && cached->second.text == entry.body && cached->second.loaded == entry.loaded) return cached->second.element;
    auto rendered = entry.kind == Entry::Kind::Tool ? toolView(entry) : richText(entry.body, entry.kind != Entry::Kind::User && entry.kind != Entry::Kind::Footer);
    if (entry.kind == Entry::Kind::Footer) rendered = hbox({filler(), rendered}) | color(MUTED);
    renderedBodies_[entry.id] = {entry.body, entry.loaded, rendered};
    return rendered;
}
Element App::render() {
    draft_.sync();
    std::vector<ViewportRow> history;
    std::erase_if(renderedBodies_, [&](const auto& cached) { return std::none_of(model_.entries.begin(), model_.entries.end(), [&](const Entry& entry) { return entry.id == cached.first; }); });
    boxes_.resize(model_.entries.size());
    approvalBoxes_.resize(model_.entries.size());
    refusedBoxes_.resize(model_.entries.size());
    for (auto& box : boxes_) box = {-1, -2, -1, -2};
    for (auto& box : approvalBoxes_) box = {-1, -2, -1, -2};
    for (auto& box : refusedBoxes_) box = {-1, -2, -1, -2};
    if (model_.entries.empty()) history.push_back({"welcome", vbox({text("sloporsane") | bold | color(SANE), text("Use /connect to connect a provider. Tab switches Sane / Slop.") | color(MUTED)})});
    for (std::size_t i = 0; i < model_.entries.size(); ++i) {
        const auto& entry = model_.entries[i];
        if (!entry.group.empty() && entry.kind != Entry::Kind::Activity) {
            auto parent = std::find_if(model_.entries.begin(), model_.entries.end(), [&](const Entry& e) { return e.id == entry.group; });
            if (parent != model_.entries.end() && parent->folded) continue;
        }
        Elements block;
        if (entry.expandable) {
            Elements title{text(std::string(entry.folded ? "▸ " : "▾ ") + entry.title) | color(entry.kind == Entry::Kind::Compaction ? SLOP : entry.kind == Entry::Kind::Tool ? CORAL : entry.kind == Entry::Kind::Activity ? MUTED : MISC)};
            if (entry.kind == Entry::Kind::Activity) {
                for (auto [bit, tint] : {std::pair{1u, CORAL}, {2u, GOLD}, {4u, MISC}, {8u, SLOP}})
                    if (entry.activityKinds & bit) title.push_back(text(" ▪") | color(tint));
                if (entry.durationMs) title.push_back(text("  " + std::to_string(*entry.durationMs / 1000) + "s") | color(MUTED));
                if (entry.modelCallMs && *entry.modelCallMs > 0 && entry.outputTokens) {
                    const auto speed = std::llround(1000.0 * *entry.outputTokens / *entry.modelCallMs);
                    title.push_back(text(" - " + std::to_string(speed) + " tok/s") | color(MUTED));
                }
            }
            block.push_back(hbox(std::move(title)) | reflect(boxes_[i]));
        } else if (!entry.title.empty()) block.push_back(text(entry.title) | bold | color(entry.toolName == "read" ? GOLD : MISC));
        if (entry.kind == Entry::Kind::Activity && !entry.folded && (entry.activityKinds & 8)) {
            block.push_back(text("  Refused tools") | bold | color(CORAL));
            for (std::size_t j = 0; j < model_.entries.size(); ++j) {
                const auto& refused = model_.entries[j];
                if (refused.group != entry.id || refused.refusedId.empty()) continue;
                const bool open = refusedOpen_[refused.id];
                block.push_back(text(std::string(open ? "  ▾ " : "  ▸ ") + refused.title + (refused.approved ? "  Manually approved" : "")) | color(refused.approved ? SANE : SLOP) | reflect(refusedBoxes_[j]));
                if (open) {
                    block.push_back(hbox({text("    "), toolView(refused) | flex}));
                    block.push_back(text(refused.approved ? "    [Revoke approval]" : "    [Approve]") | color(refused.approved ? SANE : SLOP) | reflect(approvalBoxes_[j]));
                }
            }
            block.push_back(separator() | color(MUTED));
            block.push_back(text("  History") | bold | color(CORAL));
        }
        if (!entry.folded) {
            auto content = body(entry);
            if (entry.kind == Entry::Kind::User) content = hbox({text("▌ ") | color(entry.mode == "slop" ? SLOP : SANE), content | flex}) | bgcolor(Color::RGB(36, 42, 49));
            if (entry.kind == Entry::Kind::Recap) content = hbox({text("▌ ") | color(MISC), content | flex}) | bgcolor(Color::RGB(36, 42, 49));
            if (entry.kind == Entry::Kind::Compaction) content = vbox({separator() | color(SLOP), content});
            if (!(entry.kind == Entry::Kind::Activity && entry.body.empty()) && !(entry.toolName == "read" && !entry.error)) block.push_back(content);
        }
        if (entry.kind == Entry::Kind::User || entry.kind == Entry::Kind::Text) block.push_back(text(" "));
        history.push_back({entry.id.empty() ? "footer-" + std::to_string(i) : entry.id, vbox(std::move(block)), entry.kind == Entry::Kind::User});
    }
    if (model_.busy) {
        const char* spinner[] = {"-", "\\", "|", "/"};
        auto frame = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count() / 100 % 4;
        if (recapping_) history.push_back({"recap-pending", hbox({text("▌ ") | color(MISC), vbox({text("Conversation Recap") | bold | color(MISC), text(spinner[frame])}) | flex}) | bgcolor(Color::RGB(36, 42, 49))});
        else if (compacting_) history.push_back({"compaction-pending", vbox({separator(), text(std::string("Compaction ") + spinner[frame])}) | color(SLOP)});
        else history.push_back({"working", text(std::string("Activity ") + spinner[frame] + "  " + model_.status) | color(MUTED)});
        const auto lastMarker = std::find_if(model_.entries.rbegin(), model_.entries.rend(), [](const Entry& e) { return e.kind == Entry::Kind::Footer || e.kind == Entry::Kind::User; });
        if (!recapping_ && !runModel_.empty() && (lastMarker == model_.entries.rend() || lastMarker->kind != Entry::Kind::Footer)) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - runStarted_).count();
            history.push_back({"live-footer", hbox({filler(), text(runModel_ + " | " + runReasoning_ + " | " + std::to_string(elapsed) + "s") | color(MUTED)})});
        }
    }
    if (!permission_.is_null()) {
        Elements buttons;
        const char* names[] = {" Yes ", " Always yes ", " No "};
        for (int i = 0; i < 3; ++i) buttons.push_back(text(names[i]) | (i == permissionChoice_ ? inverted : bold) | reflect(permissionBoxes_[i]));
        history.push_back({"permission", vbox({text("Allow " + stringField(permission_, "tool") + "?") | bold,
            richText(permission_.value("details", Json::object()).dump(2), false),
            permissionSent_ ? text("Waiting for permission acknowledgement...") : hbox(std::move(buttons))}) | color(GOLD)});
    }
    if (!model_.queued.empty() || !model_.queuedImages.empty()) history.push_back({"pending", vbox({text("[Pending]") | color(MISC), richText(model_.queued, false), text(std::to_string(model_.queuedImages.size()) + " images") | color(GOLD)})});
    auto chat = historyViewport(std::move(history), viewport_) | flex | reflect(chatBox_);
    Elements suggestionElements;
    for (const auto& image : model_.images) suggestionElements.push_back(text("[Image: " + image.name + "] ") | color(GOLD));
    const auto matches = suggestions();
    for (std::size_t i = 0; i < matches.size(); ++i) {
        auto item = text(matches[i] + "  ") | color(MUTED);
        if (static_cast<int>(i) == suggestion_) item = item | inverted;
        suggestionElements.push_back(item);
    }
    auto modeColor = model_.mode == "slop" ? SLOP : SANE;
    auto bar = statusBar(model_.info, model_.mode, model_.busy && !recapping_, model_.loading, screen_.dimx(), exitDeadline_, cancelDeadline_, modeBox_);
    if (cancelling_ && Clock::now() >= exitDeadline_) bar = text("Cancelling...") | center | bgcolor(SLOP) | reflect(modeBox_);
    auto layout = vbox({text(" Discussion ") | bold | color(CORAL), separator(), chat,
        text(model_.busy ? "" : model_.status) | color(SLOP), hbox(std::move(suggestionElements)), bar,
        hbox({text("▌ ") | color(modeColor), input_->Render() | flex}) | bgcolor(INPUT) | size(HEIGHT, LESS_THAN, 6),
        hbox({text(model_.workspace) | flex, text(model_.title)}) | color(MUTED)}) | color(FOREGROUND) | bgcolor(BACKGROUND);
    if (dialog_ != Dialog::None) layout = dbox({layout, renderDialog()});
    if (Clock::now() < copiedUntil_) {
        auto badge = hbox({vbox({text("▌"), text("▌"), text("▌")}) | color(GOLD), vbox({text("        "), text(" Copied "), text("        ")}) | color(GOLD)}) | bgcolor(INPUT) | clear_under;
        layout = dbox({layout, vbox({filler(), hbox({badge, filler()}), text("") | size(HEIGHT, EQUAL, dialog_ == Dialog::None ? composer_->rows() + 2 : 0)})});
    }
    return layout;
}
}
