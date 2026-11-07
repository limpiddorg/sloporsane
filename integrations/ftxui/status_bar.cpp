#include "status_bar.hpp"
#include <algorithm>
#include <cmath>
#include <ftxui/screen/string.hpp>
namespace sos::tui {
ftxui::Element statusBar(const Info& info, const std::string& mode, bool busy, bool loading, int width,
    std::chrono::steady_clock::time_point exit, std::chrono::steady_clock::time_point cancel, ftxui::Box& button) {
    using namespace ftxui;
    const auto now = std::chrono::steady_clock::now();
    const auto background = Color::RGB(24, 27, 27), slop = Color::RGB(245, 29, 83), sane = Color::RGB(5, 176, 125);
    if (now < exit || now < cancel) {
        const auto deadline = now < exit ? exit : cancel;
        const auto remaining = std::chrono::duration<float>(deadline - now).count() / 3;
        return dbox({gauge(std::clamp(remaining, 0.f, 1.f)) | color(slop) | bgcolor(background),
            text(now < exit ? "Exit? Confirm with CTRL-C" : "Cancel Execution? Confirm with ESC") | center}) | reflect(button);
    }
    const auto tint = mode == "slop" ? slop : sane;
    const auto seconds = std::chrono::duration<double>(now.time_since_epoch()).count();
    auto action = text(busy ? "Cancel" : mode == "slop" ? "Slop" : "Sane implementation") | center;
    if (busy) action = dbox({gauge(static_cast<float>((std::sin(seconds * 2) + 1) / 2)) | color(Color::RGB(48, 58, 72)) | bgcolor(tint), action});
    else action = action | bgcolor(tint) | color(background);
    action = action | size(WIDTH, EQUAL, std::min(22, std::max(1, width))) | reflect(button);
    std::string model = info.model, reasoning = info.reasoning == "none" ? "No reasoning" : info.reasoning;
    const auto count = [](std::optional<long long> n) { return n ? std::to_string(*n) : "?"; };
    std::string ratio = count(info.used) + " / " + count(info.total), percent;
    if (info.used && info.total && *info.total > 0) percent = "[" + std::to_string(std::llround(100.0 * *info.used / *info.total)) + "%]";
    auto left = [&] { return model + (reasoning.empty() ? "" : " | " + reasoning); };
    auto right = [&] { return ratio + (percent.empty() ? "" : " " + percent); };
    for (auto* field : {&ratio, &reasoning, &percent, &model}) {
        if (string_width(left()) + string_width(right()) + 26 <= width) break;
        field->clear();
    }
    Element context = text(right());
    if (loading) context = gauge(static_cast<float>((std::sin(seconds * 3) + 1) / 2)) | color(Color::RGB(129, 216, 208)) | size(WIDTH, EQUAL, std::max(0, std::min(24, width - 26 - string_width(left()))));
    return hbox({text(left()), filler(), context, text("  "), action});
}
}
