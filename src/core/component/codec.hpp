#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "nbt/tag.hpp"
#include "network/friendly_byte_buf.hpp"

// A small port of the two serialization systems a data component has in vanilla: its Codec
// (DataFixerUpper; here always encoding into NBT, i.e. NbtOps, the form saved in worlds) and its
// StreamCodec (the network form). A Schema describes one value type in both at once, so values
// can be converted either way and hashed like HashOps does (for the client's HashedStack).
//
// The canonical in-memory form is the NBT the Codec would produce: read() decodes the network
// form into it, write() encodes it back (throwing CodecError if the NBT doesn't fit, e.g. data
// written by another program), hash() is Codec.encode(HashOps.CRC32C_INSTANCE).
namespace mc::codec {

struct CodecError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// HashOps.CRC32C_INSTANCE: CRC32C over a typed encoding of the value.
namespace hash {
uint32_t ofByte(int8_t v);
uint32_t ofShort(int16_t v);
uint32_t ofInt(int32_t v);
uint32_t ofLong(int64_t v);
uint32_t ofFloat(float v);
uint32_t ofDouble(double v);
uint32_t ofBool(bool v);
uint32_t ofString(std::string_view utf8);
uint32_t empty();
uint32_t ofList(std::span<const uint32_t> elements);
uint32_t ofMap(std::vector<std::pair<uint32_t, uint32_t>> entries);
uint32_t ofByteArray(std::span<const int8_t> v);
uint32_t ofIntArray(std::span<const int32_t> v);
uint32_t ofLongArray(std::span<const int64_t> v);
// NbtOps.convertTo(HashOps): how a raw Tag (CompoundTag.CODEC, CustomData) hashes.
uint32_t ofTag(const nbt::Tag& tag);
} // namespace hash

class Schema {
public:
    virtual ~Schema() = default;
    virtual nbt::Tag read(ByteReader& in) const = 0;
    virtual void write(ByteWriter& out, const nbt::Tag& value) const = 0;
    virtual uint32_t hash(const nbt::Tag& value) const = 0;
    // Codec.withAlternative and friends: maps an accepted alternative NBT form onto the primary
    // one. write()/hash() of composite schemas call this on their children.
    virtual nbt::Tag normalize(const nbt::Tag& value) const { return value; }
};
using Ptr = std::shared_ptr<const Schema>;

// --- primitives
Ptr varInt();                 // Codec.INT / ByteBufCodecs.VAR_INT
Ptr unsignedByte();           // ExtraCodecs.UNSIGNED_BYTE (a byte tag) / ByteBufCodecs.VAR_INT
Ptr int32();                  // Codec.INT / ByteBufCodecs.INT
Ptr int64();                  // Codec.LONG / ByteBufCodecs.LONG
Ptr varLong();                // Codec.LONG / ByteBufCodecs.VAR_LONG
Ptr float32();
Ptr float64();
Ptr boolean();
Ptr string(size_t maxLength = 32767);
Ptr identifier();             // Identifier (a string)
Ptr unit();                   // Unit.CODEC: {} on disk, nothing on the network
Ptr text();                   // Component: the Codec's NBT, which is also the network form
Ptr compoundTag();            // CompoundTag.CODEC / ByteBufCodecs.COMPOUND_TAG
Ptr snbtCompound();           // TagParser.LENIENT_CODEC (SNBT string on disk) / COMPOUND_TAG
Ptr uuid();                   // UUIDUtil.CODEC (int array) / UUIDUtil.STREAM_CODEC
Ptr blockPos();               // BlockPos.CODEC (int array) / BlockPos.STREAM_CODEC (packed long)
// ByteBufCodecs.fromCodecWithRegistries: the network form is the Codec's NBT.
Ptr nbtOf(Ptr codec);
// Any tag, hashed as NbtOps.convertTo(HashOps) (for Codecs not ported in detail).
Ptr anyTag();

// --- names and registries
Ptr enumeration(std::string_view extractedName); // StringRepresentable + ByteBufCodecs.idMapper
Ptr registryId(std::string_view registry);       // byNameCodec / ByteBufCodecs.registry
// RegistryCodecs.holder / ByteBufCodecs.holder: a reference (id + 1) or an inline value (0).
Ptr holder(std::string_view registry, Ptr direct = nullptr);
Ptr holderSet(std::string_view registry);        // RegistryCodecs.holderSet / ByteBufCodecs.holderSet

// --- structure
Ptr list(Ptr element, size_t maxSize = 1u << 31);
Ptr fixedList(Ptr element, size_t size);  // ByteBufCodecs.fixedSizeList (no count on the network)
Ptr compactList(Ptr element);             // ExtraCodecs.compactListCodec: a single element unwrapped
Ptr optional(Ptr element);                // ByteBufCodecs.optional; absent = an End tag
Ptr map(Ptr key, Ptr value, size_t maxSize = 1u << 31); // unboundedMap / ByteBufCodecs.map
Ptr either(Ptr left, Ptr right);          // Codec.either / ByteBufCodecs.either (true = left)
// Codec.withAlternative: `convert` turns an alternative form into the primary one (or fails).
Ptr alternative(Ptr primary, std::function<std::optional<nbt::Tag>(const nbt::Tag&)> convert);

struct Field {
    enum class Mode {
        Required,      // fieldOf
        Optional,      // optionalFieldOf(name): absent on disk; ByteBufCodecs.optional on the network
        Default,       // optionalFieldOf(name, default): omitted on disk when equal to default
        AlwaysDefault, // optionalAlwaysPresentFieldOf: always written, default when missing
    };
    std::string name;
    Ptr schema;
    Mode mode = Mode::Required;
    nbt::Tag def;
};
Field req(std::string name, Ptr schema);
Field opt(std::string name, Ptr schema);
Field def(std::string name, Ptr schema, nbt::Tag defaultValue);
Field always(std::string name, Ptr schema, nbt::Tag defaultValue);

// RecordCodecBuilder + StreamCodec.composite with the same field order.
Ptr record(std::vector<Field> fields);
// Field lists of MapCodecs spliced into a record (e.g. AttributeModifier.MAP_CODEC).
std::vector<Field> fields(std::initializer_list<std::vector<Field>> parts);

// Codec.dispatch / StreamCodec.dispatch: {type: <name>, ...fields of that type}.
struct DispatchCase {
    std::string type;
    std::vector<Field> fields;
};
Ptr dispatchEnum(std::string_view extractedEnum, std::vector<DispatchCase> cases);
Ptr dispatchRegistry(std::string_view registry, std::vector<DispatchCase> cases);

// Lazily resolved reference, for recursive definitions (MobEffectInstance.Details, item templates).
Ptr lazy(std::function<Ptr()> resolve);

// NbtOps.createList: always a ListTag (typed arrays only come from createIntList and friends).
nbt::Tag makeList(std::vector<nbt::Tag> items);
// Elements of a List, ByteArray, IntArray or LongArray (throws CodecError otherwise).
std::vector<nbt::Tag> listItems(const nbt::Tag& tag);

} // namespace mc::codec
