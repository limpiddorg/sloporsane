#include "oauth_http.hpp"
#include "config.hpp"
#include <curl/curl.h>
#include <memory>
#include <algorithm>
#include <openssl/evp.h>

namespace mcp::oauth {
namespace {
std::string part(const std::string& value, CURLUPart key) {
    std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(curl_url(), curl_url_cleanup);
    if (!url || curl_url_set(url.get(), CURLUPART_URL, value.c_str(), 0) != CURLUE_OK)
        throw std::runtime_error("Invalid OAuth URL");
    char* raw = nullptr;
    if (curl_url_get(url.get(), key, &raw, 0) != CURLUE_OK) return {};
    std::unique_ptr<char, decltype(&curl_free)> memory(raw, curl_free);
    return raw;
}
struct Response {
    std::string bytes;
    core::CancellationCheck cancelled;
    std::exception_ptr failure;
    std::size_t headerBytes = 0;
    std::vector<std::string> challenges;
    static size_t header(char* bytes, size_t size, size_t count, void* raw) noexcept {
        auto& self = *static_cast<Response*>(raw);
        try {
            self.headerBytes += size * count;
            if (self.headerBytes > 65536) return 0;
            std::string line(bytes, size * count);
            const auto colon = line.find(':');
            auto name = line.substr(0, colon);
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
            if (name == "www-authenticate") self.challenges.push_back(line.substr(colon + 1));
            return size * count;
        } catch (...) { self.failure = std::current_exception(); return 0; }
    }
    static size_t write(char* bytes, size_t size, size_t count, void* raw) noexcept {
        auto& self = *static_cast<Response*>(raw);
        try {
            if (self.bytes.size() + size * count > 1024 * 1024) throw std::runtime_error("OAuth response exceeds limit");
            self.bytes.append(bytes, size * count);
            return size * count;
        } catch (...) { self.failure = std::current_exception(); return 0; }
    }
    static int progress(void* raw, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
        auto& self = *static_cast<Response*>(raw);
        try { core::checkCancellation(self.cancelled); return 0; }
        catch (...) { self.failure = std::current_exception(); return 1; }
    }
};
}
void validateEndpoint(const std::string& url) { validate(Remote{url}); }
std::string origin(const std::string& url) {
    validateEndpoint(url);
    const auto port = part(url, CURLUPART_PORT);
    return part(url, CURLUPART_SCHEME) + "://" + part(url, CURLUPART_HOST) + (port.empty() ? "" : ":" + port);
}
std::string path(const std::string& url) { return part(url, CURLUPART_PATH); }
std::string encode(const std::string& value) {
    constexpr char HEX[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char ch : value) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~') result += ch;
        else { result += '%'; result += HEX[ch >> 4]; result += HEX[ch & 15]; }
    }
    return result;
}
std::string form(const std::map<std::string, std::string>& values) {
    std::string result;
    for (const auto& [key, value] : values) {
        if (!result.empty()) result += '&';
        result += encode(key) + "=" + encode(value);
    }
    return result;
}
HttpResult request(const std::string& url, const std::string& body, const std::string& contentType,
    core::CancellationCheck cancelled, HttpOptions options) {
    validateEndpoint(url);
    core::checkCancellation(cancelled);
    static const auto initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (initialized != CURLE_OK) throw std::runtime_error("Cannot initialize OAuth transport");
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) throw std::runtime_error("Cannot create OAuth request");
    Response response{{}, cancelled, {}};
    curl_slist* raw = curl_slist_append(nullptr, options.acceptEvents ? "Accept: application/json, text/event-stream" : "Accept: application/json");
    if (!raw) throw std::bad_alloc();
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(raw, curl_slist_free_all);
    if (!options.authorization.empty()) {
        if (options.authorization.size() > 65536 || options.authorization.find_first_of("\r\n") != std::string::npos || options.authorization.find('\0') != std::string::npos)
            throw std::invalid_argument("Invalid OAuth authorization header");
        auto* next = curl_slist_append(headers.get(), ("Authorization: " + options.authorization).c_str());
        if (!next) throw std::bad_alloc();
        headers.release(); headers.reset(next);
    }
    if (!contentType.empty()) {
        auto* next = curl_slist_append(headers.get(), ("Content-Type: " + contentType).c_str());
        if (!next) throw std::bad_alloc();
        headers.release(); headers.reset(next);
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROXY, "localhost,127.0.0.1,::1");
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, 30000L);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, &Response::write);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, &Response::header);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &response);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, &Response::progress);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &response);
    const auto code = curl_easy_perform(curl.get());
    core::checkCancellation(cancelled);
    if (response.failure) std::rethrow_exception(response.failure);
    if (code != CURLE_OK) throw TransportError("MCP OAuth network request failed (curl " + std::to_string(code) + "): " + curl_easy_strerror(code));
    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    auto json = nlohmann::json::parse(response.bytes, nullptr, false);
    if (!options.allowNonJson && status >= 200 && status < 300 && !json.is_object()) throw std::runtime_error("Invalid MCP OAuth response");
    return {status, json.is_object() ? std::move(json) : nlohmann::json::object(), std::move(response.challenges)};
}

std::string basicAuthorization(const std::string& id, const std::string& secret) {
    const auto input = encode(id) + ":" + encode(secret);
    std::string output(4 * ((input.size() + 2) / 3) + 1, '\0');
    EVP_EncodeBlock(reinterpret_cast<unsigned char*>(output.data()), reinterpret_cast<const unsigned char*>(input.data()), input.size());
    output.pop_back();
    return "Basic " + output;
}

std::map<std::string, std::string> bearerChallenge(const std::vector<std::string>& headers) {
    std::map<std::string, std::string> result;
    for (const auto& header : headers) {
        std::size_t offset = 0;
        bool bearer = false;
        const auto skip = [&] { while (offset < header.size() && std::isspace(static_cast<unsigned char>(header[offset]))) ++offset; };
        const auto token = [&] {
            const auto start = offset;
            while (offset < header.size() && (std::isalnum(static_cast<unsigned char>(header[offset])) ||
                std::string_view("!#$%&'*+-.^_`|~").find(header[offset]) != std::string_view::npos)) ++offset;
            return header.substr(start, offset - start);
        };
        while (offset < header.size()) {
            skip();
            if (offset == header.size()) break;
            if (header[offset] == ',') { ++offset; continue; }
            auto name = token();
            if (name.empty()) throw std::runtime_error("Invalid OAuth challenge");
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
            skip();
            if (offset == header.size() || header[offset] != '=') { bearer = name == "bearer"; continue; }
            ++offset; skip();
            std::string value;
            if (offset < header.size() && header[offset] == '"') {
                ++offset;
                while (offset < header.size() && header[offset] != '"') {
                    if (header[offset] == '\\') ++offset;
                    if (offset == header.size()) throw std::runtime_error("Invalid OAuth challenge");
                    value += header[offset++];
                }
                if (offset == header.size()) throw std::runtime_error("Invalid OAuth challenge");
                ++offset;
            } else value = token();
            if (bearer && (name == "resource_metadata" || name == "scope")) {
                if (result.contains(name) && result.at(name) != value) throw std::runtime_error("Conflicting OAuth challenges");
                result[name] = value;
            }
        }
    }
    return result;
}
}
