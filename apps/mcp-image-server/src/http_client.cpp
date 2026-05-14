#include "http_client.hpp"

#include "httplib.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#endif

namespace mcdk::image {

namespace {

struct ParsedUrl {
    std::string scheme;
    std::string host;
    int port = -1;
    std::string path;
};

ParsedUrl parse_url(const std::string& url) {
    const auto scheme_pos = url.find("://");
    if (scheme_pos == std::string::npos) {
        throw std::runtime_error("invalid url: missing scheme");
    }

    ParsedUrl parsed;
    parsed.scheme = url.substr(0, scheme_pos);
    const auto authority_begin = scheme_pos + 3;
    const auto path_begin = url.find('/', authority_begin);
    std::string authority = path_begin == std::string::npos
        ? url.substr(authority_begin)
        : url.substr(authority_begin, path_begin - authority_begin);
    parsed.path = path_begin == std::string::npos ? "/" : url.substr(path_begin);

    const auto colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(']') == std::string::npos) {
        parsed.host = authority.substr(0, colon);
        parsed.port = std::stoi(authority.substr(colon + 1));
    } else {
        parsed.host = authority;
        parsed.port = parsed.scheme == "https" ? 443 : 80;
    }

    if (parsed.host.empty()) {
        throw std::runtime_error("invalid url: missing host");
    }
    return parsed;
}

httplib::Headers to_httplib_headers(const std::map<std::string, std::string>& headers) {
    httplib::Headers result;
    for (const auto& [key, value] : headers) {
        result.emplace(key, value);
    }
    return result;
}

HttpResponse from_result(const httplib::Result& result) {
    HttpResponse response;
    if (!result) {
        response.error = httplib::to_string(result.error());
        return response;
    }
    response.status = result->status;
    response.body = result->body;
    return response;
}

#ifdef _WIN32

struct WinHttpHandle {
    HINTERNET handle = nullptr;

    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET value) : handle(value) {}
    ~WinHttpHandle() {
        if (handle) {
            WinHttpCloseHandle(handle);
        }
    }

    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;

    WinHttpHandle(WinHttpHandle&& other) noexcept : handle(other.handle) {
        other.handle = nullptr;
    }

    WinHttpHandle& operator=(WinHttpHandle&& other) noexcept {
        if (this != &other) {
            if (handle) {
                WinHttpCloseHandle(handle);
            }
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }

    explicit operator bool() const { return handle != nullptr; }
};

std::wstring utf8_to_wide(const std::string& value) {
    if (value.empty()) {
        return {};
    }

    const int required = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) {
        return std::wstring(value.begin(), value.end());
    }

    std::wstring wide(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), wide.data(), required);
    return wide;
}

std::string win32_error_message(DWORD code) {
    return "WinHTTP error " + std::to_string(code);
}

std::wstring build_header_block(const std::map<std::string, std::string>& headers, const std::string& content_type) {
    std::string block;
    if (!content_type.empty()) {
        block += "Content-Type: " + content_type + "\r\n";
    }
    for (const auto& [key, value] : headers) {
        if (!content_type.empty() && _stricmp(key.c_str(), "Content-Type") == 0) {
            continue;
        }
        block += key + ": " + value + "\r\n";
    }
    return utf8_to_wide(block);
}

HttpResponse winhttp_request(
    const ParsedUrl& parsed,
    const std::string& method,
    const std::map<std::string, std::string>& headers,
    const std::string& body,
    const std::string& content_type,
    int timeout_seconds
) {
    HttpResponse response;

    WinHttpHandle session(WinHttpOpen(
        L"mcdk-image/0.1 WinHTTP",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    ));
    if (!session) {
        response.error = win32_error_message(GetLastError());
        return response;
    }

    const int timeout_ms = std::max(1, timeout_seconds) * 1000;
    WinHttpSetTimeouts(session.handle, 30000, 30000, timeout_ms, timeout_ms);

    WinHttpHandle connection(WinHttpConnect(
        session.handle,
        utf8_to_wide(parsed.host).c_str(),
        static_cast<INTERNET_PORT>(parsed.port),
        0
    ));
    if (!connection) {
        response.error = win32_error_message(GetLastError());
        return response;
    }

    const DWORD flags = parsed.scheme == "https" ? WINHTTP_FLAG_SECURE : 0;
    WinHttpHandle request(WinHttpOpenRequest(
        connection.handle,
        utf8_to_wide(method).c_str(),
        utf8_to_wide(parsed.path).c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        flags
    ));
    if (!request) {
        response.error = win32_error_message(GetLastError());
        return response;
    }

    const std::wstring header_block = build_header_block(headers, content_type);
    const void* body_ptr = body.empty() ? WINHTTP_NO_REQUEST_DATA : static_cast<const void*>(body.data());
    const DWORD body_size = static_cast<DWORD>(body.size());

    if (!WinHttpSendRequest(
            request.handle,
            header_block.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : header_block.c_str(),
            header_block.empty() ? 0 : static_cast<DWORD>(header_block.size()),
            const_cast<void*>(body_ptr),
            body_size,
            body_size,
            0
        )) {
        response.error = win32_error_message(GetLastError());
        return response;
    }

    if (!WinHttpReceiveResponse(request.handle, nullptr)) {
        response.error = win32_error_message(GetLastError());
        return response;
    }

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (WinHttpQueryHeaders(
            request.handle,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status,
            &status_size,
            WINHTTP_NO_HEADER_INDEX
        )) {
        response.status = static_cast<int>(status);
    }

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.handle, &available)) {
            response.error = win32_error_message(GetLastError());
            return response;
        }
        if (available == 0) {
            break;
        }

        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request.handle, chunk.data(), available, &read)) {
            response.error = win32_error_message(GetLastError());
            return response;
        }
        chunk.resize(read);
        response.body += chunk;
    }

    return response;
}

#endif // _WIN32

} // namespace

HttpClient::HttpClient(int timeout_seconds)
    : timeout_seconds_(timeout_seconds > 0 ? timeout_seconds : 400) {}

HttpResponse HttpClient::post_json(
    const std::string& url,
    const std::map<std::string, std::string>& headers,
    const std::string& body
) const {
    const ParsedUrl parsed = parse_url(url);
    const auto http_headers = to_httplib_headers(headers);

    if (parsed.scheme == "http") {
        httplib::Client client(parsed.host, parsed.port);
        client.set_connection_timeout(30, 0);
        client.set_read_timeout(timeout_seconds_, 0);
        client.set_write_timeout(timeout_seconds_, 0);
        return from_result(client.Post(parsed.path, http_headers, body, "application/json"));
    }

    if (parsed.scheme == "https") {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        httplib::SSLClient client(parsed.host, parsed.port);
        client.set_connection_timeout(30, 0);
        client.set_read_timeout(timeout_seconds_, 0);
        client.set_write_timeout(timeout_seconds_, 0);
        return from_result(client.Post(parsed.path, http_headers, body, "application/json"));
#elif defined(_WIN32)
        return winhttp_request(parsed, "POST", headers, body, "application/json", timeout_seconds_);
#else
        return HttpResponse{0, {}, "HTTPS requires MCP_SSL=ON / CPPHTTPLIB_OPENSSL_SUPPORT on this platform"};
#endif
    }

    return HttpResponse{0, {}, "unsupported URL scheme: " + parsed.scheme};
}

HttpResponse HttpClient::get(
    const std::string& url,
    const std::map<std::string, std::string>& headers
) const {
    const ParsedUrl parsed = parse_url(url);
    const auto http_headers = to_httplib_headers(headers);

    if (parsed.scheme == "http") {
        httplib::Client client(parsed.host, parsed.port);
        client.set_connection_timeout(30, 0);
        client.set_read_timeout(timeout_seconds_, 0);
        return from_result(client.Get(parsed.path, http_headers));
    }

    if (parsed.scheme == "https") {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        httplib::SSLClient client(parsed.host, parsed.port);
        client.set_connection_timeout(30, 0);
        client.set_read_timeout(timeout_seconds_, 0);
        return from_result(client.Get(parsed.path, http_headers));
#elif defined(_WIN32)
        return winhttp_request(parsed, "GET", headers, {}, {}, timeout_seconds_);
#else
        return HttpResponse{0, {}, "HTTPS requires MCP_SSL=ON / CPPHTTPLIB_OPENSSL_SUPPORT on this platform"};
#endif
    }

    return HttpResponse{0, {}, "unsupported URL scheme: " + parsed.scheme};
}

} // namespace mcdk::image
