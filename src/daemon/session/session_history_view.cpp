#include "session_history_view.hpp"
#include <agent-runtime/conversation/messages/message_json.hpp>
#include <agent-runtime/common/path_resolution.hpp>
#include <algorithm>
#include <stdexcept>
#include <sstream>

namespace {
nlohmann::json textMessage(const Message& message) {
    nlohmann::json result{{"id", message.id}, {"role", message.role == MessageRole::USER ? "user" : "assistant"},
        {"content", nlohmann::json::array()}};
    for (const auto& part : message.content)
        if (const auto* text = std::get_if<TextContent>(&part))
            result["content"].push_back({{"type", "text"}, {"text", text->text}});
    if (message.metadata) result["metadata"] = *message.metadata;
    if (message.userMetadata) {
        result["metadata"] = {{"mode", message.userMetadata->mode}};
        if (message.userMetadata->saneRequest) result["metadata"]["sane_request"] = true;
        if (message.userMetadata->implementationRequest) result["metadata"]["implementation_request"] = true;
    }
    if (message.compaction) result["compaction"] = {{"mode", message.compaction->mode}};
    if (message.recap) result["recap"] = true;
    if (message.objectivePlan) result["objective_plan"] = true;
    if (message.implementationPlan) result["implementation_plan"] = *message.implementationPlan;
    if (message.objectiveStack) result["objective_stack"] = *message.objectiveStack;
    if (!message.images.empty()) {
        auto images = message.images;
        for (auto& image : images) image.data.clear();
        result["images"] = images;
    }
    return result;
}
}

std::size_t SessionHistoryView::ensureGroup(const Message& message, std::size_t source, std::size_t part) {
    if (currentGroup) return *currentGroup;
    if (previousGroup) {
        auto& previous = records[*previousGroup];
        previous.terminal = false;
        if (message.elapsedMs && previous.startedMs && *message.elapsedMs >= *previous.startedMs)
            previous.durationMs = *message.elapsedMs - *previous.startedMs;
    }
    const auto id = message.id + "/activity/" + std::to_string(part);
    currentGroup = records.size();
    previousGroup = currentGroup;
    groups.emplace(id, *currentGroup);
    records.push_back({Kind::ACTIVITY, id, source, {}, {}, true});
    records.back().startedMs = message.role == MessageRole::USER ? std::optional<std::uint64_t>{0} : message.elapsedMs;
    records.back().generation = turnGeneration;
    turnGroups.push_back(*currentGroup);
    return *currentGroup;
}

void SessionHistoryView::setReader(std::function<Message(std::size_t)> reader) { read = std::move(reader); }

void SessionHistoryView::update(const std::vector<Message>& history) {
    if (indexed > history.size()) throw std::logic_error("History must be append-only");
    for (; indexed < history.size(); ++indexed) {
        const auto& message = history[indexed];
        if (message.recap) {
            messages.emplace(message.id, visible.size());
            visible.push_back(records.size());
            records.push_back({Kind::MESSAGE, message.id, indexed, {}, {}, true});
            continue;
        }
        if (message.compaction) {
            records.push_back({Kind::COMPACTION, message.id, indexed, {}, {}, true});
            currentGroup.reset();
            previousGroup.reset();
            continue;
        }
        if (message.role == MessageRole::SYSTEM) continue;
        if (message.role == MessageRole::USER) {
            turnGroups.clear();
            turnGeneration.reset();
            messages.emplace(message.id, visible.size());
            visible.push_back(records.size());
            records.push_back({Kind::MESSAGE, message.id, indexed, {}, {}, true});
            currentGroup.reset();
            previousGroup.reset();
            ensureGroup(message, indexed, 0);
            continue;
        }
        if (message.role == MessageRole::ASSISTANT && !message.userSkill && (currentGroup || previousGroup || message.modelCall)) {
            // A response belongs to the activity waiting for it. Never split one
            // provider usage count between text/reasoning/tool parts of a response.
            const auto group = currentGroup ? *currentGroup : previousGroup ? *previousGroup : ensureGroup(message, indexed, 0);
            auto& activity = records[group];
            if (message.modelCall) {
                const auto& call = *message.modelCall;
                turnGeneration = GenerationMetadata{call.model, call.reasoning, 0};
                activity.generation = turnGeneration;
                if (activity.modelCall) activity.modelCall->accumulate(call);
                else activity.modelCall = call;
                if (activity.missingModelCall) activity.modelCall->outputTokens.reset();
            } else {
                activity.missingModelCall = true;
                if (activity.modelCall) activity.modelCall->outputTokens.reset();
            }
        }
        if (message.metadata) {
            const auto group = currentGroup ? *currentGroup : previousGroup ? *previousGroup : ensureGroup(message, indexed, 0);
            records[group].metadata = indexed;
            turnGeneration = *message.metadata;
            for (const auto turnGroup : turnGroups)
                if (!records[turnGroup].generation) records[turnGroup].generation = turnGeneration;
        }
        bool displayed = false;
        for (std::size_t part = 0; part < message.content.size(); ++part) {
            const auto& content = message.content[part];
            if (const auto* text = std::get_if<TextContent>(&content)) {
                if (message.role == MessageRole::ASSISTANT && text->text.find_first_not_of(" \r\n\t") != std::string::npos) {
                    if (!displayed) {
                        messages.emplace(message.id, visible.size());
                        visible.push_back(records.size());
                    }
                    records.push_back({Kind::MESSAGE, message.id, indexed, {}, {}, true, part});
                    displayed = true;
                    currentGroup.reset();
                }
            } else if (const auto* result = std::get_if<ToolResultContent>(&content)) {
                const auto found = calls.find(result->callId);
                if (found != calls.end()) {
                    const auto [group, activity] = found->second;
                    records[group].activities[activity].result = Reference{indexed, part};
                    calls.erase(found);
                } else {
                    records[ensureGroup(message, indexed, part)].activities.push_back({{indexed, part}, {}});
                }
            } else if (const auto* call = std::get_if<ToolCallContent>(&content)) {
                const auto group = ensureGroup(message, indexed, part);
                calls[call->callId] = {group, records[group].activities.size()};
                records[group].activities.push_back({{indexed, part}, {}});
            } else if (const auto* reasoning = std::get_if<ReasoningContent>(&content); reasoning && reasoning->text && !reasoning->text->empty()) {
                records[ensureGroup(message, indexed, part)].activities.push_back({{indexed, part}, {}});
            }
        }
    }
}

SessionHistoryView::Page SessionHistoryView::page(const std::string& direction, const std::string& anchor, bool /*clean*/) const {
    std::size_t end = visible.size(), start = end > PAGE_SIZE ? end - PAGE_SIZE : 0;
    if (direction != "latest") {
        const auto found = messages.find(anchor);
        if (found == messages.end()) throw std::invalid_argument("Unknown history anchor");
        if (direction == "before") {
            start = found->second > PAGE_STEP ? found->second - PAGE_STEP : 0;
            end = std::min(visible.size(), start + PAGE_SIZE);
        } else if (direction == "after") {
            end = std::min(visible.size(), found->second + 1 + PAGE_STEP);
            start = end > PAGE_SIZE ? end - PAGE_SIZE : 0;
        } else throw std::invalid_argument("Unknown history direction");
    }
    // The client makes room for a submitted message; snapshots use the full page.
    Page result;
    result.hasOlder = start > 0;
    result.hasNewer = end < visible.size();
    if (start < end) {
        result.first = records[visible[start]].id;
        result.last = records[visible[end - 1]].id;
    }
    const auto beginRecord = start == 0 ? 0 : (start < visible.size() ? visible[start] : records.size());
    const auto endRecord = end < visible.size() ? visible[end] : records.size();
    for (auto i = beginRecord; i < endRecord; ++i) result.records.push_back(i);
    // Preload only the last group in the entire session, including an empty or
    // in-progress group. Older windows never substitute their own last group.
    for (auto i = records.size(); i > 0; --i) {
        if (records[i - 1].kind != Kind::ACTIVITY) continue;
        if (i - 1 >= beginRecord && i - 1 < endRecord) result.loadedGroups.insert(i - 1);
        break;
    }
    return result;
}

nlohmann::json SessionHistoryView::fileArguments(const std::string& tool, const nlohmann::json& arguments) {
    auto result = nlohmann::json::object();
    if (!arguments.is_object()) return result;
    if ((tool == "grep" || tool == "glob") && arguments.contains("pattern") && arguments.at("pattern").is_string())
        result["pattern"] = arguments.at("pattern");
    if (tool == "bash" && arguments.contains("mainCommand") && arguments.at("mainCommand").is_string())
        result["mainCommand"] = arguments.at("mainCommand");
    if ((tool == "write" || tool == "edit") && arguments.contains("filePath") && arguments.at("filePath").is_string())
        result["filePath"] = arguments.at("filePath");
    if (tool != "apply_patch") return result;
    auto paths = nlohmann::json::array();
    const auto text = arguments.find("patchText");
    if (text == arguments.end() && arguments.contains("filePaths") && arguments.at("filePaths").is_array()) {
        for (const auto& path : arguments.at("filePaths")) if (path.is_string()) paths.push_back(path);
        return {{"filePaths", std::move(paths)}};
    }
    if (text == arguments.end() || !text->is_string()) return result;
    std::istringstream patch(text->get<std::string>());
    for (std::string line; std::getline(patch, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        for (const auto* header : {"*** Add File: ", "*** Update File: ", "*** Delete File: "})
            if (line.starts_with(header)) paths.push_back(line.substr(std::char_traits<char>::length(header)));
        if (line.starts_with("*** Move to: ") && !paths.empty()) paths.back() = line.substr(13);
    }
    if (!paths.empty()) result["filePaths"] = std::move(paths);
    return result;
}

nlohmann::json SessionHistoryView::fileMetadata(const nlohmann::json& metadata) {
    auto result = nlohmann::json::object();
    if (metadata.contains("filepath")) result["filepath"] = metadata.at("filepath");
    if (metadata.contains("files") && metadata.at("files").is_array()) {
        result["files"] = nlohmann::json::array();
        for (const auto& file : metadata.at("files")) {
            auto brief = nlohmann::json::object();
            for (const auto* key : {"filePath", "relativePath", "movePath", "type"})
                if (file.contains(key)) brief[key] = file.at(key);
            result["files"].push_back(std::move(brief));
        }
    }
    return result;
}

std::optional<std::string> SessionHistoryView::skillScriptCommand(const std::string& tool, const nlohmann::json& arguments) {
    const auto command = arguments.find("command");
    if (tool != "bash" || command == arguments.end() || !command->is_string()) return std::nullopt;
    const auto& text = command->get_ref<const std::string&>();
    if (text.size() > 8192 || (text.find(".sos/skills/") == std::string::npos &&
        text.find(PathResolution::skillsDirectory().generic_string() + "/") == std::string::npos)) return std::nullopt;
    return text;
}

std::optional<std::string> SessionHistoryView::skillScriptName(const std::string& tool, const nlohmann::json& arguments) {
    const auto script = skillScript(tool, arguments);
    return script ? std::optional<std::string>{script->name} : std::nullopt;
}

std::optional<SessionHistoryView::SkillScript> SessionHistoryView::skillScript(const std::string& tool, const nlohmann::json& arguments) {
    const auto command = skillScriptCommand(tool, arguments);
    if (!command || command->find_first_of("\n\r$`;&|<>()*?[]{}~\\") != std::string::npos) return std::nullopt;
    // Only literal simple commands. This classification never authorizes execution.
    std::vector<std::string> words;
    std::string word;
    char quote = 0;
    bool started = false;
    for (char character : *command) {
        if (quote) {
            if (character == quote) quote = 0;
            else word += character;
        } else if (character == '\'' || character == '"') {
            quote = character;
            started = true;
        } else if (character == '#' && !started) break;
        else if (character == ' ' || character == '\t') {
            if (started) { words.push_back(word); word.clear(); started = false; }
        } else { word += character; started = true; }
    }
    if (quote) return std::nullopt;
    if (started) words.push_back(word);
    if (words.empty() || words[0].find('=') != std::string::npos) return std::nullopt;
    const auto executable = words[0].substr(words[0].find_last_of('/') + 1);
    const std::size_t index = executable == "python" || executable == "python3" || executable == "bash" || executable == "sh" ? 1 : 0;
    if (words.size() <= index || words[index].empty() || words[index].front() == '-') return std::nullopt;
    std::vector<std::string> parts;
    auto path = words[index];
    const auto userPrefix = PathResolution::skillsDirectory().generic_string() + "/";
    if (path.starts_with(userPrefix)) path = ".sos/skills/" + path.substr(userPrefix.size());
    for (std::size_t start = 0; start < path.size();) {
        auto end = path.find('/', start);
        if (end == std::string::npos) end = path.size();
        const auto part = path.substr(start, end - start);
        if (part == "..") return std::nullopt;
        if (!part.empty() && part != ".") parts.push_back(part);
        start = end + 1;
    }
    for (std::size_t i = 0; i + 4 < parts.size(); ++i) {
        const auto& name = parts[i + 2];
        if (parts[i] != ".sos" || parts[i + 1] != "skills" || parts[i + 3] != "scripts") continue;
        if (name.size() > 64 || name.front() < 'a' || name.front() > 'z') continue;
        if (name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos) continue;
        std::string relative;
        for (auto j = i + 4; j < parts.size(); ++j) {
            if (!relative.empty()) relative += '/';
            relative += parts[j];
        }
        return SkillScript{name, relative};
    }
    return std::nullopt;
}

nlohmann::json SessionHistoryView::activityData(const std::vector<Message>& history, const Record& group) const {
    auto items = nlohmann::json::array();
    for (std::size_t i = 0; i < group.activities.size(); ++i)
        items.push_back(activityItem(history, group.activities[i], i, false));
    return items;
}

nlohmann::json SessionHistoryView::activityItem(const std::vector<Message>& history,
    const Activity& activity, std::size_t index, bool detail) const {
    const auto sourceMessage = detail && read ? read(activity.source.message) : history[activity.source.message];
    const auto& source = sourceMessage.content.at(activity.source.part);
    nlohmann::json item{{"activityIndex", index}};
    if (const auto* reasoning = std::get_if<ReasoningContent>(&source)) {
        item["kind"] = "reasoning";
        if (detail) item["text"] = reasoning->text.value_or("");
        return item;
    }
    item.update({{"kind", "tool"}, {"toolName", "tool"}, {"arguments", nlohmann::json::object()}});
    if (sourceMessage.userSkill) item["user_skill"] = *sourceMessage.userSkill;
    bool read = false;
    if (const auto* call = std::get_if<ToolCallContent>(&source)) {
        item.update({{"callId", call->callId}, {"toolName", call->toolName}});
        if (const auto script = skillScript(call->toolName, call->arguments))
            item.update({{"skill_script", script->name}, {"skill_script_path", script->path}});
        read = call->toolName == "read";
        if (detail) item["arguments"] = call->arguments;
        else item["arguments"].update(fileArguments(call->toolName, call->arguments));
        if (!detail && read && call->arguments.contains("filePath")) item["arguments"]["filePath"] = call->arguments["filePath"];
        else if (!detail && call->toolName == "skill") {
            if (call->arguments.contains("name")) item["arguments"]["name"] = call->arguments["name"];
        }
    }
    const auto* result = std::get_if<ToolResultContent>(&source);
    std::optional<Message> resultMessage;
    if (activity.result) {
        const auto& ref = *activity.result;
        const auto& brief = std::get<ToolResultContent>(history[ref.message].content.at(ref.part));
        if (brief.metadata.contains("approved_action")) read = brief.metadata.at("approved_action").at("tool") == "read";
        resultMessage = this->read && (detail || (read && brief.isError))
            ? this->read(ref.message) : history[ref.message];
        result = &std::get<ToolResultContent>(resultMessage->content.at(ref.part));
    }
    if (!result) return item;
    if (result->metadata.contains("approved_action")) {
        const auto& replay = result->metadata.at("approved_action");
        auto arguments = replay.value("arguments", nlohmann::json::object());
        item["toolName"] = replay.at("tool");
        if (const auto script = skillScript(replay.at("tool"), arguments))
            item.update({{"skill_script", script->name}, {"skill_script_path", script->path}});
        if (detail || replay.at("tool") == "read") item["arguments"] = arguments;
        else item["arguments"].update(fileArguments(replay.at("tool"), arguments));
    }
    item["callId"] = result->callId;
    auto metadata = nlohmann::json::object();
    if (detail) metadata = result->metadata;
    else {
        metadata.update(fileMetadata(result->metadata));
        for (const auto* key : {"exit", "timedOut", "cancelled", "started", "refused_message_id", "mcp_server", "mcp_tool"})
            if (result->metadata.contains(key)) metadata[key] = result->metadata[key];
        const auto permissions = result->metadata.find("permissions");
        if (permissions != result->metadata.end() && permissions->is_array()) {
            metadata["permissions"] = nlohmann::json::array();
            for (const auto& permission : *permissions) {
                auto summary = nlohmann::json::object();
                for (const auto* key : {"id", "decision", "confidence"})
                    if (permission.contains(key)) summary[key] = permission[key];
                metadata["permissions"].push_back(std::move(summary));
            }
        }
    }
    item["result"] = {{"isError", result->isError}, {"metadata", std::move(metadata)}};
    if (detail || (read && result->isError)) item["result"]["output"] = result->output;
    if (detail) {
        item["result"]["attachments"] = nlohmann::json::array();
        for (const auto& attachment : result->attachments)
            item["result"]["attachments"].push_back({{"mime", attachment.mime}, {"url", attachment.url}});
    }
    return item;
}

nlohmann::json SessionHistoryView::record(const std::vector<Message>& history, std::size_t index, bool includeActivities) const {
    const auto& entry = records.at(index);
    if (entry.kind != Kind::ACTIVITY) {
        const auto message = read ? read(entry.source) : history[entry.source];
        auto result = textMessage(message);
        if (entry.textPart) result["content"] = nlohmann::json::array({{
            {"type", "text"}, {"text", std::get<TextContent>(message.content.at(*entry.textPart)).text}}});
        return result;
    }
    nlohmann::json result{{"id", entry.id}, {"role", "activity"}, {"content", nlohmann::json::array()},
        {"count", entry.activities.size()}, {"terminal", entry.terminal}, {"kinds", nlohmann::json::array()}};
    bool tools = false, reads = false, reasoning = false, refused = false, skills = false;
    for (const auto& activity : entry.activities) {
        const auto& source = history[activity.source.message].content[activity.source.part];
        if (history[activity.source.message].userSkill) skills = true;
        else if (std::holds_alternative<ReasoningContent>(source)) reasoning = true;
        else if (const auto* call = std::get_if<ToolCallContent>(&source); call && call->toolName == "read") reads = true;
        else if (const auto* call = std::get_if<ToolCallContent>(&source); call && (call->toolName == "skill" || skillScriptName(call->toolName, call->arguments))) skills = true;
        else tools = true;
        const auto* toolResult = std::get_if<ToolResultContent>(&source);
        if (activity.result) {
            const auto& ref = *activity.result;
            toolResult = std::get_if<ToolResultContent>(&history[ref.message].content[ref.part]);
        }
        if (toolResult) for (const auto& permission : toolResult->metadata.value("permissions", nlohmann::json::array()))
            if (permission.value("decision", "") == "ai_refused") refused = true;
    }
    if (tools) result["kinds"].push_back("tools");
    if (reads) result["kinds"].push_back("read");
    if (skills) result["kinds"].push_back("skill");
    if (reasoning) result["kinds"].push_back("reasoning");
    if (refused) result["kinds"].push_back("refused");
    if (entry.metadata) result["metadata"] = *history[*entry.metadata].metadata;
    if (entry.durationMs) result["duration_ms"] = *entry.durationMs;
    if (entry.startedMs) result["started_ms"] = *entry.startedMs;
    if (entry.generation) result["generation"] = {{"model", entry.generation->model}, {"reasoning", entry.generation->reasoning}};
    if (entry.modelCall) result["model_call"] = *entry.modelCall;
    if (includeActivities) result["activities"] = activityData(history, entry);
    return result;
}

nlohmann::json SessionHistoryView::activities(const std::vector<Message>& history, const std::string& groupId) const {
    const auto found = groups.find(groupId);
    if (found == groups.end()) throw std::invalid_argument("Unknown activity group");
    return record(history, found->second, true);
}

nlohmann::json SessionHistoryView::activityDetail(const std::vector<Message>& history,
    const std::string& groupId, std::size_t index) const {
    const auto found = groups.find(groupId);
    if (found == groups.end() || index >= records[found->second].activities.size())
        throw std::invalid_argument("Unknown activity detail");
    return {{"id", groupId}, {"role", "activity"}, {"content", nlohmann::json::array()},
        {"activities", nlohmann::json::array({activityItem(history, records[found->second].activities[index], index, true)})}};
}
