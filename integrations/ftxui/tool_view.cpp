#include "tool_view.hpp"
#include "markdown.hpp"
#include <sstream>
namespace sos::tui {
namespace {
using namespace ftxui;
const Color GOLD = Color::RGB(233, 196, 106), MISC = Color::RGB(129, 216, 208);
const Color SLOP = Color::RGB(245, 29, 83), ALGAE = Color::RGB(138, 212, 161), MUTED = Color::RGB(113, 128, 142);
Element diff(const std::string& value) {
    std::istringstream input(value); std::string line; Elements lines;
    while (std::getline(input, line)) {
        auto tint = line.starts_with('+') ? ALGAE : line.starts_with('-') ? SLOP : line.starts_with("@@") ? MISC : MUTED;
        lines.push_back(text(line) | color(tint));
    }
    return vbox(std::move(lines));
}
std::string decisionLabel(const std::string& value) {
    if (value == "ai_accepted") return "Accepted by AI (Jev)";
    if (value == "ai_refused") return "Refused by AI (Jev)";
    if (value == "manually_approved") return "Manually approved";
    if (value == "always") return "Always yes";
    if (value == "yes") return "Yes";
    if (value == "no") return "No";
    return value;
}
}
ftxui::Element toolView(const Entry& entry) {
    using namespace ftxui;
    if (entry.toolName == "read") return entry.error ? richText(entry.output, false) | color(SLOP) : text("");
    if (!entry.loaded) return text("Loading details...") | color(MUTED);
    Elements sections;
    sections.push_back(text("Arguments") | bold | color(GOLD));
    if (entry.toolName == "bash") {
        sections.push_back(richText(stringField(entry.arguments, "command"), false));
        const auto directory = stringField(entry.arguments, "workdir");
        if (!directory.empty()) sections.push_back(text("Working directory: " + directory) | color(MUTED));
    } else if (entry.toolName == "webfetch") sections.push_back(richText(stringField(entry.arguments, "url"), false) | color(MISC));
    else sections.push_back(richText(entry.arguments.dump(2), false));
    if (entry.resultReceived) {
        sections.push_back(text("Results") | bold | color(GOLD));
        const auto patch = stringField(entry.resultMetadata, "diff");
        if (!patch.empty()) sections.push_back(diff(patch));
        if (entry.resultMetadata.contains("files") && entry.resultMetadata["files"].is_array()) {
            for (const auto& file : entry.resultMetadata["files"]) {
                sections.push_back(text(stringField(file, "filepath", stringField(file, "path"))) | color(GOLD));
                const auto fileDiff = stringField(file, "diff");
                if (!fileDiff.empty()) sections.push_back(diff(fileDiff));
            }
        }
        if (!entry.output.empty()) {
            auto output = richText(entry.output, entry.toolName == "webfetch" && stringField(entry.arguments, "format", "markdown") == "markdown");
            sections.push_back(entry.error ? output | color(SLOP) : output);
        }
        if (entry.toolName == "bash") {
            const auto exit = entry.resultMetadata.find("exit");
            std::string status = exit != entry.resultMetadata.end() ? "Exit: " + exit->dump() : "";
            if (entry.resultMetadata.value("timedOut", false)) status += "  Timed out";
            if (entry.resultMetadata.value("cancelled", false)) status += "  Cancelled";
            if (!status.empty()) sections.push_back(text(status) | color(entry.error ? SLOP : MUTED));
        }
        for (const auto& permission : entry.resultMetadata.value("permissions", Json::array())) {
            const auto decision = stringField(permission, "decision");
            sections.push_back(text(" -> " + decisionLabel(decision)) | color(decision == "ai_refused" || decision == "no" ? SLOP : ALGAE));
        }
    }
    return vbox(std::move(sections));
}
}
