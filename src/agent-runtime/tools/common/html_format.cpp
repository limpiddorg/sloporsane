#include "html_format.hpp"
#include "../fs/common.hpp"

#include <libxml/HTMLparser.h>
#include <libxml/uri.h>
#include <algorithm>
#include <memory>
#include <stdexcept>

namespace {
std::string take(xmlChar* text) {
    if (!text) return {};
    std::string result(reinterpret_cast<const char*>(text));
    xmlFree(text);
    return result;
}

std::string property(xmlNode* node, const char* key) { return take(xmlGetProp(node, BAD_CAST key)); }

std::string whitespace(const std::string& text) {
    std::string result;
    bool space = false;
    for (const auto c : text) {
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            if (!space) result += ' ';
            space = true;
        } else { result += c; space = false; }
    }
    return result;
}

std::string render(xmlNode* nodes, const std::string& url, bool markdown, int depth = 0) {
    if (depth > 128) throw std::runtime_error("HTML nesting limit exceeded");
    std::string output;
    for (auto* node = nodes; node; node = node->next) {
        if (node->type == XML_TEXT_NODE || node->type == XML_CDATA_SECTION_NODE) {
            if (node->content) output += whitespace(reinterpret_cast<const char*>(node->content));
            continue;
        }
        if (node->type != XML_ELEMENT_NODE) continue;
        const std::string tag = reinterpret_cast<const char*>(node->name);
        if (tag == "head" || tag == "script" || tag == "style" || tag == "noscript" || tag == "iframe" ||
            tag == "object" || tag == "embed" || tag == "meta" || tag == "link") continue;
        if (tag == "pre") {
            const auto text = take(xmlNodeGetContent(node));
            std::size_t run = 0;
            std::size_t longest = 0;
            for (char c : text) { run = c == '`' ? run + 1 : 0; longest = std::max(longest, run); }
            const std::string fence(std::max<std::size_t>(3, longest + 1), '`');
            output += markdown ? "\n\n" + fence + "\n" + text + "\n" + fence + "\n\n" : "\n" + text + "\n";
            continue;
        }
        auto content = render(node->children, url, markdown, depth + 1);
        if (tag == "br") output += '\n';
        else if (tag == "hr") output += markdown ? "\n\n---\n\n" : "\n";
        else if (tag.size() == 2 && tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6')
            output += "\n\n" + (markdown ? std::string(tag[1] - '0', '#') + " " : "") + fs_tools::trim(content) + "\n\n";
        else if (tag == "p" || tag == "div" || tag == "section" || tag == "article") output += "\n\n" + fs_tools::trim(content) + "\n\n";
        else if (tag == "ul" || tag == "ol") output += "\n" + content + "\n";
        else if (tag == "li") {
            const bool ordered = node->parent && std::string(reinterpret_cast<const char*>(node->parent->name)) == "ol";
            output += "\n" + std::string(markdown ? ordered ? "1. " : "- " : "") + fs_tools::trim(content);
        } else if (markdown && (tag == "strong" || tag == "b")) output += "**" + content + "**";
        else if (markdown && (tag == "em" || tag == "i")) output += "*" + content + "*";
        else if (markdown && tag == "code") output += "`" + content + "`";
        else if (tag == "a" && markdown && !property(node, "href").empty())
            output += "[" + fs_tools::trim(content) + "](" + html_format::resolveUrl(property(node, "href"), url) + ")";
        else if (tag == "img") {
            const auto alt = property(node, "alt");
            const auto src = property(node, "src");
            output += markdown && !src.empty() ? "![" + alt + "](" + html_format::resolveUrl(src, url) + ")" : alt;
        } else if (tag == "tr") output += content + '\n';
        else if (tag == "td" || tag == "th") output += fs_tools::trim(content) + (markdown ? " | " : "\t");
        else if (markdown && tag == "blockquote") output += "\n\n> " + fs_tools::trim(content) + "\n\n";
        else output += content;
    }
    return output;
}
}

void html_format::validateUrl(const std::string& url) {
    if (url.find_first_of("\r\n\t ") != std::string::npos || url.find('\0') != std::string::npos)
        throw std::invalid_argument("Invalid URL characters");
    std::unique_ptr<xmlURI, decltype(&xmlFreeURI)> uri(xmlParseURI(url.c_str()), xmlFreeURI);
    if (!uri || !uri->scheme || !uri->server || !*uri->server || uri->user ||
        (std::string(uri->scheme) != "http" && std::string(uri->scheme) != "https"))
        throw std::invalid_argument("URL must be HTTP(S), with a hostname and without embedded credentials");
}

std::string html_format::resolveUrl(const std::string& reference, const std::string& base) {
    auto result = take(xmlBuildURI(BAD_CAST reference.c_str(), BAD_CAST base.c_str()));
    return result.empty() ? reference : result;
}

std::string html_format::convert(const std::string& html, const std::string& url, bool markdown) {
    std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> document(htmlReadMemory(html.data(), static_cast<int>(html.size()),
        url.c_str(), "UTF-8", HTML_PARSE_RECOVER | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING | HTML_PARSE_NONET), xmlFreeDoc);
    if (!document) throw std::runtime_error("Could not parse HTML response");
    return fs_tools::trim(render(xmlDocGetRootElement(document.get()), url, markdown));
}
