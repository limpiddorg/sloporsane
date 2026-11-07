#include "sse_decoder.hpp"
#include <string>

void forEachSseData(std::string_view stream, const std::function<void(std::string_view)>& consume) {
    if (stream.starts_with("\xEF\xBB\xBF")) stream.remove_prefix(3);
    std::string data;
    bool hasData = false;
    const auto flush = [&] {
        if (!hasData) return;
        data.pop_back(); // Remove the last field's newline, as required by SSE.
        consume(data);
        data.clear();
        hasData = false;
    };
    while (!stream.empty()) {
        const auto end = stream.find_first_of("\r\n");
        const auto line = stream.substr(0, end);
        if (end == std::string_view::npos) stream = {};
        else {
            const auto length = stream[end] == '\r' && end + 1 < stream.size() && stream[end + 1] == '\n' ? 2 : 1;
            stream.remove_prefix(end + length);
        }
        if (line.empty()) { flush(); continue; }
        if (line == "data" || line.starts_with("data:")) {
            auto value = line == "data" ? std::string_view{} : line.substr(5);
            if (value.starts_with(' ')) value.remove_prefix(1);
            data.append(value);
            data += '\n';
            hasData = true;
        }
    }
    flush(); // Preserve support for a final frame without an extra blank line.
}
