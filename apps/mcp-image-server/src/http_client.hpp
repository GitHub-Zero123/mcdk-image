#pragma once

#include <map>
#include <string>

namespace mcdk::image {

struct HttpResponse {
    int status = 0;
    std::string body;
    std::string error;
};

class HttpClient {
public:
    explicit HttpClient(int timeout_seconds);

    HttpResponse post_json(
        const std::string& url,
        const std::map<std::string, std::string>& headers,
        const std::string& body
    ) const;

    HttpResponse get(
        const std::string& url,
        const std::map<std::string, std::string>& headers = {}
    ) const;

private:
    int timeout_seconds_;
};

} // namespace mcdk::image
