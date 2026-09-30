#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "core/component/codec.hpp"

// Port of DataComponents: every item data component type with its Codec (disk) and StreamCodec
// (network), described once as a codec::Schema.
namespace mc::components {

struct ComponentType {
    std::string_view name;
    int32_t id = -1;
    codec::Ptr schema;       // network form + Codec NBT form
    bool persistent = true;  // has a Codec (saved in worlds); transient ones are network-only
};

void bootstrap();
const ComponentType* byId(int32_t id);
const ComponentType* byName(std::string_view name);

// Network value bytes -> the NBT vanilla saves. Empty if the bytes don't decode (or the type is
// transient).
std::optional<nbt::Tag> toDisk(int32_t type, std::span<const uint8_t> value);
// Saved NBT -> network value bytes. Empty if the NBT doesn't fit the Codec.
std::optional<std::vector<uint8_t>> toNetwork(int32_t type, const nbt::Tag& value);
// HashedPatchMap hash of a value (TypedDataComponent encoded into HashOps.CRC32C_INSTANCE).
std::optional<int32_t> hash(int32_t type, std::span<const uint8_t> value);

// ItemStackTemplate.CODEC / STREAM_CODEC and DataComponentPatch.CODEC / STREAM_CODEC.
codec::Ptr itemStackTemplate();
codec::Ptr patch();

} // namespace mc::components
