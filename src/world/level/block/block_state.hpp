#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "core/direction.hpp"
#include "generated/behaviour.hpp"
#include "generated/blocks.hpp"

namespace mc {

// A block state is identified by its global network id (Block.BLOCK_STATE_REGISTRY).
using BlockStateId = uint32_t;

namespace blocks {

// Bits of gen::kStateFlags, see tools/extractor/Extractor.java.
enum BlockFlag : uint32_t {
    kAir = 1u << 0,
    kReplaceable = 1u << 1,
    kHasCollision = 1u << 2,
    kFullCollision = 1u << 3,
    kRandomlyTicking = 1u << 4, // BlockState.isRandomlyTicking
    kWater = 1u << 5,
    kLava = 1u << 6,
    kSturdyFaceShift = 7, // + Direction id, 6 bits
    kSolid = 1u << 13,
    kCanOcclude = 1u << 14,
    kFluidSource = 1u << 15,
    kRequiresCorrectTool = 1u << 24,
    kCenterSupportUp = 1u << 25,   // isFaceSturdy(UP, SupportType.CENTER)
    kCenterSupportDown = 1u << 26, // isFaceSturdy(DOWN, SupportType.CENTER)
    kRigidSupportUp = 1u << 27,    // isFaceSturdy(UP, SupportType.RIGID): Block.canSupportRigidBlock
    kSignalSource = 1u << 28,      // BlockState.isSignalSource
    kRedstoneConductor = 1u << 29, // BlockState.isRedstoneConductor
    kUseShapeForLightOcclusion = 1u << 30, // BlockState.useShapeForLightOcclusion
    kFluidRandomlyTicking = 1u << 31,       // FluidState.isRandomlyTicking (lava)
};

void bootstrap();

inline BlockStateId defaultState(uint32_t block) { return gen::kBlocks[block].default_state; }
uint32_t blockOf(BlockStateId state);
inline bool is(BlockStateId state, uint32_t block) { return blockOf(state) == block; }
std::optional<uint32_t> byName(std::string_view name); // accepts "stone" or "minecraft:stone"

// Value of `property` for `state`, or empty if the block has no such property.
std::optional<std::string_view> getValue(BlockStateId state, std::string_view property);
bool hasProperty(BlockStateId state, std::string_view property);
// Returns `state` with `property` set to `value`; unchanged if the property/value doesn't exist.
BlockStateId setValue(BlockStateId state, std::string_view property, std::string_view value);
// Separate name on purpose: an overload taking bool would win over string_view for string literals.
inline BlockStateId setBool(BlockStateId state, std::string_view property, bool value) {
    return setValue(state, property, std::string_view(value ? "true" : "false"));
}
inline bool getBool(BlockStateId state, std::string_view property) { return getValue(state, property) == "true"; }

inline uint32_t flags(BlockStateId state) { return state < gen::kStateFlags.size() ? gen::kStateFlags[state] : 0; }
inline bool isAir(BlockStateId state) { return flags(state) & kAir; }
inline bool hasFluid(BlockStateId state) { return flags(state) & (kWater | kLava); }
inline bool isWaterSource(BlockStateId state) { return (flags(state) & (kWater | kFluidSource)) == (kWater | kFluidSource); }
inline bool canBeReplaced(BlockStateId state) { return flags(state) & kReplaceable; }
inline bool isSolid(BlockStateId state) { return flags(state) & kSolid; }
inline bool isCollisionFullBlock(BlockStateId state) { return flags(state) & kFullCollision; }
inline bool isFaceSturdy(BlockStateId state, Direction face) {
    return flags(state) & (1u << (kSturdyFaceShift + static_cast<int>(face)));
}
inline int lightEmission(BlockStateId state) { return (flags(state) >> 16) & 15; }
inline int lightDampening(BlockStateId state) { return (flags(state) >> 20) & 15; }

inline const gen::BlockBehaviourInfo& behaviour(uint32_t block) { return gen::kBlockBehaviour[block]; }
// True if the block's Java class (or a superclass) has this simple name, e.g. "StairBlock".
bool isInstance(uint32_t block, std::string_view javaClass);

// Block tags as sent to the client (nested tags already expanded), e.g. "minecraft:dirt".
bool hasTag(uint32_t block, std::string_view tag);
inline bool stateHasTag(BlockStateId state, std::string_view tag) { return hasTag(blockOf(state), tag); }

} // namespace blocks
} // namespace mc
