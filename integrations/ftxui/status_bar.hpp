#ifndef SOS_STATUS_BAR_HPP
#define SOS_STATUS_BAR_HPP
#include "model.hpp"
#include <chrono>
#include <ftxui/dom/elements.hpp>
namespace sos::tui {
ftxui::Element statusBar(const Info& info, const std::string& mode, bool busy, bool loading, int width,
    std::chrono::steady_clock::time_point exit, std::chrono::steady_clock::time_point cancel, ftxui::Box& button);
}
#endif
