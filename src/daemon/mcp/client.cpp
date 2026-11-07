#include "client.hpp"
#include <set>
#include <algorithm>
#include <utility>

namespace mcp {
namespace {
using Json = nlohmann::json;
constexpr std::size_t MAX_TOOLS = 512;
constexpr std::size_t MAX_MESSAGES = 4096;
Deadline after(int milliseconds) {
    if (milliseconds < 100 || milliseconds > 600000) throw std::invalid_argument("Invalid MCP timeout");
    return std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
}

}

Client::Client(std::unique_ptr<Transport> transport, int requestTimeoutMs)
    : transport(std::move(transport)), timeout(requestTimeoutMs) {
    if (!this->transport) throw std::invalid_argument("Missing MCP transport");
    (void)after(requestTimeoutMs);
}
Client::~Client() { transport->close(); }

void Client::close() {
    std::lock_guard lock(mutex);
    state = State::CLOSED;
    transport->close();
}

void Client::disconnect() {
    std::lock_guard lock(mutex);
    const auto ready = state == State::READY;
    state = State::CLOSED;
    try { if (ready) transport->terminate(std::chrono::steady_clock::now() + std::chrono::seconds(2), {}); }
    catch (...) { transport->close(); throw; }
    transport->close();
}

void Client::requireReady() const {
    if (state != State::READY) throw std::runtime_error("MCP server is not connected");
}

void Client::notify(const std::string& method, const Json& params, Deadline deadline, core::CancellationCheck cancelled) {
    transport->send({{"jsonrpc", "2.0"}, {"method", method}, {"params", params}}, deadline, cancelled);
}

Json Client::request(const std::string& method, const Json& params, Deadline deadline, core::CancellationCheck cancelled) {
    const auto id = ++nextId;
    try {
        core::checkCancellation(cancelled);
        transport->send({{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}}, deadline, cancelled);
        for (std::size_t count = 0; count < MAX_MESSAGES; ++count) {
            core::checkCancellation(cancelled);
            if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("MCP request timed out");
            auto message = transport->receive(deadline, cancelled);
            if (!message.is_object() || message.value("jsonrpc", "") != "2.0")
                throw std::runtime_error("Invalid MCP response envelope");
            if (message.contains("method")) {
                const auto operation = message.at("method").get<std::string>();
                if (message.contains("id")) {
                    Json response{{"jsonrpc", "2.0"}, {"id", message.at("id")}};
                    if (operation == "ping") response["result"] = Json::object();
                    else response["error"] = {{"code", -32601}, {"message", "Client capability not supported"}};
                    transport->send(response, deadline, cancelled);
                }
                if (!message.contains("id") && operation == "notifications/tools/list_changed") toolsChanged = true;
                // Resource/prompt catalogues are loaded at every explicit browse
                // and before preview. Notifications never read or inject content.
                continue;
            }
            if (!message.contains("id") || message.at("id") != id ||
                message.contains("error") == message.contains("result"))
                throw std::runtime_error("MCP response does not match the pending request");
            if (message.contains("error")) {
                const auto& error = message.at("error");
                if (!error.is_object() || !error.contains("code") || !error.at("code").is_number_integer() ||
                    error.at("code") < INT32_MIN || error.at("code") > INT32_MAX ||
                    !error.contains("message") || !error.at("message").is_string())
                    throw std::runtime_error("Invalid MCP error envelope");
                throw RpcError(error.at("code").get<int>()); // Never expose server-provided diagnostics.
            }
            if (!message.at("result").is_object()) throw std::runtime_error("Invalid MCP result");
            return message.at("result");
        }
        throw std::runtime_error("MCP notification limit exceeded");
    } catch (const RpcError&) { throw; }
    catch (...) {
        state = State::CLOSED;
        transport->close();
        throw;
    }
}

void Client::initialize(int startupTimeoutMs, core::CancellationCheck cancelled) {
    std::lock_guard lock(mutex);
    if (state != State::NEW) throw std::logic_error("MCP client cannot be initialized again");
    const auto deadline = after(startupTimeoutMs);
    try {
        const auto result = request("initialize", {{"protocolVersion", LATEST_PROTOCOL_VERSION}, {"capabilities", Json::object()},
            {"clientInfo", {{"name", "sos"}, {"version", "1"}}}}, deadline, cancelled);
        const auto version = result.at("protocolVersion").get<std::string>();
        if (!supportedProtocolVersion(version))
            throw std::runtime_error("Unsupported MCP protocol version");
        if (!result.at("capabilities").is_object() || !result.at("serverInfo").is_object())
            throw std::runtime_error("Invalid MCP initialization result");
        for (const auto* key : {"tools", "resources", "prompts"})
            if (result.at("capabilities").contains(key) && !result.at("capabilities").at(key).is_object())
                throw std::runtime_error("Invalid MCP server capability");
        hasTools = result.at("capabilities").contains("tools");
        hasResources = result.at("capabilities").contains("resources");
        hasPrompts = result.at("capabilities").contains("prompts");
        transport->setProtocolVersion(version);
        notify("notifications/initialized", Json::object(), deadline, cancelled);
        state = State::READY;
    } catch (...) {
        state = State::CLOSED;
        transport->close();
        throw;
    }
}

std::vector<ToolDefinition> Client::tools(core::CancellationCheck cancelled) {
    std::lock_guard lock(mutex);
    requireReady();
    toolsChanged = false;
    try { return loadTools(cancelled); }
    catch (const RpcError&) { throw; }
    catch (...) {
        state = State::CLOSED;
        transport->close();
        throw;
    }
}

bool Client::takeToolsChanged() {
    std::lock_guard lock(mutex);
    return std::exchange(toolsChanged, false);
}

std::vector<ToolDefinition> Client::loadTools(core::CancellationCheck cancelled) {
    std::vector<ToolDefinition> result;
    if (!hasTools) return result;
    std::set<std::string> cursors, names;
    Json params = Json::object();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (unsigned page = 0; page < 64; ++page) {
        const auto response = request("tools/list", params, deadline, cancelled);
        if (!response.at("tools").is_array()) throw std::runtime_error("Invalid MCP tool catalogue");
        for (const auto& tool : response.at("tools")) {
            ToolDefinition definition{tool.at("name"), tool.value("description", ""), tool.at("inputSchema"), false};
            if (definition.name.empty() || definition.name.size() > 128 || definition.description.size() > 65536 ||
                !definition.inputSchema.is_object() || definition.inputSchema.value("type", "") != "object" ||
                !names.insert(definition.name).second || result.size() == MAX_TOOLS)
                throw std::runtime_error("Invalid or oversized MCP tool catalogue");
            if (tool.contains("annotations") && tool.at("annotations").is_object()) {
                const auto& annotations = tool.at("annotations");
                definition.readOnly = annotations.contains("readOnlyHint") && annotations.at("readOnlyHint") == true;
            }
            result.push_back(std::move(definition));
        }
        if (!response.contains("nextCursor")) return result;
        const auto cursor = response.at("nextCursor").get<std::string>();
        if (cursor.empty() || cursor.size() > 16384 || !cursors.insert(cursor).second)
            throw std::runtime_error("Invalid MCP pagination cursor");
        params["cursor"] = cursor;
    }
    throw std::runtime_error("MCP catalogue page limit exceeded");
}

Json Client::call(const std::string& name, const Json& arguments, core::CancellationCheck cancelled) {
    std::lock_guard lock(mutex);
    requireReady();
    if (name.empty() || !arguments.is_object()) throw std::invalid_argument("Invalid MCP tool invocation");
    return request("tools/call", {{"name", name}, {"arguments", arguments}}, std::chrono::steady_clock::now() + timeout, cancelled);
}

std::vector<ContentEntry> Client::loadContent(ContentKind kind, Deadline deadline, core::CancellationCheck cancelled) {
    std::vector<ContentEntry> result;
    if (kind == ContentKind::PROMPT ? !hasPrompts : !hasResources) return result;
    const std::string method = kind == ContentKind::PROMPT ? "prompts/list" :
        kind == ContentKind::RESOURCE ? "resources/list" : "resources/templates/list";
    const char* field = kind == ContentKind::PROMPT ? "prompts" : kind == ContentKind::RESOURCE ? "resources" : "resourceTemplates";
    std::set<std::string> cursors, targets;
    Json params = Json::object();
    std::size_t bytes = 0;
    for (unsigned page = 0; page < 64; ++page) {
        Json response;
        try { response = request(method, params, deadline, cancelled); }
        catch (const RpcError& error) {
            if (page == 0 && error.code == -32601) return {}; // Optional catalogue method not implemented.
            throw;
        }
        bytes += response.dump().size();
        if (bytes > 4 * 1024 * 1024 || !response.at(field).is_array()) throw std::runtime_error("Invalid or oversized MCP content catalogue");
        for (const auto& item : response.at(field)) {
            auto entry = parseContentEntry(kind, item);
            if (result.size() == 512 || !targets.insert(entry.target).second) throw std::runtime_error("Invalid MCP content catalogue");
            result.push_back(std::move(entry));
        }
        if (!response.contains("nextCursor")) return result;
        const auto cursor = response.at("nextCursor").get<std::string>();
        if (cursor.empty() || cursor.size() > 16384 || !cursors.insert(cursor).second) throw std::runtime_error("Invalid MCP pagination cursor");
        params["cursor"] = cursor;
    }
    throw std::runtime_error("MCP catalogue page limit exceeded");
}

std::vector<ContentEntry> Client::catalogue(core::CancellationCheck cancelled) {
    std::lock_guard lock(mutex);
    requireReady();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::vector<ContentEntry> result;
    for (auto kind : {ContentKind::RESOURCE, ContentKind::TEMPLATE, ContentKind::PROMPT}) {
        auto entries = loadContent(kind, deadline, cancelled);
        result.insert(result.end(), entries.begin(), entries.end());
    }
    return result;
}

Json Client::preview(const ContentSelection& selection, core::CancellationCheck cancelled) {
    std::lock_guard lock(mutex);
    requireReady();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const auto entries = loadContent(selection.kind, deadline, cancelled);
    const auto entry = std::find_if(entries.begin(), entries.end(), [&](const auto& item) { return item.target == selection.target; });
    if (entry == entries.end()) throw std::invalid_argument("MCP content changed; refresh and select it again");
    if (selection.kind == ContentKind::PROMPT) {
        for (const auto& argument : entry->arguments)
            if (argument.required && !selection.arguments.contains(argument.name)) throw std::invalid_argument("Missing required MCP prompt argument");
        for (const auto& kv : selection.arguments) {
            const auto& argName = kv.first;
            const auto& value = kv.second;
            if (value.size() > 16384 || std::none_of(entry->arguments.begin(), entry->arguments.end(),
                [&](const auto& item) { return item.name == argName; })) throw std::invalid_argument("Invalid MCP prompt argument");
        }
        return previewContent(selection.kind, request("prompts/get", {{"name", selection.target}, {"arguments", selection.arguments}}, deadline, cancelled));
    }
    const auto uri = selection.kind == ContentKind::RESOURCE ? selection.target : selection.uri;
    if (uri.empty() || uri.size() > 16384 || uri.find_first_of("\r\n") != std::string::npos || uri.find('\0') != std::string::npos)
        throw std::invalid_argument("Invalid MCP resource URI");
    return previewContent(selection.kind, request("resources/read", {{"uri", uri}}, deadline, cancelled));
}
}
