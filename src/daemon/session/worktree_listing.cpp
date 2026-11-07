#include "worktree_listing.hpp"
#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace {
bool isPathTerminator(const std::string& line) {
    if (line == "bare") return true;
    if (!line.starts_with("HEAD ") || (line.size() != 45 && line.size() != 69)) return false;
    return std::all_of(line.begin() + 5, line.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

std::string decodePath(const std::string& value) {
    if (value.empty() || value.front() != '"') return value;
    if (value.size() < 2 || value.back() != '"') throw std::runtime_error("Invalid quoted worktree path");
    std::string decoded;
    const auto limit = value.size() - 1;
    for (std::size_t i = 1; i < limit; ++i) {
        if (value[i] != '\\') { decoded += value[i]; continue; }
        if (++i >= limit) throw std::runtime_error("Invalid worktree path escape");
        const auto escaped = value[i];
        if (escaped >= '0' && escaped <= '7') {
            if (i + 2 >= limit || value[i + 1] < '0' || value[i + 1] > '7' ||
                value[i + 2] < '0' || value[i + 2] > '7')
                throw std::runtime_error("Invalid worktree path octal escape");
            const auto byte = (escaped - '0') * 64 + (value[i + 1] - '0') * 8 + value[i + 2] - '0';
            if (byte > 255) throw std::runtime_error("Invalid worktree path byte");
            decoded += static_cast<char>(byte); i += 2;
        } else {
            switch (escaped) {
                case 'a': decoded += '\a'; break;
                case 'b': decoded += '\b'; break;
                case 't': decoded += '\t'; break;
                case 'n': decoded += '\n'; break;
                case 'v': decoded += '\v'; break;
                case 'f': decoded += '\f'; break;
                case 'r': decoded += '\r'; break;
                case '\\': decoded += '\\'; break;
                case '"': decoded += '"'; break;
                default: throw std::runtime_error("Invalid worktree path escape");
            }
        }
    }
    return decoded;
}
}

std::vector<std::string> worktreeListingFields(const std::string& output) {
    std::vector<std::string> fields;
    std::istringstream lines(output);
    bool rawPath = false;
    for (std::string line; std::getline(lines, line);) {
        // Older Git writes raw paths, including newlines. HEAD (or bare) follows
        // the path; recent Git quotes it instead.
        if (rawPath && !isPathTerminator(line)) {
            fields.back() += '\n' + line;
            continue;
        }
        rawPath = false;
        if (line.starts_with("worktree ")) {
            rawPath = line.size() > 9 && line[9] != '"';
            line = "worktree " + decodePath(line.substr(9));
        }
        fields.push_back(std::move(line));
    }
    return fields;
}
