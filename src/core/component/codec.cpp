#include "core/component/codec.hpp"

#include <algorithm>
#include <array>
#include <bit>

#include "core/registries.hpp"
#include "generated/behaviour.hpp"
#include "generated/registries.hpp"
#include "nbt/snbt.hpp"

namespace mc::codec {

// ---------------------------------------------------------------- HashOps (CRC32C, like Guava)
namespace hash {
namespace {
constexpr std::array<uint32_t, 256> makeCrcTable() {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
        t[i] = c;
    }
    return t;
}
constexpr auto kCrcTable = makeCrcTable();

// Guava's Hasher: primitives are written little-endian.
class Hasher {
public:
    Hasher& putByte(uint8_t b) {
        crc_ = kCrcTable[(crc_ ^ b) & 0xFF] ^ (crc_ >> 8);
        return *this;
    }
    template <class T>
    Hasher& putLE(T v) {
        auto u = std::bit_cast<std::make_unsigned_t<T>>(v);
        for (size_t i = 0; i < sizeof(T); i++) putByte(static_cast<uint8_t>(u >> (8 * i)));
        return *this;
    }
    Hasher& putHash(uint32_t h) { return putLE(h); } // HashCode.asBytes of a 32-bit hash
    uint32_t done() const { return ~crc_; }

private:
    uint32_t crc_ = 0xFFFFFFFFu;
};

// UTF-16 code units of a UTF-8 string (Java String chars).
std::vector<uint16_t> utf16(std::string_view s) {
    std::vector<uint16_t> out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        uint8_t c = static_cast<uint8_t>(s[i]);
        uint32_t cp;
        int len;
        if (c < 0x80) cp = c, len = 1;
        else if ((c >> 5) == 6) cp = c & 0x1F, len = 2;
        else if ((c >> 4) == 14) cp = c & 0x0F, len = 3;
        else cp = c & 0x07, len = 4;
        for (int k = 1; k < len && i + k < s.size(); k++) cp = (cp << 6) | (static_cast<uint8_t>(s[i + k]) & 0x3F);
        i += len;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<uint16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<uint16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<uint16_t>(cp));
        }
    }
    return out;
}
} // namespace

uint32_t ofByte(int8_t v) { return Hasher().putByte(6).putByte(static_cast<uint8_t>(v)).done(); }
uint32_t ofShort(int16_t v) { return Hasher().putByte(7).putLE(v).done(); }
uint32_t ofInt(int32_t v) { return Hasher().putByte(8).putLE(v).done(); }
uint32_t ofLong(int64_t v) { return Hasher().putByte(9).putLE(v).done(); }
uint32_t ofFloat(float v) { return Hasher().putByte(10).putLE(std::bit_cast<int32_t>(v)).done(); }
uint32_t ofDouble(double v) { return Hasher().putByte(11).putLE(std::bit_cast<int64_t>(v)).done(); }
uint32_t ofBool(bool v) { return Hasher().putByte(13).putByte(v ? 1 : 0).done(); }
uint32_t empty() { return Hasher().putByte(1).done(); }

uint32_t ofString(std::string_view s) {
    auto units = utf16(s);
    Hasher h;
    h.putByte(12).putLE(static_cast<int32_t>(units.size()));
    for (uint16_t u : units) h.putLE(u);
    return h.done();
}

uint32_t ofList(std::span<const uint32_t> elements) {
    Hasher h;
    h.putByte(4);
    for (uint32_t e : elements) h.putHash(e);
    return h.putByte(5).done();
}

uint32_t ofMap(std::vector<std::pair<uint32_t, uint32_t>> entries) {
    // HashOps.MAP_ENTRY_ORDER: by key hash, then value hash (HashCode.padToLong: unsigned).
    std::sort(entries.begin(), entries.end());
    Hasher h;
    h.putByte(2);
    for (auto [k, v] : entries) h.putHash(k).putHash(v);
    return h.putByte(3).done();
}

uint32_t ofByteArray(std::span<const int8_t> v) {
    Hasher h;
    h.putByte(14);
    for (int8_t b : v) h.putByte(static_cast<uint8_t>(b));
    return h.putByte(15).done();
}

uint32_t ofIntArray(std::span<const int32_t> v) {
    Hasher h;
    h.putByte(16);
    for (int32_t x : v) h.putLE(x);
    return h.putByte(17).done();
}

uint32_t ofLongArray(std::span<const int64_t> v) {
    Hasher h;
    h.putByte(18);
    for (int64_t x : v) h.putLE(x);
    return h.putByte(19).done();
}

uint32_t ofTag(const nbt::Tag& tag) {
    return std::visit(
        [](const auto& v) -> uint32_t {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::monostate>) return empty();
            else if constexpr (std::is_same_v<T, int8_t>) return ofByte(v);
            else if constexpr (std::is_same_v<T, int16_t>) return ofShort(v);
            else if constexpr (std::is_same_v<T, int32_t>) return ofInt(v);
            else if constexpr (std::is_same_v<T, int64_t>) return ofLong(v);
            else if constexpr (std::is_same_v<T, float>) return ofFloat(v);
            else if constexpr (std::is_same_v<T, double>) return ofDouble(v);
            else if constexpr (std::is_same_v<T, std::string>) return ofString(v);
            else if constexpr (std::is_same_v<T, std::vector<int8_t>>) return ofByteArray(v);
            else if constexpr (std::is_same_v<T, std::vector<int32_t>>) return ofIntArray(v);
            else if constexpr (std::is_same_v<T, std::vector<int64_t>>) return ofLongArray(v);
            else if constexpr (std::is_same_v<T, nbt::List>) {
                std::vector<uint32_t> hs;
                for (const auto& e : v.items) hs.push_back(ofTag(e));
                return ofList(hs);
            } else {
                std::vector<std::pair<uint32_t, uint32_t>> es;
                for (const auto& [k, e] : v.entries) es.emplace_back(ofString(k), ofTag(e));
                return ofMap(std::move(es));
            }
        },
        tag.value);
}
} // namespace hash

// ---------------------------------------------------------------- helpers
nbt::Tag makeList(std::vector<nbt::Tag> items) {
    nbt::List l;
    if (!items.empty()) l.elementType = items[0].type();
    l.items = std::move(items);
    return l;
}

std::vector<nbt::Tag> listItems(const nbt::Tag& tag) {
    if (const nbt::List* l = tag.asList()) return l->items;
    std::vector<nbt::Tag> out;
    if (auto* b = std::get_if<std::vector<int8_t>>(&tag.value)) for (int8_t x : *b) out.emplace_back(x);
    else if (auto* i = std::get_if<std::vector<int32_t>>(&tag.value)) for (int32_t x : *i) out.emplace_back(x);
    else if (auto* l = std::get_if<std::vector<int64_t>>(&tag.value)) for (int64_t x : *l) out.emplace_back(x);
    else throw CodecError("expected a list");
    return out;
}

namespace {

int64_t num(const nbt::Tag& t) {
    if (!t.isNumeric()) throw CodecError("expected a number");
    return t.asLong();
}
double numD(const nbt::Tag& t) {
    if (!t.isNumeric()) throw CodecError("expected a number");
    return t.asDouble();
}
const std::string& str(const nbt::Tag& t) {
    const std::string* s = t.asString();
    if (!s) throw CodecError("expected a string");
    return *s;
}
const nbt::Compound& comp(const nbt::Tag& t) {
    const nbt::Compound* c = t.asCompound();
    if (!c) throw CodecError("expected a compound");
    return *c;
}
size_t readCount(ByteReader& in, size_t max) {
    int32_t n = in.readVarInt();
    if (n < 0 || static_cast<size_t>(n) > max) throw DecoderException("collection too large");
    return static_cast<size_t>(n);
}

// "minecraft:" is implied for identifiers without a namespace (Identifier.parse).
std::string withNamespace(std::string_view s) {
    return s.find(':') == std::string_view::npos ? "minecraft:" + std::string(s) : std::string(s);
}

const gen::RegistryInfo* findRegistry(std::string_view name) {
    for (const auto& r : gen::kStaticRegistries)
        if (r.name == name) return &r;
    for (const auto& r : gen::kSyncedRegistries)
        if (r.name == name) return &r;
    return nullptr;
}

// ---------------------------------------------------------------- primitives
template <class ReadFn, class WriteFn, class HashFn>
class Simple final : public Schema {
public:
    Simple(ReadFn r, WriteFn w, HashFn h) : r_(r), w_(w), h_(h) {}
    nbt::Tag read(ByteReader& in) const override { return r_(in); }
    void write(ByteWriter& out, const nbt::Tag& v) const override { w_(out, v); }
    uint32_t hash(const nbt::Tag& v) const override { return h_(v); }

private:
    ReadFn r_;
    WriteFn w_;
    HashFn h_;
};
template <class R, class W, class H>
Ptr simple(R r, W w, H h) {
    return std::make_shared<Simple<R, W, H>>(r, w, h);
}

// Identifier.CODEC / STREAM_CODEC: a string; "x" means "minecraft:x".
class IdentifierSchema final : public Schema {
public:
    nbt::Tag read(ByteReader& in) const override { return in.readUtf(); }
    void write(ByteWriter& out, const nbt::Tag& v) const override { out.writeUtf(withNamespace(str(v))); }
    uint32_t hash(const nbt::Tag& v) const override { return hash::ofString(withNamespace(str(v))); }
    nbt::Tag normalize(const nbt::Tag& v) const override { return v.asString() ? nbt::Tag(withNamespace(*v.asString())) : v; }
};

class Text final : public Schema {
public:
    nbt::Tag read(ByteReader& in) const override { return nbt::readNetwork(in); }
    void write(ByteWriter& out, const nbt::Tag& v) const override { nbt::writeNetwork(out, v); }
    uint32_t hash(const nbt::Tag& v) const override { return hashText(v); }

    // The Component codec is mirrored by its NBT, except that booleans become bytes there.
    static uint32_t hashText(const nbt::Tag& v) {
        if (const nbt::Compound* c = v.asCompound()) {
            std::vector<std::pair<uint32_t, uint32_t>> es;
            for (const auto& [k, e] : c->entries) {
                bool isBool = k == "bold" || k == "italic" || k == "underlined" || k == "strikethrough" || k == "obfuscated" ||
                              k == "interpret" || k == "hat";
                es.emplace_back(hash::ofString(k), isBool && e.isNumeric() ? hash::ofBool(e.asLong() != 0) : hashText(e));
            }
            return hash::ofMap(std::move(es));
        }
        if (const nbt::List* l = v.asList()) {
            std::vector<uint32_t> hs;
            for (const auto& e : l->items) hs.push_back(hashText(e));
            return hash::ofList(hs);
        }
        return hash::ofTag(v);
    }
};

class NbtOf final : public Schema {
public:
    explicit NbtOf(Ptr codec) : codec_(std::move(codec)) {}
    nbt::Tag read(ByteReader& in) const override { return nbt::readNetwork(in); }
    void write(ByteWriter& out, const nbt::Tag& v) const override { nbt::writeNetwork(out, codec_->normalize(v)); }
    uint32_t hash(const nbt::Tag& v) const override { return codec_->hash(v); }
    nbt::Tag normalize(const nbt::Tag& v) const override { return codec_->normalize(v); }

private:
    Ptr codec_;
};

// ---------------------------------------------------------------- enums and registries
class Enum final : public Schema {
public:
    explicit Enum(std::string_view name) {
        for (const auto& e : gen::kEnums)
            if (e.name == name) values_ = e.values;
        if (values_.empty()) throw std::logic_error("unknown enum " + std::string(name));
    }
    nbt::Tag read(ByteReader& in) const override { return std::string(byId(in.readVarInt())); }
    void write(ByteWriter& out, const nbt::Tag& v) const override { out.writeVarInt(idOf(str(v))); }
    uint32_t hash(const nbt::Tag& v) const override {
        idOf(str(v));
        return hash::ofString(str(v));
    }
    std::string_view byId(int32_t id) const {
        // ByIdMap with a fallback: unknown ids map to the first value (OutOfBoundsStrategy.ZERO/CLAMP-like).
        for (const auto& e : values_)
            if (e.id == id) return e.name;
        return values_[0].name;
    }
    int32_t idOf(std::string_view name) const {
        for (const auto& e : values_)
            if (e.name == name) return e.id;
        throw CodecError("unknown enum value " + std::string(name));
    }

private:
    std::span<const gen::EnumValue> values_;
};

class RegistryId final : public Schema {
public:
    explicit RegistryId(std::string_view registry) : registry_(registry), info_(findRegistry(registry)) {
        if (!info_) throw std::logic_error("unknown registry " + registry_);
    }
    nbt::Tag read(ByteReader& in) const override { return std::string(nameOf(in.readVarInt())); }
    void write(ByteWriter& out, const nbt::Tag& v) const override { out.writeVarInt(idOf(str(v))); }
    uint32_t hash(const nbt::Tag& v) const override {
        idOf(str(v));
        return hash::ofString(withNamespace(str(v)));
    }
    nbt::Tag normalize(const nbt::Tag& v) const override {
        if (const std::string* s = v.asString()) return withNamespace(*s);
        return v;
    }
    std::string_view nameOf(int32_t id) const {
        if (id < 0 || static_cast<size_t>(id) >= info_->entries.size()) throw DecoderException("unknown " + registry_ + " id");
        return info_->entries[id];
    }
    int32_t idOf(std::string_view name) const {
        auto id = registries::id(registry_, withNamespace(name));
        if (!id) throw CodecError("unknown " + registry_ + " entry " + std::string(name));
        return *id;
    }

private:
    std::string registry_;
    const gen::RegistryInfo* info_;
};

class Holder final : public Schema {
public:
    Holder(std::string_view registry, Ptr direct) : ref_(registry), direct_(std::move(direct)) {}
    nbt::Tag read(ByteReader& in) const override {
        int32_t id = in.readVarInt();
        if (id == 0) {
            if (!direct_) throw DecoderException("inline holder not supported here");
            return direct_->read(in);
        }
        return std::string(ref_.nameOf(id - 1));
    }
    void write(ByteWriter& out, const nbt::Tag& v) const override {
        if (v.asString()) {
            out.writeVarInt(ref_.idOf(*v.asString()) + 1);
            return;
        }
        if (!direct_) throw CodecError("inline holder not supported here");
        out.writeVarInt(0);
        direct_->write(out, direct_->normalize(v));
    }
    uint32_t hash(const nbt::Tag& v) const override {
        if (v.asString()) return ref_.hash(v);
        if (!direct_) throw CodecError("inline holder not supported here");
        return direct_->hash(direct_->normalize(v));
    }
    nbt::Tag normalize(const nbt::Tag& v) const override { return v.asString() ? ref_.normalize(v) : v; }

private:
    RegistryId ref_;
    Ptr direct_;
};

class HolderSet final : public Schema {
public:
    explicit HolderSet(std::string_view registry) : ref_(registry) {}
    nbt::Tag read(ByteReader& in) const override {
        int32_t n = in.readVarInt() - 1;
        if (n == -1) return "#" + in.readUtf();
        if (n < 0 || n > 65536 * 16) throw DecoderException("bad holder set");
        std::vector<nbt::Tag> items;
        for (int i = 0; i < n; i++) items.emplace_back(std::string(ref_.nameOf(in.readVarInt())));
        if (items.size() == 1) return items[0]; // compactListCodec
        return makeList(std::move(items));
    }
    void write(ByteWriter& out, const nbt::Tag& v) const override {
        if (const std::string* s = v.asString()) {
            if (!s->empty() && (*s)[0] == '#') {
                out.writeVarInt(0);
                out.writeUtf(withNamespace(s->substr(1)));
                return;
            }
            out.writeVarInt(2);
            out.writeVarInt(ref_.idOf(*s));
            return;
        }
        auto items = listItems(v);
        out.writeVarInt(static_cast<int32_t>(items.size()) + 1);
        for (const auto& e : items) out.writeVarInt(ref_.idOf(str(e)));
    }
    uint32_t hash(const nbt::Tag& v) const override {
        if (const std::string* s = v.asString()) {
            if (!s->empty() && (*s)[0] == '#') return hash::ofString("#" + withNamespace(s->substr(1)));
            return ref_.hash(v);
        }
        auto items = listItems(v);
        if (items.size() == 1) return ref_.hash(items[0]);
        std::vector<uint32_t> hs;
        for (const auto& e : items) hs.push_back(ref_.hash(e));
        return hash::ofList(hs);
    }

private:
    RegistryId ref_;
};

// ---------------------------------------------------------------- structure
class ListSchema final : public Schema {
public:
    ListSchema(Ptr element, size_t max, bool fixed) : element_(std::move(element)), max_(max), fixed_(fixed) {}
    nbt::Tag read(ByteReader& in) const override {
        size_t n = fixed_ ? max_ : readCount(in, max_);
        std::vector<nbt::Tag> items;
        items.reserve(std::min<size_t>(n, 4096));
        for (size_t i = 0; i < n; i++) items.push_back(element_->read(in));
        return makeList(std::move(items));
    }
    void write(ByteWriter& out, const nbt::Tag& v) const override {
        auto items = listItems(v);
        if (fixed_ ? items.size() != max_ : items.size() > max_) throw CodecError("list has the wrong size");
        if (!fixed_) out.writeVarInt(static_cast<int32_t>(items.size()));
        for (const auto& e : items) element_->write(out, element_->normalize(e));
    }
    uint32_t hash(const nbt::Tag& v) const override {
        std::vector<uint32_t> hs;
        for (const auto& e : listItems(v)) hs.push_back(element_->hash(element_->normalize(e)));
        return hash::ofList(hs);
    }

private:
    Ptr element_;
    size_t max_;
    bool fixed_;
};

class CompactList final : public Schema {
public:
    explicit CompactList(Ptr element) : element_(std::move(element)) {}
    nbt::Tag read(ByteReader& in) const override {
        size_t n = readCount(in, 1u << 20);
        std::vector<nbt::Tag> items;
        for (size_t i = 0; i < n; i++) items.push_back(element_->read(in));
        if (items.size() == 1) return items[0];
        return makeList(std::move(items));
    }
    void write(ByteWriter& out, const nbt::Tag& v) const override {
        auto items = elements(v);
        out.writeVarInt(static_cast<int32_t>(items.size()));
        for (const auto& e : items) element_->write(out, element_->normalize(e));
    }
    uint32_t hash(const nbt::Tag& v) const override {
        auto items = elements(v);
        if (items.size() == 1) return element_->hash(element_->normalize(items[0]));
        std::vector<uint32_t> hs;
        for (const auto& e : items) hs.push_back(element_->hash(element_->normalize(e)));
        return hash::ofList(hs);
    }

private:
    // Either a list of elements or a single one; a list is tried first (Codec.either(list, element)).
    std::vector<nbt::Tag> elements(const nbt::Tag& v) const {
        if (v.asList() || v.type() == nbt::TagType::IntArray || v.type() == nbt::TagType::ByteArray || v.type() == nbt::TagType::LongArray)
            return listItems(v);
        return {v};
    }
    Ptr element_;
};

class OptionalSchema final : public Schema {
public:
    explicit OptionalSchema(Ptr element) : element_(std::move(element)) {}
    nbt::Tag read(ByteReader& in) const override { return in.readBool() ? element_->read(in) : nbt::Tag(); }
    void write(ByteWriter& out, const nbt::Tag& v) const override {
        bool present = v.type() != nbt::TagType::End;
        out.writeBool(present);
        if (present) element_->write(out, element_->normalize(v));
    }
    uint32_t hash(const nbt::Tag& v) const override { return v.type() == nbt::TagType::End ? hash::empty() : element_->hash(element_->normalize(v)); }

private:
    Ptr element_;
};

class MapSchema final : public Schema {
public:
    MapSchema(Ptr key, Ptr value, size_t max) : key_(std::move(key)), value_(std::move(value)), max_(max) {}
    nbt::Tag read(ByteReader& in) const override {
        size_t n = readCount(in, max_);
        nbt::Compound out;
        for (size_t i = 0; i < n; i++) {
            nbt::Tag k = key_->read(in);
            nbt::Tag v = value_->read(in);
            out.put(str(k), std::move(v));
        }
        return out;
    }
    void write(ByteWriter& out, const nbt::Tag& v) const override {
        const nbt::Compound& c = comp(v);
        if (c.entries.size() > max_) throw CodecError("map too large");
        out.writeVarInt(static_cast<int32_t>(c.entries.size()));
        for (const auto& [k, e] : c.entries) {
            key_->write(out, key_->normalize(nbt::Tag(k)));
            value_->write(out, value_->normalize(e));
        }
    }
    uint32_t hash(const nbt::Tag& v) const override {
        std::vector<std::pair<uint32_t, uint32_t>> es;
        for (const auto& [k, e] : comp(v).entries) es.emplace_back(key_->hash(key_->normalize(nbt::Tag(k))), value_->hash(value_->normalize(e)));
        return hash::ofMap(std::move(es));
    }

private:
    Ptr key_, value_;
    size_t max_;
};

// Whether `v` is encodable by `s` (used to pick a Codec.either branch for NBT input).
bool fits(const Schema& s, const nbt::Tag& v) {
    try {
        ByteWriter scratch;
        s.write(scratch, s.normalize(v));
        return true;
    } catch (const CodecError&) {
        return false;
    } catch (const std::exception&) {
        return false;
    }
}

class Either final : public Schema {
public:
    Either(Ptr left, Ptr right) : left_(std::move(left)), right_(std::move(right)) {}
    nbt::Tag read(ByteReader& in) const override { return in.readBool() ? left_->read(in) : right_->read(in); }
    void write(ByteWriter& out, const nbt::Tag& v) const override {
        if (fits(*left_, v)) {
            out.writeBool(true);
            left_->write(out, left_->normalize(v));
        } else {
            out.writeBool(false);
            right_->write(out, right_->normalize(v));
        }
    }
    uint32_t hash(const nbt::Tag& v) const override { return fits(*left_, v) ? left_->hash(left_->normalize(v)) : right_->hash(right_->normalize(v)); }

private:
    Ptr left_, right_;
};

class Alternative final : public Schema {
public:
    Alternative(Ptr primary, std::function<std::optional<nbt::Tag>(const nbt::Tag&)> convert)
        : primary_(std::move(primary)), convert_(std::move(convert)) {}
    nbt::Tag read(ByteReader& in) const override { return primary_->read(in); }
    void write(ByteWriter& out, const nbt::Tag& v) const override { primary_->write(out, normalize(v)); }
    uint32_t hash(const nbt::Tag& v) const override { return primary_->hash(normalize(v)); }
    nbt::Tag normalize(const nbt::Tag& v) const override {
        if (fits(*primary_, v)) return primary_->normalize(v);
        if (auto c = convert_(v)) return primary_->normalize(*c);
        return v;
    }

private:
    Ptr primary_;
    std::function<std::optional<nbt::Tag>(const nbt::Tag&)> convert_;
};

// Record fields: shared by records and dispatch cases.
nbt::Compound readFields(const std::vector<Field>& fields, ByteReader& in) {
    nbt::Compound out;
    for (const auto& f : fields) {
        switch (f.mode) {
        case Field::Mode::Required:
        case Field::Mode::AlwaysDefault: out.put(f.name, f.schema->read(in)); break;
        case Field::Mode::Optional:
            if (in.readBool()) out.put(f.name, f.schema->read(in));
            break;
        case Field::Mode::Default: {
            nbt::Tag v = f.schema->read(in);
            if (!(v == f.def)) out.put(f.name, std::move(v));
            break;
        }
        }
    }
    return out;
}

void writeFields(const std::vector<Field>& fields, ByteWriter& out, const nbt::Compound& c) {
    for (const auto& f : fields) {
        const nbt::Tag* v = c.get(f.name);
        switch (f.mode) {
        case Field::Mode::Required:
            if (!v) throw CodecError("missing field " + f.name);
            f.schema->write(out, f.schema->normalize(*v));
            break;
        case Field::Mode::Optional:
            out.writeBool(v != nullptr);
            if (v) f.schema->write(out, f.schema->normalize(*v));
            break;
        case Field::Mode::Default:
        case Field::Mode::AlwaysDefault: f.schema->write(out, f.schema->normalize(v ? *v : f.def)); break;
        }
    }
}

void hashFields(const std::vector<Field>& fields, const nbt::Compound& c, std::vector<std::pair<uint32_t, uint32_t>>& es) {
    for (const auto& f : fields) {
        const nbt::Tag* v = c.get(f.name);
        nbt::Tag value;
        switch (f.mode) {
        case Field::Mode::Required:
            if (!v) throw CodecError("missing field " + f.name);
            value = f.schema->normalize(*v);
            break;
        case Field::Mode::Optional:
            if (!v) continue;
            value = f.schema->normalize(*v);
            break;
        case Field::Mode::Default:
            if (!v) continue;
            value = f.schema->normalize(*v);
            if (value == f.def) continue;
            break;
        case Field::Mode::AlwaysDefault: value = f.schema->normalize(v ? *v : f.def); break;
        }
        es.emplace_back(hash::ofString(f.name), f.schema->hash(value));
    }
}

nbt::Tag normalizeFields(const std::vector<Field>& fields, const nbt::Compound& c) {
    nbt::Compound out = c;
    for (const auto& f : fields)
        if (nbt::Tag* v = out.get(f.name)) *v = f.schema->normalize(*v);
    return out;
}

class Record final : public Schema {
public:
    explicit Record(std::vector<Field> fields) : fields_(std::move(fields)) {}
    nbt::Tag read(ByteReader& in) const override { return readFields(fields_, in); }
    void write(ByteWriter& out, const nbt::Tag& v) const override { writeFields(fields_, out, comp(v)); }
    uint32_t hash(const nbt::Tag& v) const override {
        std::vector<std::pair<uint32_t, uint32_t>> es;
        hashFields(fields_, comp(v), es);
        return hash::ofMap(std::move(es));
    }
    nbt::Tag normalize(const nbt::Tag& v) const override { return v.asCompound() ? normalizeFields(fields_, *v.asCompound()) : v; }

private:
    std::vector<Field> fields_;
};

class Dispatch final : public Schema {
public:
    Dispatch(Ptr type, std::vector<DispatchCase> cases) : type_(std::move(type)), cases_(std::move(cases)) {}
    nbt::Tag read(ByteReader& in) const override {
        nbt::Tag type = type_->read(in);
        const DispatchCase& c = find(str(type));
        nbt::Compound out = readFields(c.fields, in);
        out.entries.insert(out.entries.begin(), {"type", std::move(type)});
        return out;
    }
    void write(ByteWriter& out, const nbt::Tag& v) const override {
        const nbt::Compound& c = comp(v);
        const nbt::Tag* type = c.get("type");
        if (!type) throw CodecError("missing type");
        nbt::Tag t = type_->normalize(*type);
        type_->write(out, t);
        writeFields(find(str(t)).fields, out, c);
    }
    uint32_t hash(const nbt::Tag& v) const override {
        const nbt::Compound& c = comp(v);
        const nbt::Tag* type = c.get("type");
        if (!type) throw CodecError("missing type");
        nbt::Tag t = type_->normalize(*type);
        std::vector<std::pair<uint32_t, uint32_t>> es{{hash::ofString("type"), type_->hash(t)}};
        hashFields(find(str(t)).fields, c, es);
        return hash::ofMap(std::move(es));
    }

private:
    const DispatchCase& find(const std::string& type) const {
        std::string full = withNamespace(type);
        for (const auto& c : cases_)
            if (c.type == type || c.type == full) return c;
        throw CodecError("unsupported type " + type);
    }
    Ptr type_;
    std::vector<DispatchCase> cases_;
};

class Lazy final : public Schema {
public:
    explicit Lazy(std::function<Ptr()> resolve) : resolve_(std::move(resolve)) {}
    nbt::Tag read(ByteReader& in) const override { return get().read(in); }
    void write(ByteWriter& out, const nbt::Tag& v) const override { get().write(out, v); }
    uint32_t hash(const nbt::Tag& v) const override { return get().hash(v); }
    nbt::Tag normalize(const nbt::Tag& v) const override { return get().normalize(v); }

private:
    const Schema& get() const {
        if (!cached_) cached_ = resolve_();
        return *cached_;
    }
    std::function<Ptr()> resolve_;
    mutable Ptr cached_;
};

} // namespace

// ---------------------------------------------------------------- factories
Ptr varInt() {
    static Ptr p = simple([](ByteReader& in) { return nbt::Tag(in.readVarInt()); },
                          [](ByteWriter& out, const nbt::Tag& v) { out.writeVarInt(static_cast<int32_t>(num(v))); },
                          [](const nbt::Tag& v) { return hash::ofInt(static_cast<int32_t>(num(v))); });
    return p;
}
Ptr unsignedByte() {
    static Ptr p = simple([](ByteReader& in) { return nbt::Tag(static_cast<int8_t>(in.readVarInt())); },
                          [](ByteWriter& out, const nbt::Tag& v) { out.writeVarInt(static_cast<int32_t>(num(v) & 0xFF)); },
                          [](const nbt::Tag& v) { return hash::ofByte(static_cast<int8_t>(num(v))); });
    return p;
}
Ptr int32() {
    static Ptr p = simple([](ByteReader& in) { return nbt::Tag(in.readInt()); },
                          [](ByteWriter& out, const nbt::Tag& v) { out.writeInt(static_cast<int32_t>(num(v))); },
                          [](const nbt::Tag& v) { return hash::ofInt(static_cast<int32_t>(num(v))); });
    return p;
}
Ptr int64() {
    static Ptr p = simple([](ByteReader& in) { return nbt::Tag(in.readLong()); },
                          [](ByteWriter& out, const nbt::Tag& v) { out.writeLong(num(v)); },
                          [](const nbt::Tag& v) { return hash::ofLong(num(v)); });
    return p;
}
Ptr varLong() {
    static Ptr p = simple([](ByteReader& in) { return nbt::Tag(in.readVarLong()); },
                          [](ByteWriter& out, const nbt::Tag& v) { out.writeVarLong(num(v)); },
                          [](const nbt::Tag& v) { return hash::ofLong(num(v)); });
    return p;
}
Ptr float32() {
    static Ptr p = simple([](ByteReader& in) { return nbt::Tag(in.readFloat()); },
                          [](ByteWriter& out, const nbt::Tag& v) { out.writeFloat(static_cast<float>(numD(v))); },
                          [](const nbt::Tag& v) { return hash::ofFloat(static_cast<float>(numD(v))); });
    return p;
}
Ptr float64() {
    static Ptr p = simple([](ByteReader& in) { return nbt::Tag(in.readDouble()); },
                          [](ByteWriter& out, const nbt::Tag& v) { out.writeDouble(numD(v)); },
                          [](const nbt::Tag& v) { return hash::ofDouble(numD(v)); });
    return p;
}
Ptr boolean() {
    static Ptr p = simple([](ByteReader& in) { return nbt::byteTag(in.readBool()); },
                          [](ByteWriter& out, const nbt::Tag& v) { out.writeBool(num(v) != 0); },
                          [](const nbt::Tag& v) { return hash::ofBool(num(v) != 0); });
    return p;
}
Ptr string(size_t maxLength) {
    return simple([maxLength](ByteReader& in) { return nbt::Tag(in.readUtf(maxLength)); },
                  [](ByteWriter& out, const nbt::Tag& v) { out.writeUtf(str(v)); },
                  [](const nbt::Tag& v) { return hash::ofString(str(v)); });
}
Ptr identifier() {
    static Ptr p = std::make_shared<IdentifierSchema>();
    return p;
}
Ptr anyTag() {
    static Ptr p = simple([](ByteReader& in) { return nbt::readNetwork(in); }, [](ByteWriter& out, const nbt::Tag& v) { nbt::writeNetwork(out, v); },
                          [](const nbt::Tag& v) { return hash::ofTag(v); });
    return p;
}
Ptr unit() {
    static Ptr p = simple([](ByteReader&) { return nbt::Tag(nbt::Compound{}); }, [](ByteWriter&, const nbt::Tag&) {},
                          [](const nbt::Tag&) { return hash::ofMap({}); });
    return p;
}
Ptr text() {
    static Ptr p = std::make_shared<Text>();
    return p;
}
Ptr compoundTag() {
    static Ptr p = simple(
        [](ByteReader& in) {
            nbt::Tag t = nbt::readNetwork(in);
            if (!t.asCompound()) throw DecoderException("expected a compound tag");
            return t;
        },
        [](ByteWriter& out, const nbt::Tag& v) {
            comp(v);
            nbt::writeNetwork(out, v);
        },
        [](const nbt::Tag& v) { return hash::ofTag(v); });
    return p;
}
Ptr snbtCompound() {
    static Ptr p = simple(
        [](ByteReader& in) {
            nbt::Tag t = nbt::readNetwork(in);
            if (!t.asCompound()) throw DecoderException("expected a compound tag");
            return nbt::Tag(nbt::toSnbt(t));
        },
        [](ByteWriter& out, const nbt::Tag& v) {
            if (v.asCompound()) {
                nbt::writeNetwork(out, v);
                return;
            }
            auto parsed = nbt::parseSnbt(str(v));
            if (!parsed || !parsed->asCompound()) throw CodecError("bad SNBT");
            nbt::writeNetwork(out, *parsed);
        },
        [](const nbt::Tag& v) { return hash::ofString(v.asCompound() ? nbt::toSnbt(v) : str(v)); });
    return p;
}
Ptr uuid() {
    static Ptr p = simple(
        [](ByteReader& in) {
            int64_t most = in.readLong(), least = in.readLong();
            return nbt::Tag(std::vector<int32_t>{static_cast<int32_t>(most >> 32), static_cast<int32_t>(most), static_cast<int32_t>(least >> 32),
                                                 static_cast<int32_t>(least)});
        },
        [](ByteWriter& out, const nbt::Tag& v) {
            auto* a = std::get_if<std::vector<int32_t>>(&v.value);
            if (!a || a->size() != 4) throw CodecError("expected a UUID int array");
            out.writeLong((static_cast<int64_t>((*a)[0]) << 32) | static_cast<uint32_t>((*a)[1]));
            out.writeLong((static_cast<int64_t>((*a)[2]) << 32) | static_cast<uint32_t>((*a)[3]));
        },
        [](const nbt::Tag& v) {
            auto* a = std::get_if<std::vector<int32_t>>(&v.value);
            if (!a || a->size() != 4) throw CodecError("expected a UUID int array");
            return hash::ofIntArray(*a);
        });
    return p;
}
Ptr blockPos() {
    static Ptr p = simple(
        [](ByteReader& in) {
            int64_t v = in.readLong();
            int32_t x = static_cast<int32_t>(v >> 38), y = static_cast<int32_t>(v << 52 >> 52), z = static_cast<int32_t>(v << 26 >> 38);
            return nbt::Tag(std::vector<int32_t>{x, y, z});
        },
        [](ByteWriter& out, const nbt::Tag& v) {
            auto items = listItems(v);
            if (items.size() != 3) throw CodecError("expected a block position");
            out.writeBlockPos(static_cast<int>(num(items[0])), static_cast<int>(num(items[1])), static_cast<int>(num(items[2])));
        },
        [](const nbt::Tag& v) {
            auto items = listItems(v);
            if (items.size() != 3) throw CodecError("expected a block position");
            std::vector<int32_t> a;
            for (auto& e : items) a.push_back(static_cast<int32_t>(num(e)));
            return hash::ofIntArray(a);
        });
    return p;
}
Ptr nbtOf(Ptr codec) { return std::make_shared<NbtOf>(std::move(codec)); }

Ptr enumeration(std::string_view extractedName) { return std::make_shared<Enum>(extractedName); }
Ptr registryId(std::string_view registry) { return std::make_shared<RegistryId>(registry); }
Ptr holder(std::string_view registry, Ptr direct) { return std::make_shared<Holder>(registry, std::move(direct)); }
Ptr holderSet(std::string_view registry) { return std::make_shared<HolderSet>(registry); }

Ptr list(Ptr element, size_t maxSize) { return std::make_shared<ListSchema>(std::move(element), maxSize, false); }
Ptr fixedList(Ptr element, size_t size) { return std::make_shared<ListSchema>(std::move(element), size, true); }
Ptr compactList(Ptr element) { return std::make_shared<CompactList>(std::move(element)); }
Ptr optional(Ptr element) { return std::make_shared<OptionalSchema>(std::move(element)); }
Ptr map(Ptr key, Ptr value, size_t maxSize) { return std::make_shared<MapSchema>(std::move(key), std::move(value), maxSize); }
Ptr either(Ptr left, Ptr right) { return std::make_shared<Either>(std::move(left), std::move(right)); }
Ptr alternative(Ptr primary, std::function<std::optional<nbt::Tag>(const nbt::Tag&)> convert) {
    return std::make_shared<Alternative>(std::move(primary), std::move(convert));
}

Field req(std::string name, Ptr schema) { return {std::move(name), std::move(schema), Field::Mode::Required, {}}; }
Field opt(std::string name, Ptr schema) { return {std::move(name), std::move(schema), Field::Mode::Optional, {}}; }
Field def(std::string name, Ptr schema, nbt::Tag d) { return {std::move(name), std::move(schema), Field::Mode::Default, std::move(d)}; }
Field always(std::string name, Ptr schema, nbt::Tag d) { return {std::move(name), std::move(schema), Field::Mode::AlwaysDefault, std::move(d)}; }

Ptr record(std::vector<Field> fields) { return std::make_shared<Record>(std::move(fields)); }
std::vector<Field> fields(std::initializer_list<std::vector<Field>> parts) {
    std::vector<Field> out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

Ptr dispatchEnum(std::string_view extractedEnum, std::vector<DispatchCase> cases) {
    return std::make_shared<Dispatch>(enumeration(extractedEnum), std::move(cases));
}
Ptr dispatchRegistry(std::string_view registry, std::vector<DispatchCase> cases) {
    return std::make_shared<Dispatch>(registryId(registry), std::move(cases));
}
Ptr lazy(std::function<Ptr()> resolve) { return std::make_shared<Lazy>(std::move(resolve)); }

} // namespace mc::codec
