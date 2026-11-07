#include "oauth_credentials.hpp"
#include <thread>

namespace mcp {
namespace {
std::shared_ptr<std::mutex> identityMutex(const std::string& identity) {
    static std::mutex mutex;
    static std::map<std::string, std::weak_ptr<std::mutex>> locks;
    std::lock_guard lock(mutex);
    auto shared = locks[identity].lock();
    if (!shared) locks[identity] = shared = std::make_shared<std::mutex>();
    return shared;
}
}
OAuthCredentials::OAuthCredentials(AuthStore store, std::string identity)
    : store(std::move(store)), identity(std::move(identity)), mutex(identityMutex(this->identity)) {}

std::unique_lock<std::mutex> OAuthCredentials::acquire(core::CancellationCheck cancelled) {
    std::unique_lock lock(*mutex, std::defer_lock);
    while (!lock.try_lock()) {
        core::checkCancellation(cancelled);
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    core::checkCancellation(cancelled);
    if (!store.trusted(identity)) throw AuthenticationRequired();
    return lock;
}

std::string OAuthCredentials::bearer(core::CancellationCheck cancelled) {
    auto lock = acquire(cancelled);
    const auto previous = store.get(identity);
    if (!previous) throw AuthenticationRequired();
    auto current = *previous;
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (current.accessToken.empty() || (current.expiresAt != 0 && current.expiresAt <= now + 30)) {
        current = refreshOAuth(current, cancelled);
        core::checkCancellation(cancelled);
        store.replaceIfCurrent(identity, *previous, current);
    }
    return current.accessToken;
}

void OAuthCredentials::revoke(core::CancellationCheck cancelled) {
    auto lock = acquire(cancelled);
    const auto previous = store.get(identity);
    if (!previous) throw AuthenticationRequired();
    revokeOAuth(*previous, cancelled);
    auto next = *previous;
    next.accessToken.clear(); next.refreshToken.clear(); next.expiresAt = 0;
    store.replaceIfCurrent(identity, *previous, next);
}
}
