// Pistons: ports of PistonBaseBlock, PistonHeadBlock, PistonStructureResolver and
// PistonMovingBlockEntity (net.minecraft.world.level.block.piston).
#include "world/level/block/piston.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/registries.hpp"
#include "generated/behaviour.hpp"
#include "server/level/server_level.hpp"
#include "world/level/block/block_behaviour.hpp"
#include "world/level/material/fluids.hpp"
#include "world/level/redstone/redstone.hpp"

namespace mc::block_behaviour::piston {

using namespace gen::block;
namespace bs = blocks;

namespace {

// PushReaction
constexpr int kPushPull = 0, kPush = 1, kPopped = 2, kImmoveable = 3, kIgnoreEntity = 4;
constexpr int kMaxPushDepth = 12;

Direction facing(BlockStateId s) { return direction::byName(bs::getValue(s, "facing").value_or("north")); }
int pushReaction(BlockStateId s) { return bs::behaviour(bs::blockOf(s)).pushReaction; }
bool isPiston(BlockStateId s) { return bs::is(s, PISTON) || bs::is(s, STICKY_PISTON); }

void removeBlock(ServerLevel& level, BlockPos pos, bool movedByPiston) { // Level.removeBlock
    level.setBlock(pos, fluids::legacyBlock(level.getBlockState(pos)), ServerLevel::kUpdateAll | (movedByPiston ? ServerLevel::kMovedByPiston : 0));
}

int32_t sound(std::string_view name) { return registries::id("minecraft:sound_event", name).value_or(-1); }

// PistonBaseBlock.isPushable
bool isPushable(ServerLevel& level, BlockStateId s, BlockPos pos, Direction dir, bool allowDestroyable, Direction connection) {
    if (pos.y < level.minY() || pos.y > level.maxY()) return false; // (no world border yet)
    if (bs::isAir(s)) return true;
    if (dir == Direction::Down && pos.y == level.minY()) return false;
    if (dir == Direction::Up && pos.y == level.maxY()) return false;
    uint32_t b = bs::blockOf(s);
    if (!isPiston(s)) {
        if (bs::behaviour(b).hardness == -1.0f) return false;
        switch (pushReaction(s)) {
        case kImmoveable: return false;
        case kPopped: return allowDestroyable;
        case kPush: return dir == connection;
        default: break;
        }
    } else if (bs::getBool(s, "extended")) {
        return false;
    }
    return bs::behaviour(b).blockEntity.empty(); // !hasBlockEntity
}

bool isStickyBlock(BlockStateId s) { return bs::is(s, SLIME_BLOCK) || bs::is(s, HONEY_BLOCK); }
bool canStickToEachOther(BlockStateId a, BlockStateId b) {
    if (bs::is(a, HONEY_BLOCK) && bs::is(b, SLIME_BLOCK)) return false;
    if (bs::is(a, SLIME_BLOCK) && bs::is(b, HONEY_BLOCK)) return false;
    return isStickyBlock(a) || isStickyBlock(b);
}

// PistonStructureResolver
struct Resolver {
    ServerLevel& level;
    BlockPos pistonPos;
    bool extending;
    BlockPos startPos;
    Direction pushDirection, pistonDirection;
    std::vector<BlockPos> toPush, toDestroy;

    Resolver(ServerLevel& l, BlockPos piston, Direction dir, bool ext) : level(l), pistonPos(piston), extending(ext), pistonDirection(dir) {
        pushDirection = ext ? dir : direction::opposite(dir);
        startPos = ext ? piston.relative(dir) : piston.relative(dir, 2);
    }
    int indexOf(BlockPos p) const {
        for (size_t i = 0; i < toPush.size(); i++)
            if (toPush[i] == p) return static_cast<int>(i);
        return -1;
    }
    bool resolve() {
        toPush.clear();
        toDestroy.clear();
        BlockStateId next = level.getBlockState(startPos);
        if (!isPushable(level, next, startPos, pushDirection, false, pistonDirection)) {
            if (extending && pushReaction(next) == kPopped) {
                toDestroy.push_back(startPos);
                return true;
            }
            return false;
        }
        if (!addBlockLine(startPos, pushDirection)) return false;
        for (size_t i = 0; i < toPush.size(); i++)
            if (isStickyBlock(level.getBlockState(toPush[i])) && !addBranchingBlocks(toPush[i])) return false;
        return true;
    }
    bool addBlockLine(BlockPos start, Direction dir) {
        BlockStateId next = level.getBlockState(start);
        if (bs::isAir(next)) return true;
        if (!isPushable(level, next, start, pushDirection, false, dir)) return true;
        if (start == pistonPos) return true;
        if (indexOf(start) >= 0) return true;
        int blockCount = 1;
        if (blockCount + static_cast<int>(toPush.size()) > kMaxPushDepth) return false;
        Direction back = direction::opposite(pushDirection);
        while (isStickyBlock(next)) {
            BlockPos p = start.relative(back, blockCount);
            BlockStateId previous = next;
            next = level.getBlockState(p);
            if (bs::isAir(next) || !canStickToEachOther(previous, next) || !isPushable(level, next, p, pushDirection, false, back) || p == pistonPos) break;
            if (++blockCount + static_cast<int>(toPush.size()) > kMaxPushDepth) return false;
        }
        int blocksAdded = 0;
        for (int i = blockCount - 1; i >= 0; i--) {
            toPush.push_back(start.relative(back, i));
            blocksAdded++;
        }
        for (int i = 1;; i++) {
            BlockPos p = start.relative(pushDirection, i);
            int collision = indexOf(p);
            if (collision > -1) {
                reorderListAtCollision(blocksAdded, collision);
                for (int j = 0; j <= collision + blocksAdded; j++)
                    if (isStickyBlock(level.getBlockState(toPush[j])) && !addBranchingBlocks(toPush[j])) return false;
                return true;
            }
            next = level.getBlockState(p);
            if (bs::isAir(next)) return true;
            if (!isPushable(level, next, p, pushDirection, true, pushDirection) || p == pistonPos) return false;
            if (pushReaction(next) == kPopped) {
                toDestroy.push_back(p);
                return true;
            }
            if (static_cast<int>(toPush.size()) >= kMaxPushDepth) return false;
            toPush.push_back(p);
            blocksAdded++;
        }
    }
    void reorderListAtCollision(int blocksAdded, int collision) {
        std::vector<BlockPos> head(toPush.begin(), toPush.begin() + collision);
        std::vector<BlockPos> lastLine(toPush.end() - blocksAdded, toPush.end());
        std::vector<BlockPos> collisionToLine(toPush.begin() + collision, toPush.end() - blocksAdded);
        toPush = head;
        toPush.insert(toPush.end(), lastLine.begin(), lastLine.end());
        toPush.insert(toPush.end(), collisionToLine.begin(), collisionToLine.end());
    }
    bool addBranchingBlocks(BlockPos from) {
        BlockStateId fromState = level.getBlockState(from);
        for (Direction d : direction::kAll) {
            if (direction::axis(d) == direction::axis(pushDirection)) continue;
            BlockPos n = from.relative(d);
            if (canStickToEachOther(level.getBlockState(n), fromState) && !addBlockLine(n, d)) return false;
        }
        return true;
    }
};

// PistonBaseBlock.getNeighborSignal: powered from any side but the front, or quasi-connected
// from the block above.
bool neighborSignal(ServerLevel& level, BlockPos pos, Direction push) {
    for (Direction d : direction::kAll)
        if (d != push && redstone::hasSignal(level, pos.relative(d), d)) return true;
    if (redstone::hasSignal(level, pos, Direction::Down)) return true;
    BlockPos above = pos.above();
    for (Direction d : direction::kAll)
        if (d != Direction::Down && redstone::hasSignal(level, above.relative(d), d)) return true;
    return false;
}

// Java's HashMap<BlockPos, BlockState> iteration order (deleteAfterMove holds at most 12 entries,
// so the table stays at 16 buckets): by bucket, then in insertion order.
int hashBucket(BlockPos p, int buckets) {
    int32_t h = static_cast<int32_t>((static_cast<uint32_t>(p.y) + static_cast<uint32_t>(p.z) * 31u) * 31u + static_cast<uint32_t>(p.x));
    return (h ^ static_cast<int32_t>(static_cast<uint32_t>(h) >> 16)) & (buckets - 1);
}

BlockStateId movingPistonState(Direction dir, bool sticky) {
    BlockStateId s = bs::setValue(bs::defaultState(MOVING_PISTON), "facing", direction::name(dir));
    return bs::setValue(s, "type", sticky ? "sticky" : "normal");
}

// PistonBaseBlock.moveBlocks
bool moveBlocks(ServerLevel& level, BlockPos pistonPos, Direction dir, bool extending, bool sticky) {
    BlockPos armPos = pistonPos.relative(dir);
    if (!extending && bs::is(level.getBlockState(armPos), PISTON_HEAD))
        level.setBlock(armPos, bs::defaultState(AIR), ServerLevel::kSkipBlockEntitySideEffects | ServerLevel::kUpdateKnownShape | ServerLevel::kUpdateInvisible);
    Resolver resolver(level, pistonPos, dir, extending);
    if (!resolver.resolve()) return false;
    struct Entry {
        BlockPos pos;
        BlockStateId state;
        bool removed = false;
    };
    std::vector<Entry> deleteAfterMove; // (a HashMap in vanilla; see hashBucket)
    auto removeEntry = [&](BlockPos p) {
        for (auto& e : deleteAfterMove)
            if (e.pos == p) e.removed = true;
    };
    const auto& toPush = resolver.toPush;
    std::vector<BlockStateId> toPushShapes;
    for (BlockPos p : toPush) {
        BlockStateId s = level.getBlockState(p);
        toPushShapes.push_back(s);
        deleteAfterMove.push_back({p, s});
    }
    const auto& toDestroy = resolver.toDestroy;
    std::vector<BlockStateId> toUpdate;
    Direction pushDirection = extending ? dir : direction::opposite(dir);
    for (int i = static_cast<int>(toDestroy.size()) - 1; i >= 0; i--) {
        BlockPos p = toDestroy[i];
        BlockStateId s = level.getBlockState(p);
        LevelChunk* chunk = level.getChunkIfLoaded({p.x >> 4, p.z >> 4});
        auto* be = chunk ? chunk->blockEntityAt(p.x, p.y, p.z) : nullptr;
        level.dropResources(s, p, be && be->blockAtLoad == bs::blockOf(s) ? &be->tag : nullptr, nullptr, nullptr);
        level.setBlock(p, bs::defaultState(AIR), ServerLevel::kUpdateKnownShape | ServerLevel::kUpdateClients);
        toUpdate.push_back(s);
    }
    constexpr int kMovingFlags = ServerLevel::kSkipBlockEntitySideEffects | ServerLevel::kMovedByPiston | ServerLevel::kUpdateInvisible; // 324
    for (int i = static_cast<int>(toPush.size()) - 1; i >= 0; i--) {
        BlockPos p = toPush[i];
        BlockStateId s = level.getBlockState(p);
        p = p.relative(pushDirection);
        removeEntry(p);
        BlockStateId moving = bs::setValue(bs::defaultState(MOVING_PISTON), "facing", direction::name(dir));
        level.setBlock(p, moving, kMovingFlags);
        level.setMovingPiston(p, {toPushShapes[i], dir, extending, false});
        toUpdate.push_back(s);
    }
    if (extending) {
        BlockStateId head = bs::setValue(bs::setValue(bs::defaultState(PISTON_HEAD), "facing", direction::name(dir)), "type", sticky ? "sticky" : "normal");
        removeEntry(armPos);
        level.setBlock(armPos, movingPistonState(dir, sticky), kMovingFlags);
        level.setMovingPiston(armPos, {head, dir, true, true});
    }
    std::vector<const Entry*> order;
    for (int b = 0; b < 16; b++)
        for (const auto& e : deleteAfterMove)
            if (!e.removed && hashBucket(e.pos, 16) == b) order.push_back(&e);
    BlockStateId air = bs::defaultState(AIR);
    for (const Entry* e : order) level.setBlock(e->pos, air, ServerLevel::kMovedByPiston | ServerLevel::kUpdateKnownShape | ServerLevel::kUpdateClients); // 82
    for (const Entry* e : order) {
        updateIndirectNeighbourShapes(level, e->pos, e->state, ServerLevel::kUpdateClients, 0);
        level.updateNeighbourShapes(e->pos, air, ServerLevel::kUpdateClients, 0);
    }
    size_t updateIndex = 0;
    for (int i = static_cast<int>(toDestroy.size()) - 1; i >= 0; i--) {
        BlockStateId s = toUpdate[updateIndex++];
        BlockPos p = toDestroy[i];
        affectNeighborsAfterRemoval(level, p, s, false);
        updateIndirectNeighbourShapes(level, p, s, ServerLevel::kUpdateClients, 0);
        level.updateNeighborsAt(p, bs::blockOf(s));
    }
    for (int i = static_cast<int>(toPush.size()) - 1; i >= 0; i--) level.updateNeighborsAt(toPush[i], bs::blockOf(toUpdate[updateIndex++]));
    if (extending) level.updateNeighborsAt(armPos, PISTON_HEAD);
    return true;
}

// ---------------------------------------------------------------- entities in the way

// PistonMath.getMovementArea
AABB movementArea(const AABB& a, Direction d, double amount) {
    double delta = amount * (d == Direction::East || d == Direction::Up || d == Direction::South ? 1 : -1);
    double mn = std::min(delta, 0.0), mx = std::max(delta, 0.0);
    switch (d) {
    case Direction::West: return {a.minX + mn, a.minY, a.minZ, a.minX + mx, a.maxY, a.maxZ};
    case Direction::East: return {a.maxX + mn, a.minY, a.minZ, a.maxX + mx, a.maxY, a.maxZ};
    case Direction::Down: return {a.minX, a.minY + mn, a.minZ, a.maxX, a.minY + mx, a.maxZ};
    case Direction::North: return {a.minX, a.minY, a.minZ + mn, a.maxX, a.maxY, a.minZ + mx};
    case Direction::South: return {a.minX, a.minY, a.maxZ + mn, a.maxX, a.maxY, a.maxZ + mx};
    default: return {a.minX, a.maxY + mn, a.minZ, a.maxX, a.maxY + mx, a.maxZ};
    }
}

double movementOutOf(const AABB& outside, Direction d, const AABB& a) { // PistonMovingBlockEntity.getMovement
    switch (d) {
    case Direction::East: return outside.maxX - a.minX;
    case Direction::West: return a.maxX - outside.minX;
    case Direction::Down: return a.maxY - outside.minY;
    case Direction::South: return outside.maxZ - a.minZ;
    case Direction::North: return a.maxZ - outside.minZ;
    default: return outside.maxY - a.minY;
    }
}

// PistonMovingBlockEntity.moveCollidedEntities, for the entities we simulate (items, experience
// orbs, falling blocks). Players move themselves.
void moveCollidedEntities(ServerLevel& level, BlockPos pos, float newProgress, const ServerLevel::MovingPiston& p) {
    Direction movement = p.extending ? p.direction : direction::opposite(p.direction);
    double deltaProgress = newProgress - p.progress;
    BlockStateId collisionState = p.movedState; // getCollisionRelatedBlockState
    if (!p.extending && p.source && isPiston(p.movedState)) {
        collisionState = bs::setValue(bs::defaultState(PISTON_HEAD), "short", p.progress > 0.25f ? "true" : "false");
        collisionState = bs::setValue(collisionState, "type", bs::is(p.movedState, STICKY_PISTON) ? "sticky" : "normal");
        collisionState = bs::setValue(collisionState, "facing", direction::name(facing(p.movedState)));
    }
    const auto& boxes = gen::kShapes[gen::kStateShape[collisionState]];
    if (boxes.empty()) return;
    double extended = p.extending ? p.progress - 1.0 : 1.0 - p.progress; // getExtendedProgress
    double ox = pos.x + extended * direction::stepX(p.direction), oy = pos.y + extended * direction::stepY(p.direction),
           oz = pos.z + extended * direction::stepZ(p.direction);
    AABB bounds{1, 1, 1, 0, 0, 0};
    for (const auto& b : boxes)
        bounds = {std::min(bounds.minX, static_cast<double>(b.minX)), std::min(bounds.minY, static_cast<double>(b.minY)),
                  std::min(bounds.minZ, static_cast<double>(b.minZ)), std::max(bounds.maxX, static_cast<double>(b.maxX)),
                  std::max(bounds.maxY, static_cast<double>(b.maxY)), std::max(bounds.maxZ, static_cast<double>(b.maxZ))};
    AABB moved = bounds.move(ox, oy, oz);
    AABB area = movementArea(moved, movement, deltaProgress);
    AABB search{std::min(area.minX, moved.minX), std::min(area.minY, moved.minY), std::min(area.minZ, moved.minZ),
                std::max(area.maxX, moved.maxX), std::max(area.maxY, moved.maxY), std::max(area.maxZ, moved.maxZ)};
    std::vector<Entity*> entities;
    for (const auto& e : level.entities())
        if (!e->removed && e->boundingBox().intersects(search)) entities.push_back(e.get());
    for (Entity* e : entities) {
        double delta = 0.0;
        for (const auto& b : boxes) {
            AABB box{b.minX + ox, b.minY + oy, b.minZ + oz, b.maxX + ox, b.maxY + oy, b.maxZ + oz};
            AABB moving = movementArea(box, movement, deltaProgress);
            AABB entityBox = e->boundingBox();
            if (moving.intersects(entityBox)) {
                delta = std::max(delta, movementOutOf(moving, movement, entityBox));
                if (delta >= deltaProgress) break;
            }
        }
        if (delta <= 0.0) continue;
        delta = std::min(delta, deltaProgress) + 0.01;
        e->moveByPiston(level, delta * direction::stepX(movement), delta * direction::stepY(movement), delta * direction::stepZ(movement));
    }
}

} // namespace

bool isPistonBase(uint32_t block) { return block == PISTON || block == STICKY_PISTON; }

void checkIfExtend(ServerLevel& level, BlockPos pos, BlockStateId s) {
    Direction dir = facing(s);
    bool extend = neighborSignal(level, pos, dir);
    uint32_t b = bs::blockOf(s);
    if (extend && !bs::getBool(s, "extended")) {
        if (Resolver(level, pos, dir, true).resolve()) level.blockEvent(pos, b, 0, static_cast<int>(dir));
    } else if (!extend && bs::getBool(s, "extended")) {
        BlockPos pushed = pos.relative(dir, 2);
        BlockStateId ps = level.getBlockState(pushed);
        int event = 1;
        if (bs::is(ps, MOVING_PISTON) && facing(ps) == dir)
            if (auto* mp = level.movingPiston(pushed); mp && mp->extending &&
                                                       (mp->progressO < 0.5f || level.gameTime() == mp->lastTicked || level.isHandlingTick()))
                event = 2;
        level.blockEvent(pos, b, event, static_cast<int>(dir));
    }
}

bool triggerEvent(ServerLevel& level, BlockPos pos, BlockStateId s, int b0, int b1) {
    Direction dir = facing(s);
    BlockStateId extendedState = bs::setBool(s, "extended", true);
    bool sticky = bs::is(s, STICKY_PISTON);
    bool extend = neighborSignal(level, pos, dir);
    if (extend && (b0 == 1 || b0 == 2)) {
        level.setBlock(pos, extendedState, ServerLevel::kUpdateClients);
        return false;
    }
    if (!extend && b0 == 0) return false;
    auto& random = level.levelRandom();
    if (b0 == 0) {
        if (!moveBlocks(level, pos, dir, true, sticky)) return false;
        level.setBlock(pos, extendedState, ServerLevel::kMovedByPiston | ServerLevel::kUpdateAll); // 67
        static const int32_t kExtend = sound("minecraft:block.piston.extend");
        level.playBlockSound(-1, pos, kExtend, 0.5f, random.nextFloat() * 0.25f + 0.6f);
    } else if (b0 == 1 || b0 == 2) {
        if (level.movingPiston(pos.relative(dir))) finalTick(level, pos.relative(dir));
        BlockStateId moving = movingPistonState(dir, sticky);
        level.setBlock(pos, moving, ServerLevel::kSkipBlockEntitySideEffects | ServerLevel::kUpdateKnownShape | ServerLevel::kUpdateInvisible); // 276
        BlockStateId base = bs::setValue(bs::defaultState(bs::blockOf(s)), "facing", direction::name(static_cast<Direction>(std::clamp(b1 & 7, 0, 5))));
        level.setMovingPiston(pos, {base, dir, false, true});
        level.updateNeighborsAt(pos, MOVING_PISTON);
        level.updateNeighbourShapes(pos, moving, ServerLevel::kUpdateClients, 0);
        if (sticky) {
            BlockPos two = pos.relative(dir, 2);
            BlockStateId ms = level.getBlockState(two);
            bool pistonPiece = false;
            if (bs::is(ms, MOVING_PISTON))
                if (auto* mp = level.movingPiston(two); mp && mp->direction == dir && mp->extending) {
                    finalTick(level, two);
                    pistonPiece = true;
                }
            if (!pistonPiece) {
                if (b0 != 1 || bs::isAir(ms) || !isPushable(level, ms, two, direction::opposite(dir), false, dir) ||
                    (pushReaction(ms) != kPushPull && !isPiston(ms)))
                    removeBlock(level, pos.relative(dir), false);
                else
                    moveBlocks(level, pos, dir, false, sticky);
            }
        } else {
            removeBlock(level, pos.relative(dir), false);
        }
        static const int32_t kContract = sound("minecraft:block.piston.contract");
        level.playBlockSound(-1, pos, kContract, 0.5f, random.nextFloat() * 0.15f + 0.6f);
    }
    return true;
}

// ---------------------------------------------------------------- PistonHeadBlock

namespace {
bool isFittingBase(BlockStateId arm, BlockStateId base) {
    uint32_t want = bs::getValue(arm, "type") == "sticky" ? STICKY_PISTON : PISTON;
    return bs::is(base, want) && bs::getBool(base, "extended") && facing(base) == facing(arm);
}
} // namespace

bool headCanSurvive(ServerLevel& level, BlockPos pos, BlockStateId s) {
    BlockStateId base = level.getBlockState(pos.relative(direction::opposite(facing(s))));
    return isFittingBase(s, base) || (bs::is(base, MOVING_PISTON) && facing(base) == facing(s));
}

void headNeighborChanged(ServerLevel& level, BlockPos pos, BlockStateId s, uint32_t sourceBlock) {
    if (headCanSurvive(level, pos, s)) level.neighborChanged(pos.relative(direction::opposite(facing(s))), sourceBlock);
}

void headAffectNeighborsAfterRemoval(ServerLevel& level, BlockPos pos, BlockStateId s) {
    BlockPos basePos = pos.relative(direction::opposite(facing(s)));
    if (isFittingBase(s, level.getBlockState(basePos))) level.destroyBlock(basePos, true);
}

// ---------------------------------------------------------------- PistonMovingBlockEntity

void finalTick(ServerLevel& level, BlockPos pos) {
    ServerLevel::MovingPiston* mp = level.movingPiston(pos);
    if (!mp || mp->progressO >= 1.0f) return;
    ServerLevel::MovingPiston p = *mp;
    level.removeBlockEntity(pos);
    if (!bs::is(level.getBlockState(pos), MOVING_PISTON)) return;
    BlockStateId n = p.source ? bs::defaultState(AIR) : updateFromNeighbourShapes(p.movedState, level, pos);
    level.setBlock(pos, n);
    level.neighborChanged(pos, bs::blockOf(n));
}

void tickMoving(ServerLevel& level, BlockPos pos) {
    ServerLevel::MovingPiston* mp = level.movingPiston(pos);
    if (!mp) return;
    mp->lastTicked = level.gameTime();
    mp->progressO = mp->progress;
    if (mp->progressO >= 1.0f) {
        ServerLevel::MovingPiston p = *mp;
        level.removeBlockEntity(pos);
        if (!bs::is(level.getBlockState(pos), MOVING_PISTON)) return;
        BlockStateId n = updateFromNeighbourShapes(p.movedState, level, pos);
        if (bs::isAir(n)) {
            level.setBlock(pos, p.movedState, ServerLevel::kSkipBlockEntitySideEffects | ServerLevel::kMovedByPiston | ServerLevel::kUpdateKnownShape |
                                                  ServerLevel::kUpdateInvisible); // 340
            // Block.updateOrDestroy(movedState, newState, level, pos, 3): it can't stay there
            if (n != p.movedState) level.destroyBlock(pos, true);
        } else {
            if (bs::getBool(n, "waterlogged")) n = bs::setBool(n, "waterlogged", false);
            level.setBlock(pos, n, ServerLevel::kMovedByPiston | ServerLevel::kUpdateAll); // 67
            level.neighborChanged(pos, bs::blockOf(n));
        }
        return;
    }
    float newProgress = mp->progress + 0.5f;
    ServerLevel::MovingPiston copy = *mp;
    moveCollidedEntities(level, pos, newProgress, copy);
    if ((mp = level.movingPiston(pos))) {
        mp->progress = std::min(newProgress, 1.0f);
        level.saveMovingPiston(pos);
    }
}

} // namespace mc::block_behaviour::piston
