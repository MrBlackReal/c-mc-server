#include "util/uuid.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <format>

namespace mc {
namespace {
UUID fromBytes(const unsigned char* b) {
    UUID u;
    for (int i = 0; i < 8; i++) u.most = (u.most << 8) | b[i];
    for (int i = 8; i < 16; i++) u.least = (u.least << 8) | b[i];
    return u;
}
} // namespace

UUID UUID::random() {
    unsigned char b[16];
    RAND_bytes(b, sizeof b);
    b[6] = (b[6] & 0x0f) | 0x40;
    b[8] = (b[8] & 0x3f) | 0x80;
    return fromBytes(b);
}

UUID UUID::nameBased(std::string_view bytes) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_Digest(bytes.data(), bytes.size(), md, &len, EVP_md5(), nullptr);
    md[6] = (md[6] & 0x0f) | 0x30;
    md[8] = (md[8] & 0x3f) | 0x80;
    return fromBytes(md);
}

std::string UUID::toString() const {
    return std::format("{:08x}-{:04x}-{:04x}-{:04x}-{:012x}", most >> 32, (most >> 16) & 0xffff, most & 0xffff, least >> 48,
                       least & 0xffffffffffffull);
}

std::optional<UUID> UUID::parse(std::string_view s) {
    std::string hex;
    for (char c : s)
        if (c != '-') hex += c;
    if (hex.size() != 32 || (s.size() != 32 && s.size() != 36)) return std::nullopt;
    UUID u;
    for (size_t i = 0; i < 32; i++) {
        char c = hex[i];
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0) return std::nullopt;
        uint64_t& half = i < 16 ? u.most : u.least;
        half = (half << 4) | static_cast<uint64_t>(v);
    }
    return u;
}

std::optional<UUID> UUID::fromJavaString(std::string_view s) {
    if (s.size() > 36) return std::nullopt;
    // Long.parseLong(s, begin, end, 16)
    auto parseLong = [](std::string_view f) -> std::optional<int64_t> {
        bool negative = false;
        if (!f.empty() && (f[0] == '-' || f[0] == '+')) negative = f[0] == '-', f.remove_prefix(1);
        if (f.empty()) return std::nullopt;
        uint64_t v = 0;
        for (char c : f) {
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0 || v > (static_cast<uint64_t>(INT64_MAX) + (negative ? 1 : 0) - d) / 16) return std::nullopt;
            v = v * 16 + d;
        }
        return negative ? static_cast<int64_t>(0 - v) : static_cast<int64_t>(v);
    };
    size_t d[5];
    size_t from = 0;
    for (int i = 0; i < 5; i++) {
        d[i] = s.find('-', from);
        if (d[i] == std::string_view::npos) {
            if (i < 4) return std::nullopt;
            break;
        }
        if (i == 4) return std::nullopt; // a fifth dash
        from = d[i] + 1;
    }
    std::optional<int64_t> f[5] = {parseLong(s.substr(0, d[0])), parseLong(s.substr(d[0] + 1, d[1] - d[0] - 1)),
                                   parseLong(s.substr(d[1] + 1, d[2] - d[1] - 1)), parseLong(s.substr(d[2] + 1, d[3] - d[2] - 1)),
                                   parseLong(s.substr(d[3] + 1))};
    for (const auto& v : f)
        if (!v) return std::nullopt;
    UUID u;
    u.most = (static_cast<uint64_t>(*f[0]) & 0xffffffffull) << 32 | (static_cast<uint64_t>(*f[1]) & 0xffff) << 16 | (static_cast<uint64_t>(*f[2]) & 0xffff);
    u.least = (static_cast<uint64_t>(*f[3]) & 0xffff) << 48 | (static_cast<uint64_t>(*f[4]) & 0xffffffffffffull);
    return u;
}

} // namespace mc
