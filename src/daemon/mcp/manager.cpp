#include "manager.hpp"
#include "http_transport.hpp"
#include "stdio_transport.hpp"
#include "oauth_credentials.hpp"

namespace mcp {
Manager::Manager(std::filesystem::path workspace, TransportFactory factory, std::shared_ptr<Preferences> preferences)
    : config(workspace), factory(std::move(factory)), preferences(preferences ? std::move(preferences) : std::make_shared<Preferences>()) {
    if (!this->factory) this->factory = [workspace = std::filesystem::canonical(workspace), config = config, auth = auth](const Server& server, const Credentials& credentials) -> std::unique_ptr<Transport> {
        if (const auto* local = std::get_if<Local>(&server.connection))
            return std::make_unique<StdioTransport>(*local, workspace, credentials);
        const auto& remote = std::get<Remote>(server.connection);
        if (remote.authentication == Authentication::OAUTH ||
            (remote.authentication == Authentication::AUTO && !credentials.accessToken.empty())) {
            auto provider = std::make_shared<OAuthCredentials>(auth, config.identity(server));
            return std::make_unique<HttpTransport>(remote, "", [provider](core::CancellationCheck cancelled) {
                return provider->bearer(cancelled);
            });
        }
        return std::make_unique<HttpTransport>(remote, remote.authentication == Authentication::NONE ? "" : credentials.accessToken);
    };
}

Manager::~Manager() { cancelAll(); }

void Manager::cancelAll() noexcept {
    std::lock_guard lock(mutex);
    shuttingDown = true;
    for (const auto& [name, entry] : entries) entry->retire();
}

std::vector<ConfigIssue> Manager::refresh() {
    // Destroy transports after unlocking: destruction can wait for workers/processes.
    std::vector<std::shared_ptr<ServerConnection>> removed;
    std::lock_guard lock(mutex);
    if (shuttingDown) throw core::Cancelled();
    const auto loaded = config.load();
    for (auto iterator = entries.begin(); iterator != entries.end();) {
        const auto found = loaded.servers.find(iterator->first);
        const auto status = iterator->second->snapshot();
        auto previous = status.server;
        if (found != loaded.servers.end()) previous.enabled = found->second.enabled;
        if (found == loaded.servers.end() || config.identity(found->second) != status.identity ||
            nlohmann::json(found->second) != nlohmann::json(previous)) {
            iterator->second->retire();
            removed.push_back(iterator->second);
            iterator = entries.erase(iterator);
        } else {
            iterator->second->setEnabled(found->second.enabled);
            ++iterator;
        }
    }
    for (const auto& [name, server] : loaded.servers) {
        if (entries.contains(name)) continue;
        const auto identity = config.identity(server);
        auto& attempted = attempts[identity];
        if (!attempted) attempted = std::make_shared<std::atomic<bool>>(false);
        auto entry = std::make_shared<ServerConnection>(server, identity, attempted, factory, auth);
        preferences->attach(entry);
        entries.emplace(name, std::move(entry));
    }
    return loaded.issues;
}

std::shared_ptr<ServerConnection> Manager::find(const std::string& name) {
    refresh();
    std::lock_guard lock(mutex);
    const auto found = entries.find(name);
    if (found == entries.end()) throw std::runtime_error("MCP server is not configured or its configuration is invalid");
    return found->second;
}

ServerList Manager::list() {
    ServerList result;
    result.issues = refresh();
    std::lock_guard lock(mutex);
    for (const auto& [name, entry] : entries) result.servers.push_back(entry->snapshot());
    return result;
}

void Manager::save(const Server& server, const std::optional<Credentials>& credentials) {
    validate(server);
    config.save(server);
    refresh();
    if (!credentials) return;
    const auto identity = config.identity(server);
    // A saved global entry can be shadowed by the workspace. Store its secrets
    // without touching the unrelated effective connection.
    std::shared_ptr<ServerConnection> effective;
    {
        std::lock_guard lock(mutex);
        const auto found = entries.find(server.name);
        if (found != entries.end()) effective = found->second;
    }
    if (effective && effective->snapshot().identity == identity) effective->authenticate(identity, *credentials);
    else auth.set(identity, *credentials);
}

void Manager::remove(const std::string& name) {
    const auto entry = find(name);
    const auto status = entry->snapshot();
    entry->retire();
    try {
        config.remove(status.server.scope, name);
        auth.forget(status.identity);
    } catch (...) {
        // Evict the retired object so unchanged disk configuration can be reloaded.
        std::lock_guard lock(mutex);
        const auto found = entries.find(name);
        if (found != entries.end() && found->second == entry) entries.erase(found);
        throw;
    }
    refresh();
}

void Manager::enable(const std::string& name, bool enabled) {
    find(name);
    preferences->enable(name, enabled);
}

void Manager::trust(const std::string& name, const std::string& identity, bool accepted) {
    find(name)->trust(identity, accepted);
}

void Manager::authenticate(const std::string& name, const std::string& identity, const Credentials& credentials) {
    find(name)->authenticate(identity, credentials);
}

void Manager::disconnect(const std::string& name) {
    auto entry = find(name);
    preferences->connection(name, false, entry->snapshot().server.enabled);
    entry->restore({});
}

void Manager::connect(const std::string& name, core::CancellationCheck cancelled) {
    auto entry = find(name);
    // An unapproved explicit attempt must not publish a global connect choice.
    if (!auth.trusted(entry->snapshot().identity)) throw std::runtime_error("Approve this MCP configuration before connecting");
    preferences->connection(name, true, entry->snapshot().server.enabled);
    entry->connect(false, std::move(cancelled));
}

void Manager::restore(core::CancellationCheck cancelled) {
    refresh();
    std::vector<std::shared_ptr<ServerConnection>> pending;
    { std::lock_guard lock(mutex); for (const auto& [name, entry] : entries) pending.push_back(entry); }
    for (const auto& entry : pending) {
        core::checkCancellation(cancelled);
        try { entry->restore(cancelled); }
        catch (const core::Cancelled&) { core::checkCancellation(cancelled); }
        catch (const std::exception&) { /* The server retains its own connection diagnostic. */ }
    }
}

void Manager::prepare(core::CancellationCheck cancelled) {
    core::checkCancellation(cancelled);
    refresh();
    std::vector<std::shared_ptr<ServerConnection>> pending;
    {
        std::lock_guard lock(mutex);
        for (const auto& [name, entry] : entries) pending.push_back(entry);
    }
    for (const auto& entry : pending) {
        core::checkCancellation(cancelled);
        try {
            const auto connected = entry->snapshot().state == ConnectionState::CONNECTED;
            entry->connect(true, cancelled);
            if (connected) entry->refreshTools(cancelled);
        }
        catch (const core::Cancelled&) { core::checkCancellation(cancelled); }
        catch (const std::exception&) {
            // The connection retains its diagnostic. Other servers remain usable.
        }
    }
    core::checkCancellation(cancelled);
}

nlohmann::json Manager::call(const std::string& name, const std::string& identity, const ToolDefinition& tool,
    const nlohmann::json& arguments, core::CancellationCheck cancelled) {
    return find(name)->call(identity, tool, arguments, std::move(cancelled));
}

nlohmann::json Manager::browse(const std::string& name, const std::string& identity,
    const std::optional<ContentSelection>& selection, core::CancellationCheck cancelled) {
    return find(name)->browse(identity, selection, std::move(cancelled));
}
}
