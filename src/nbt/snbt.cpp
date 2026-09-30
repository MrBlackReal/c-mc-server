#include "nbt/snbt.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>

namespace mc::nbt {
namespace {

// Shortest round-trip digits and decimal exponent of v (v finite, non-zero): v = 0.d1d2d3... * 10^exp10.
template <class F>
void shortestDigits(F v, std::string& digits, int& exp10) {
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::scientific);
    std::string_view s(buf, res.ptr - buf); // e.g. "-1.2345e+07"
    if (s[0] == '-') s.remove_prefix(1);
    size_t e = s.find('e');
    digits.clear();
    for (char c : s.substr(0, e))
        if (c != '.') digits += c;
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
    int exp = 0;
    std::from_chars(s.data() + e + 1 + (s[e + 1] == '+'), s.data() + s.size(), exp);
    exp10 = exp + 1;
}

template <class F>
std::string javaFloating(F v) {
    if (std::isnan(v)) return "NaN";
    if (std::isinf(v)) return v > 0 ? "Infinity" : "-Infinity";
    std::string out = std::signbit(v) ? "-" : "";
    if (v == 0) return out + "0.0";
    std::string digits;
    int exp10;
    shortestDigits(v, digits, exp10);
    F a = std::fabs(v);
    if (a >= static_cast<F>(1e-3) && a < static_cast<F>(1e7)) {
        if (exp10 <= 0) {
            out += "0." + std::string(-exp10, '0') + digits;
        } else if (static_cast<size_t>(exp10) >= digits.size()) {
            out += digits + std::string(exp10 - digits.size(), '0') + ".0";
        } else {
            out += digits.substr(0, exp10) + "." + digits.substr(exp10);
        }
    } else {
        out += digits.substr(0, 1) + "." + (digits.size() > 1 ? digits.substr(1) : "0") + "E" + std::to_string(exp10 - 1);
    }
    return out;
}

bool isUnquotedKeyChar(char c, bool first) {
    bool alpha = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '.' || c == '_';
    if (first) return alpha;
    return alpha || (c >= '0' && c <= '9') || c == '+' || c == '-';
}

bool needsQuotes(std::string_view key) {
    if (key.empty()) return true;
    auto lower = [](std::string_view s) {
        std::string r(s);
        for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return r;
    };
    std::string l = lower(key);
    if (l == "true" || l == "false") return true;
    // UNQUOTED_KEY_MATCH: [A-Za-z._]+[A-Za-z0-9._+-]*
    if (!isUnquotedKeyChar(key[0], true)) return true;
    for (char c : key)
        if (!isUnquotedKeyChar(c, false)) return true;
    return false;
}

void quoteAndEscape(std::string_view s, std::string& out) {
    size_t quotePos = out.size();
    out += ' ';
    char quote = 0;
    for (char c : s) {
        if (c == '\\') {
            out += "\\\\";
        } else if (c == '"' || c == '\'') {
            if (!quote) quote = c == '"' ? '\'' : '"';
            if (quote == c) out += '\\';
            out += c;
        } else {
            switch (c) {
            case '\b': out += "\\b"; break;
            case '\t': out += "\\t"; break;
            case '\n': out += "\\n"; break;
            case '\f': out += "\\f"; break;
            case '\r': out += "\\r"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char hex[8];
                    std::snprintf(hex, sizeof hex, "\\x%02X", static_cast<unsigned char>(c));
                    out += hex;
                } else {
                    out += c;
                }
            }
        }
    }
    if (!quote) quote = '"';
    out[quotePos] = quote;
    out += quote;
}

void print(const Tag& tag, std::string& out) {
    std::visit(
        [&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::monostate>) out += "END";
            else if constexpr (std::is_same_v<T, int8_t>) out += std::to_string(v) + "b";
            else if constexpr (std::is_same_v<T, int16_t>) out += std::to_string(v) + "s";
            else if constexpr (std::is_same_v<T, int32_t>) out += std::to_string(v);
            else if constexpr (std::is_same_v<T, int64_t>) out += std::to_string(v) + "L";
            else if constexpr (std::is_same_v<T, float>) out += javaFloating(v) + "f";
            else if constexpr (std::is_same_v<T, double>) out += javaFloating(v) + "d";
            else if constexpr (std::is_same_v<T, std::string>) quoteAndEscape(v, out);
            else if constexpr (std::is_same_v<T, std::vector<int8_t>>) {
                out += "[B;";
                for (size_t i = 0; i < v.size(); i++) out += (i ? "," : "") + std::to_string(v[i]) + "B";
                out += ']';
            } else if constexpr (std::is_same_v<T, std::vector<int32_t>>) {
                out += "[I;";
                for (size_t i = 0; i < v.size(); i++) out += (i ? "," : "") + std::to_string(v[i]);
                out += ']';
            } else if constexpr (std::is_same_v<T, std::vector<int64_t>>) {
                out += "[L;";
                for (size_t i = 0; i < v.size(); i++) out += (i ? "," : "") + std::to_string(v[i]) + "L";
                out += ']';
            } else if constexpr (std::is_same_v<T, List>) {
                out += '[';
                for (size_t i = 0; i < v.items.size(); i++) {
                    if (i) out += ',';
                    print(v.items[i], out);
                }
                out += ']';
            } else if constexpr (std::is_same_v<T, Compound>) {
                std::vector<const std::pair<std::string, Tag>*> entries;
                for (const auto& e : v.entries) entries.push_back(&e);
                std::sort(entries.begin(), entries.end(), [](auto* a, auto* b) { return a->first < b->first; });
                out += '{';
                for (size_t i = 0; i < entries.size(); i++) {
                    if (i) out += ',';
                    if (needsQuotes(entries[i]->first)) quoteAndEscape(entries[i]->first, out);
                    else out += entries[i]->first;
                    out += ':';
                    print(entries[i]->second, out);
                }
                out += '}';
            }
        },
        tag.value);
}

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

struct ParseError {
    std::string message;
    size_t pos = 0;
};

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    // A value at the start of the text (TagParser.parseAsArgument); `pos()` is where it ended.
    Tag parsePrefix() { return value(); }
    size_t pos() const { return i_; }

    Tag parseRoot() {
        Tag t = value();
        skipWs();
        if (i_ != s_.size()) fail("trailing data");
        return t;
    }

private:
    std::string_view s_;
    size_t i_ = 0;

    [[noreturn]] void fail(const std::string& what) { throw ParseError{what + " at position " + std::to_string(i_), i_}; }
    void skipWs() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) i_++;
    }
    bool peek(char c) {
        skipWs();
        return i_ < s_.size() && s_[i_] == c;
    }
    void expect(char c) {
        if (!peek(c)) fail(std::string("expected '") + c + "'");
        i_++;
    }

    Tag value() {
        skipWs();
        if (i_ >= s_.size()) fail("expected value");
        char c = s_[i_];
        if (c == '{') return compound();
        if (c == '[') return list();
        if (c == '"' || c == '\'') return quoted();
        return unquoted();
    }

    Tag compound() {
        expect('{');
        Compound out;
        if (peek('}')) {
            i_++;
            return out;
        }
        while (true) {
            skipWs();
            std::string key;
            if (i_ < s_.size() && (s_[i_] == '"' || s_[i_] == '\'')) key = *quoted().asString();
            else key = unquotedWord();
            if (key.empty()) fail("expected key");
            expect(':');
            out.put(std::move(key), value());
            if (peek(',')) {
                i_++;
                if (peek('}')) break;
                continue;
            }
            break;
        }
        expect('}');
        return out;
    }

    Tag list() {
        expect('[');
        skipWs();
        if (i_ + 1 < s_.size() && (s_[i_] == 'B' || s_[i_] == 'I' || s_[i_] == 'L') && s_[i_ + 1] == ';') {
            char kind = s_[i_];
            i_ += 2;
            std::vector<Tag> items;
            while (!peek(']')) {
                items.push_back(value());
                if (peek(',')) i_++;
                else break;
            }
            expect(']');
            auto num = [&](const Tag& t) {
                if (!t.isNumeric()) fail("expected a number in array");
                return t.asLong();
            };
            if (kind == 'B') {
                std::vector<int8_t> v;
                for (auto& t : items) v.push_back(static_cast<int8_t>(num(t)));
                return v;
            }
            if (kind == 'I') {
                std::vector<int32_t> v;
                for (auto& t : items) v.push_back(static_cast<int32_t>(num(t)));
                return v;
            }
            std::vector<int64_t> v;
            for (auto& t : items) v.push_back(num(t));
            return v;
        }
        List out;
        while (!peek(']')) {
            out.items.push_back(value());
            if (peek(',')) i_++;
            else break;
        }
        expect(']');
        if (!out.items.empty()) out.elementType = out.items[0].type();
        return out;
    }

    Tag quoted() {
        char q = s_[i_++];
        std::string out;
        while (true) {
            if (i_ >= s_.size()) fail("unterminated string");
            char c = s_[i_++];
            if (c == q) break;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i_ >= s_.size()) fail("unterminated escape");
            char e = s_[i_++];
            auto hex = [&](int n) {
                if (i_ + n > s_.size()) fail("bad escape");
                uint32_t v = 0;
                auto r = std::from_chars(s_.data() + i_, s_.data() + i_ + n, v, 16);
                if (r.ptr != s_.data() + i_ + n) fail("bad escape");
                i_ += n;
                return v;
            };
            switch (e) {
            case '\\': case '"': case '\'': out += e; break;
            case 'b': out += '\b'; break;
            case 't': out += '\t'; break;
            case 'n': out += '\n'; break;
            case 'f': out += '\f'; break;
            case 'r': out += '\r'; break;
            case 's': out += ' '; break;
            case 'x': appendUtf8(out, hex(2)); break;
            case 'u': appendUtf8(out, hex(4)); break;
            case 'U': appendUtf8(out, hex(8)); break;
            default: fail(std::string("unknown escape \\") + e);
            }
        }
        return out;
    }

    std::string unquotedWord() {
        size_t start = i_;
        while (i_ < s_.size()) {
            char c = s_[i_];
            if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '-' || c == '.' || c == '+')
                i_++;
            else break;
        }
        return std::string(s_.substr(start, i_ - start));
    }

    Tag unquoted() {
        std::string word = unquotedWord();
        if (word.empty()) fail("expected value");
        if (word == "true") return byteTag(true);
        if (word == "false") return byteTag(false);
        if (auto n = number(word)) return *n;
        return word;
    }

    static std::optional<Tag> number(std::string word) {
        std::string w;
        for (char c : word)
            if (c != '_') w += c;
        if (w.empty()) return std::nullopt;
        std::string_view v = w;
        bool negative = false;
        if (v[0] == '+' || v[0] == '-') {
            negative = v[0] == '-';
            v.remove_prefix(1);
        }
        if (v.empty()) return std::nullopt;
        char last = static_cast<char>(std::tolower(static_cast<unsigned char>(v.back())));
        bool hexOrBin = v.size() > 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X' || v[1] == 'b' || v[1] == 'B');
        // Floating point: has '.', or an exponent, or an f/d suffix (not in hex, where they're digits).
        bool isHex = hexOrBin && (v[1] == 'x' || v[1] == 'X');
        if (!hexOrBin && (v.find('.') != std::string_view::npos || v.find_first_of("eE") != std::string_view::npos || last == 'f' || last == 'd')) {
            bool isFloat = last == 'f';
            std::string_view body = (last == 'f' || last == 'd') ? v.substr(0, v.size() - 1) : v;
            double d = 0;
            auto r = std::from_chars(body.data(), body.data() + body.size(), d);
            if (r.ec != std::errc() || r.ptr != body.data() + body.size()) return std::nullopt;
            if (negative) d = -d;
            if (isFloat) return Tag(static_cast<float>(d));
            return Tag(d);
        }
        // Integer: optional type suffix (b/s/i/l), optionally preceded by a signedness letter (u/s).
        // In hex literals 'b' is a digit, so there a byte suffix can't be written.
        char type = 'i';
        bool unsignedHint = false;
        auto isTypeSuffix = [&](char c) { return c == 's' || c == 'l' || c == 'i' || (c == 'b' && !isHex); };
        if (isTypeSuffix(last) && v.size() > (hexOrBin ? 3u : 1u)) {
            type = last;
            v.remove_suffix(1);
            char sgn = static_cast<char>(std::tolower(static_cast<unsigned char>(v.back())));
            if ((sgn == 'u' || sgn == 's') && v.size() > (hexOrBin ? 3u : 1u)) {
                unsignedHint = sgn == 'u';
                v.remove_suffix(1);
            }
        }
        int base = 10;
        if (hexOrBin) {
            base = (v[1] == 'x' || v[1] == 'X') ? 16 : 2;
            v.remove_prefix(2);
        }
        if (v.empty()) return std::nullopt;
        uint64_t mag = 0;
        auto r = std::from_chars(v.data(), v.data() + v.size(), mag, base);
        if (r.ec != std::errc() || r.ptr != v.data() + v.size()) return std::nullopt;
        int64_t value = negative ? -static_cast<int64_t>(mag) : static_cast<int64_t>(mag);
        auto fits = [&](int64_t lo, int64_t hi, uint64_t umax) {
            return unsignedHint || base != 10 ? mag <= umax && !negative : value >= lo && value <= hi;
        };
        switch (type) {
        case 'b':
            if (!fits(-128, 127, 255)) return std::nullopt;
            return Tag(static_cast<int8_t>(value));
        case 's':
            if (!fits(-32768, 32767, 65535)) return std::nullopt;
            return Tag(static_cast<int16_t>(value));
        case 'l': return Tag(value);
        default:
            if (!fits(std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max(), 0xFFFFFFFFull)) return std::nullopt;
            return Tag(static_cast<int32_t>(value));
        }
    }
};

} // namespace

std::string toSnbt(const Tag& tag) {
    std::string out;
    print(tag, out);
    return out;
}

std::optional<Tag> parseSnbt(std::string_view text, std::string* error) {
    try {
        return Parser(text).parseRoot();
    } catch (const ParseError& e) {
        if (error) *error = e.message;
        return std::nullopt;
    }
}

std::optional<Tag> parseSnbtPrefix(std::string_view text, size_t& consumed, std::string* error, size_t* errorPos) {
    Parser parser(text);
    try {
        Tag t = parser.parsePrefix();
        consumed = parser.pos();
        return t;
    } catch (const ParseError& e) {
        if (error) *error = e.message;
        if (errorPos) *errorPos = e.pos;
        return std::nullopt;
    }
}

std::string javaFloatString(float v) { return javaFloating(v); }
std::string javaDoubleString(double v) { return javaFloating(v); }

} // namespace mc::nbt
