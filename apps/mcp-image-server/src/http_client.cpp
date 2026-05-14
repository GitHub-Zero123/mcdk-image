#include "http_client.hpp"

#include "httplib.h"

#include <algorithm>
#include <stdexcept>
#include <string>

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
#else
        return HttpResponse{0, {}, "HTTPS requires MCP_SSL=ON / CPPHTTPLIB_OPENSSL_SUPPORT"};
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
#else
        return HttpResponse{0, {}, "HTTPS requires MCP_SSL=ON / CPPHTTPLIB_OPENSSL_SUPPORT"};
#endif
    }

    return HttpResponse{0, {}, "unsupported URL scheme: " + parsed.scheme};
}

} // namespace mcdk::image
