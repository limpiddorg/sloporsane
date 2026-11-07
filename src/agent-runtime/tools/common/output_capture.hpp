#ifndef SOS_OUTPUT_CAPTURE_HPP
#define SOS_OUTPUT_CAPTURE_HPP

#include <filesystem>
#include <string>
#include <string_view>

namespace tool_output {
std::string utf8(std::string_view input);
std::string base64(std::string_view input);
}

// Keeps a bounded tail in memory; spills the full output when the display limit is reached.
class OutputCapture {
public:
    explicit OutputCapture(std::filesystem::path directory, bool keepTail = true);
    ~OutputCapture();
    OutputCapture(const OutputCapture&) = delete;
    OutputCapture& operator=(const OutputCapture&) = delete;

    void append(std::string_view bytes);
    std::string finish();
    bool isTruncated() const;
    const std::filesystem::path& getPath() const;

private:
    void spill(std::string_view bytes);
    std::filesystem::path directory;
    std::filesystem::path path;
    std::string tail;
    std::size_t lineCount = 0;
    int file = -1;
    bool keepTail;
};

#endif // SOS_OUTPUT_CAPTURE_HPP
