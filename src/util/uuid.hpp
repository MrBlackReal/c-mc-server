#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace mc {

struct UUID {
    uint64_t most = 0;
    uint64_t least = 0;

    static UUID random();
    // java.util.UUID.nameUUIDFromBytes: MD5 based, version 3.
    static UUID nameBased(std::string_view bytes);
    // UUIDUtil.createOfflinePlayerUUID
    static UUID offlinePlayer(std::string_view name) { return nameBased(std::string("OfflinePlayer:") + std::string(name)); }

    std::string toString() const;
    // UUID.fromString (dashed) or UndashedUuid (32 hex digits).
    static std::optional<UUID> parse(std::string_view s);
    // java.util.UUID.fromString exactly: five dash-separated hex fields of any length (with an
    // optional sign), e.g. "0-0-0-0-1".
    static std::optional<UUID> fromJavaString(std::string_view s);
    // UUID.hashCode
    int32_t javaHashCode() const {
        uint64_t hilo = most ^ least;
        return static_cast<int32_t>(static_cast<uint32_t>(hilo >> 32) ^ static_cast<uint32_t>(hilo));
    }
    bool operator==(const UUID&) const = default;
};

} // namespace mc

template <>
struct std::hash<mc::UUID> {
    size_t operator()(const mc::UUID& u) const noexcept { return std::hash<uint64_t>{}(u.most ^ (u.least * 0x9E3779B97F4A7C15ull)); }
};
