#include "nbt/tag.hpp"

#include <bit>

#include <zlib.h>

#include <fstream>

#include "util/log.hpp"

namespace mc::nbt {
namespace {

constexpr int kMaxDepth = 512;

// ---------------------------------------------------------------- modified UTF-8

// Java's DataOutput.writeUTF: UTF-16 code units, NUL as C0 80, surrogates encoded individually.
void appendMutf8(std::string& out, uint32_t unit) {
    if (unit != 0 && unit < 0x80) {
        out += static_cast<char>(unit);
    } else if (unit < 0x800) {
        out += static_cast<char>(0xC0 | (unit >> 6));
        out += static_cast<char>(0x80 | (unit & 0x3F));
    } else {
        out += static_cast<char>(0xE0 | (unit >> 12));
        out += static_cast<char>(0x80 | ((unit >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (unit & 0x3F));
    }
}

std::string utf8ToMutf8(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        auto c = static_cast<unsigned char>(s[i]);
        uint32_t cp;
        int len;
        if (c < 0x80) cp = c, len = 1;
        else if ((c >> 5) == 6) cp = c & 0x1F, len = 2;
        else if ((c >> 4) == 14) cp = c & 0x0F, len = 3;
        else if ((c >> 3) == 30) cp = c & 0x07, len = 4;
        else cp = 0xFFFD, len = 1;
        if (i + len > s.size()) cp = 0xFFFD, len = 1;
        for (int k = 1; k < len; k++) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        i += len;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            appendMutf8(out, 0xD800 + (cp >> 10));
            appendMutf8(out, 0xDC00 + (cp & 0x3FF));
        } else {
            appendMutf8(out, cp);
        }
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

std::string mutf8ToUtf8(std::span<const uint8_t> b) {
    std::string out;
    out.reserve(b.size());
    uint32_t pendingHigh = 0;
    for (size_t i = 0; i < b.size();) {
        uint32_t unit;
        uint8_t c = b[i];
        if (c < 0x80) {
            unit = c, i += 1;
        } else if ((c >> 5) == 6 && i + 1 < b.size()) {
            unit = (c & 0x1F) << 6 | (b[i + 1] & 0x3F), i += 2;
        } else if ((c >> 4) == 14 && i + 2 < b.size()) {
            unit = (c & 0x0F) << 12 | (b[i + 1] & 0x3F) << 6 | (b[i + 2] & 0x3F), i += 3;
        } else {
            unit = 0xFFFD, i += 1;
        }
        if (unit >= 0xD800 && unit < 0xDC00) {
            if (pendingHigh) appendUtf8(out, 0xFFFD);
            pendingHigh = unit;
            continue;
        }
        if (unit >= 0xDC00 && unit < 0xE000 && pendingHigh) {
            appendUtf8(out, 0x10000 + ((pendingHigh - 0xD800) << 10) + (unit - 0xDC00));
            pendingHigh = 0;
            continue;
        }
        if (pendingHigh) appendUtf8(out, 0xFFFD), pendingHigh = 0;
        appendUtf8(out, unit);
    }
    if (pendingHigh) appendUtf8(out, 0xFFFD);
    return out;
}

void writeString(ByteWriter& out, std::string_view s) {
    std::string m = utf8ToMutf8(s);
    if (m.size() > 65535) m.resize(65535); // DataOutput.writeUTF limit; vanilla throws instead
    out.writeShort(static_cast<int16_t>(static_cast<uint16_t>(m.size())));
    out.writeBytes(m.data(), m.size());
}

std::string readString(ByteReader& in) {
    auto len = in.readUnsignedShort();
    return mutf8ToUtf8(in.readBytes(len));
}

// ---------------------------------------------------------------- lists

bool isWrapper(const Tag& t) {
    const Compound* c = t.asCompound();
    return c && c->entries.size() == 1 && c->entries[0].first.empty();
}

TagType listElementType(const List& l) {
    TagType type = TagType::End;
    for (const auto& item : l.items) {
        if (type == TagType::End) type = item.type();
        else if (type != item.type()) return TagType::Compound;
    }
    return type;
}

void writePayload(ByteWriter& out, const Tag& tag);

void writeListPayload(ByteWriter& out, const List& l) {
    TagType type = listElementType(l);
    out.writeByte(static_cast<uint8_t>(type));
    out.writeInt(static_cast<int32_t>(l.items.size()));
    for (const auto& item : l.items) {
        if (type == TagType::Compound && (item.type() != TagType::Compound || isWrapper(item))) {
            Compound wrapper;
            wrapper.entries.emplace_back("", item);
            writePayload(out, wrapper);
        } else {
            writePayload(out, item);
        }
    }
}

void writePayload(ByteWriter& out, const Tag& tag) {
    std::visit(
        [&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
            } else if constexpr (std::is_same_v<T, int8_t>) {
                out.writeByte(static_cast<uint8_t>(v));
            } else if constexpr (std::is_same_v<T, int16_t>) {
                out.writeShort(v);
            } else if constexpr (std::is_same_v<T, int32_t>) {
                out.writeInt(v);
            } else if constexpr (std::is_same_v<T, int64_t>) {
                out.writeLong(v);
            } else if constexpr (std::is_same_v<T, float>) {
                out.writeFloat(v);
            } else if constexpr (std::is_same_v<T, double>) {
                out.writeDouble(v);
            } else if constexpr (std::is_same_v<T, std::vector<int8_t>>) {
                out.writeInt(static_cast<int32_t>(v.size()));
                out.writeBytes(v.data(), v.size());
            } else if constexpr (std::is_same_v<T, std::string>) {
                writeString(out, v);
            } else if constexpr (std::is_same_v<T, List>) {
                writeListPayload(out, v);
            } else if constexpr (std::is_same_v<T, Compound>) {
                for (const auto& [key, child] : v.entries) {
                    out.writeByte(static_cast<uint8_t>(child.type()));
                    writeString(out, key);
                    writePayload(out, child);
                }
                out.writeByte(0);
            } else if constexpr (std::is_same_v<T, std::vector<int32_t>>) {
                out.writeInt(static_cast<int32_t>(v.size()));
                for (int32_t x : v) out.writeInt(x);
            } else if constexpr (std::is_same_v<T, std::vector<int64_t>>) {
                out.writeInt(static_cast<int32_t>(v.size()));
                for (int64_t x : v) out.writeLong(x);
            }
        },
        tag.value);
}

int32_t readLength(ByteReader& in, size_t elementSize) {
    int32_t n = in.readInt();
    if (n < 0 || static_cast<size_t>(n) * elementSize > in.remaining()) throw DecoderException("NBT array length out of range");
    return n;
}

Tag readPayload(ByteReader& in, TagType type, int depth) {
    if (depth > kMaxDepth) throw DecoderException("NBT too deep");
    switch (type) {
    case TagType::End: return {};
    case TagType::Byte: return in.readByte();
    case TagType::Short: return in.readShort();
    case TagType::Int: return in.readInt();
    case TagType::Long: return in.readLong();
    case TagType::Float: return in.readFloat();
    case TagType::Double: return in.readDouble();
    case TagType::ByteArray: {
        auto b = in.readBytes(static_cast<size_t>(readLength(in, 1)));
        return std::vector<int8_t>(b.begin(), b.end());
    }
    case TagType::String: return readString(in);
    case TagType::List: {
        List list;
        list.elementType = static_cast<TagType>(in.readUnsignedByte());
        int32_t n = readLength(in, list.elementType == TagType::End ? 0 : 1);
        if (list.elementType == TagType::End && n > 0) throw DecoderException("Missing type on ListTag");
        list.items.reserve(n);
        for (int32_t i = 0; i < n; i++) {
            Tag item = readPayload(in, list.elementType, depth + 1);
            if (isWrapper(item)) {
                // ListTag unwrap. Move the inner value out first: assigning a member of `item`
                // into `item` itself would destroy the source mid-assignment.
                Tag inner = std::move(item.asCompound()->entries[0].second);
                item = std::move(inner);
            }
            list.items.push_back(std::move(item));
        }
        return list;
    }
    case TagType::Compound: {
        Compound c;
        while (true) {
            auto t = static_cast<TagType>(in.readUnsignedByte());
            if (t == TagType::End) break;
            std::string key = readString(in);
            c.entries.emplace_back(std::move(key), readPayload(in, t, depth + 1));
        }
        return c;
    }
    case TagType::IntArray: {
        int32_t n = readLength(in, 4);
        std::vector<int32_t> v(n);
        for (auto& x : v) x = in.readInt();
        return v;
    }
    case TagType::LongArray: {
        int32_t n = readLength(in, 8);
        std::vector<int64_t> v(n);
        for (auto& x : v) x = in.readLong();
        return v;
    }
    }
    throw DecoderException("invalid NBT tag type");
}

std::optional<std::vector<uint8_t>> inflateStream(std::span<const uint8_t> data, int windowBits) {
    z_stream zs{};
    if (inflateInit2(&zs, windowBits) != Z_OK) return std::nullopt;
    std::vector<uint8_t> out(std::max<size_t>(data.size() * 4, 4096));
    zs.next_in = const_cast<Bytef*>(data.data());
    zs.avail_in = static_cast<uInt>(data.size());
    int rc;
    do {
        if (zs.total_out == out.size()) {
            if (out.size() > (256u << 20)) break; // refuse absurd sizes
            out.resize(out.size() * 2);
        }
        zs.next_out = out.data() + zs.total_out;
        zs.avail_out = static_cast<uInt>(out.size() - zs.total_out);
        rc = inflate(&zs, Z_NO_FLUSH);
    } while (rc == Z_OK);
    size_t total = zs.total_out;
    inflateEnd(&zs);
    if (rc != Z_STREAM_END) return std::nullopt;
    out.resize(total);
    return out;
}

std::vector<uint8_t> deflateStream(std::span<const uint8_t> data, int windowBits) {
    z_stream zs{};
    deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, windowBits, 8, Z_DEFAULT_STRATEGY);
    std::vector<uint8_t> out(deflateBound(&zs, data.size()) + 32);
    zs.next_in = const_cast<Bytef*>(data.data());
    zs.avail_in = static_cast<uInt>(data.size());
    zs.next_out = out.data();
    zs.avail_out = static_cast<uInt>(out.size());
    deflate(&zs, Z_FINISH);
    out.resize(zs.total_out);
    deflateEnd(&zs);
    return out;
}

} // namespace

// ---------------------------------------------------------------- Tag / Compound

bool operator==(const Tag& a, const Tag& b) {
    if (a.type() != b.type()) return false;
    if (const Compound* ca = a.asCompound()) {
        const Compound* cb = b.asCompound();
        if (ca->entries.size() != cb->entries.size()) return false;
        for (const auto& [k, v] : ca->entries) {
            const Tag* other = cb->get(k);
            if (!other || !(v == *other)) return false;
        }
        return true;
    }
    if (const List* la = a.asList()) {
        const List* lb = b.asList();
        if (la->items.size() != lb->items.size()) return false;
        for (size_t i = 0; i < la->items.size(); i++)
            if (!(la->items[i] == lb->items[i])) return false;
        return true;
    }
    // Floats compare by bits, like FloatTag.equals.
    if (a.type() == TagType::Float) return std::bit_cast<uint32_t>(std::get<float>(a.value)) == std::bit_cast<uint32_t>(std::get<float>(b.value));
    if (a.type() == TagType::Double) return std::bit_cast<uint64_t>(std::get<double>(a.value)) == std::bit_cast<uint64_t>(std::get<double>(b.value));
    return std::visit(
        [&](const auto& x) {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, List> || std::is_same_v<T, Compound>) return false; // handled above
            else return x == std::get<T>(b.value);
        },
        a.value);
}

double Tag::asDouble() const {
    return std::visit(
        [](const auto& v) -> double {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_arithmetic_v<T>) return static_cast<double>(v);
            else return 0.0;
        },
        value);
}

int64_t Tag::asLong() const {
    return std::visit(
        [](const auto& v) -> int64_t {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_integral_v<T>) return static_cast<int64_t>(v);
            else if constexpr (std::is_floating_point_v<T>) return static_cast<int64_t>(v);
            else return 0;
        },
        value);
}

Compound& Compound::put(std::string key, Tag value) {
    for (auto& [k, v] : entries) {
        if (k == key) {
            v = std::move(value);
            return *this;
        }
    }
    entries.emplace_back(std::move(key), std::move(value));
    return *this;
}

const Tag* Compound::get(std::string_view key) const {
    for (const auto& [k, v] : entries)
        if (k == key) return &v;
    return nullptr;
}

Tag* Compound::get(std::string_view key) {
    for (auto& [k, v] : entries)
        if (k == key) return &v;
    return nullptr;
}

void Compound::remove(std::string_view key) {
    std::erase_if(entries, [&](const auto& e) { return e.first == key; });
}

int64_t Compound::getLong(std::string_view key, int64_t def) const {
    const Tag* t = get(key);
    return t && t->isNumeric() ? t->asLong() : def;
}

double Compound::getDouble(std::string_view key, double def) const {
    const Tag* t = get(key);
    return t && t->isNumeric() ? t->asDouble() : def;
}

std::string Compound::getString(std::string_view key, std::string def) const {
    const Tag* t = get(key);
    return t && t->asString() ? *t->asString() : def;
}

const Compound* Compound::getCompound(std::string_view key) const {
    const Tag* t = get(key);
    return t ? t->asCompound() : nullptr;
}

Compound* Compound::getCompound(std::string_view key) {
    Tag* t = get(key);
    return t ? t->asCompound() : nullptr;
}

const List* Compound::getList(std::string_view key) const {
    const Tag* t = get(key);
    return t ? t->asList() : nullptr;
}

const std::vector<int64_t>* Compound::getLongArray(std::string_view key) const {
    const Tag* t = get(key);
    return t ? std::get_if<std::vector<int64_t>>(&t->value) : nullptr;
}

const std::vector<int32_t>* Compound::getIntArray(std::string_view key) const {
    const Tag* t = get(key);
    return t ? std::get_if<std::vector<int32_t>>(&t->value) : nullptr;
}

const std::vector<int8_t>* Compound::getByteArray(std::string_view key) const {
    const Tag* t = get(key);
    return t ? std::get_if<std::vector<int8_t>>(&t->value) : nullptr;
}

// ---------------------------------------------------------------- IO

void writeNetwork(ByteWriter& out, const Tag& tag) {
    out.writeByte(static_cast<uint8_t>(tag.type()));
    if (tag.type() != TagType::End) writePayload(out, tag);
}

Tag readNetwork(ByteReader& in) {
    auto type = static_cast<TagType>(in.readUnsignedByte());
    return readPayload(in, type, 0);
}

void writeNamed(ByteWriter& out, std::string_view name, const Tag& tag) {
    out.writeByte(static_cast<uint8_t>(tag.type()));
    if (tag.type() == TagType::End) return;
    writeString(out, name);
    writePayload(out, tag);
}

Tag readNamed(ByteReader& in, std::string* nameOut) {
    auto type = static_cast<TagType>(in.readUnsignedByte());
    if (type == TagType::End) return {};
    std::string name = readString(in);
    if (nameOut) *nameOut = std::move(name);
    return readPayload(in, type, 0);
}

std::vector<uint8_t> gzipCompress(std::span<const uint8_t> data) { return deflateStream(data, 15 + 16); }
std::optional<std::vector<uint8_t>> gzipDecompress(std::span<const uint8_t> data) { return inflateStream(data, 15 + 16); }
std::vector<uint8_t> zlibCompress(std::span<const uint8_t> data) { return deflateStream(data, 15); }
std::optional<std::vector<uint8_t>> zlibDecompress(std::span<const uint8_t> data) { return inflateStream(data, 15); }

std::optional<Compound> readCompressedFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::vector<uint8_t> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto data = gzipDecompress(raw);
    if (!data) {
        log::error("Failed to decompress {}", path.string());
        return std::nullopt;
    }
    try {
        ByteReader r(*data);
        Tag root = readNamed(r);
        if (auto* c = root.asCompound()) return std::move(*c);
    } catch (const std::exception& e) {
        log::error("Failed to read {}: {}", path.string(), e.what());
    }
    return std::nullopt;
}

bool writeCompressedFile(const std::filesystem::path& path, const Compound& root) {
    namespace fs = std::filesystem;
    ByteWriter w;
    writeNamed(w, "", Tag(root));
    auto gz = gzipCompress(w.data());
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(gz.data()), static_cast<std::streamsize>(gz.size()));
        if (!out.flush()) {
            log::error("Failed to write {}", tmp.string());
            return false;
        }
    }
    fs::path old = path;
    old += "_old";
    if (fs::exists(path, ec)) fs::rename(path, old, ec);
    fs::rename(tmp, path, ec);
    if (ec) {
        log::error("Failed to replace {}: {}", path.string(), ec.message());
        return false;
    }
    return true;
}

} // namespace mc::nbt
