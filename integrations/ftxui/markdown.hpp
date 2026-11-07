#ifndef SOS_TUI_MARKDOWN_HPP
#define SOS_TUI_MARKDOWN_HPP
#include <ftxui/dom/elements.hpp>
#include <string>
namespace sos::tui {
ftxui::Element richText(const std::string& value, bool markdown = true);
}
#endif
