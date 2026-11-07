#include "skill_template.hpp"
#include "skill_catalog.hpp"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <optional>
#include <regex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
// One pass over the original body: inserted values are never interpreted again.
const std::regex PARAMETER(R"(\$ARGUMENTS\[(\d+)\]|\$(\d+)|\$([A-Za-z_][A-Za-z0-9_-]*))");

bool escapedDollar(const std::string& body, std::size_t offset) {
    return body[offset] == '$' && offset > 0 && body[offset - 1] == '\\' &&
        (offset < 2 || body[offset - 2] != '\\');
}

std::optional<std::size_t> argumentIndex(const std::smatch& match) {
    const auto digits = match[1].matched ? match[1].str() : match[2].str();
    if (digits.empty()) return {};
    std::size_t index;
    const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), index);
    return result.ec == std::errc{} ? std::optional(index) : std::nullopt;
}

} // namespace

// Shell-style grouping only. No shell, environment lookup, globbing or evaluation.
std::vector<std::string> skill_templates::splitArguments(const std::string& input) {
    std::vector<std::string> values;
    std::string value;
    char quote = 0;
    bool started = false;
    for (std::size_t i = 0; i < input.size(); ++i) {
        const auto character = input[i];
        if (character == '\\' && quote != '\'') {
            if (i + 1 == input.size()) throw std::invalid_argument("Unfinished escape in skill input");
            const auto next = input[i + 1];
            if (!quote || next == '"' || next == '\\' || next == '$' || next == '`' || next == '\n') {
                ++i;
                if (next != '\n') value += next;
            } else value += character;
            started = true;
        } else if (quote) {
            if (character == quote) quote = 0;
            else value += character;
        } else if (character == '\'' || character == '"') {
            quote = character;
            started = true;
        } else if (std::isspace(static_cast<unsigned char>(character))) {
            if (started) { values.push_back(std::move(value)); value.clear(); started = false; }
        } else {
            value += character;
            started = true;
        }
    }
    if (quote) throw std::invalid_argument("Unclosed quote in skill input");
    if (started) values.push_back(std::move(value));
    return values;
}

namespace {
class Expansion {
public:
    Expansion(const SkillDefinition& skill, const SkillInvocation& invocation)
        : skill(skill), input(invocation.input) {}

    std::string run() {
        std::string output;
        std::size_t offset = 0;
        for (auto it = std::sregex_iterator(skill.content.begin(), skill.content.end(), PARAMETER);
             it != std::sregex_iterator(); ++it) {
            const auto& match = *it;
            output.append(skill.content, offset, match.position() - offset);
            if (!recognized(match)) output += match.str();
            else if (escapedDollar(skill.content, match.position())) {
                output.pop_back();
                output += match.str();
            } else output += replacement(match);
            offset = match.position() + match.length();
        }
        output.append(skill.content, offset);
        if (!received && !input.empty()) output += "\n\nARGUMENTS: " + input;
        return output;
    }

private:
    bool recognized(const std::smatch& match) const {
        return !match[3].matched || match[3].str() == "ARGUMENTS" ||
            std::find(skill.arguments.begin(), skill.arguments.end(), match[3].str()) != skill.arguments.end();
    }

    std::string replacement(const std::smatch& match) {
        if (match.str() == "$ARGUMENTS") { received = true; return input; }
        if (!positions) positions = skill_templates::splitArguments(input);
        if (match[3].matched) return namedReplacement(match[3].str());
        const auto index = argumentIndex(match);
        if (!index || *index >= positions->size()) return match.str();
        received = true;
        return positions->at(*index);
    }

    std::string namedReplacement(const std::string& name) {
        const auto found = std::find(skill.arguments.begin(), skill.arguments.end(), name);
        const auto index = static_cast<std::size_t>(found - skill.arguments.begin());
        received = true;
        return index < positions->size() ? positions->at(index) : "";
    }

    const SkillDefinition& skill;
    const std::string& input;
    std::optional<std::vector<std::string>> positions;
    bool received = false;
};
}

std::string skill_templates::expand(const SkillDefinition& skill, const SkillInvocation& invocation) {
    return Expansion(skill, invocation).run();
}

