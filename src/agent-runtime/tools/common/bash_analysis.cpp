#include "bash_analysis.hpp"
#include <tree_sitter/api.h>
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <set>
#include <stdexcept>

extern "C" const TSLanguage* tree_sitter_bash();

namespace {
std::string text(TSNode node, const std::string& source) {
    return source.substr(ts_node_start_byte(node), ts_node_end_byte(node) - ts_node_start_byte(node));
}
bool literal(TSNode node, const std::string& source) {
    const std::string type = ts_node_type(node);
    if (type == "expansion" || type == "simple_expansion" || type == "command_substitution" ||
        type == "process_substitution" || type == "concatenation") return false;
    for (std::uint32_t i = 0; i < ts_node_named_child_count(node); ++i)
        if (!literal(ts_node_named_child(node, i), source)) return false;
    return true;
}
std::string unquote(std::string value) {
    if (value.size() >= 2 && ((value.front() == '\'' && value.back() == '\'') ||
        (value.front() == '"' && value.back() == '"'))) return value.substr(1, value.size() - 2);
    return value;
}
bool readOnly(const std::vector<std::string>& words) {
    if (words.empty()) return false;
    static const std::set<std::string> READ_COMMANDS{
        "pwd", "ls", "cat", "head", "tail", "wc", "grep", "rg", "stat", "file", "du", "df", "which", "whoami", "uname", "printf", "echo"};
    if (words[0] == "git") {
        if (words.size() < 2) return false;
        static const std::set<std::string> READ_GIT{"status", "diff", "log", "show", "rev-parse", "ls-files", "ls-tree"};
        if (!READ_GIT.contains(words[1])) return false;
        return std::none_of(words.begin() + 2, words.end(), [](const auto& word) {
            return word == "--ext-diff" || word == "--textconv" || word == "--no-index" || word.starts_with("--output") || word.starts_with("--exec");
        });
    }
    if (!READ_COMMANDS.contains(words[0])) return false;
    // Options capable of launching other programs or loading arbitrary configuration are not read-only approvals.
    return std::none_of(words.begin() + 1, words.end(), [](const auto& word) {
        return word == "--pre" || word.starts_with("--pre=") || word == "--pager" || word.starts_with("--pager=");
    });
}
std::string scope(const std::vector<std::string>& words, const std::string& command, bool simple) {
    if (!simple || words.empty()) return command;
    if (words[0] == "git" && words.size() >= 2 && !words[1].starts_with("-")) return "git " + words[1] + " *";
    static const std::set<std::string> SIMPLE{"ls", "cat", "head", "tail", "wc", "grep", "rg", "rm", "cp", "mv", "mkdir", "touch", "chmod", "chown"};
    if (SIMPLE.contains(words[0])) return words[0] + " *";
    // Keep unknown scripts/commands exact instead of approving an entire interpreter.
    return command;
}
void addPath(BashAnalysis& result, std::string value, const std::filesystem::path& cwd) {
    value = unquote(value);
    if (value.empty() || value.starts_with("-") || value.find("://") != std::string::npos || value.find('$') != std::string::npos) return;
    if (value.starts_with("~/")) {
        const auto* home = std::getenv("HOME");
        if (!home) return;
        value = std::string(home) + value.substr(1);
    }
    const auto wildcard = value.find_first_of("*?[");
    if (wildcard != std::string::npos) value = value.substr(0, wildcard);
    if (value.empty()) return;
    auto path = std::filesystem::path(value);
    if (!path.is_absolute()) path = cwd / path;
    path = std::filesystem::weakly_canonical(path);
    if (std::find(result.paths.begin(), result.paths.end(), path) == result.paths.end()) result.paths.push_back(path);
}
void visit(TSNode node, const std::string& source, const std::filesystem::path& cwd, BashAnalysis& result) {
    const std::string type = ts_node_type(node);
    if (type == "command") {
        std::vector<std::string> words;
        bool simple = literal(node, source);
        for (std::uint32_t i = 0; i < ts_node_named_child_count(node); ++i) {
            const auto child = ts_node_named_child(node, i);
            const std::string childType = ts_node_type(child);
            if (childType == "variable_assignment") { simple = false; continue; }
            words.push_back(unquote(text(child, source)));
        }
        const auto command = text(node, source);
        result.commands.push_back(command);
        result.approvalScopes.push_back(scope(words, command, simple));
        result.readOnly = result.readOnly && simple && readOnly(words);
        for (std::size_t i = 0; i < words.size(); ++i) {
            const auto& word = words[i];
            const auto equal = word.find('=');
            if (word.starts_with("-") && equal != std::string::npos) {
                const auto value = word.substr(equal + 1);
                if (value.starts_with("/") || value.starts_with("../") || value.starts_with("./") || value.starts_with("~/"))
                    addPath(result, value, cwd);
            }
            // Filesystem commands accept bare relative paths; all commands get explicit-path detection.
            const bool filesystem = !words.empty() && (words[0] == "cat" || words[0] == "ls" || words[0] == "rm" ||
                words[0] == "cp" || words[0] == "mv" || words[0] == "mkdir" || words[0] == "touch" ||
                words[0] == "head" || words[0] == "tail" || words[0] == "stat" || words[0] == "file" || words[0] == "cd" ||
                words[0] == "grep" || words[0] == "rg" || words[0] == "wc" || words[0] == "du" || words[0] == "df" ||
                words[0] == "chmod" || words[0] == "chown");
            if ((i > 0 && filesystem) || word.starts_with("/") || word.starts_with("../") || word.starts_with("./") || word.starts_with("~/"))
                addPath(result, word, cwd);
        }
    } else if (type == "file_redirect") {
        const auto raw = text(node, source);
        if (raw.find('>') != std::string::npos || raw.starts_with("<>")) result.readOnly = false;
        const auto destination = ts_node_child_by_field_name(node, "destination", 11);
        if (!ts_node_is_null(destination) && literal(destination, source)) addPath(result, text(destination, source), cwd);
    } else if (type != "program" && type != "list" && type != "pipeline" && type != "redirected_statement" &&
        type != "command_name" && type != "word" && type != "string" && type != "raw_string" && type != "string_content" &&
        type != "comment" && type != "number") {
        result.readOnly = false;
    }
    for (std::uint32_t i = 0; i < ts_node_named_child_count(node); ++i)
        visit(ts_node_named_child(node, i), source, cwd, result);
}
}

BashAnalysis analyzeBash(const std::string& command, const std::filesystem::path& cwd) {
    const auto parser = std::unique_ptr<TSParser, decltype(&ts_parser_delete)>(ts_parser_new(), ts_parser_delete);
    if (!parser || !ts_parser_set_language(parser.get(), tree_sitter_bash())) throw std::runtime_error("Cannot initialize Bash parser");
    const auto tree = std::unique_ptr<TSTree, decltype(&ts_tree_delete)>(
        ts_parser_parse_string(parser.get(), nullptr, command.data(), static_cast<std::uint32_t>(command.size())), ts_tree_delete);
    if (!tree) throw std::runtime_error("Cannot parse Bash command");
    BashAnalysis result;
    visit(ts_tree_root_node(tree.get()), command, cwd, result);
    if (ts_node_has_error(ts_tree_root_node(tree.get())) || result.commands.empty()) result.readOnly = false;
    // Redirections and compound syntax must not inherit a broad approval for a harmless prefix.
    if (!result.readOnly && (command.find_first_of("><$`\n") != std::string::npos || result.commands.empty()))
        result.approvalScopes = {command};
    return result;
}
