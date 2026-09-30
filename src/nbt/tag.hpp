#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "network/friendly_byte_buf.hpp"

namespace mc::nbt {

enum class TagType : uint8_t {
    End = 0, Byte, Short, Int, Long, Float, Double, ByteArray, String, List, Compound, IntArray, LongArray
};

struct Tag;
using CompoundEntries = std::vector<std::pair<std::string, Tag>>; // insertion ordered

// Like vanilla's ListTag, a list may hold mixed element types in memory. On write, mixed lists
// become compound lists with non-compound elements wrapped as {"": value}; reading unwraps them.
struct List {
    TagType elementType = TagType::End; // hint only; the written type is derived from the items
    std::vector<Tag> items;
};

struct Compound {
    CompoundEntries entries;

    Compound& put(std::string key, Tag value);
    const Tag* get(std::string_view key) const;
    Tag* get(std::string_view key);
    bool contains(std::string_view key) const { return get(key) != nullptr; }
    void remove(std::string_view key);
    bool empty() const; // defined below, once Tag is complete

    // Typed getters with defaults (CompoundTag.getIntOr etc.). Numeric getters accept any numeric tag.
    int64_t getLong(std::string_view key, int64_t def = 0) const;
    int32_t getInt(std::string_view key, int32_t def = 0) const { return static_cast<int32_t>(getLong(key, def)); }
    bool getBool(std::string_view key, bool def = false) const { return getLong(key, def ? 1 : 0) != 0; }
    double getDouble(std::string_view key, double def = 0) const;
    std::string getString(std::string_view key, std::string def = "") const;
    const Compound* getCompound(std::string_view key) const;
    Compound* getCompound(std::string_view key);
    const List* getList(std::string_view key) const;
    const std::vector<int64_t>* getLongArray(std::string_view key) const;
    const std::vector<int32_t>* getIntArray(std::string_view key) const;
    const std::vector<int8_t>* getByteArray(std::string_view key) const;
};

struct Tag {
    using Value = std::variant<std::monostate, int8_t, int16_t, int32_t, int64_t, float, double, std::vector<int8_t>, std::string,
                               List, Compound, std::vector<int32_t>, std::vector<int64_t>>;
    Value value;

    Tag() = default;
    template <class T>
        requires std::is_constructible_v<Value, T&&>
    Tag(T&& v) : value(std::forward<T>(v)) {}
    Tag(const char* s) : value(std::string(s)) {}

    TagType type() const { return static_cast<TagType>(value.index()); }
    bool isNumeric() const { return type() >= TagType::Byte && type() <= TagType::Double; }
    double asDouble() const;
    int64_t asLong() const;
    const std::string* asString() const { return std::get_if<std::string>(&value); }
    const Compound* asCompound() const { return std::get_if<Compound>(&value); }
    Compound* asCompound() { return std::get_if<Compound>(&value); }
    const List* asList() const { return std::get_if<List>(&value); }
};

inline bool Compound::empty() const { return entries.empty(); }

inline Tag byteTag(bool b) { return Tag(static_cast<int8_t>(b ? 1 : 0)); }

// Tag.equals: same type and value; compound key order doesn't matter.
bool operator==(const Tag& a, const Tag& b);

// Network form: type id followed by the payload, without a root name (since 1.20.2).
void writeNetwork(ByteWriter& out, const Tag& tag);
Tag readNetwork(ByteReader& in);
// File form: type id, root name, payload.
void writeNamed(ByteWriter& out, std::string_view name, const Tag& tag);
Tag readNamed(ByteReader& in, std::string* nameOut = nullptr);

// Gzip-compressed named root compound (NbtIo.readCompressed / writeCompressed).
std::optional<Compound> readCompressedFile(const std::filesystem::path& path);
// Writes via a temp file and keeps the previous version as <name>_old (NbtIo + Util.safeReplaceFile).
bool writeCompressedFile(const std::filesystem::path& path, const Compound& root);

std::vector<uint8_t> gzipCompress(std::span<const uint8_t> data);
std::optional<std::vector<uint8_t>> gzipDecompress(std::span<const uint8_t> data);
std::vector<uint8_t> zlibCompress(std::span<const uint8_t> data);
std::optional<std::vector<uint8_t>> zlibDecompress(std::span<const uint8_t> data);

} // namespace mc::nbt
