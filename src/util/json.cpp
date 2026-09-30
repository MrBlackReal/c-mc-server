#include "util/json.hpp"

#include <charconv>
#include <cstdint>
#include <stdexcept>

namespace mc::json {
namespace {
const Value kNull;
const std::string kEmptyString;
const Value::Array kEmptyArray;
const Value::Object kEmptyObject;

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    Value parseDocument() {
        Value v = parseValue(0);
        skipWs();
        if (i_ != s_.size()) fail("trailing data");
        return v;
    }

private:
    [[noreturn]] void fail(const char* what) { throw std::runtime_error(std::string("JSON: ") + what + " at " + std::to_string(i_)); }
    void skipWs() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\n' || s_[i_] == '\r' || s_[i_] == '\t')) i_++;
    }
    char peek() {
        skipWs();
        if (i_ >= s_.size()) fail("unexpected end");
        return s_[i_];
    }
    void expect(char c) {
        if (peek() != c) fail("unexpected character");
        i_++;
    }

    Value parseValue(int depth) {
        if (depth > 256) fail("too deep");
        char c = peek();
        Value out;
        if (c == '{') {
            i_++;
            auto obj = std::make_shared<Value::Object>();
            if (peek() == '}') {
                i_++;
            } else {
                while (true) {
                    std::string key = parseString();
                    expect(':');
                    (*obj)[std::move(key)] = parseValue(depth + 1);
                    if (peek() == ',') {
                        i_++;
                        continue;
                    }
                    expect('}');
                    break;
                }
            }
            out.v = std::move(obj);
        } else if (c == '[') {
            i_++;
            auto arr = std::make_shared<Value::Array>();
            if (peek() == ']') {
                i_++;
            } else {
                while (true) {
                    arr->push_back(parseValue(depth + 1));
                    if (peek() == ',') {
                        i_++;
                        continue;
                    }
                    expect(']');
                    break;
                }
            }
            out.v = std::move(arr);
        } else if (c == '"') {
            out.v = parseString();
        } else if (s_.substr(i_, 4) == "true") {
            i_ += 4, out.v = true;
        } else if (s_.substr(i_, 5) == "false") {
            i_ += 5, out.v = false;
        } else if (s_.substr(i_, 4) == "null") {
            i_ += 4;
        } else {
            double d;
            auto [p, ec] = std::from_chars(s_.data() + i_, s_.data() + s_.size(), d);
            if (ec != std::errc()) fail("bad value");
            i_ = static_cast<size_t>(p - s_.data());
            out.v = d;
        }
        return out;
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

    uint32_t hex4() {
        if (i_ + 4 > s_.size()) fail("bad escape");
        uint32_t v = 0;
        auto [p, ec] = std::from_chars(s_.data() + i_, s_.data() + i_ + 4, v, 16);
        if (ec != std::errc() || p != s_.data() + i_ + 4) fail("bad escape");
        i_ += 4;
        return v;
    }

    std::string parseString() {
        expect('"');
        std::string out;
        while (true) {
            if (i_ >= s_.size()) fail("unterminated string");
            char c = s_[i_++];
            if (c == '"') return out;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i_ >= s_.size()) fail("bad escape");
            char e = s_[i_++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                uint32_t cp = hex4();
                if (cp >= 0xD800 && cp < 0xDC00 && s_.substr(i_, 2) == "\\u") {
                    i_ += 2;
                    uint32_t lo = hex4();
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                appendUtf8(out, cp);
                break;
            }
            default: fail("bad escape");
            }
        }
    }

    std::string_view s_;
    size_t i_ = 0;
};
} // namespace

const std::string& Value::string() const { return isString() ? std::get<std::string>(v) : kEmptyString; }
const Value::Array& Value::array() const { return isArray() ? *std::get<std::shared_ptr<Array>>(v) : kEmptyArray; }
const Value::Object& Value::object() const { return isObject() ? *std::get<std::shared_ptr<Object>>(v) : kEmptyObject; }

const Value& Value::operator[](std::string_view key) const {
    if (!isObject()) return kNull;
    const auto& o = object();
    auto it = o.find(key);
    return it == o.end() ? kNull : it->second;
}

Value parse(std::string_view text) { return Parser(text).parseDocument(); }

} // namespace mc::json
