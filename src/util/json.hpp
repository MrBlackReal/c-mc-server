#pragma once

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace mc::json {

// Minimal JSON value for reading data files (loot tables, predicates).
struct Value {
    using Object = std::map<std::string, Value, std::less<>>;
    using Array = std::vector<Value>;
    std::variant<std::nullptr_t, bool, double, std::string, std::shared_ptr<Array>, std::shared_ptr<Object>> v = nullptr;

    bool isNull() const { return std::holds_alternative<std::nullptr_t>(v); }
    bool isBool() const { return std::holds_alternative<bool>(v); }
    bool isNumber() const { return std::holds_alternative<double>(v); }
    bool isString() const { return std::holds_alternative<std::string>(v); }
    bool isArray() const { return std::holds_alternative<std::shared_ptr<Array>>(v); }
    bool isObject() const { return std::holds_alternative<std::shared_ptr<Object>>(v); }

    double number(double def = 0) const { return isNumber() ? std::get<double>(v) : def; }
    bool boolean(bool def = false) const { return isBool() ? std::get<bool>(v) : def; }
    const std::string& string() const;
    const Array& array() const;
    const Object& object() const;

    // Object member or a null value.
    const Value& operator[](std::string_view key) const;
    bool has(std::string_view key) const { return isObject() && object().contains(key); }
};

// Throws std::runtime_error on malformed input.
Value parse(std::string_view text);

} // namespace mc::json
