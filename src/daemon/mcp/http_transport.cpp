#include "http_transport.hpp"
#include "oauth_http.hpp"
#include <curl/curl.h>
#include <algorithm>
#include <cctype>
#include <functional>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <regex>
#include <unistd.h>
#include <fcntl.h>

namespace mcp {
namespace {
using Json = nlohmann::json;
constexpr std::size_t MAX_BYTES = 4 * 1024 * 1024;
constexpr std::size_t MAX_HEADERS = 64 * 1024;
constexpr std::size_t MAX_MESSAGES = 4096;

bool headerSafe(const std::string& value) {
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) { return ch >= 33 && ch <= 126; });
}
std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}
std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return std::tolower(ch); });
    return value;
}
std::string mediaType(const std::string& value) { return lower(trim(value.substr(0, value.find(';')))); }

// All curl callbacks catch exceptions: none may unwind through C frames.
struct Exchange {
    CURL* curl;
    std::atomic<bool>& closed;
    core::CancellationCheck cancelled;
    std::function<void(Json)> publish;
    std::function<void(const std::string&)> acceptSession;
    Json requestId;
    bool expectsResponse;
    bool complete = false;
    bool streaming = false;
    bool firstLine = true, afterCarriageReturn = false;
    std::size_t bytes = 0, headerBytes = 0;
    std::string body, line, event, session;
    std::exception_ptr failure;
    Json terminalResponse;
    std::vector<std::string> challenges;

    void emit(const std::string& data) {
        auto json = Json::parse(data, nullptr, false);
        if (!json.is_object()) throw std::runtime_error("Invalid MCP HTTP JSON");
        const bool response = !json.contains("method") && json.contains("id") && json.at("id") == requestId;
        if (response) {
            // Do not let receive() accept a terminal result before HTTP status,
            // framing and transfer completion have all been validated.
            terminalResponse = std::move(json);
            complete = true;
        } else publish(std::move(json));
    }

    void consumeLine() {
        if (firstLine && line.starts_with("\xef\xbb\xbf")) line.erase(0, 3);
        firstLine = false;
        if (line.empty()) {
            if (!event.empty()) {
                event.pop_back(); // Remove the last data-line newline.
                emit(event);
                event.clear();
            }
        } else if (line == "data" || line.starts_with("data:")) {
            auto data = line == "data" ? std::string{} : line.substr(5);
            if (data.starts_with(' ')) data.erase(0, 1);
            event += data + "\n";
        }
        line.clear();
    }

    static size_t header(char* data, size_t size, size_t count, void* context) noexcept {
        auto& self = *static_cast<Exchange*>(context);
        const auto length = size * count;
        try {
            self.headerBytes += length;
            if (self.headerBytes > MAX_HEADERS) throw std::runtime_error("MCP HTTP headers exceed limit");
            std::string line(data, length);
            if (line.starts_with("HTTP/")) { self.streaming = false; self.session.clear(); self.challenges.clear(); }
            if (trim(line).empty() && !self.session.empty()) {
                long status = 0;
                curl_easy_getinfo(self.curl, CURLINFO_RESPONSE_CODE, &status);
                if (status >= 200 && status < 300) self.acceptSession(self.session);
            }
            const auto colon = line.find(':');
            if (colon != std::string::npos) {
                const auto key = lower(line.substr(0, colon)), value = trim(line.substr(colon + 1));
                if (key == "content-type") self.streaming = mediaType(value) == "text/event-stream";
                if (key == "www-authenticate") self.challenges.push_back(value);
                if (key == "mcp-session-id") {
                    if (value.empty() || value.size() > 512 || !headerSafe(value))
                        throw std::runtime_error("Invalid MCP HTTP session identifier");
                    if (!self.session.empty() && self.session != value)
                        throw std::runtime_error("Conflicting MCP HTTP session identifiers");
                    self.session = value;
                }
            }
            return length;
        } catch (...) { self.failure = std::current_exception(); return 0; }
    }

    static size_t write(char* data, size_t size, size_t count, void* context) noexcept {
        auto& self = *static_cast<Exchange*>(context);
        const auto length = size * count;
        try {
            self.bytes += length;
            if (self.bytes > MAX_BYTES) throw std::runtime_error("MCP HTTP response exceeds 4 MiB");
            long status = 0;
            curl_easy_getinfo(self.curl, CURLINFO_RESPONSE_CODE, &status);
            if (status != 200) return length; // Never expose authentication/error bodies.
            if (!self.streaming) self.body.append(data, length);
            else for (std::size_t index = 0; index < length; ++index) {
                const char ch = data[index];
                if (ch == '\r') self.consumeLine();
                else if (ch == '\n') {
                    if (!self.afterCarriageReturn) self.consumeLine();
                } else self.line += ch;
                self.afterCarriageReturn = ch == '\r';
                // transfer() removes the paused handle; returning 0 would report a
                // write failure (CURLE_RECV_ERROR on some HTTP/2 curl versions).
                if (self.complete) return CURL_WRITEFUNC_PAUSE;
            }
            return length;
        } catch (...) { self.failure = std::current_exception(); return 0; }
    }

    static int progress(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
        auto& self = *static_cast<Exchange*>(context);
        try {
            core::checkCancellation(self.cancelled);
            return self.closed.load() ? 1 : 0;
        } catch (...) { self.failure = std::current_exception(); return 1; }
    }
};

// Polling keeps cancellation responsive even if the server sends no bytes.
// This loop owns registration; cleanup always removes the easy handle first.
CURLcode transfer(CURL* curl, Exchange& exchange, Deadline deadline) {
    std::unique_ptr<CURLM, decltype(&curl_multi_cleanup)> multi(curl_multi_init(), curl_multi_cleanup);
    if (!multi || curl_multi_add_handle(multi.get(), curl) != CURLM_OK)
        throw std::runtime_error("Cannot start MCP HTTP transfer");
    CURLcode result = CURLE_OK;
    try {
        int running = 0;
        do {
            core::checkCancellation(exchange.cancelled);
            if (exchange.closed) throw std::runtime_error("MCP HTTP connection was closed");
            if (std::chrono::steady_clock::now() >= deadline) throw TransportError("MCP HTTP request timed out");
            if (curl_multi_perform(multi.get(), &running) != CURLM_OK)
                throw std::runtime_error("MCP HTTP transfer failed");
            // A complete SSE result deliberately paused the write callback.
            // Do not poll a paused stream or wait for the server to close it.
            if (exchange.streaming && exchange.complete) break;
            if (running && curl_multi_poll(multi.get(), nullptr, 0, 25, nullptr) != CURLM_OK)
                throw std::runtime_error("MCP HTTP polling failed");
        } while (running);
        int remaining = 0;
        while (auto* info = curl_multi_info_read(multi.get(), &remaining))
            if (info->msg == CURLMSG_DONE) result = info->data.result;
        curl_multi_remove_handle(multi.get(), curl);
        return result;
    } catch (...) {
        curl_multi_remove_handle(multi.get(), curl);
        throw;
    }
}

void finish(Exchange& exchange, CURLcode result) {
    if (exchange.failure) std::rethrow_exception(exchange.failure);
    core::checkCancellation(exchange.cancelled);
    if (exchange.closed) throw std::runtime_error("MCP HTTP connection was closed");
    if (result != CURLE_OK)
        throw TransportError("MCP HTTP transport failed (curl " + std::to_string(result) + "): " + curl_easy_strerror(result));
    long status = 0;
    curl_easy_getinfo(exchange.curl, CURLINFO_RESPONSE_CODE, &status);
    if (status == 401 || status == 403) {
        const auto challenge = oauth::bearerChallenge(exchange.challenges);
        throw AuthenticationRequired({challenge.contains("resource_metadata") ? challenge.at("resource_metadata") : "",
            challenge.contains("scope") ? challenge.at("scope") : ""});
    }
    if (status == 404) throw TransportError("MCP endpoint or session is unavailable (HTTP 404); reconnect explicitly");
    if (status != (exchange.expectsResponse ? 200 : 202)) throw TransportError("Unexpected MCP HTTP status " + std::to_string(status));
    if (exchange.expectsResponse && !exchange.streaming) {
        char* type = nullptr;
        curl_easy_getinfo(exchange.curl, CURLINFO_CONTENT_TYPE, &type);
        if (!type || mediaType(type) != "application/json") throw std::runtime_error("Unsupported MCP HTTP content type");
        exchange.emit(exchange.body);
    }
    if (exchange.expectsResponse && !exchange.complete) throw std::runtime_error("MCP HTTP response ended without a matching result");
    if (exchange.complete) exchange.publish(std::move(exchange.terminalResponse));
}

// Opt-in diagnostics without curl's verbose, header or body traces.
// One private file per transfer avoids interleaving concurrent connections.
void logHttpDiagnostic(CURL* curl, CURLcode code, const char* detail, const Exchange& exchange,
                       const std::string& access, const std::string& session, const std::string& url) noexcept {
    try {
        const auto* enabled = std::getenv("SOS_MCP_HTTP_DEBUG");
        if (!enabled || std::string_view(enabled) != "1") return;
        std::string error(detail);
        for (const auto& secret : {access, session, url}) {
            if (secret.empty()) continue;
            std::size_t position = 0;
            while ((position = error.find(secret, position)) != std::string::npos) {
                error.replace(position, secret.size(), "[redacted]");
                position += 10;
            }
        }
        // libcurl may mention a proxy URL, including credentials. Remove whole URLs.
        error = std::regex_replace(error, std::regex(R"([A-Za-z][A-Za-z0-9+.-]*://[^\s]+)"), "[url redacted]");
        const auto number = [curl](CURLINFO key) {
            long value = 0;
            curl_easy_getinfo(curl, key, &value);
            return value;
        };
        const auto seconds = [curl](CURLINFO key) {
            double value = 0;
            curl_easy_getinfo(curl, key, &value);
            return value;
        };
        const Json record = {
            {"pid", getpid()}, {"curl_version", curl_version()}, {"curl_code", static_cast<int>(code)},
            {"detail", error}, {"http_status", number(CURLINFO_RESPONSE_CODE)},
            {"http_version", number(CURLINFO_HTTP_VERSION)}, {"os_errno", number(CURLINFO_OS_ERRNO)},
            {"ssl_verify_result", number(CURLINFO_SSL_VERIFYRESULT)},
            {"dns_seconds", seconds(CURLINFO_NAMELOOKUP_TIME)},
            {"connect_seconds", seconds(CURLINFO_CONNECT_TIME)},
            {"tls_seconds", seconds(CURLINFO_APPCONNECT_TIME)},
            {"first_byte_seconds", seconds(CURLINFO_STARTTRANSFER_TIME)},
            {"total_seconds", seconds(CURLINFO_TOTAL_TIME)},
            {"header_bytes", exchange.headerBytes}, {"body_bytes", exchange.bytes},
            {"auth_challenge_received", !exchange.challenges.empty()},
            {"bearer_sent", !access.empty()}, {"session_sent", !session.empty()},
            {"streaming", exchange.streaming}, {"complete", exchange.complete}
        };
        const auto bytes = record.dump() + "\n";
        auto path = (std::filesystem::temp_directory_path() / "sos-mcp-http-XXXXXX").string();
        const int descriptor = mkstemp(path.data()); // Exclusive creation, mode 0600.
        if (descriptor < 0) return;
        fcntl(descriptor, F_SETFD, FD_CLOEXEC);
        std::size_t written = 0;
        while (written < bytes.size()) {
            const auto count = write(descriptor, bytes.data() + written, bytes.size() - written);
            if (count <= 0) break;
            written += static_cast<std::size_t>(count);
        }
        close(descriptor);
        std::fprintf(stderr, "[mcp-http-debug] %s\n", path.c_str());
    } catch (...) {
        // Diagnostic failures must not change the connection's outcome.
    }
}
}

HttpTransport::HttpTransport(Remote remote, std::string bearer, Authorize authorize)
    : remote(std::move(remote)), bearer(std::move(bearer)), authorize(std::move(authorize)) {
    validate(this->remote);
    if (this->bearer.size() > 16384 || !headerSafe(this->bearer)) throw std::invalid_argument("Invalid MCP bearer token");
    static const auto initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (initialized != CURLE_OK) throw std::runtime_error("Cannot initialize MCP HTTP transport");
}
HttpTransport::~HttpTransport() { close(); }

void HttpTransport::setProtocolVersion(const std::string& version) {
    if (!supportedProtocolVersion(version)) throw std::invalid_argument("Unsupported MCP HTTP version");
    std::lock_guard lock(mutex);
    protocolVersion = version;
}

void HttpTransport::publish(Json message) {
    std::lock_guard lock(mutex);
    if (messages.size() >= MAX_MESSAGES) throw std::runtime_error("MCP HTTP message queue exceeds limit");
    messages.push_back(std::move(message));
    changed.notify_all();
}

void HttpTransport::send(const Json& message, Deadline deadline, core::CancellationCheck cancelled) {
    core::checkCancellation(cancelled);
    if (closed) throw std::runtime_error("MCP HTTP connection is closed");
    if (!message.contains("method") || !message.contains("id")) {
        post(message, deadline, cancelled); // Notifications and responses expect 202.
        return;
    }
    if (worker.joinable()) worker.join();
    {
        std::lock_guard lock(mutex);
        if (failure) std::rethrow_exception(failure);
        receiving = true;
    }
    worker = std::thread([this, message, deadline] {
        // Cancellation is checked by receive() on the caller's thread. close()
        // interrupts this transfer without invoking the callback concurrently.
        try { post(message, deadline, {}); }
        catch (...) {
            std::lock_guard lock(mutex);
            failure = std::current_exception();
        }
        std::lock_guard lock(mutex);
        receiving = false;
        changed.notify_all();
    });
}

Json HttpTransport::receive(Deadline deadline, core::CancellationCheck cancelled) {
    std::unique_lock lock(mutex);
    for (;;) {
        core::checkCancellation(cancelled);
        if (closed) throw std::runtime_error("MCP HTTP connection is closed");
        if (failure) std::rethrow_exception(failure);
        if (!messages.empty()) {
            auto result = std::move(messages.front());
            messages.pop_front();
            return result;
        }
        if (!receiving) throw std::runtime_error("MCP HTTP response ended without a result");
        if (std::chrono::steady_clock::now() >= deadline) throw TransportError("MCP HTTP request timed out");
        changed.wait_for(lock, std::chrono::milliseconds(25));
    }
}

void HttpTransport::post(const Json& message, Deadline deadline, core::CancellationCheck cancelled, bool terminating) {
    char error[CURL_ERROR_SIZE]{}; // Must outlive the easy handle, including cleanup.
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) throw std::runtime_error("Cannot create MCP HTTP request");
    const auto body = message.dump();
    if (body.size() > MAX_BYTES) throw std::runtime_error("MCP HTTP request exceeds 4 MiB");
    const bool expectsResponse = message.contains("id") && message.contains("method");
    Exchange exchange{curl.get(), closed, cancelled, [this](Json json) { publish(std::move(json)); },
        [this](const std::string& value) {
            std::lock_guard lock(mutex);
            if (!sessionId.empty() && sessionId != value) throw std::runtime_error("MCP HTTP session changed unexpectedly");
            sessionId = value;
        }, message.value("id", Json()), expectsResponse};
    std::string currentSession, currentVersion;
    {
        std::lock_guard lock(mutex);
        currentSession = sessionId;
        currentVersion = protocolVersion;
    }
    if (terminating && currentSession.empty()) return;
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(nullptr, curl_slist_free_all);
    const auto addHeader = [&](const std::string& value) {
        auto* next = curl_slist_append(headers.get(), value.c_str());
        if (!next) throw std::bad_alloc();
        headers.release(); // append retains the existing nodes.
        headers.reset(next);
    };
    addHeader("Content-Type: application/json");
    addHeader("Accept: application/json, text/event-stream");
    const auto access = authorize ? authorize([this, cancelled, deadline] {
        return closed.load() || std::chrono::steady_clock::now() >= deadline || (cancelled && cancelled());
    }) : bearer;
    if (access.size() > 16384 || !headerSafe(access)) throw std::runtime_error("Invalid MCP access token");
    if (!access.empty()) addHeader("Authorization: Bearer " + access);
    if (!currentSession.empty()) addHeader("Mcp-Session-Id: " + currentSession);
    if (!currentVersion.empty()) addHeader("MCP-Protocol-Version: " + currentVersion);
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    if (remaining <= 0) throw std::runtime_error("MCP HTTP request timed out");
    curl_easy_setopt(curl.get(), CURLOPT_URL, remote.url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_ERRORBUFFER, error);
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    if (terminating) {
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, nullptr);
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, 0L);
        curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, "DELETE");
    }
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, static_cast<long>(remaining));
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROXY, "localhost,127.0.0.1,::1");
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, &Exchange::write);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &exchange);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, &Exchange::header);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &exchange);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, &Exchange::progress);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &exchange);
    CURLcode code;
    try {
        code = transfer(curl.get(), exchange, deadline);
    } catch (...) {
        logHttpDiagnostic(curl.get(), CURLE_ABORTED_BY_CALLBACK, "Transfer interrupted before completion",
                          exchange, access, currentSession, remote.url);
        throw;
    }
    logHttpDiagnostic(curl.get(), code, error, exchange, access, currentSession, remote.url);
    if (!terminating) { finish(exchange, code); return; }
    if (exchange.failure) std::rethrow_exception(exchange.failure);
    core::checkCancellation(cancelled);
    if (code != CURLE_OK) throw std::runtime_error("MCP session termination failed");
    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    if (status == 401 || status == 403) throw AuthenticationRequired();
    if ((status < 200 || status >= 300) && status != 404 && status != 405)
        throw std::runtime_error("MCP server refused session termination");
}

void HttpTransport::terminate(Deadline deadline, core::CancellationCheck cancelled) {
    if (closed) return;
    if (worker.joinable()) worker.join();
    try { post(Json::object(), deadline, cancelled, true); }
    catch (...) { close(); throw; }
    close();
}

void HttpTransport::close() noexcept {
    closed = true;
    changed.notify_all();
    if (worker.joinable()) worker.join();
}
}
