#include "markdown.hpp"
#include <cmark-gfm.h>
#include <cmark-gfm-extension_api.h>
#include <cmark-gfm-core-extensions.h>
#include <ftxui/dom/table.hpp>
#include <algorithm>
#include <cctype>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_set>
namespace sos::tui {
namespace {
using namespace ftxui;
const Color GOLD = Color::RGB(233, 196, 106), CORAL = Color::RGB(244, 163, 138);
const Color MISC = Color::RGB(129, 216, 208), DEPTH = Color::RGB(39, 123, 227), ALGAE = Color::RGB(138, 212, 161);
std::string literal(cmark_node* node) { const auto* value = cmark_node_get_literal(node); return value ? value : ""; }
void words(Elements& out, const std::string& value, const Decorator& style) {
    std::size_t start = 0;
    while (start < value.size()) {
        auto end = value.find(' ', start);
        if (end == std::string::npos) end = value.size(); else ++end;
        out.push_back(text(value.substr(start, end - start)) | style); start = end;
    }
}
void inlineNodes(cmark_node* node, std::vector<Elements>& lines, Decorator style, unsigned depth = 0) {
    if (depth > 256) return;
    for (; node; node = cmark_node_next(node)) {
        const auto type = cmark_node_get_type(node);
        auto decorated = style;
        if (type == CMARK_NODE_TEXT || type == CMARK_NODE_HTML_INLINE) { words(lines.back(), literal(node), style); continue; }
        if (type == CMARK_NODE_CODE) { words(lines.back(), literal(node), style | color(MISC)); continue; }
        if (type == CMARK_NODE_SOFTBREAK) { words(lines.back(), " ", style); continue; }
        if (type == CMARK_NODE_LINEBREAK) { lines.emplace_back(); continue; }
        if (type == CMARK_NODE_STRONG) decorated = style | bold | color(CORAL);
        if (type == CMARK_NODE_EMPH) decorated = style | italic | color(MISC);
        if (std::string(cmark_node_get_type_string(node)) == "strikethrough") decorated = style | strikethrough;
        if (type == CMARK_NODE_LINK) {
            const auto* raw = cmark_node_get_url(node);
            const std::string url = raw ? raw : "";
            decorated = style | color(DEPTH) | underlined;
            if ((url.starts_with("http://") || url.starts_with("https://")) &&
                std::none_of(url.begin(), url.end(), [](unsigned char c) { return c < 32 || c == 127; })) decorated = decorated | hyperlink(url);
        }
        if (type == CMARK_NODE_IMAGE) words(lines.back(), "[Image: ", style | color(MISC));
        inlineNodes(cmark_node_first_child(node), lines, decorated, depth + 1);
        if (type == CMARK_NODE_IMAGE) words(lines.back(), "]", style | color(MISC));
    }
}
Element inlineText(cmark_node* parent) {
    std::vector<Elements> lines(1);
    inlineNodes(cmark_node_first_child(parent), lines, [](Element e) { return e; });
    Elements rendered;
    for (auto& line : lines) rendered.push_back(line.empty() ? text(" ") : flexbox(std::move(line)));
    return vbox(std::move(rendered));
}
Element codeLine(const std::string& line, const std::string& language) {
    static const std::unordered_set<std::string> LANGUAGES = {"cpp", "c++", "c", "h", "hpp", "python", "py", "javascript", "js", "typescript", "ts", "tsx", "jsx", "json", "yaml", "yml", "toml", "bash", "sh", "shell", "rust", "rs", "go", "java", "sql", "html", "xml", "css", "cmake", "diff"};
    if (!LANGUAGES.contains(language)) return text(line.empty() ? " " : line);
    if (language == "diff") return text(line.empty() ? " " : line) | color(line.starts_with('+') ? ALGAE : line.starts_with('-') ? CORAL : GOLD);
    static const std::unordered_set<std::string> KEYWORDS = {"if", "else", "for", "while", "return", "class", "struct", "namespace", "const", "static", "auto", "void", "int", "bool", "double", "float", "include", "def", "import", "from", "as", "try", "catch", "except", "throw", "public", "private", "using", "template", "typename", "switch", "case", "break", "continue", "true", "false", "nullptr", "None", "True", "False", "let", "var", "function", "async", "await", "fn", "pub", "use", "impl", "match", "select", "where", "insert", "update", "delete", "into", "values", "create", "table"};
    Elements spans;
    for (std::size_t i = 0; i < line.size();) {
        auto end = i + 1; Color tint = MISC; const char c = line[i];
        if (c == '"' || c == '\'') {
            while (end < line.size()) { if (line[end] == '\\') { end += 2; continue; } if (line[end++] == c) break; }
            end = std::min(end, line.size()); tint = ALGAE;
        } else if ((c == '/' && end < line.size() && line[end] == '/') || (c == '#' && language != "cpp" && language != "c" && language != "c++")) {
            end = line.size(); tint = DEPTH;
        } else if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            while (end < line.size() && (std::isalnum(static_cast<unsigned char>(line[end])) || line[end] == '_')) ++end;
            auto token = line.substr(i, end - i);
            tint = KEYWORDS.contains(token) ? CORAL : std::isdigit(static_cast<unsigned char>(c)) || (end < line.size() && line[end] == '(') || std::isupper(static_cast<unsigned char>(c)) ? GOLD : DEPTH;
        }
        spans.push_back(text(line.substr(i, end - i)) | color(tint)); i = end;
    }
    return spans.empty() ? text(" ") : hbox(std::move(spans));
}
Element block(cmark_node* node, unsigned depth = 0) {
    if (depth > 256) return text("[Markdown nesting limit]");
    const auto type = cmark_node_get_type(node);
    const std::string name = cmark_node_get_type_string(node);
    if (type == CMARK_NODE_PARAGRAPH || type == CMARK_NODE_HEADING) {
        auto value = inlineText(node);
        return type == CMARK_NODE_HEADING ? value | bold | color(GOLD) : value;
    }
    if (type == CMARK_NODE_THEMATIC_BREAK) return separator();
    if (type == CMARK_NODE_HTML_BLOCK) return paragraph(literal(node));
    if (type == CMARK_NODE_CODE_BLOCK) {
        const auto* info = cmark_node_get_fence_info(node);
        std::istringstream fields(info ? info : ""); std::string language; fields >> language;
        std::istringstream input(literal(node)); std::string line; Elements code;
        while (std::getline(input, line)) code.push_back(codeLine(line, language));
        if (code.empty()) code.push_back(text(" "));
        return vbox(std::move(code)) | bgcolor(Color::RGB(16, 18, 21));
    }
    if (name == "table") {
        std::vector<std::vector<Element>> cells;
        for (auto* row = cmark_node_first_child(node); row; row = cmark_node_next(row)) {
            cells.emplace_back();
            for (auto* cell = cmark_node_first_child(row); cell; cell = cmark_node_next(cell)) cells.back().push_back(inlineText(cell));
        }
        Table table(std::move(cells)); table.SelectAll().Border(); table.SelectAll().SeparatorVertical();
        table.SelectRow(0).Decorate(bold | color(GOLD)); table.SelectRow(0).BorderBottom(); return table.Render();
    }
    Elements children;
    int index = cmark_node_get_list_start(node);
    for (auto* child = cmark_node_first_child(node); child; child = cmark_node_next(child)) {
        auto element = block(child, depth + 1);
        if (type == CMARK_NODE_LIST) {
            auto marker = cmark_node_get_list_type(node) == CMARK_ORDERED_LIST ? std::to_string(index++) + ". " : "• ";
            if (std::string(cmark_node_get_type_string(child)) == "tasklist") marker = cmark_gfm_extensions_get_tasklist_item_checked(child) ? "[x] " : "[ ] ";
            element = hbox({text(marker) | color(MISC), element | flex});
        }
        children.push_back(element);
        if (type == CMARK_NODE_DOCUMENT && cmark_node_next(child)) children.push_back(text(" "));
    }
    if (children.empty()) children.push_back(text(" "));
    auto rendered = vbox(std::move(children));
    return type == CMARK_NODE_BLOCK_QUOTE ? hbox({text("│ ") | color(MISC), rendered | flex}) : rendered;
}
}
ftxui::Element richText(const std::string& value, bool markdown) {
    if (!markdown) {
        ftxui::Elements lines; std::istringstream stream(value); std::string line;
        while (std::getline(stream, line)) lines.push_back(ftxui::paragraph(line.empty() ? " " : line));
        if (lines.empty()) lines.push_back(ftxui::text(" "));
        return ftxui::vbox(std::move(lines));
    }
    static std::once_flag extensions;
    std::call_once(extensions, cmark_gfm_core_extensions_ensure_registered);
    std::unique_ptr<cmark_parser, decltype(&cmark_parser_free)> parser(cmark_parser_new(CMARK_OPT_VALIDATE_UTF8), cmark_parser_free);
    if (!parser) throw std::bad_alloc();
    for (const auto* name : {"table", "strikethrough", "autolink", "tasklist"})
        cmark_parser_attach_syntax_extension(parser.get(), cmark_find_syntax_extension(name));
    cmark_parser_feed(parser.get(), value.data(), value.size());
    std::unique_ptr<cmark_node, decltype(&cmark_node_free)> document(cmark_parser_finish(parser.get()), cmark_node_free);
    if (!document) throw std::bad_alloc();
    return block(document.get());
}
}
