#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "core/direction.hpp"
#include "generated/behaviour.hpp"
#include "world/level/block/block_state.hpp"

namespace mc {

class ServerLevel;

// Fluids (net.minecraft.world.level.material): FluidState, FlowingFluid with WaterFluid and
// LavaFluid, and the fluid side of LiquidBlock / LiquidBlockContainer.
namespace fluids {

// minecraft:fluid ids
enum Type : uint8_t { kEmpty = 0, kFlowingWater = 1, kWater = 2, kFlowingLava = 3, kLava = 4 };

std::string_view name(uint8_t type); // "minecraft:flowing_water", ...
std::optional<uint8_t> byName(std::string_view name);
// FluidTags.WATER / LAVA, and Fluid.isSame (water and flowing water are the same fluid).
inline bool isWater(uint8_t t) { return t == kWater || t == kFlowingWater; }
inline bool isLava(uint8_t t) { return t == kLava || t == kFlowingLava; }
inline bool isSame(uint8_t a, uint8_t b) { return (isWater(a) && isWater(b)) || (isLava(a) && isLava(b)); }

// net.minecraft.world.level.material.FluidState
struct FluidState {
    uint8_t type = kEmpty;
    uint8_t amount = 0;  // getAmount: 8 for sources, LEVEL (1-8) for flowing fluid
    bool falling = false; // FlowingFluid.FALLING
    bool isEmpty() const { return type == kEmpty; }
    bool isSource() const { return type == kWater || type == kLava; }
    bool operator==(const FluidState&) const = default;
};

// What tools/extractor recorded about a block state (gen::kStateFluid): its fluid, and vanilla's
// answers to the per-state questions fluids ask about it.
namespace info {
inline uint32_t of(BlockStateId s) { return s < gen::kStateFluid.size() ? gen::kStateFluid[s] : 0; }
inline int index(uint8_t fluid) { return fluid - 1; } // bit index of a fluid in the per-fluid sets
inline bool canHoldAnyFluid(BlockStateId s) { return of(s) >> 8 & 1; }      // FlowingFluid.canHoldAnyFluid
inline bool collisionIsBlock(BlockStateId s) { return of(s) >> 9 & 1; }     // collision shape == Shapes.block()
inline bool isLiquidBlockContainer(BlockStateId s) { return of(s) >> 10 & 1; }
inline bool isBucketPickup(BlockStateId s) { return of(s) >> 11 & 1; }
inline bool isSimpleWaterlogged(BlockStateId s) { return of(s) >> 20 & 1; }
// LiquidBlockContainer.canPlaceLiquid / BlockState.canBeReplaced(Fluid) / FlowingFluid.canHoldFluid
// and canHoldSpecificFluid, for a fluid other than empty.
inline bool canPlaceLiquid(BlockStateId s, uint8_t f) { return f != kEmpty && (of(s) >> (12 + index(f)) & 1); }
inline bool canBeReplacedBy(BlockStateId s, uint8_t f) { return f != kEmpty && (of(s) >> (16 + index(f)) & 1); }
inline bool canHoldFluid(BlockStateId s, uint8_t f) { return f != kEmpty && (of(s) >> (21 + index(f)) & 1); }
inline bool canHoldSpecificFluid(BlockStateId s, uint8_t f) {
    return f == kEmpty ? !isLiquidBlockContainer(s) : (of(s) >> (25 + index(f)) & 1);
}
} // namespace info

// BlockState.getFluidState
inline FluidState stateOf(BlockStateId s) {
    uint32_t i = info::of(s);
    return {static_cast<uint8_t>(i & 7), static_cast<uint8_t>(i >> 3 & 15), (i >> 7 & 1) != 0};
}
// FluidState.createLegacyBlock: the block a fluid stands for on its own (air when empty).
BlockStateId legacyBlock(FluidState fluid);
inline BlockStateId legacyBlock(BlockStateId state) { return legacyBlock(stateOf(state)); }
// FlowingFluid.getSource(false) / getFlowing(amount, falling) for `type`'s fluid.
FluidState source(uint8_t type);
FluidState flowing(uint8_t type, int amount, bool falling);

// Fluid.getTickDelay in `level` (water 5; lava 30, or 10 where lava is fast).
int tickDelay(ServerLevel& level, uint8_t type);
// FluidState.getHeight / getOwnHeight
float ownHeight(FluidState fluid);
float height(ServerLevel& level, BlockPos pos, FluidState fluid);

// FlowingFluid.tick: a scheduled fluid tick at `pos` for the fluid that's there.
void tick(ServerLevel& level, BlockPos pos, BlockStateId state, FluidState fluid);

// LiquidBlock: onPlace and neighborChanged schedule the fluid if it should flow (lava next to
// water turns to obsidian or cobblestone instead); updateShape schedules it next to sources.
void liquidOnPlace(ServerLevel& level, BlockPos pos, BlockStateId state);
void liquidNeighborChanged(ServerLevel& level, BlockPos pos, BlockStateId state);
// BlockBehaviour.updateShape's fluid part: LiquidBlock, and every block holding water
// (waterlogged blocks, kelp, seagrass, bubble columns) schedules its fluid.
void scheduleOnShapeUpdate(ServerLevel& level, BlockPos pos, BlockStateId state, Direction toNeighbour, BlockStateId neighbour);
// LiquidBlock.tick: water over soul sand or magma becomes a bubble column (BubbleColumnBlock.updateColumn).
void liquidTick(ServerLevel& level, BlockPos pos, BlockStateId state);

// BubbleColumnBlock: tick (the column follows what's below), updateShape (schedules that) and canSurvive.
void bubbleColumnTick(ServerLevel& level, BlockPos pos, BlockStateId state);
void bubbleColumnShapeUpdate(ServerLevel& level, BlockPos pos, BlockStateId state, Direction toNeighbour, BlockStateId neighbour);
bool bubbleColumnCanSurvive(ServerLevel& level, BlockPos pos);

// LiquidBlockContainer.placeLiquid: waterlog the block (campfires go out, candles too). False
// if it can't take `fluid`.
bool placeLiquid(ServerLevel& level, BlockPos pos, BlockStateId state, FluidState fluid);

// BucketPickup.pickupBlock: takes the fluid (or powder snow) out of the block at `pos` and returns
// the filled bucket's item id, or nothing. `pickupSound` gets BucketPickup.getPickupSound.
std::optional<std::string_view> pickupBlock(ServerLevel& level, BlockPos pos, BlockStateId state, std::string_view* pickupSound = nullptr);

} // namespace fluids
} // namespace mc
