#pragma once

#include <array>
#include <optional>

#include "core/direction.hpp"
#include "world/item/item_stack.hpp"
#include "util/legacy_random.hpp"
#include "world/level/block/block_state.hpp"

namespace mc {

class ServerLevel;

// Port of BlockPlaceContext / UseOnContext.
struct PlaceContext {
    ServerLevel& level;
    BlockPos hitPos;      // block that was clicked
    Direction face;       // clicked face
    double hitX, hitY, hitZ; // absolute click location
    float yRot, xRot;     // player rotation
    bool secondaryUse;    // sneaking
    ItemStack item;
    bool creative = false; // the placing player can use game master blocks (op + creative; no ops yet)

    bool replaceClicked = false;
    BlockPos pos;         // where the block goes (getClickedPos)

    Direction horizontalDirection() const { return direction::fromYRot(yRot); }
    Direction nearestLookingDirection() const { return direction::orderedByNearest(yRot, xRot)[0]; }
    std::array<Direction, 6> nearestLookingDirections() const;
    double clickYWithinBlock() const { return hitY - pos.y; }
};

// Ports of the per-block behaviour the server needs for building. Dispatch is by the block's
// Java class (from data/extracted.json), mirroring the overrides in net.minecraft.world.level.block.
namespace block_behaviour {

// BlockBehaviour.canBeReplaced(state, context)
bool canBeReplaced(BlockStateId state, const PlaceContext& ctx);
// BlockItem.updatePlacementContext (ScaffoldingBlockItem: clicking scaffolding extends it). May move
// ctx.pos; false when the item can't be placed at all.
bool updatePlacementContext(PlaceContext& ctx);
// Block.getStateForPlacement; empty when placement isn't possible.
std::optional<BlockStateId> getStateForPlacement(uint32_t block, const PlaceContext& ctx);
// BlockBehaviour.canSurvive
bool canSurvive(BlockStateId state, ServerLevel& level, BlockPos pos);
// Block.withPropertiesOf: `block`'s default state with every property it shares with `from`.
BlockStateId withPropertiesOf(uint32_t block, BlockStateId from);
// Block.updateFromNeighbourShapes: `state` at `pos` after updateShape towards every neighbour.
BlockStateId updateFromNeighbourShapes(BlockStateId state, ServerLevel& level, BlockPos pos);
// BlockBehaviour.onPlace, after any block change (`old` is what was there before).
void onPlace(ServerLevel& level, BlockPos pos, BlockStateId state, BlockStateId old);
// Fallable.onLand / onBrokenAfterFall: a falling block became a block again (concrete powder
// in water hardens, anvils clang), or broke.
void onLand(ServerLevel& level, BlockPos pos, BlockStateId state, BlockStateId replaced);
void onBrokenAfterFall(ServerLevel& level, BlockPos pos, BlockStateId state);
// BaseFireBlock.getState / canBePlacedAt: fire or soul fire for `pos`, and whether it may go there.
BlockStateId fireStateFor(ServerLevel& level, BlockPos pos);
bool fireCanBePlacedAt(ServerLevel& level, BlockPos pos);
// BlockBehaviour.updateShape: new state for `state` at `pos` after its neighbour changed.
BlockStateId updateShape(BlockStateId state, ServerLevel& level, BlockPos pos, Direction toNeighbour, BlockPos neighbourPos,
                         BlockStateId neighbourState);
// BlockBehaviour.updateIndirectNeighbourShapes (redstone wire: wires one step up or down).
void updateIndirectNeighbourShapes(ServerLevel& level, BlockPos pos, BlockStateId state, int flags, int depth);
// CampfireBlock.isSmokeyPos: smoke from a lit campfire up to 5 blocks below reaches `pos`.
bool isSmokeyPos(ServerLevel& level, BlockPos pos);
// BlockBehaviour.randomTick for a state flagged kRandomlyTicking, and FluidState.randomTick
// (lava setting things on fire) for one flagged kFluidRandomlyTicking.
void randomTick(ServerLevel& level, BlockPos pos, BlockStateId state, LegacyRandom& random);
void fluidRandomTick(ServerLevel& level, BlockPos pos, BlockStateId state, LegacyRandom& random);
// BoneMealItem.growCrop for the block at `pos` (BonemealableBlock: isValidBonemealTarget, then
// isBonemealSuccess -> performBonemeal). False if bone meal does nothing there (not consumed).
bool growCrop(ServerLevel& level, BlockPos pos);
// Whether the block's scheduled tick (BlockBehaviour.tick) is ported, and the tick itself.
bool hasTick(uint32_t block);
void tick(ServerLevel& level, BlockPos pos, BlockStateId state);
// Block.setPlacedBy and onPlace: the other half of doors, beds and tall plants; rail shapes.
void setPlacedBy(ServerLevel& level, BlockPos pos, BlockStateId state);
// BlockBehaviour.useWithoutItem. Returns true if the click was consumed (door toggled, ...).
// `playerId` doesn't hear the sounds (its client plays them itself); `mayBuild`: the player's
// abilities allow building (not adventure mode), needed to adjust repeaters and the like.
bool useWithoutItem(ServerLevel& level, BlockPos pos, BlockStateId state, float playerYRot, int playerId, bool mayBuild);
// BlockBehaviour.attack: a survival player starts hitting the block (note blocks play).
void attack(ServerLevel& level, BlockPos pos, BlockStateId state);
// BlockBehaviour.triggerEvent for a queued Level.blockEvent; true if clients should get it.
bool triggerEvent(ServerLevel& level, BlockPos pos, BlockStateId state, int b0, int b1);

} // namespace block_behaviour
} // namespace mc
