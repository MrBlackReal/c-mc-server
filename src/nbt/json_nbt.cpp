#include "nbt/json_nbt.hpp"

#include <cmath>
#include <limits>

namespace mc::nbt {

Tag fromJson(const json::Value& v) {
    if (v.isBool()) return byteTag(v.boolean());
    if (v.isNumber()) {
        double d = v.number();
        if (d == std::floor(d) && std::abs(d) < 9.2e18) {
            if (d >= std::numeric_limits<int32_t>::min() && d <= std::numeric_limits<int32_t>::max()) return static_cast<int32_t>(d);
            return static_cast<int64_t>(d);
        }
        return d;
    }
    if (v.isString()) return v.string();
    if (v.isArray()) {
        List l;
        for (const auto& e : v.array()) l.items.push_back(fromJson(e));
        return l;
    }
    Compound c;
    if (v.isObject())
        for (const auto& [k, e] : v.object()) c.put(k, fromJson(e));
    return c;
}

} // namespace mc::nbt
