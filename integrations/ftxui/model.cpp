#include "model.hpp"
#include <algorithm>
#include <stdexcept>
#include <limits>
namespace sos::tui {
std::string stringField(const Json& value, const char* key, std::string fallback) {
    auto it = value.find(key);
    return it != value.end() && it->is_string() ? it->get<std::string>() : fallback;
}
namespace {
std::optional<long long> number(const Json& value, const char* key) {
    auto it = value.find(key);
    if (it != value.end() && it->is_number_integer()) return it->get<long long>();
    return std::nullopt;
}
std::optional<long long> sum(std::optional<long long> a, std::optional<long long> b) {
    if (!a || !b || *a < 0 || *b < 0 || *a > std::numeric_limits<long long>::max() - *b) return {};
    return *a + *b;
}
std::string display(const Json& value) { return value.is_string() ? value.get<std::string>() : value.dump(2); }
Entry activityEntry(const Json& row, const std::string& group, bool detail) {
    Entry entry;
    entry.group = group;
    entry.activityIndex = row.at("activityIndex").get<int>();
    entry.id = group + "/item/" + std::to_string(entry.activityIndex);
    entry.callId = stringField(row, "callId");
    entry.kind = stringField(row, "kind") == "reasoning" ? Entry::Kind::Reasoning : Entry::Kind::Tool;
    entry.title = entry.kind == Entry::Kind::Reasoning ? "Reasoning" : stringField(row, "toolName", "Tool");
    entry.toolName = stringField(row, "toolName"); entry.arguments = row.value("arguments", Json::object());
    entry.loaded = detail;
    entry.expandable = entry.folded = true;
    entry.body = entry.kind == Entry::Kind::Reasoning ? stringField(row, "text") : "Arguments\n" + display(row.value("arguments", Json::object()));
    if (row.contains("result")) {
        const auto& result = row.at("result");
        const auto metadata = result.value("metadata", Json::object());
        entry.output = display(result.value("output", Json(""))); entry.resultMetadata = metadata;
        entry.resultReceived = true; entry.error = result.value("isError", false);
        entry.body += "\nResults\n" + display(result.value("output", Json("")));
        if (metadata.contains("diff")) entry.body += "\n" + display(metadata["diff"]);
        if (result.value("isError", false)) entry.title += " [Error]";
        entry.refusedId = stringField(metadata, "refused_message_id");
        entry.approved = metadata.value("manually_approved", false);
        for (const auto& permission : metadata.value("permissions", Json::array())) entry.body += "\n -> " + stringField(permission, "decision");
    }
    if (stringField(row, "toolName") == "read") {
        entry.title += " · " + stringField(row.value("arguments", Json::object()), "filePath");
        entry.expandable = false;
        entry.folded = false;
        entry.body.clear();
    }
    return entry;
}
std::string textContent(const Json& message) {
    std::string result;
    for (const auto& part : message.value("content", Json::array())) {
        if (part.value("type", "") == "text") {
            if (!result.empty()) result += '\n';
            result += stringField(part, "text");
        }
    }
    return result;
}
void prepend(std::string& target, const std::string& value) {
    if (!value.empty()) target = value + (target.empty() ? "" : "\n" + target);
}
}
void Info::update(const Json& value) {
    model = stringField(value, "model", model);
    provider = stringField(value, "provider", provider);
    reasoning = stringField(value, "reasoning", reasoning);
    permissionMode = stringField(value, "permission_mode", permissionMode);
    used = number(value, "last_turn_used_context");
    total = number(value, "total_context");
    contextLimit = number(value, "context_limit");
    hasKey = value.value("has_permission_key", hasKey);
    lastRecap = value.value("last_history_is_recap", lastRecap);
}
Transfer::Transfer(const Json& header) {
    request = header.at("requestId").get<std::string>();
    sessionId = header.at("sessionId").get<std::string>();
    session = header.at("session");
    auto count = number(header, "messageCount");
    if (!count || *count < 0 || *count > 1000000 || session.at("id") != sessionId)
        throw std::runtime_error("Invalid history header");
    count_ = static_cast<std::size_t>(*count);
    purpose = stringField(header, "purpose", "page");
    group = stringField(header, "groupId");
    activityIndex = static_cast<int>(number(header, "activityIndex").value_or(-1));
    branch = header.value("branch", Json::object());
    const auto p = header.value("page", Json::object());
    page = {stringField(p, "first"), stringField(p, "last"), p.value("hasOlder", false), p.value("hasNewer", false)};
}
bool Transfer::accept(const Json& event) {
    if (event.at("requestId") != request || event.at("sessionId") != sessionId)
        throw std::runtime_error("Mismatched history transfer");
    const auto type = event.at("type").get<std::string>();
    if (type == "session.attached" || type == "btw.finished") {
        if (!fragment_.empty() || messages.size() != count_ || number(event, "messageCount") != std::optional<long long>(count_))
            throw std::runtime_error("Incomplete history transfer");
        return true;
    }
    if ((type != "session.history_chunk" && type != "btw.chunk") || messages.size() >= count_ ||
        number(event, "messageIndex") != std::optional<long long>(messages.size()) ||
        number(event, "partIndex") != std::optional<long long>(part_) ||
        !event.at("last").is_boolean() || !event.at("data").is_string() || event.at("data").get_ref<const std::string&>().empty())
        throw std::runtime_error("Invalid or out-of-order history fragment");
    fragment_ += event.at("data").get_ref<const std::string&>();
    ++part_;
    if (event.at("last").get<bool>()) {
        auto message = Json::parse(fragment_);
        const auto role = stringField(message, "role");
        if (!message.is_object() || !message.at("content").is_array() ||
            (role != "user" && role != "assistant" && role != "tool" && role != "system" && role != "activity"))
            throw std::runtime_error("Invalid history message");
        messages.push_back(std::move(message));
        fragment_.clear();
        part_ = 0;
    }
    return false;
}
void Model::begin(const std::string& text, const std::string& request) {
    openBranch_.clear();
    for (auto& [id, branch] : branches) branch.entry.folded = true;
    liveGroup_.clear();
    for (auto& entry : entries) if (entry.expandable) {
        entry.folded = true;
        if (entry.kind == Entry::Kind::Activity) entry.loaded = false;
    }
    std::erase_if(entries, [](const Entry& entry) { return !entry.group.empty() && entry.kind != Entry::Kind::Activity; });
    provisional = text;
    pendingRequest = request;
    busy = true;
    status = "Preparing conversation...";
    for (auto& entry : entries) if (entry.expandable) entry.folded = true;
    Entry entry;
    entry.id = "local-pending";
    entry.kind = Entry::Kind::User;
    entry.body = text;
    entry.mode = mode;
    entries.push_back(std::move(entry));
}
void Model::restore() {
    prepend(draft, provisional);
    images.insert(images.begin(), provisionalImages.begin(), provisionalImages.end());
    provisionalImages.clear();
    provisional.clear();
    pendingRequest.clear();
    std::erase_if(entries, [](const Entry& entry) { return entry.id == "local-pending"; });
}
void Model::restoreQueue() {
    prepend(draft, queued); queued.clear();
    images.insert(images.begin(), queuedImages.begin(), queuedImages.end()); queuedImages.clear();
}
void Model::ensureGroup(const std::string& id, unsigned part) {
    if (!liveGroup_.empty()) return;
    Entry group;
    group.group = group.id = liveGroup_ = id + "/activity/" + std::to_string(part);
    group.kind = Entry::Kind::Activity; group.title = "Activity";
    group.expandable = group.folded = true;
    entries.push_back(std::move(group));
}
void Model::append(const Json& message, bool projected) {
    const auto role = stringField(message, "role");
    if (role == "system") return;
    const auto id = stringField(message, "id");
    if (!projected && !id.empty() && std::any_of(entries.begin(), entries.end(), [&](const Entry& e) { return e.id == id || e.id == id + "/text/0"; })) return;
    Entry entry;
    entry.id = id;
    entry.body = textContent(message);
    if (role == "user") {
        if (!pendingRequest.empty()) {
            provisional.clear(); provisionalImages.clear(); pendingRequest.clear();
            std::erase_if(entries, [](const Entry& e) { return e.id == "local-pending"; });
        }
        entry.kind = Entry::Kind::User;
        entry.mode = stringField(message.value("metadata", Json::object()), "mode", mode);
        for (const auto& image : message.value("images", Json::array()))
            entry.body += "\n[Image: " + stringField(image, "name", stringField(image, "id")) + "]";
        entries.push_back(std::move(entry));
        if (!projected) { liveGroup_.clear(); ensureGroup(id, 0); }
        return;
    }
    if (role == "activity") {
        entry.kind = Entry::Kind::Activity;
        entry.title = "Activity (" + std::to_string(message.value("count", 0)) + ")";
        entry.group = id;
        entry.expandable = entry.folded = true;
        entry.loaded = message.contains("activities");
        entry.durationMs = number(message, "duration_ms");
        if (!entry.durationMs) entry.durationMs = number(message.value("metadata", Json::object()), "duration_ms");
        const auto call = message.value("model_call", Json::object());
        entry.modelCallMs = number(call, "duration_ms"); entry.outputTokens = number(call, "output_tokens");
        for (const auto& kind : message.value("kinds", Json::array()))
            entry.activityKinds |= kind == "tools" ? 1u : kind == "read" ? 2u : kind == "reasoning" ? 4u : kind == "refused" ? 8u : 0u;
        entries.push_back(entry);
        for (const auto& row : message.value("activities", Json::array())) entries.push_back(activityEntry(row, id, false));
        return;
    }
    if (message.value("recap", false) || message.contains("compaction")) {
        if (message.contains("compaction")) liveGroup_.clear();
        entry.kind = message.value("recap", false) ? Entry::Kind::Recap : Entry::Kind::Compaction;
        entry.title = message.value("recap", false) ? "Conversation Recap" : "Compaction";
        entry.expandable = entry.folded = message.contains("compaction");
        entries.push_back(std::move(entry));
        return;
    }
    if (role == "assistant" && message.contains("model_call")) {
        auto group = std::find_if(entries.rbegin(), entries.rend(), [](const Entry& e) { return e.kind == Entry::Kind::Activity; });
        if (group != entries.rend()) {
            const auto& call = message["model_call"];
            const auto duration = number(call, "duration_ms"), tokens = number(call, "output_tokens");
            group->modelCallMs = group->modelCalls ? sum(group->modelCallMs, duration) : duration;
            group->outputTokens = group->modelCalls ? sum(group->outputTokens, tokens) : tokens;
            ++group->modelCalls;
        }
    }
    int partIndex = 0;
    for (const auto& part : message.value("content", Json::array())) {
        Entry item;
        item.id = id + "/" + std::to_string(partIndex++);
        const auto type = stringField(part, "type");
        if (type == "text") {
            item.body = stringField(part, "text");
            if (item.body.find_first_not_of(" \t\r\n") == std::string::npos) continue;
            const auto ordinal = std::count_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.id.starts_with(id + "/text/"); });
            item.id = id + "/text/" + std::to_string(ordinal); liveGroup_.clear();
        }
        else if (type == "reasoning") {
            item.kind = Entry::Kind::Reasoning; item.title = "Reasoning";
            item.body = stringField(part, "text"); item.expandable = item.folded = true;
            if (item.body.empty()) continue;
        } else if (type == "tool_call") {
            item.kind = Entry::Kind::Tool; item.title = stringField(part, "toolName");
            item.toolName = item.title; item.arguments = part.value("arguments", Json::object());
            item.callId = stringField(part, "callId");
            item.body = "Arguments\n" + display(part.value("arguments", Json::object()));
            item.expandable = item.folded = true;
        } else if (type == "tool_result") {
            const auto call = stringField(part, "callId");
            auto found = std::find_if(entries.begin(), entries.end(), [&](const Entry& e) { return !call.empty() && e.callId == call; });
            auto result = "\nResults\n" + display(part.value("output", Json("")));
            const auto metadata = part.value("metadata", Json::object());
            if (metadata.contains("diff")) result += "\n" + display(metadata["diff"]);
            for (const auto& permission : metadata.value("permissions", Json::array())) result += "\n -> " + stringField(permission, "decision");
            if (found != entries.end()) {
                found->body += result;
                found->output = display(part.value("output", Json(""))); found->resultMetadata = metadata;
                found->resultReceived = true; found->error = part.value("isError", false);
                if (part.value("isError", false)) found->title += " [Error]";
                if (metadata.contains("refused_action")) {
                    found->refusedId = stringField(metadata, "refused_message_id", id);
                    found->approved = metadata.value("manually_approved", false);
                    const auto group = found->group;
                    for (auto& entry : entries) if (entry.id == group) entry.activityKinds |= 8;
                }
                continue;
            }
            item.kind = Entry::Kind::Tool; item.title = "Tool result"; item.body = result;
            item.output = display(part.value("output", Json(""))); item.resultMetadata = metadata;
            item.resultReceived = true; item.error = part.value("isError", false);
            item.expandable = item.folded = true;
        } else continue;
        if (item.kind == Entry::Kind::Reasoning || item.kind == Entry::Kind::Tool) {
            ensureGroup(id, partIndex - 1);
            item.group = liveGroup_;
            item.activityIndex = static_cast<int>(std::count_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.group == liveGroup_ && e.kind != Entry::Kind::Activity; }));
            item.id = liveGroup_ + "/item/" + std::to_string(item.activityIndex);
            for (auto& entry : entries) if (entry.id == liveGroup_)
                entry.activityKinds |= item.kind == Entry::Kind::Reasoning ? 4 : item.toolName == "read" ? 2 : 1;
            if (item.title == "read") {
                item.title += " · " + stringField(part.value("arguments", Json::object()), "filePath");
                item.expandable = false; item.folded = false; item.body.clear();
            }
        }
        if (!item.body.empty() || item.expandable || !item.title.empty()) entries.push_back(std::move(item));
    }
    const auto metadata = message.value("metadata", Json::object());
    if (metadata.contains("duration_ms")) {
        std::erase_if(entries, [&](const Entry& e) { return e.id == id + "/footer"; });
        Entry footer;
        footer.id = id + "/footer"; footer.kind = Entry::Kind::Footer;
        footer.body = stringField(metadata, "model") + " | " + stringField(metadata, "reasoning") + " | " + std::to_string(metadata.value("duration_ms", 0LL) / 1000) + "s";
        entries.push_back(std::move(footer));
    }
}
void Model::finishTransfer(Transfer& transfer) {
    if (transfer.purpose == "page") {
        const bool sameSession = sessionId == transfer.sessionId;
        if (!sameSession) { branches.clear(); openBranch_.clear(); }
        auto previous = sameSession ? std::move(entries) : std::vector<Entry>{};
        liveGroup_.clear();
        entries.clear();
        sessionId = transfer.sessionId;
        title = stringField(transfer.session, "title", "-");
        workspace = stringField(transfer.session, "workspace", launchWorkspace);
        if (transfer.request.starts_with("attach-")) mode = stringField(transfer.session, "mode", mode);
        page = transfer.page;
        for (const auto& message : transfer.messages) append(message, true);
        std::map<std::string, const Entry*> old;
        for (const auto& entry : previous) old[entry.id] = &entry;
        std::vector<Entry> reused;
        for (auto& entry : entries) {
            const auto found = old.find(entry.id);
            const Entry* before = found == old.end() ? nullptr : found->second;
            if (before && before->kind == entry.kind) {
                entry.folded = before->folded;
                if (entry.activityIndex >= 0 && before->loaded && !entry.loaded && entry.title == before->title) {
                    const auto approved = entry.approved;
                    entry = *before; entry.approved = approved;
                }
            }
            reused.push_back(entry);
            if (before && entry.kind == Entry::Kind::Activity && before->loaded && !entry.loaded && entry.title == before->title) {
                reused.back().loaded = true;
                for (const auto& child : previous) if (child.group == entry.id && child.kind != Entry::Kind::Activity) reused.push_back(child);
            }
        }
        entries = std::move(reused);
        if (busy && !page.newer && !entries.empty()) {
            const auto& last = entries.back();
            if (last.kind == Entry::Kind::Activity) liveGroup_ = last.id;
            else if (last.kind == Entry::Kind::Tool || last.kind == Entry::Kind::Reasoning) liveGroup_ = last.group;
        }
        placeBranches();
        loading = false;
    } else if (transfer.purpose == "activities" || transfer.purpose == "activity-detail") {
        if (transfer.purpose == "activity-detail") {
            for (const auto& message : transfer.messages) for (const auto& row : message.value("activities", Json::array())) {
                auto entry = activityEntry(row, transfer.group, true);
                auto found = std::find_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.id == entry.id; });
                if (found != entries.end()) { entry.folded = found->folded; *found = std::move(entry); }
            }
            return;
        }
        std::erase_if(entries, [&](const Entry& e) { return e.group == transfer.group && e.kind != Entry::Kind::Activity; });
        auto found = std::find_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.id == transfer.group; });
        if (found == entries.end()) return;
        found->loaded = true;
        std::vector<Entry> children;
        for (const auto& message : transfer.messages) for (const auto& row : message.value("activities", Json::array())) children.push_back(activityEntry(row, transfer.group, false));
        entries.insert(found + 1, children.begin(), children.end());
    }
}
void Model::placeBranches() {
    std::erase_if(entries, [](const Entry& entry) { return entry.kind == Entry::Kind::Btw; });
    for (const auto& [id, branch] : branches) {
        auto found = std::find_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.id == branch.anchor || e.id == branch.anchor + "/text/0"; });
        if (found != entries.end()) entries.insert(found + 1, branch.entry);
        else if (!page.newer) entries.push_back(branch.entry);
    }
}
void Model::apply(const Json& event) {
    if (event.contains("info")) info.update(event["info"]);
    const auto type = stringField(event, "type");
    if (type == "btw.accepted") {
        openBranch_ = stringField(event, "branchId");
        for (auto& [id, branch] : branches) branch.entry.folded = id != openBranch_;
        placeBranches();
    }
    if (type == "session.attach_started" || type == "btw.started") {
        Transfer transfer(event);
        if (type == "btw.started") transfer.purpose = "btw";
        const auto request = transfer.request;
        if (transfers.contains(request)) throw std::runtime_error("Duplicate history transfer");
        transfers.emplace(request, std::move(transfer));
    } else if (type == "session.history_chunk" || type == "session.attached" || type == "btw.chunk" || type == "btw.finished") {
        auto found = transfers.find(event.at("requestId").get<std::string>());
        if (found == transfers.end()) throw std::runtime_error("Unexpected history fragment");
        if (found->second.accept(event)) {
            if (found->second.purpose == "btw") {
                const auto& transfer = found->second;
                const auto branchId = stringField(transfer.branch, "id");
                Entry item;
                item.id = branchId; item.title = "BTW " + branchId;
                item.kind = Entry::Kind::Btw; item.expandable = true;
                item.folded = branches.contains(branchId) ? branches.at(branchId).entry.folded : branchId != openBranch_;
                for (const auto& message : transfer.messages) item.body += stringField(message, "role") + ": " + textContent(message) + "\n\n";
                item.body += stringField(transfer.branch, "error");
                item.body += "\n" + branchId + " | " + stringField(transfer.branch, "model") + " | " + stringField(transfer.branch, "reasoning") + " | " + stringField(transfer.branch, "status");
                branches[branchId] = {stringField(transfer.branch, "anchor"), stringField(transfer.branch, "pageAnchor"), transfer.branch.value("standalone", false), std::move(item)};
                placeBranches();
            } else finishTransfer(found->second);
            transfers.erase(found);
        }
    } else if (type == "session.created") {
        const auto& session = event.at("session");
        sessionId = session.at("id").get<std::string>();
        title = stringField(session, "title", "-");
    } else if (type == "mode.updated") mode = event.at("mode").get<std::string>();
    else if (type == "session.reset" || type == "provider.selected") {
        entries.clear(); branches.clear(); openBranch_.clear(); sessionId.clear(); title = "-"; mode = "sane"; page = {};
        workspace = launchWorkspace;
    } else if (type == "execution.preparing") status = "Preparing conversation...";
    else if (type == "execution.retry") {
        const auto retry = event.at("retry");
        status = "Connection problem | Retry " + std::to_string(retry.value("attempt", 0)) + "/" + std::to_string(retry.value("limit", 10)) + " | " + std::to_string(retry.value("seconds", 0)) + "s";
    } else if (type == "message.accepted") status = "Working...";
    else if (type == "recap.started" || type == "compaction.started") {
        busy = true; status = type == "recap.started" ? "Conversation Recap..." : "Compacting...";
    }
    if (event.contains("messages") && !page.newer) for (const auto& message : event["messages"]) append(message);
    const bool terminal = type == "execution.completed" || type == "execution.failed" || type == "execution.cancelled" ||
        type == "recap.completed" || type == "recap.cancelled" ||
        ((type == "compaction.completed" || type == "compaction.cancelled") && !event.value("automatic", false));
    if (terminal) {
        busy = false; status.clear();
        if (!pendingRequest.empty()) restore();
        if (type.ends_with("cancelled") || type.ends_with("failed")) restoreQueue();
    }
    if (type == "request.failed") {
        status = stringField(event, "error", "Request failed");
        if (event.value("requestId", "") == pendingRequest || event.value("requestId", "").starts_with("work-")) {
            restore(); restoreQueue(); busy = false;
        }
        loading = false;
    }
    if (type == "client.disconnected") {
        connected = false; busy = loading = false; restore(); restoreQueue(); transfers.clear();
    }
    if (!stringField(event, "error").empty()) status = stringField(event, "error");
    if (type == "permission.manual_updated") {
        for (auto& entry : entries) if (!entry.refusedId.empty() && event.at("states").contains(entry.refusedId))
            entry.approved = event.at("states").at(entry.refusedId).get<bool>();
    }
}
}
