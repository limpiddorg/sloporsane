#ifndef SOS_HTML_FORMAT_HPP
#define SOS_HTML_FORMAT_HPP

#include <string>

namespace html_format {
std::string convert(const std::string& html, const std::string& url, bool markdown);
std::string resolveUrl(const std::string& reference, const std::string& base);
void validateUrl(const std::string& url);
}

#endif // SOS_HTML_FORMAT_HPP
