#ifndef SOS_MCP_JSON_FILE_HPP
#define SOS_MCP_JSON_FILE_HPP

#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>

namespace mcp {
// Bounded reads and locked read-modify-write, with atomic 0600 publication.
class JsonFile {
public:
    explicit JsonFile(std::filesystem::path path);
    nlohmann::json read() const;
    void update(const std::function<void(nlohmann::json&)>& change) const;
private:
    std::filesystem::path path;
};
}

#endif // SOS_MCP_JSON_FILE_HPP
