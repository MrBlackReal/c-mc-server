#include "util/base64.hpp"

namespace mc::base64 {
namespace {
constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
int value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
} // namespace

std::string encode(std::span<const uint8_t> data) {
    std::string out;
    for (size_t i = 0; i < data.size(); i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < data.size()) n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < data.size()) n |= data[i + 2];
        out += kAlphabet[n >> 18 & 63];
        out += kAlphabet[n >> 12 & 63];
        out += i + 1 < data.size() ? kAlphabet[n >> 6 & 63] : '=';
        out += i + 2 < data.size() ? kAlphabet[n & 63] : '=';
    }
    return out;
}

std::optional<std::vector<uint8_t>> decode(std::string_view text) {
    while (!text.empty() && text.back() == '=') text.remove_suffix(1);
    std::vector<uint8_t> out;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : text) {
        int v = value(c);
        if (v < 0) return std::nullopt;
        acc = acc << 6 | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>(acc >> bits & 0xFF));
        }
    }
    return out;
}

} // namespace mc::base64
