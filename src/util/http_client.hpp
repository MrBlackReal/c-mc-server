#pragma once

#include <optional>
#include <string>

namespace mc::http {

struct Response {
    int status = 0;
    std::string body;
};

// A blocking HTTP/1.1 GET for http:// and https:// URLs (TLS with the system's CA certificates
// and host name verification), with `timeoutSeconds` for connecting and each read. Empty with
// `error` set if the request couldn't be made (the server's status codes are returned as they are).
// Meant for worker threads.
std::optional<Response> get(const std::string& url, std::string& error, int timeoutSeconds = 10);

// URLEncoder.encode (application/x-www-form-urlencoded, UTF-8).
std::string urlEncode(const std::string& s);

} // namespace mc::http
