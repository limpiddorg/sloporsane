#include "openai_oauth.hpp"
#include <cpr/cpr.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <thread>
#include <poll.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
using Json = nlohmann::json;
struct Socket {
    int fd = -1;
    ~Socket() { if (fd >= 0) ::close(fd); }
};

std::string base64(const unsigned char* bytes, std::size_t size) {
    std::string result(4 * ((size + 2) / 3) + 1, '\0');
    EVP_EncodeBlock(reinterpret_cast<unsigned char*>(result.data()), bytes, static_cast<int>(size));
    result.pop_back();
    std::replace(result.begin(), result.end(), '+', '-');
    std::replace(result.begin(), result.end(), '/', '_');
    while (!result.empty() && result.back() == '=') result.pop_back();
    return result;
}

std::string randomCode() {
    std::array<unsigned char, 32> bytes;
    if (RAND_bytes(bytes.data(), bytes.size()) != 1) throw std::runtime_error("Cannot generate OAuth randomness");
    return base64(bytes.data(), bytes.size());
}

std::string encode(const std::string& value) {
    static constexpr char HEX[] = "0123456789ABCDEF";
    std::string result;
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') result += c;
        else { result += '%'; result += HEX[c >> 4]; result += HEX[c & 15]; }
    }
    return result;
}

std::string decode(const std::string& value) {
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        throw std::runtime_error("Invalid OAuth callback");
    };
    std::string result;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%') {
            if (i + 2 >= value.size()) throw std::runtime_error("Invalid OAuth callback");
            result += static_cast<char>((hex(value[i + 1]) << 4) | hex(value[i + 2]));
            i += 2;
        } else result += value[i] == '+' ? ' ' : value[i];
    }
    return result;
}

std::string account(const std::string& token) {
    const auto first = token.find('.');
    if (first == std::string::npos) return {};
    auto payload = token.substr(first + 1, token.find('.', first + 1) - first - 1);
    if (payload.size() > 65536) return {};
    std::replace(payload.begin(), payload.end(), '-', '+');
    std::replace(payload.begin(), payload.end(), '_', '/');
    while (payload.size() % 4) payload += '=';
    std::string decoded(payload.size(), '\0');
    const int length = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(decoded.data()),
        reinterpret_cast<const unsigned char*>(payload.data()), payload.size());
    if (length < 0) return {};
    decoded.resize(length);
    while (!decoded.empty() && decoded.back() == '\0') decoded.pop_back();
    const auto claims = Json::parse(decoded, nullptr, false);
    if (!claims.is_object()) return {};
    if (claims.contains("chatgpt_account_id")) return claims.value("chatgpt_account_id", "");
    if (claims.contains("https://api.openai.com/auth"))
        return claims.at("https://api.openai.com/auth").value("chatgpt_account_id", "");
    if (claims.contains("organizations") && claims.at("organizations").is_array() && !claims.at("organizations").empty())
        return claims.at("organizations")[0].value("id", "");
    return {};
}

cpr::Response post(const std::string& url, const std::string& body, bool form, core::CancellationCheck cancelled) {
    core::checkCancellation(cancelled);
    auto response = cpr::Post(cpr::Url{url}, cpr::Body{body},
        cpr::Header{{"Content-Type", form ? "application/x-www-form-urlencoded" : "application/json"}, {"User-Agent", "sos"}},
        cpr::Timeout{30000}, cpr::ConnectTimeout{10000}, cpr::Redirect{false},
        cpr::ProgressCallback{[cancelled](auto, auto, auto, auto, intptr_t) { return !cancelled || !cancelled(); }});
    core::checkCancellation(cancelled);
    if (response.error.code != cpr::ErrorCode::OK) throw std::runtime_error("OpenAI authentication network error");
    return response;
}

Json body(const cpr::Response& response) {
    if (response.status_code < 200 || response.status_code >= 300)
        throw std::runtime_error("OpenAI authentication HTTP error " + std::to_string(response.status_code));
    auto json = Json::parse(response.text, nullptr, false);
    if (!json.is_object()) throw std::runtime_error("Invalid OpenAI authentication response");
    return json;
}

ProviderCredential credential(const Json& tokens, const ProviderCredential& previous = {}) {
    try {
        ProviderCredential result;
        result.kind = AuthKind::OAUTH;
        result.method = previous.method;
        result.access = tokens.at("access_token").get<std::string>();
        result.refresh = tokens.value("refresh_token", previous.refresh);
        const auto seconds = tokens.value("expires_in", 3600LL);
        if (seconds <= 0 || seconds > 31536000) throw std::runtime_error("Invalid token expiration");
        result.expires = oauthNow() + seconds * 1000;
        result.accountId = account(tokens.value("id_token", ""));
        if (result.accountId.empty()) result.accountId = account(result.access);
        if (result.accountId.empty()) result.accountId = previous.accountId;
        if (result.access.empty() || result.refresh.empty()) throw std::runtime_error("Missing tokens");
        return result;
    } catch (...) { throw std::runtime_error("Invalid OpenAI authentication tokens"); }
}

ProviderCredential exchange(const OpenAIOAuthSettings& settings, const std::string& code,
    const std::string& redirect, const std::string& verifier, core::CancellationCheck cancelled) {
    return credential(body(post(settings.issuer + "/oauth/token",
        "grant_type=authorization_code&client_id=" + encode(settings.clientId) + "&code=" + encode(code) +
        "&redirect_uri=" + encode(redirect) + "&code_verifier=" + encode(verifier), true, cancelled)));
}

void wait(std::chrono::steady_clock::time_point until, core::CancellationCheck cancelled) {
    while (std::chrono::steady_clock::now() < until) {
        core::checkCancellation(cancelled);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}
}

std::int64_t oauthNow() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

ProviderCredential refreshOpenAICredential(const ProviderCredential& previous,
    core::CancellationCheck cancelled, OpenAIOAuthSettings settings) {
    return credential(body(post(settings.issuer + "/oauth/token", "grant_type=refresh_token&client_id=" +
        encode(settings.clientId) + "&refresh_token=" + encode(previous.refresh), true, cancelled)), previous);
}

namespace {
struct DeviceAuthorization {
    std::string id;
    std::string code;
    std::chrono::seconds interval;
};
struct AuthorizationCode {
    std::string value;
    std::string verifier;
};
struct BrowserAuthorization {
    std::string verifier;
    std::string state;
    std::string redirect;
    std::string url;
};
struct AuthorizationCallback {
    std::string code;
    std::string state;
    bool denied = false;
};

DeviceAuthorization requestDeviceCode(const OpenAIOAuthSettings& settings, core::CancellationCheck check) {
    const auto device = body(post(settings.issuer + "/api/accounts/deviceauth/usercode",
        Json{{"client_id", settings.clientId}}.dump(), false, check));
    try {
        int seconds = 5;
        if (device.contains("interval")) seconds = device.at("interval").is_string()
            ? std::stoi(device.at("interval").get<std::string>()) : device.at("interval").get<int>();
        return {device.at("device_auth_id").get<std::string>(), device.at("user_code").get<std::string>(),
            std::chrono::seconds(std::max(seconds, 1)) + std::chrono::seconds(3)};
    } catch (...) { throw std::runtime_error("Invalid OpenAI device authorization response"); }
}

AuthorizationCode waitForDeviceCode(const OpenAIOAuthSettings& settings, const DeviceAuthorization& device,
    core::CancellationCheck check) {
    while (true) {
        const auto response = post(settings.issuer + "/api/accounts/deviceauth/token",
            Json{{"device_auth_id", device.id}, {"user_code", device.code}}.dump(), false, check);
        if (response.status_code == 200) {
            const auto data = body(response);
            try { return {data.at("authorization_code").get<std::string>(), data.at("code_verifier").get<std::string>()}; }
            catch (...) { throw std::runtime_error("Invalid OpenAI device token response"); }
        }
        if (response.status_code != 403 && response.status_code != 404) body(response);
        wait(std::chrono::steady_clock::now() + device.interval, check);
    }
}

void startDeviceLogin(ProviderAuthFlow& flow, const OpenAIOAuthSettings& settings, core::CancellationCheck check) {
    const auto device = requestDeviceCode(settings, check);
    flow.url = settings.issuer + "/codex/device";
    flow.instructions = "Enter code: " + device.code;
    flow.completion = std::async(std::launch::async, [settings, device, check] {
        const auto code = waitForDeviceCode(settings, device, check);
        return exchange(settings, code.value, settings.issuer + "/deviceauth/callback", code.verifier, check);
    });
}

std::shared_ptr<Socket> listenForCallback(unsigned short port) {
    auto socket = std::make_shared<Socket>();
    socket->fd = ::socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;
    if (socket->fd >= 0) ::setsockopt(socket->fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (socket->fd < 0 || ::bind(socket->fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(socket->fd, 4) != 0) throw std::runtime_error("Cannot listen on OAuth callback port; use device code or free port 1455");
    return socket;
}

BrowserAuthorization prepareBrowserAuthorization(const OpenAIOAuthSettings& settings) {
    BrowserAuthorization auth{randomCode(), randomCode(),
        "http://localhost:" + std::to_string(settings.callbackPort) + "/auth/callback", {}};
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest;
    SHA256(reinterpret_cast<const unsigned char*>(auth.verifier.data()), auth.verifier.size(), digest.data());
    auth.url = settings.issuer + "/oauth/authorize?response_type=code&client_id=" + encode(settings.clientId) +
        "&redirect_uri=" + encode(auth.redirect) + "&scope=openid%20profile%20email%20offline_access&code_challenge=" +
        base64(digest.data(), digest.size()) + "&code_challenge_method=S256&state=" + auth.state +
        "&id_token_add_organizations=true&codex_cli_simplified_flow=true&originator=sos";
    return auth;
}

std::string readCallbackRequest(const Socket& client, core::CancellationCheck check) {
    std::string request;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (request.find("\r\n\r\n") == std::string::npos && request.size() < 16384 && std::chrono::steady_clock::now() < deadline) {
        core::checkCancellation(check);
        pollfd readable{client.fd, POLLIN, 0};
        if (::poll(&readable, 1, 100) <= 0) continue;
        char bytes[2048];
        const auto count = ::recv(client.fd, bytes, sizeof(bytes), 0);
        if (count <= 0) break;
        request.append(bytes, count);
    }
    return request;
}

std::optional<AuthorizationCallback> parseCallback(const std::string& request) {
    constexpr std::string_view PREFIX = "GET /auth/callback?";
    if (!request.starts_with(PREFIX)) return std::nullopt;
    const auto end = request.find(' ', PREFIX.size());
    if (end == std::string::npos) return std::nullopt;
    const auto query = request.substr(PREFIX.size(), end - PREFIX.size());
    AuthorizationCallback callback;
    try {
        for (std::size_t start = 0; start < query.size();) {
            const auto next = query.find('&', start);
            const auto part = query.substr(start, next - start);
            const auto equal = part.find('=');
            if (equal != std::string::npos) {
                const auto name = decode(part.substr(0, equal)), value = decode(part.substr(equal + 1));
                if (name == "state") callback.state = value;
                if (name == "code") callback.code = value;
                if (name == "error") callback.denied = true;
            }
            if (next == std::string::npos) break;
            start = next + 1;
        }
    } catch (...) { return std::nullopt; }
    return callback;
}

void replyToCallback(const Socket& client, bool valid) {
    const std::string response = valid
        ? "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nAuthorization received. Return to SlopOrSane."
        : "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nInvalid authorization callback.";
#ifdef SO_NOSIGPIPE
    int noSignal = 1;
    ::setsockopt(client.fd, SOL_SOCKET, SO_NOSIGPIPE, &noSignal, sizeof(noSignal));
#endif
#ifdef MSG_NOSIGNAL
    ::send(client.fd, response.data(), response.size(), MSG_NOSIGNAL);
#else
    ::send(client.fd, response.data(), response.size(), 0);
#endif
}

std::string waitForBrowserCode(const Socket& socket, const std::string& expectedState, core::CancellationCheck check) {
    while (true) {
        core::checkCancellation(check);
        pollfd ready{socket.fd, POLLIN, 0};
        if (::poll(&ready, 1, 100) <= 0) continue;
        Socket client{::accept(socket.fd, nullptr, nullptr)};
        if (client.fd < 0) continue;
        const auto callback = parseCallback(readCallbackRequest(client, check));
        if (!callback) continue;
        const bool valid = callback->state == expectedState && !callback->code.empty();
        replyToCallback(client, valid);
        if (valid) return callback->code;
        if (callback->state == expectedState && callback->denied) throw std::runtime_error("OpenAI authorization was denied");
    }
}

void startBrowserLogin(ProviderAuthFlow& flow, const OpenAIOAuthSettings& settings, core::CancellationCheck check) {
    const auto socket = listenForCallback(settings.callbackPort);
    const auto auth = prepareBrowserAuthorization(settings);
    flow.url = auth.url;
    flow.instructions = "Complete authorization in your browser.";
    flow.completion = std::async(std::launch::async, [socket, settings, auth, check] {
        const auto code = waitForBrowserCode(*socket, auth.state, check);
        return exchange(settings, code, auth.redirect, auth.verifier, check);
    });
}
}

std::unique_ptr<ProviderAuthFlow> startOpenAIOAuth(const std::string& method,
    std::shared_ptr<std::atomic<bool>> cancelled, OpenAIOAuthSettings settings) {
    auto flow = std::make_unique<ProviderAuthFlow>();
    flow->cancelled = cancelled;
    core::CancellationCheck check = [cancelled, deadline = std::chrono::steady_clock::now() + settings.deadline] {
        return cancelled->load() || std::chrono::steady_clock::now() >= deadline;
    };
    if (method == "device") startDeviceLogin(*flow, settings, check);
    else if (method == "browser") startBrowserLogin(*flow, settings, check);
    else throw std::invalid_argument("Unknown OpenAI connection method");
    return flow;
}
