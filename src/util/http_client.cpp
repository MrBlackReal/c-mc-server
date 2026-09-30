#include "util/http_client.hpp"

#include <netdb.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>

namespace mc::http {
namespace {

struct Url {
    bool tls = false;
    std::string host, port, target;
};

bool parseUrl(const std::string& url, Url& out) {
    size_t scheme = url.find("://");
    if (scheme == std::string::npos) return false;
    std::string s = url.substr(0, scheme);
    if (s == "https") out.tls = true;
    else if (s != "http") return false;
    size_t hostStart = scheme + 3;
    size_t pathStart = url.find('/', hostStart);
    std::string authority = url.substr(hostStart, pathStart == std::string::npos ? std::string::npos : pathStart - hostStart);
    out.target = pathStart == std::string::npos ? "/" : url.substr(pathStart);
    size_t colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(']') == std::string::npos) {
        out.host = authority.substr(0, colon);
        out.port = authority.substr(colon + 1);
    } else {
        out.host = authority;
        out.port = out.tls ? "443" : "80";
    }
    return !out.host.empty();
}

// Connects with a timeout; -1 and `error` on failure.
int connectTo(const Url& url, int timeoutSeconds, std::string& error) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (int rc = getaddrinfo(url.host.c_str(), url.port.c_str(), &hints, &res); rc != 0) {
        error = std::string("can't resolve ") + url.host + ": " + gai_strerror(rc);
        return -1;
    }
    int fd = -1;
    for (addrinfo* a = res; a && fd < 0; a = a->ai_next) {
        fd = ::socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
        if (fd < 0) continue;
        int flags = fcntl(fd, F_GETFL);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int rc = ::connect(fd, a->ai_addr, a->ai_addrlen);
        if (rc < 0 && errno == EINPROGRESS) {
            pollfd p{fd, POLLOUT, 0};
            int err = 0;
            socklen_t len = sizeof err;
            if (poll(&p, 1, timeoutSeconds * 1000) == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) rc = 0;
            else errno = err ? err : ETIMEDOUT;
        }
        if (rc != 0) {
            error = "can't connect to " + url.host + ": " + std::strerror(errno);
            ::close(fd);
            fd = -1;
            continue;
        }
        fcntl(fd, F_SETFL, flags);
        timeval tv{timeoutSeconds, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    }
    freeaddrinfo(res);
    return fd;
}

// Plain or TLS transport over a connected socket.
class Stream {
public:
    Stream(int fd) : fd_(fd) {}
    ~Stream() {
        if (ssl_) {
            SSL_shutdown(ssl_);
            SSL_free(ssl_);
        }
        if (ctx_) SSL_CTX_free(ctx_);
        ::close(fd_);
    }
    bool startTls(const std::string& host, std::string& error) {
        ctx_ = SSL_CTX_new(TLS_client_method());
        if (!ctx_ || SSL_CTX_set_default_verify_paths(ctx_) != 1) return fail("TLS setup failed", error);
        SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
        ssl_ = SSL_new(ctx_);
        if (!ssl_ || SSL_set_fd(ssl_, fd_) != 1 || SSL_set_tlsext_host_name(ssl_, host.c_str()) != 1 || SSL_set1_host(ssl_, host.c_str()) != 1)
            return fail("TLS setup failed", error);
        if (SSL_connect(ssl_) != 1) return fail("TLS handshake with " + host + " failed", error);
        return true;
    }
    bool writeAll(const std::string& data) {
        size_t off = 0;
        while (off < data.size()) {
            ssize_t n = ssl_ ? SSL_write(ssl_, data.data() + off, static_cast<int>(data.size() - off))
                             : ::send(fd_, data.data() + off, data.size() - off, MSG_NOSIGNAL);
            if (n <= 0) return false;
            off += static_cast<size_t>(n);
        }
        return true;
    }
    // Reads until the peer closes (the request asks for Connection: close).
    bool readAll(std::string& out) {
        char buf[16384];
        while (true) {
            ssize_t n = ssl_ ? SSL_read(ssl_, buf, sizeof buf) : ::recv(fd_, buf, sizeof buf, 0);
            if (n > 0) {
                out.append(buf, static_cast<size_t>(n));
                if (out.size() > (16u << 20)) return false;
                continue;
            }
            if (n == 0) return true;
            if (ssl_) {
                int e = SSL_get_error(ssl_, static_cast<int>(n));
                return e == SSL_ERROR_ZERO_RETURN || (e == SSL_ERROR_SYSCALL && errno == 0); // (servers that close without close_notify)
            }
            if (errno == EINTR) continue;
            return false;
        }
    }

private:
    bool fail(const std::string& what, std::string& error) {
        unsigned long e = ERR_get_error();
        char detail[256] = "";
        if (e) ERR_error_string_n(e, detail, sizeof detail);
        error = what + (e ? std::string(": ") + detail : "");
        return false;
    }
    int fd_;
    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;
};

std::string decodeChunked(const std::string& body) {
    std::string out;
    size_t pos = 0;
    while (pos < body.size()) {
        size_t eol = body.find("\r\n", pos);
        if (eol == std::string::npos) break;
        size_t size = std::stoul(body.substr(pos, eol - pos), nullptr, 16);
        if (size == 0) break;
        out += body.substr(eol + 2, size);
        pos = eol + 2 + size + 2;
    }
    return out;
}

} // namespace

std::optional<Response> get(const std::string& url, std::string& error, int timeoutSeconds) {
    Url u;
    if (!parseUrl(url, u)) {
        error = "bad URL " + url;
        return std::nullopt;
    }
    int fd = connectTo(u, timeoutSeconds, error);
    if (fd < 0) return std::nullopt;
    Stream stream(fd);
    if (u.tls && !stream.startTls(u.host, error)) return std::nullopt;
    std::string request = "GET " + u.target + " HTTP/1.1\r\nHost: " + u.host + "\r\nUser-Agent: cmc-server\r\nAccept: application/json\r\n" +
                          "Connection: close\r\n\r\n";
    std::string raw;
    if (!stream.writeAll(request) || !stream.readAll(raw)) {
        error = "request to " + u.host + " failed";
        return std::nullopt;
    }
    size_t headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos || raw.compare(0, 5, "HTTP/") != 0) {
        error = "bad response from " + u.host;
        return std::nullopt;
    }
    Response r;
    r.status = std::atoi(raw.c_str() + raw.find(' ') + 1);
    std::string headers = raw.substr(0, headerEnd);
    for (char& c : headers) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    r.body = raw.substr(headerEnd + 4);
    if (headers.find("transfer-encoding: chunked") != std::string::npos) r.body = decodeChunked(r.body);
    return r;
}

std::string urlEncode(const std::string& s) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '.' || c == '-' || c == '*' || c == '_') out += static_cast<char>(c);
        else if (c == ' ') out += '+';
        else out += '%', out += kHex[c >> 4], out += kHex[c & 15];
    }
    return out;
}

} // namespace mc::http
