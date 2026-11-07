#include "webfetch.hpp"
#include "html_format.hpp"
#include "output_capture.hpp"
#include "../fs/common.hpp"

#include <cpr/cpr.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <stdexcept>

namespace {
constexpr std::size_t MAX_RESPONSE_SIZE = 5 * 1024 * 1024;
const char* formatName(WebFetchFormat format) {
    switch (format) {
        case WebFetchFormat::TEXT: return "text";
        case WebFetchFormat::MARKDOWN: return "markdown";
        case WebFetchFormat::HTML: return "html";
    }
    throw std::invalid_argument("Unknown webfetch format");
}
}

void from_json(const nlohmann::json& json, WebFetchArguments& value) {
    json.at("url").get_to(value.url);
    html_format::validateUrl(value.url);
    const auto format = json.value("format", "markdown");
    if (format == "text") value.format = WebFetchFormat::TEXT;
    else if (format == "markdown") value.format = WebFetchFormat::MARKDOWN;
    else if (format == "html") value.format = WebFetchFormat::HTML;
    else throw std::invalid_argument("format must be text, markdown or html");
    if (json.contains("timeout") && !json.at("timeout").is_number()) throw std::invalid_argument("timeout must be numeric");
    value.timeout = json.value("timeout", 30.0);
    if (!std::isfinite(value.timeout) || value.timeout <= 0) throw std::invalid_argument("timeout must be positive");
    value.timeout = std::min(value.timeout, 120.0);
}

ToolDefinition WebFetchTool::getDefinition() const {
    return {"webfetch", "Fetch an HTTP(S) URL. Returns markdown by default, or text/html when requested. "
        "HTML is parsed to remove scripts and styles and convert readable content. Images are returned as attachments. "
        "Timeout is in seconds (default 30, maximum 120); response limit is 5 MiB. "
        "Follows up to 10 HTTP(S) redirects. Does not execute JavaScript or use browser cookies. "
        "Large text output is truncated and saved locally for further reading.",
        {{"type", "object"}, {"properties", {
            {"url", {{"type", "string"}, {"description", "Fully formed HTTP(S) URL"}}},
            {"format", {{"type", "string"}, {"enum", {"text", "markdown", "html"}}, {"default", "markdown"}}},
            {"timeout", {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 120}, {"description", "Timeout in seconds"}}}
        }}, {"required", {"url"}}}};
}

ToolResult WebFetchTool::execute(const nlohmann::json& arguments, const ToolContext& context) const {
    const auto params = arguments.get<WebFetchArguments>();
    if (!context.ask) throw std::invalid_argument("Tool context requires a permission handler");
    fs_tools::checkpoint(context);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<long>(params.timeout * 1000));
    std::string url = params.url;
    std::string accept;
    switch (params.format) {
        case WebFetchFormat::MARKDOWN: accept = "text/markdown;q=1.0, text/x-markdown;q=0.9, text/plain;q=0.8, text/html;q=0.7, */*;q=0.1"; break;
        case WebFetchFormat::TEXT: accept = "text/plain;q=1.0, text/markdown;q=0.9, text/html;q=0.8, */*;q=0.1"; break;
        case WebFetchFormat::HTML: accept = "text/html;q=1.0, application/xhtml+xml;q=0.9, text/plain;q=0.8, */*;q=0.1"; break;
    }
    for (int redirects = 0; redirects <= 10; ++redirects) {
        html_format::validateUrl(url);
        context.ask({"webfetch", {url}, {"*"}, {{"url", url}, {"format", formatName(params.format)}, {"timeout", params.timeout}}});
        fs_tools::checkpoint(context);
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) throw std::runtime_error("Web request timed out");
        cpr::Session client;
        client.SetUrl(cpr::Url{url});
        client.SetHeader(cpr::Header{{"Accept", accept}, {"Accept-Language", "en-US,en;q=0.9"}, {"User-Agent", "sos"}});
        client.SetRedirect(cpr::Redirect{false});
        client.SetTimeout(cpr::Timeout{static_cast<std::int32_t>(remaining)});
        client.SetConnectTimeout(cpr::ConnectTimeout{static_cast<std::int32_t>(std::min<std::int64_t>(remaining, 10000))});
        std::string body;
        std::exception_ptr callbackError;
        bool tooLarge = false;
        client.SetWriteCallback(cpr::WriteCallback{[&](std::string_view data, intptr_t) {
            try {
                fs_tools::checkpoint(context);
                if (data.size() > MAX_RESPONSE_SIZE - body.size()) { tooLarge = true; return false; }
                body.append(data);
                return true;
            } catch (...) { callbackError = std::current_exception(); return false; }
        }});
        client.SetProgressCallback(cpr::ProgressCallback{[&](cpr::cpr_off_t total, cpr::cpr_off_t, cpr::cpr_off_t, cpr::cpr_off_t, intptr_t) {
            try {
                fs_tools::checkpoint(context);
                if (total > static_cast<cpr::cpr_off_t>(MAX_RESPONSE_SIZE)) { tooLarge = true; return false; }
                return true;
            } catch (...) { callbackError = std::current_exception(); return false; }
        }});
        const auto response = client.Get();
        if (callbackError) std::rethrow_exception(callbackError);
        if (tooLarge) throw std::runtime_error("Response too large (exceeds 5 MiB limit)");
        if (response.error.code != cpr::ErrorCode::OK)
            throw std::runtime_error("Web request failed: " + response.error.message);
        if (response.status_code == 301 || response.status_code == 302 || response.status_code == 303 ||
            response.status_code == 307 || response.status_code == 308) {
            const auto location = response.header.find("location");
            if (location == response.header.end()) throw std::runtime_error("Redirect has no Location header");
            url = html_format::resolveUrl(location->second, url);
            continue;
        }
        if (response.status_code < 200 || response.status_code >= 300)
            throw std::runtime_error("Web request failed with HTTP status " + std::to_string(response.status_code));
        const auto contentType = response.header.find("content-type");
        auto mime = contentType == response.header.end() ? std::string{} : contentType->second.substr(0, contentType->second.find(';'));
        std::transform(mime.begin(), mime.end(), mime.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        mime = fs_tools::trim(mime);
        const auto title = url + " (" + mime + ")";
        if (mime == "image/png" || mime == "image/jpeg" || mime == "image/gif" || mime == "image/webp")
            return {title, "Image fetched successfully", {{"url", url}}, {{mime, "data:" + mime + ";base64," + tool_output::base64(body)}}};
        auto text = tool_output::utf8(body);
        if ((mime == "text/html" || mime == "application/xhtml+xml") && params.format != WebFetchFormat::HTML && !text.empty())
            text = html_format::convert(text, url, params.format == WebFetchFormat::MARKDOWN);
        OutputCapture capture(context.worktree / ".sos/tool-output", false);
        capture.append(text);
        auto output = capture.finish();
        nlohmann::json metadata{{"url", url}, {"truncated", capture.isTruncated()}};
        if (capture.isTruncated()) {
            metadata["outputPath"] = capture.getPath().string();
            output = "...output truncated...\n\nFull output saved to: " + capture.getPath().string() + "\n\n" + output;
        }
        return {title, output, metadata, {}};
    }
    throw std::runtime_error("Too many HTTP redirects");
}
