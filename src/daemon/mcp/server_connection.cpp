#include "server_connection.hpp"
#include <algorithm>
#include <thread>

namespace mcp {
namespace {
bool needsCredential(const Server& server, const Credentials& credentials) {
    const auto* remote = std::get_if<Remote>(&server.connection);
    if (!remote || remote->authentication == Authentication::NONE) return false;
    if (supportsOAuth(remote->authentication) && !credentials.refreshToken.empty() && !credentials.tokenEndpoint.empty()) return false;
    if (credentials.accessToken.empty()) return remote->authentication != Authentication::AUTO;
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return supportsOAuth(remote->authentication) && credentials.expiresAt != 0 && credentials.expiresAt <= now;
}
}

const char* stateName(ConnectionState state) {
    switch (state) {
        case ConnectionState::NEEDS_TRUST: return "needs_trust";
        case ConnectionState::DISCONNECTED: return "disconnected";
        case ConnectionState::CONNECTING: return "connecting";
        case ConnectionState::CONNECTED: return "connected";
        case ConnectionState::NEEDS_AUTH: return "needs_auth";
        case ConnectionState::FAILED: return "failed";
    }
    throw std::logic_error("Unknown MCP connection state");
}

ServerConnection::ServerConnection(Server server, std::string identity, std::shared_ptr<std::atomic<bool>> attempted,
    TransportFactory factory, AuthStore auth)
    : status{std::move(server), std::move(identity)}, auth(std::move(auth)), factory(std::move(factory)), attempted(std::move(attempted)), enabled(status.server.enabled) {
    status.state = this->auth.trusted(status.identity) ? ConnectionState::DISCONNECTED : ConnectionState::NEEDS_TRUST;
}

ServerConnection::~ServerConnection() { if (client) client->close(); }

ServerStatus ServerConnection::snapshot() const {
    std::lock_guard lock(mutex);
    auto result = status;
    result.server.enabled = enabled.load();
    return result;
}

void ServerConnection::setEnabled(bool value) {
    std::lock_guard lock(mutex);
    if (!preferredEnabled) enabled.store(value);
}

void ServerConnection::setPreferredEnabled(bool value) {
    std::lock_guard lock(mutex);
    preferredEnabled = true;
    enabled.store(value);
}

void ServerConnection::wantConnection(bool value, bool retry) {
    std::lock_guard lock(mutex);
    if (retired) return;
    wanted = value;
    if (!value || retry) attempted->store(!value);
    if (!value) {
        ++revision;
        stopped->store(true);
        status.tools.clear();
        if (status.state != ConnectionState::NEEDS_TRUST) status.state = ConnectionState::DISCONNECTED;
    }
}

void ServerConnection::restore(core::CancellationCheck cancelled) {
    std::optional<bool> choice;
    { std::lock_guard lock(mutex); choice = wanted; }
    if (!choice) return;
    if (*choice) connect(true, std::move(cancelled));
    else {
        std::lock_guard administrationLock(administration);
        auto lock = acquire(cancelled);
        { std::lock_guard stateLock(mutex); if (wanted != false || retired) return; }
        if (client) {
            try { client->disconnect(); }
            catch (...) { closeClient(); throw; }
            closeClient();
        }
    }
}

std::unique_lock<std::mutex> ServerConnection::acquire(core::CancellationCheck cancelled) {
    std::unique_lock lock(operation, std::defer_lock);
    while (!lock.try_lock()) {
        core::checkCancellation(cancelled);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    core::checkCancellation(cancelled);
    return lock;
}

void ServerConnection::requireIdentity(const std::string& identity) const {
    if (retired || status.identity != identity)
        throw std::runtime_error("MCP configuration changed; review it before proceeding");
}

void ServerConnection::retire() {
    std::lock_guard lock(mutex);
    retired = true;
    ++revision;
    stopped->store(true);
    status.tools.clear();
    status.state = ConnectionState::DISCONNECTED;
}

void ServerConnection::closeClient() {
    client.reset();
    std::lock_guard lock(mutex);
    suspending = false;
    status.tools.clear();
    status.error.clear();
    status.state = status.state == ConnectionState::NEEDS_TRUST ? ConnectionState::NEEDS_TRUST : ConnectionState::DISCONNECTED;
}

std::unique_lock<std::mutex> ServerConnection::suspend(const std::optional<std::string>& identity, bool graceful) {
    {
        std::lock_guard lock(mutex);
        if (identity) requireIdentity(*identity); // Stale UI actions must not disconnect a replacement.
        ++revision;
        suspending = true;
        stopped->store(true);
    }
    auto lock = acquire({});
    if (graceful && client) {
        try { client->disconnect(); }
        catch (...) { closeClient(); throw; }
    }
    closeClient();
    return lock;
}

void ServerConnection::disconnect() {
    std::lock_guard administrationLock(administration);
    attempted->store(true);
    auto lock = suspend({}, true);
}

void ServerConnection::trust(const std::string& identity, bool accepted) {
    std::lock_guard administrationLock(administration);
    auto lock = suspend(identity);
    std::lock_guard stateLock(mutex);
    requireIdentity(identity);
    auth.trust(identity, accepted);
    status.state = accepted ? ConnectionState::DISCONNECTED : ConnectionState::NEEDS_TRUST;
}

void ServerConnection::authenticate(const std::string& identity, const Credentials& credentials) {
    std::lock_guard administrationLock(administration);
    auto lock = suspend(identity);
    std::lock_guard stateLock(mutex);
    requireIdentity(identity);
    if (!credentials.tokenEndpoint.empty() && !auth.trusted(identity))
        throw std::runtime_error("MCP approval was revoked during authorization");
    auth.set(identity, credentials);
}

void ServerConnection::failed(ConnectionState state, const std::string& error) {
    client.reset();
    std::lock_guard lock(mutex);
    status.tools.clear();
    status.state = stopped->load() ? ConnectionState::DISCONNECTED : state;
    status.error = stopped->load() ? "" : error;
}

void ServerConnection::start(core::CancellationCheck cancelled) {
    const auto credentials = auth.get(status.identity).value_or(Credentials{});
    if (needsCredential(status.server, credentials)) throw AuthenticationRequired();
    core::checkCancellation(cancelled);
    auto candidate = std::make_unique<Client>(factory(status.server, credentials), status.server.requestTimeoutMs);
    candidate->initialize(status.server.startupTimeoutMs, cancelled);
    auto tools = candidate->tools(cancelled);
    core::checkCancellation(cancelled);
    std::lock_guard lock(mutex);
    if (stopped->load()) throw core::Cancelled();
    client = std::move(candidate);
    status.tools = std::move(tools);
    status.state = ConnectionState::CONNECTED;
    status.authorization = {};
}

void ServerConnection::needsAuthentication(const AuthenticationRequired& error) {
    failed(ConnectionState::NEEDS_AUTH, "MCP credentials or additional authorization are required; use Connect to authorize");
    std::lock_guard lock(mutex);
    if (!stopped->load()) status.authorization = error.challenge;
}

void ServerConnection::connect(bool automatic, core::CancellationCheck cancelled) {
    std::uint64_t expectedRevision;
    {
        std::lock_guard lock(mutex);
        // A prepared or disabled server must not wait behind an unrelated tool call.
        if (wanted == false) return;
        if (automatic && (retired || suspending || (!enabled.load() && wanted != true) || attempted->load())) return;
        if (suspending) throw std::runtime_error("MCP connection is being stopped");
        expectedRevision = revision;
    }
    auto lock = acquire(cancelled);
    std::shared_ptr<std::atomic<bool>> token;
    {
        std::lock_guard stateLock(mutex);
        if (revision != expectedRevision || suspending) throw core::Cancelled();
        requireIdentity(status.identity);
        if (wanted == false || (automatic && !enabled.load() && wanted != true)) return;
        if (!auth.trusted(status.identity)) {
            status.state = ConnectionState::NEEDS_TRUST;
            if (automatic) return;
            throw std::runtime_error("Approve this MCP configuration before connecting");
        }
        if (automatic && attempted->exchange(true)) return;
        attempted->store(true);
        if (client && status.state == ConnectionState::CONNECTED && !stopped->load()) return;
        stopped = token = std::make_shared<std::atomic<bool>>(false);
        status.state = ConnectionState::CONNECTING;
        status.error.clear();
        status.tools.clear();
    }
    const auto check = [token, cancelled] { return token->load() || (cancelled && cancelled()); };
    try { start(check); }
    catch (const AuthenticationRequired& error) {
        needsAuthentication(error);
        throw;
    } catch (const TransportError& error) {
        failed(ConnectionState::FAILED, error.what());
        throw;
    } catch (...) {
        failed(ConnectionState::FAILED, "MCP connection failed; check the server settings and authentication");
        throw;
    }
}

void ServerConnection::refreshTools(core::CancellationCheck cancelled) {
    core::checkCancellation(cancelled);
    std::unique_lock lock(operation, std::try_to_lock);
    if (!lock.owns_lock()) return; // Never queue preparation behind another agent's tool call.
    {
        std::lock_guard stateLock(mutex);
        if (!enabled.load() || retired || suspending || stopped->load() || !client || status.state != ConnectionState::CONNECTED) return;
    }
    try {
        auto tools = client->tools([token = stopped, cancelled] { return token->load() || (cancelled && cancelled()); });
        std::lock_guard stateLock(mutex);
        if (!retired && !suspending && !stopped->load()) status.tools = std::move(tools);
    } catch (const AuthenticationRequired& error) { needsAuthentication(error); throw; }
    catch (...) { failed(ConnectionState::FAILED, "MCP catalogue refresh failed; reconnect explicitly"); throw; }
}

nlohmann::json ServerConnection::browse(const std::string& identity,
    const std::optional<ContentSelection>& selection, core::CancellationCheck cancelled) {
    auto lock = acquire(cancelled);
    std::shared_ptr<std::atomic<bool>> token;
    {
        std::lock_guard stateLock(mutex);
        requireIdentity(identity);
        if (suspending || stopped->load() || !auth.trusted(identity) || !client || status.state != ConnectionState::CONNECTED)
            throw std::runtime_error("Connect this approved MCP server before browsing");
        token = stopped;
    }
    const auto check = [token, cancelled] { return token->load() || (cancelled && cancelled()); };
    try {
        auto result = selection ? client->preview(*selection, check) : nlohmann::json{{"entries", client->catalogue(check)}};
        core::checkCancellation(check);
        std::lock_guard stateLock(mutex);
        requireIdentity(identity);
        return result;
    } catch (const std::invalid_argument&) { throw; }
    catch (const RpcError&) { throw; }
    catch (const AuthenticationRequired& error) { needsAuthentication(error); throw; }
    catch (...) { failed(ConnectionState::FAILED, "MCP content request failed; reconnect explicitly"); throw; }
}

nlohmann::json ServerConnection::call(const std::string& identity, const ToolDefinition& tool,
    const nlohmann::json& arguments, core::CancellationCheck cancelled) {
    auto lock = acquire(cancelled);
    std::shared_ptr<std::atomic<bool>> token;
    {
        std::lock_guard stateLock(mutex);
        requireIdentity(identity);
        if (!enabled.load() || suspending || stopped->load() || !auth.trusted(identity) || status.state != ConnectionState::CONNECTED || !client ||
            std::find(status.tools.begin(), status.tools.end(), tool) == status.tools.end())
            throw std::runtime_error("MCP configuration or tool catalogue changed; reconnect and retry explicitly");
        token = stopped;
    }
    try {
        const auto check = [token, cancelled] { return token->load() || (cancelled && cancelled()); };
        auto result = client->call(tool.name, arguments, check);
        if (client->takeToolsChanged()) {
            try {
                auto tools = client->tools(check);
                std::lock_guard stateLock(mutex);
                if (!retired && !suspending && !stopped->load()) status.tools = std::move(tools);
            } catch (const AuthenticationRequired& error) {
                needsAuthentication(error);
            } catch (...) {
                // Preserve a completed tool result even if subsequent discovery
                // fails. Never repeat the call to recover a catalogue.
                failed(ConnectionState::FAILED, "MCP tool completed, but catalogue refresh failed; reconnect explicitly");
            }
        }
        return result;
    } catch (const RpcError&) { throw; }
    catch (const AuthenticationRequired& error) {
        needsAuthentication(error);
        throw;
    } catch (...) {
        failed(ConnectionState::FAILED, "MCP call failed; its effects may have occurred. No automatic retry was attempted");
        throw;
    }
}
}
