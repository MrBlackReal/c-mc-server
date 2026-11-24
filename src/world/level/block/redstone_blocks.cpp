// The redstone halves of block behaviour: power sources (wire, torches, diodes, levers, buttons,
// pressure plates, observers, ...) and the blocks that react to a signal (lamps, doors, note blocks,
// hoppers, rails, ...). Ports of their onPlace / affectNeighborsAfterRemoval / neighborChanged /
// tick / useWithoutItem overrides in net.minecraft.world.level.block.
#include "world/level/block/redstone_blocks.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <random>
#include <string_view>
#include <string>
#include <unordered_map>

#include "core/registries.hpp"
#include "server/level/server_level.hpp"
#include "server/level/server_player.hpp"
#include "world/level/block/block_behaviour.hpp"
#include "world/level/block/block_behaviour_internal.hpp"
#include "world/level/material/fluids.hpp"
#include "world/level/redstone/redstone.hpp"
#include "util/mth.hpp"

namespace mc::block_behaviour::redstone_blocks {

using namespace gen::block;
namespace bs = blocks;
namespace rs = redstone;

namespace {

// TickPriority
constexpr int kExtremelyHigh = -3, kVeryHigh = -2, kHigh = -1, kNormal = 0;

bool isA(uint32_t block, std::string_view cls) { return bs::isInstance(block, cls); }
Direction dirValue(BlockStateId s, std::string_view prop = "facing") { return direction::byName(bs::getValue(s, prop).value_or("north")); }
int intValue(BlockStateId s, std::string_view prop) { return detail::intValue(s, prop); }
bool conductor(BlockStateId s) { return bs::flags(s) & bs::kRedstoneConductor; }
int32_t sound(std::string_view name) { return registries::id("minecraft:sound_event", name).value_or(-1); }
float nextFloat(ServerLevel& level) { return std::uniform_real_distribution<float>(0.0f, 1.0f)(level.random()); }

// Level.removeBlock: the block's fluid (or air) takes its place.
void removeBlock(ServerLevel& level, BlockPos pos, bool movedByPiston) {
    level.setBlock(pos, fluids::legacyBlock(level.getBlockState(pos)), ServerLevel::kUpdateAll | (movedByPiston ? ServerLevel::kMovedByPiston : 0));
}
// Block.dropResources(state, level, pos[, blockEntity])
void dropResources(ServerLevel& level, BlockPos pos, BlockStateId state) {
    LevelChunk* chunk = level.getChunkIfLoaded({pos.x >> 4, pos.z >> 4});
    auto* be = chunk ? chunk->blockEntityAt(pos.x, pos.y, pos.z) : nullptr;
    level.dropResources(state, pos, be && be->blockAtLoad == bs::blockOf(state) ? &be->tag : nullptr, nullptr, nullptr);
}
nbt::Compound* blockEntity(ServerLevel& level, BlockPos pos) {
    LevelChunk* chunk = level.getChunkIfLoaded({pos.x >> 4, pos.z >> 4});
    auto* be = chunk ? chunk->blockEntityAt(pos.x, pos.y, pos.z) : nullptr;
    return be && be->blockAtLoad == bs::blockOf(level.getBlockState(pos)) ? &be->tag : nullptr;
}

// FaceAttachedHorizontalDirectionalBlock.getConnectedDirection
Direction connectedDirection(BlockStateId s) {
    auto face = bs::getValue(s, "face");
    if (face == "ceiling") return Direction::Down;
    if (face == "floor") return Direction::Up;
    return dirValue(s);
}

// ---------------------------------------------------------------- redstone wire

// Java's HashSet<BlockPos> iteration order for a small set (a 16-bucket table, BlockPos.hashCode
// spread by HashMap.hash): DefaultRedstoneWireEvaluator updates the wire and its neighbours in it.
std::array<BlockPos, 7> hashSetOrder(const std::array<BlockPos, 7>& inserted) {
    auto bucket = [](BlockPos p) {
        int32_t h = static_cast<int32_t>((static_cast<uint32_t>(p.y) + static_cast<uint32_t>(p.z) * 31u) * 31u + static_cast<uint32_t>(p.x));
        return (h ^ static_cast<int32_t>(static_cast<uint32_t>(h) >> 16)) & 15;
    };
    std::array<BlockPos, 7> out;
    size_t n = 0;
    for (int b = 0; b < 16; b++)
        for (const BlockPos& p : inserted)
            if (bucket(p) == b) out[n++] = p;
    return out;
}

// RedstoneWireEvaluator.getIncomingWireSignal
int incomingWireSignal(ServerLevel& level, BlockPos pos) {
    auto wireSignal = [](BlockStateId s) { return bs::is(s, REDSTONE_WIRE) ? intValue(s, "power") : 0; };
    int signal = 0;
    for (Direction d : direction::kHorizontalPlane) {
        BlockPos np = pos.relative(d);
        BlockStateId ns = level.getBlockState(np);
        signal = std::max(signal, wireSignal(ns));
        if (conductor(ns) && !conductor(level.getBlockState(pos.above()))) signal = std::max(signal, wireSignal(level.getBlockState(np.above())));
        else if (!conductor(ns)) signal = std::max(signal, wireSignal(level.getBlockState(np.below())));
    }
    return std::max(0, signal - 1);
}

// DefaultRedstoneWireEvaluator.updatePowerStrength
void wireUpdatePower(ServerLevel& level, BlockPos pos, BlockStateId state) {
    int blockSignal = rs::wireBlockSignal(level, pos);
    int target = blockSignal == 15 ? 15 : std::max(blockSignal, incomingWireSignal(level, pos));
    if (intValue(state, "power") == target) return;
    if (level.getBlockState(pos) == state) level.setBlock(pos, bs::setValue(state, "power", std::to_string(target)), ServerLevel::kUpdateClients);
    std::array<BlockPos, 7> set{pos};
    for (size_t i = 0; i < 6; i++) set[i + 1] = pos.relative(direction::kAll[i]);
    for (BlockPos p : hashSetOrder(set)) level.updateNeighborsAt(p, REDSTONE_WIRE);
}

void wireCheckCornerChangeAt(ServerLevel& level, BlockPos pos) {
    if (!bs::is(level.getBlockState(pos), REDSTONE_WIRE)) return;
    level.updateNeighborsAt(pos, REDSTONE_WIRE);
    for (Direction d : direction::kAll) level.updateNeighborsAt(pos.relative(d), REDSTONE_WIRE);
}

void wireUpdateNeighborsOfNeighboringWires(ServerLevel& level, BlockPos pos) {
    for (Direction d : direction::kHorizontalPlane) wireCheckCornerChangeAt(level, pos.relative(d));
    for (Direction d : direction::kHorizontalPlane) {
        BlockPos target = pos.relative(d);
        if (conductor(level.getBlockState(target))) wireCheckCornerChangeAt(level, target.above());
        else wireCheckCornerChangeAt(level, target.below());
    }
}

// ---------------------------------------------------------------- diodes (DiodeBlock, RepeaterBlock, ComparatorBlock)

bool isComparator(uint32_t b) { return isA(b, "ComparatorBlock"); }

int diodeDelay(BlockStateId s) { return isComparator(bs::blockOf(s)) ? 2 : intValue(s, "delay") * 2; }

int diodeInputSignal(ServerLevel& level, BlockPos pos, BlockStateId s) {
    Direction dir = dirValue(s);
    BlockPos target = pos.relative(dir);
    int input = rs::getSignal(level, target, dir);
    if (input < 15) {
        BlockStateId ts = level.getBlockState(target);
        input = std::max(input, bs::is(ts, REDSTONE_WIRE) ? intValue(ts, "power") : 0);
    }
    if (!isComparator(bs::blockOf(s))) return input;
    // ComparatorBlock.getInputSignal: what a block in front (or behind a conductor in front) measures.
    BlockStateId ts = level.getBlockState(target);
    if (rs::hasAnalogOutputSignal(ts)) return rs::getAnalogOutputSignal(level, target, ts, direction::opposite(dir));
    if (input < 15 && conductor(ts)) {
        target = target.relative(dir);
        ts = level.getBlockState(target);
        if (rs::hasAnalogOutputSignal(ts)) input = rs::getAnalogOutputSignal(level, target, ts, direction::opposite(dir)); // (no item frames)
    }
    return input;
}

int diodeAlternateSignal(ServerLevel& level, BlockPos pos, BlockStateId s) {
    Direction dir = dirValue(s);
    Direction cw = direction::clockWise(dir), ccw = direction::counterClockWise(dir);
    bool diodesOnly = !isComparator(bs::blockOf(s)); // RepeaterBlock.sideInputDiodesOnly
    return std::max(rs::getControlInputSignal(level, pos.relative(cw), cw, diodesOnly), rs::getControlInputSignal(level, pos.relative(ccw), ccw, diodesOnly));
}

bool diodeIsLocked(ServerLevel& level, BlockPos pos, BlockStateId s) {
    return isA(bs::blockOf(s), "RepeaterBlock") && diodeAlternateSignal(level, pos, s) > 0;
}

bool diodeShouldTurnOn(ServerLevel& level, BlockPos pos, BlockStateId s) {
    int input = diodeInputSignal(level, pos, s);
    if (!isComparator(bs::blockOf(s))) return input > 0;
    if (input == 0) return false;
    int side = diodeAlternateSignal(level, pos, s);
    return input > side || (input == side && bs::getValue(s, "mode") == "compare");
}

bool diodeShouldPrioritize(ServerLevel& level, BlockPos pos, BlockStateId s) {
    Direction d = direction::opposite(dirValue(s));
    BlockStateId other = level.getBlockState(pos.relative(d));
    return rs::isDiode(other) && dirValue(other) != d;
}

// DiodeBlock/ObserverBlock.updateNeighborsInFront
void updateNeighborsInFront(ServerLevel& level, BlockPos pos, BlockStateId s) {
    Direction dir = dirValue(s);
    BlockPos behind = pos.relative(direction::opposite(dir));
    level.neighborChanged(behind, bs::blockOf(s));
    level.updateNeighborsAtExceptFromFacing(behind, bs::blockOf(s), dir);
}

int comparatorOutput(ServerLevel& level, BlockPos pos, BlockStateId s) { // calculateOutputSignal
    int input = diodeInputSignal(level, pos, s);
    if (input == 0) return 0;
    int side = diodeAlternateSignal(level, pos, s);
    if (side > input) return 0;
    return bs::getValue(s, "mode") == "subtract" ? input - side : input;
}

void comparatorRefreshOutputState(ServerLevel& level, BlockPos pos, BlockStateId s) {
    int out = comparatorOutput(level, pos, s);
    int old = 0;
    if (nbt::Compound* be = blockEntity(level, pos)) {
        old = be->getInt("OutputSignal");
        be->put("OutputSignal", out);
        level.blockEntityChanged(pos);
    }
    if (old == out && bs::getValue(s, "mode") != "compare") return;
    bool sourceOn = diodeShouldTurnOn(level, pos, s);
    bool on = bs::getBool(s, "powered");
    if (on && !sourceOn) level.setBlock(pos, bs::setBool(s, "powered", false), ServerLevel::kUpdateClients);
    else if (!on && sourceOn) level.setBlock(pos, bs::setBool(s, "powered", true), ServerLevel::kUpdateClients);
    updateNeighborsInFront(level, pos, s);
}

void diodeCheckTickOnNeighbor(ServerLevel& level, BlockPos pos, BlockStateId s) {
    uint32_t b = bs::blockOf(s);
    if (isComparator(b)) {
        if (level.willTickThisTick(pos, b)) return;
        int out = comparatorOutput(level, pos, s);
        nbt::Compound* be = blockEntity(level, pos);
        int old = be ? be->getInt("OutputSignal") : 0;
        if (out != old || bs::getBool(s, "powered") != diodeShouldTurnOn(level, pos, s))
            level.scheduleTick(pos, b, 2, diodeShouldPrioritize(level, pos, s) ? kHigh : kNormal);
        return;
    }
    if (diodeIsLocked(level, pos, s)) return;
    bool on = bs::getBool(s, "powered");
    if (on != diodeShouldTurnOn(level, pos, s) && !level.willTickThisTick(pos, b)) {
        int priority = diodeShouldPrioritize(level, pos, s) ? kExtremelyHigh : on ? kVeryHigh : kHigh;
        level.scheduleTick(pos, b, diodeDelay(s), priority);
    }
}

void diodeTick(ServerLevel& level, BlockPos pos, BlockStateId s) {
    if (isComparator(bs::blockOf(s))) return comparatorRefreshOutputState(level, pos, s);
    if (diodeIsLocked(level, pos, s)) return;
    bool on = bs::getBool(s, "powered");
    bool shouldTurnOn = diodeShouldTurnOn(level, pos, s);
    if (on && !shouldTurnOn) {
        level.setBlock(pos, bs::setBool(s, "powered", false), ServerLevel::kUpdateClients);
    } else if (!on) {
        level.setBlock(pos, bs::setBool(s, "powered", true), ServerLevel::kUpdateClients);
        if (!shouldTurnOn) level.scheduleTick(pos, bs::blockOf(s), diodeDelay(s), kVeryHigh);
    }
}

// ---------------------------------------------------------------- redstone torches

struct Toggle {
    BlockPos pos;
    int64_t when;
};
// RedstoneTorchBlock.RECENT_TOGGLES, per level.
std::unordered_map<const ServerLevel*, std::vector<Toggle>> gRecentToggles;

bool torchToggledTooFrequently(ServerLevel& level, BlockPos pos, bool add) {
    auto& toggles = gRecentToggles[&level];
    if (add) toggles.push_back({pos, level.gameTime()});
    int count = 0;
    for (const auto& t : toggles)
        if (t.pos == pos && ++count >= 8) return true;
    return false;
}

bool torchHasNeighborSignal(ServerLevel& level, BlockPos pos, BlockStateId s) {
    if (isA(bs::blockOf(s), "RedstoneWallTorchBlock")) {
        Direction opposite = direction::opposite(dirValue(s));
        return rs::hasSignal(level, pos.relative(opposite), opposite);
    }
    return rs::hasSignal(level, pos.below(), Direction::Down);
}

void torchNotifyNeighbors(ServerLevel& level, BlockPos pos, BlockStateId s) {
    for (Direction d : direction::kAll) level.updateNeighborsAt(pos.relative(d), bs::blockOf(s));
}

void torchTick(ServerLevel& level, BlockPos pos, BlockStateId s) {
    bool signal = torchHasNeighborSignal(level, pos, s);
    auto& toggles = gRecentToggles[&level];
    while (!toggles.empty() && level.gameTime() - toggles.front().when > 60) toggles.erase(toggles.begin());
    if (bs::getBool(s, "lit")) {
        if (signal) {
            level.setBlock(pos, bs::setBool(s, "lit", false));
            if (torchToggledTooFrequently(level, pos, true)) {
                level.levelEvent(1502, pos, 0); // the torch burns out
                level.scheduleTick(pos, bs::blockOf(level.getBlockState(pos)), 160);
            }
        }
    } else if (!signal && !torchToggledTooFrequently(level, pos, false)) {
        level.setBlock(pos, bs::setBool(s, "lit", true));
    }
}

// ---------------------------------------------------------------- levers, buttons, pressure plates

void attachedUpdateNeighbours(ServerLevel& level, BlockPos pos, BlockStateId s) { // LeverBlock/ButtonBlock.updateNeighbours
    Direction front = direction::opposite(connectedDirection(s));
    level.updateNeighborsAt(pos, bs::blockOf(s));
    level.updateNeighborsAt(pos.relative(front), bs::blockOf(s));
}

void buttonSound(ServerLevel& level, BlockPos pos, BlockStateId s, bool pressed, int exclude) {
    const auto& beh = bs::behaviour(bs::blockOf(s));
    level.playBlockSound(exclude, pos, pressed ? beh.openSound : beh.closeSound, 1.0f, 1.0f);
}

int plateSignalForState(BlockStateId s) {
    return isA(bs::blockOf(s), "WeightedPressurePlateBlock") ? intValue(s, "power") : bs::getBool(s, "powered") ? 15 : 0;
}
BlockStateId plateSetSignal(BlockStateId s, int signal) {
    if (isA(bs::blockOf(s), "WeightedPressurePlateBlock")) return bs::setValue(s, "power", std::to_string(signal));
    return bs::setBool(s, "powered", signal > 0);
}

// BasePressurePlateBlock.getEntityCount over TOUCH_AABB (a 14x4x14 pixel box on the floor).
int plateEntityCount(ServerLevel& level, BlockPos pos, bool mobsOnly) {
    AABB touch{pos.x + 1.0 / 16, static_cast<double>(pos.y), pos.z + 1.0 / 16, pos.x + 15.0 / 16, pos.y + 4.0 / 16, pos.z + 15.0 / 16};
    int count = 0;
    if (!mobsOnly)
        for (const auto& e : level.entities())
            if (!e->removed && e->boundingBox().intersects(touch)) count++;
    for (ServerPlayer* p : level.players())
        if (p->gameMode != GameType::Spectator && p->boundingBox().intersects(touch)) count++;
    return count;
}

int plateSignalStrength(ServerLevel& level, BlockPos pos, BlockStateId s) {
    uint32_t b = bs::blockOf(s);
    int param = bs::behaviour(b).redstoneParam;
    if (isA(b, "WeightedPressurePlateBlock")) {
        int count = std::min(plateEntityCount(level, pos, false), param);
        if (count <= 0) return 0;
        float percent = static_cast<float>(std::min(param, count)) / param;
        return static_cast<int>(std::ceil(percent * 15.0f));
    }
    return plateEntityCount(level, pos, param == 1) > 0 ? 15 : 0;
}

void plateUpdateNeighbours(ServerLevel& level, BlockPos pos, uint32_t block) {
    level.updateNeighborsAt(pos, block);
    level.updateNeighborsAt(pos.below(), block);
}

void plateCheckPressed(ServerLevel& level, BlockPos pos, BlockStateId s, int oldSignal) {
    uint32_t b = bs::blockOf(s);
    int signal = plateSignalStrength(level, pos, s);
    bool wasPressed = oldSignal > 0, pressed = signal > 0;
    if (oldSignal != signal) {
        level.setBlock(pos, plateSetSignal(s, signal), ServerLevel::kUpdateClients);
        plateUpdateNeighbours(level, pos, b);
    }
    const auto& beh = bs::behaviour(b);
    if (!pressed && wasPressed) level.playBlockSound(-1, pos, beh.closeSound, 1.0f, 1.0f);
    else if (pressed && !wasPressed) level.playBlockSound(-1, pos, beh.openSound, 1.0f, 1.0f);
    if (pressed) level.scheduleTick(pos, b, isA(b, "WeightedPressurePlateBlock") ? 10 : 20);
}

// ---------------------------------------------------------------- rails

// PoweredRailBlock.findPoweredRailSignal / isSameRailWithPower
bool findPoweredRailSignal(ServerLevel& level, BlockPos pos, BlockStateId s, bool forward, int depth);
bool isSameRailWithPower(ServerLevel& level, BlockPos pos, uint32_t block, bool forward, int depth, std::string_view dir) {
    BlockStateId s = level.getBlockState(pos);
    if (!bs::is(s, block)) return false;
    auto shape = bs::getValue(s, "shape").value_or("north_south");
    if (dir == "east_west" && (shape == "north_south" || shape == "ascending_north" || shape == "ascending_south")) return false;
    if (dir == "north_south" && (shape == "east_west" || shape == "ascending_east" || shape == "ascending_west")) return false;
    if (!bs::getBool(s, "powered")) return false;
    return rs::hasNeighborSignal(level, pos) || findPoweredRailSignal(level, pos, s, forward, depth + 1);
}
bool findPoweredRailSignal(ServerLevel& level, BlockPos pos, BlockStateId s, bool forward, int depth) {
    if (depth >= 8) return false;
    int x = pos.x, y = pos.y, z = pos.z;
    bool checkBelow = true;
    std::string_view shape = bs::getValue(s, "shape").value_or("north_south");
    if (shape == "north_south") {
        forward ? z++ : z--;
    } else if (shape == "east_west") {
        forward ? x-- : x++;
    } else if (shape == "ascending_east") {
        if (forward) x--;
        else x++, y++, checkBelow = false;
        shape = "east_west";
    } else if (shape == "ascending_west") {
        if (forward) x--, y++, checkBelow = false;
        else x++;
        shape = "east_west";
    } else if (shape == "ascending_north") {
        if (forward) z++;
        else z--, y++, checkBelow = false;
        shape = "north_south";
    } else if (shape == "ascending_south") {
        if (forward) z++, y++, checkBelow = false;
        else z--;
        shape = "north_south";
    }
    uint32_t b = bs::blockOf(s);
    return isSameRailWithPower(level, {x, y, z}, b, forward, depth, shape) ||
           (checkBelow && isSameRailWithPower(level, {x, y - 1, z}, b, forward, depth, shape));
}

bool isSlope(BlockStateId s) { return bs::getValue(s, "shape").value_or("").starts_with("ascending"); }

// BaseRailBlock.updateState(state, level, pos, block): RailBlock re-shapes at a T junction when a
// signal source changed; powered and activator rails switch on and off.
void railUpdateState(ServerLevel& level, BlockPos pos, BlockStateId s, uint32_t sourceBlock) {
    uint32_t b = bs::blockOf(s);
    if (isA(b, "PoweredRailBlock")) {
        bool powered = bs::getBool(s, "powered");
        bool shouldPower = rs::hasNeighborSignal(level, pos) || findPoweredRailSignal(level, pos, s, true, 0) || findPoweredRailSignal(level, pos, s, false, 0);
        if (shouldPower != powered) {
            level.setBlock(pos, bs::setBool(s, "powered", shouldPower));
            level.updateNeighborsAt(pos.below(), b);
            if (isSlope(s)) level.updateNeighborsAt(pos.above(), b);
        }
    } else if (isA(b, "RailBlock")) {
        if ((bs::flags(bs::defaultState(sourceBlock)) & bs::kSignalSource) && detail::railPotentialConnections(level, pos, s) == 3)
            detail::railUpdateDir(level, pos, s, rs::hasNeighborSignal(level, pos), false);
    }
}

// ---------------------------------------------------------------- doors, trapdoors, fence gates

void openCloseSound(ServerLevel& level, BlockPos pos, uint32_t b, bool open) {
    const auto& beh = bs::behaviour(b);
    level.playBlockSound(-1, pos, open ? beh.openSound : beh.closeSound, 1.0f, nextFloat(level) * 0.1f + 0.9f);
}

// ---------------------------------------------------------------- copper bulbs

void copperBulbCheckAndFlip(ServerLevel& level, BlockPos pos, BlockStateId s) {
    bool signal = rs::hasNeighborSignal(level, pos);
    if (signal == bs::getBool(s, "powered")) return;
    BlockStateId n = s;
    if (!bs::getBool(s, "powered")) {
        n = bs::setBool(s, "lit", !bs::getBool(s, "lit"));
        static const int32_t kOn = sound("minecraft:block.copper_bulb.turn_on"), kOff = sound("minecraft:block.copper_bulb.turn_off");
        level.playBlockSound(-1, pos, bs::getBool(n, "lit") ? kOn : kOff, 1.0f, 1.0f);
    }
    level.setBlock(pos, bs::setBool(n, "powered", signal));
}

void hopperCheckPoweredState(ServerLevel& level, BlockPos pos, BlockStateId s) {
    bool shouldBeOn = !rs::hasNeighborSignal(level, pos);
    if (shouldBeOn != bs::getBool(s, "enabled")) level.setBlock(pos, bs::setBool(s, "enabled", shouldBeOn), ServerLevel::kUpdateClients);
}

void lecternUpdateBelow(ServerLevel& level, BlockPos pos, BlockStateId s) { level.updateNeighborsAt(pos.below(), bs::blockOf(s)); }

// ---------------------------------------------------------------- tripwire hooks and tripwire

void hookNotifyNeighbors(ServerLevel& level, BlockPos pos, Direction dir) { // TripWireHookBlock.notifyNeighbors
    Direction front = direction::opposite(dir);
    level.updateNeighborsAt(pos, TRIPWIRE_HOOK);
    level.updateNeighborsAt(pos.relative(front), TRIPWIRE_HOOK);
}

void hookEmitState(ServerLevel& level, BlockPos pos, bool attached, bool powered, bool wasAttached, bool wasPowered) {
    static const int32_t kOn = sound("minecraft:block.tripwire.click_on"), kOff = sound("minecraft:block.tripwire.click_off");
    static const int32_t kAttach = sound("minecraft:block.tripwire.attach"), kDetach = sound("minecraft:block.tripwire.detach");
    if (powered && !wasPowered) level.playBlockSound(-1, pos, kOn, 0.4f, 0.6f);
    else if (!powered && wasPowered) level.playBlockSound(-1, pos, kOff, 0.4f, 0.5f);
    else if (attached && !wasAttached) level.playBlockSound(-1, pos, kAttach, 0.4f, 0.7f);
    else if (!attached && wasAttached) level.playBlockSound(-1, pos, kDetach, 0.4f, 1.2f / (level.levelRandom().nextFloat() * 0.2f + 0.9f));
}

void hookOnRemoved(ServerLevel& level, BlockPos pos, BlockStateId s);

// TripWireHookBlock.calculateState
void hookCalculateState(ServerLevel& level, BlockPos pos, BlockStateId s, bool beingDestroyed, bool canUpdate, int wireSource,
                        std::optional<BlockStateId> wireSourceState) {
    if (!bs::hasProperty(s, "facing")) return;
    Direction dir = dirValue(s);
    bool wasAttached = bs::getBool(s, "attached"), wasPowered = bs::getBool(s, "powered");
    uint32_t block = bs::blockOf(s);
    bool attached = !beingDestroyed, powered = false;
    int receiver = 0;
    std::array<std::optional<BlockStateId>, 42> wires{};
    for (int i = 1; i < 42; i++) {
        BlockPos test = pos.relative(dir, i);
        BlockStateId ws = level.getBlockState(test);
        if (bs::is(ws, TRIPWIRE_HOOK)) {
            if (dirValue(ws) == direction::opposite(dir)) receiver = i;
            break;
        }
        if (!bs::is(ws, TRIPWIRE) && i != wireSource) {
            wires[i] = std::nullopt;
            attached = false;
        } else {
            if (i == wireSource) ws = wireSourceState.value_or(ws);
            bool armed = !bs::getBool(ws, "disarmed");
            powered |= armed && bs::getBool(ws, "powered");
            wires[i] = ws;
            if (i == wireSource) {
                level.scheduleTick(pos, block, 10);
                attached &= armed;
            }
        }
    }
    attached &= receiver > 1;
    powered &= attached;
    BlockStateId n = bs::setBool(bs::setBool(bs::defaultState(block), "attached", attached), "powered", powered);
    if (receiver > 0) {
        BlockPos test = pos.relative(dir, receiver);
        Direction opposite = direction::opposite(dir);
        level.setBlock(test, bs::setValue(n, "facing", direction::name(opposite)));
        hookNotifyNeighbors(level, test, opposite);
        if (!bs::is(level.getBlockState(pos), TRIPWIRE_HOOK)) {
            hookOnRemoved(level, pos, n);
            return;
        }
        hookEmitState(level, test, attached, powered, wasAttached, wasPowered);
    }
    hookEmitState(level, pos, attached, powered, wasAttached, wasPowered);
    if (!beingDestroyed) {
        level.setBlock(pos, bs::setValue(n, "facing", direction::name(dir)));
        if (canUpdate) hookNotifyNeighbors(level, pos, dir);
    }
    if (wasAttached != attached)
        for (int i = 1; i < receiver; i++) {
            BlockPos test = pos.relative(dir, i);
            if (!wires[i]) continue;
            BlockStateId cur = level.getBlockState(test);
            if (bs::is(cur, TRIPWIRE) || bs::is(cur, TRIPWIRE_HOOK)) level.setBlock(test, bs::setBool(*wires[i], "attached", attached));
        }
}

void hookOnRemoved(ServerLevel& level, BlockPos pos, BlockStateId s) {
    bool attached = bs::getBool(s, "attached"), powered = bs::getBool(s, "powered");
    if (attached || powered) hookCalculateState(level, pos, s, true, false, -1, std::nullopt);
    if (powered) hookNotifyNeighbors(level, pos, dirValue(s));
}

void tripwireUpdateSource(ServerLevel& level, BlockPos pos, BlockStateId s) { // TripWireBlock.updateSource
    for (Direction d : {Direction::South, Direction::West})
        for (int i = 1; i < 42; i++) {
            BlockPos test = pos.relative(d, i);
            BlockStateId b = level.getBlockState(test);
            if (bs::is(b, TRIPWIRE_HOOK)) {
                if (dirValue(b) == direction::opposite(d)) hookCalculateState(level, test, b, false, true, i, s);
                break;
            }
            if (!bs::is(b, TRIPWIRE)) break;
        }
}

AABB tripwireBox(BlockPos pos, BlockStateId s) { // the shape's bounds
    double y0 = bs::getBool(s, "attached") ? 1.0 / 16 : 0.0, y1 = bs::getBool(s, "attached") ? 2.5 / 16 : 8.0 / 16;
    return {static_cast<double>(pos.x), pos.y + y0, static_cast<double>(pos.z), pos.x + 1.0, pos.y + y1, pos.z + 1.0};
}

void tripwireCheckPressed(ServerLevel& level, BlockPos pos, bool someone) {
    BlockStateId s = level.getBlockState(pos);
    bool wasPressed = bs::getBool(s, "powered");
    if (someone != wasPressed) {
        s = bs::setBool(s, "powered", someone);
        level.setBlock(pos, s);
        tripwireUpdateSource(level, pos, s);
    }
    if (someone) level.scheduleTick(pos, TRIPWIRE, 10);
    else if (wasPressed) level.scheduleTick(pos, TRIPWIRE, 1);
}

bool anyEntityIn(ServerLevel& level, const AABB& box) { // Level.getEntities(null, box): no spectators
    for (const auto& e : level.entities())
        if (!e->removed && e->boundingBox().intersects(box)) return true;
    for (ServerPlayer* p : level.players())
        if (p->gameMode != GameType::Spectator && !p->disconnected && p->boundingBox().intersects(box)) return true;
    return false;
}

} // namespace

// ================================================================ dispatch

bool onPlace(ServerLevel& level, BlockPos pos, BlockStateId s, BlockStateId old, bool movedByPiston) {
    uint32_t b = bs::blockOf(s);
    bool blockChanged = bs::blockOf(old) != b;
    if (b == REDSTONE_WIRE) {
        if (blockChanged) {
            wireUpdatePower(level, pos, s);
            for (Direction d : {Direction::Up, Direction::Down}) level.updateNeighborsAt(pos.relative(d), REDSTONE_WIRE);
            wireUpdateNeighborsOfNeighboringWires(level, pos);
        }
        return true;
    }
    if (isA(b, "DiodeBlock")) {
        updateNeighborsInFront(level, pos, s);
        return true;
    }
    if (isA(b, "RedstoneTorchBlock")) {
        torchNotifyNeighbors(level, pos, s);
        return true;
    }
    if (isA(b, "ObserverBlock")) {
        if (blockChanged && bs::getBool(s, "powered") && !level.hasScheduledTick(pos, b)) {
            BlockStateId n = bs::setBool(s, "powered", false);
            level.setBlock(pos, n, ServerLevel::kUpdateClients | ServerLevel::kUpdateKnownShape);
            updateNeighborsInFront(level, pos, n);
        }
        return true;
    }
    if (isA(b, "CopperBulbBlock")) {
        if (blockChanged) copperBulbCheckAndFlip(level, pos, s);
        return true;
    }
    if (isA(b, "HopperBlock")) {
        if (blockChanged) hopperCheckPoweredState(level, pos, s);
        return true;
    }
    if (b == TRIPWIRE) {
        if (blockChanged) tripwireUpdateSource(level, pos, s);
        return true;
    }
    if (isA(b, "TargetBlock")) {
        if (blockChanged && intValue(s, "power") > 0 && !level.hasScheduledTick(pos, b))
            level.setBlock(pos, bs::setValue(s, "power", "0"), ServerLevel::kUpdateClients | ServerLevel::kUpdateKnownShape);
        return true;
    }
    if (isA(b, "BaseRailBlock")) {
        if (blockChanged) { // BaseRailBlock.updateState(state, level, pos, movedByPiston)
            BlockStateId n = detail::railUpdateDir(level, pos, s, rs::hasNeighborSignal(level, pos), true);
            if (!isA(b, "RailBlock")) level.neighborChanged(n, pos, b, movedByPiston);
        }
        return true;
    }
    return false;
}

void affectNeighborsAfterRemoval(ServerLevel& level, BlockPos pos, BlockStateId s, bool movedByPiston) {
    uint32_t b = bs::blockOf(s);
    if (b == REDSTONE_WIRE) {
        if (movedByPiston) return;
        for (Direction d : direction::kAll) level.updateNeighborsAt(pos.relative(d), REDSTONE_WIRE);
        wireUpdatePower(level, pos, s);
        wireUpdateNeighborsOfNeighboringWires(level, pos);
    } else if (isA(b, "DiodeBlock")) {
        if (!movedByPiston) updateNeighborsInFront(level, pos, s);
    } else if (isA(b, "RedstoneTorchBlock")) {
        if (!movedByPiston) torchNotifyNeighbors(level, pos, s);
    } else if (isA(b, "LeverBlock") || isA(b, "ButtonBlock")) {
        if (!movedByPiston && bs::getBool(s, "powered")) attachedUpdateNeighbours(level, pos, s);
    } else if (isA(b, "BasePressurePlateBlock")) {
        if (!movedByPiston && plateSignalForState(s) > 0) plateUpdateNeighbours(level, pos, b);
    } else if (isA(b, "ObserverBlock")) {
        if (bs::getBool(s, "powered") && level.hasScheduledTick(pos, b)) updateNeighborsInFront(level, pos, bs::setBool(s, "powered", false));
    } else if (isA(b, "LecternBlock")) {
        if (bs::getBool(s, "powered")) lecternUpdateBelow(level, pos, s);
    } else if (b == TRIPWIRE_HOOK) {
        if (!movedByPiston) hookOnRemoved(level, pos, s);
    } else if (b == TRIPWIRE) {
        if (!movedByPiston) tripwireUpdateSource(level, pos, bs::setBool(s, "powered", true));
    } else if (isA(b, "BaseRailBlock")) {
        if (movedByPiston) return;
        if (isSlope(s)) level.updateNeighborsAt(pos.above(), b);
        if (!isA(b, "RailBlock")) {
            level.updateNeighborsAt(pos, b);
            level.updateNeighborsAt(pos.below(), b);
        }
    }
}

bool neighborChanged(ServerLevel& level, BlockPos pos, BlockStateId s, uint32_t source, bool movedByPiston) {
    uint32_t b = bs::blockOf(s);
    if (b == REDSTONE_WIRE) {
        if (canSurvive(s, level, pos)) {
            wireUpdatePower(level, pos, s);
        } else {
            dropResources(level, pos, s);
            removeBlock(level, pos, false);
        }
        return true;
    }
    if (isA(b, "DiodeBlock")) {
        if (!bs::is(level.getBlockState(pos), b)) return true;
        if (canSurvive(s, level, pos)) {
            diodeCheckTickOnNeighbor(level, pos, s);
        } else {
            dropResources(level, pos, s);
            removeBlock(level, pos, false);
            for (Direction d : direction::kAll) level.updateNeighborsAt(pos.relative(d), b);
        }
        return true;
    }
    if (isA(b, "RedstoneTorchBlock")) {
        if (bs::getBool(s, "lit") == torchHasNeighborSignal(level, pos, s) && !level.willTickThisTick(pos, b)) level.scheduleTick(pos, b, 2);
        return true;
    }
    if (isA(b, "RedstoneLampBlock")) {
        bool lit = bs::getBool(s, "lit");
        if (lit != rs::hasNeighborSignal(level, pos)) {
            if (lit) level.scheduleTick(pos, b, 4);
            else level.setBlock(pos, bs::setBool(s, "lit", true), ServerLevel::kUpdateClients);
        }
        return true;
    }
    if (isA(b, "CopperBulbBlock")) {
        copperBulbCheckAndFlip(level, pos, s);
        return true;
    }
    if (isA(b, "DoorBlock")) {
        bool lower = bs::getValue(s, "half") == "lower";
        bool signal = rs::hasNeighborSignal(level, pos) || rs::hasNeighborSignal(level, pos.relative(lower ? Direction::Up : Direction::Down));
        if (source != b && signal != bs::getBool(s, "powered")) {
            if (signal != bs::getBool(s, "open")) openCloseSound(level, pos, b, signal);
            level.setBlock(pos, bs::setBool(bs::setBool(s, "powered", signal), "open", signal), ServerLevel::kUpdateClients);
        }
        return true;
    }
    if (isA(b, "TrapDoorBlock")) {
        bool signal = rs::hasNeighborSignal(level, pos);
        if (signal != bs::getBool(s, "powered")) {
            if (bs::getBool(s, "open") != signal) {
                s = bs::setBool(s, "open", signal);
                openCloseSound(level, pos, b, signal);
            }
            level.setBlock(pos, bs::setBool(s, "powered", signal), ServerLevel::kUpdateClients);
            if (bs::getBool(s, "waterlogged")) level.scheduleFluidTick(pos, fluids::kWater, fluids::tickDelay(level, fluids::kWater));
        }
        return true;
    }
    if (isA(b, "FenceGateBlock")) {
        bool power = rs::hasNeighborSignal(level, pos);
        if (bs::getBool(s, "powered") != power) {
            level.setBlock(pos, bs::setBool(bs::setBool(s, "powered", power), "open", power), ServerLevel::kUpdateClients);
            if (bs::getBool(s, "open") != power) openCloseSound(level, pos, b, power);
        }
        return true;
    }
    if (isA(b, "NoteBlock")) {
        bool signal = rs::hasNeighborSignal(level, pos);
        if (signal != bs::getBool(s, "powered")) {
            if (signal) detail::playNote(level, pos, s);
            level.setBlock(pos, bs::setBool(s, "powered", signal));
        }
        return true;
    }
    if (isA(b, "HopperBlock")) {
        hopperCheckPoweredState(level, pos, s);
        return true;
    }
    if (isA(b, "BellBlock")) {
        bool signal = rs::hasNeighborSignal(level, pos);
        if (signal != bs::getBool(s, "powered")) {
            if (signal) detail::ringBell(level, pos, std::nullopt);
            level.setBlock(pos, bs::setBool(s, "powered", signal));
        }
        return true;
    }
    if (isA(b, "AbstractSkullBlock")) {
        bool signal = rs::hasNeighborSignal(level, pos);
        if (signal != bs::getBool(s, "powered")) level.setBlock(pos, bs::setBool(s, "powered", signal), ServerLevel::kUpdateClients);
        return true;
    }
    if (isA(b, "BaseRailBlock")) {
        if (!bs::is(level.getBlockState(pos), b)) return true;
        if (detail::railShouldBeRemoved(level, pos, s)) {
            dropResources(level, pos, s);
            removeBlock(level, pos, movedByPiston);
        } else {
            railUpdateState(level, pos, s, source);
        }
        return true;
    }
    return false;
}

bool hasTick(uint32_t b) {
    return b == REDSTONE_WIRE || isA(b, "DiodeBlock") || isA(b, "RedstoneTorchBlock") || isA(b, "ButtonBlock") ||
           isA(b, "BasePressurePlateBlock") || isA(b, "ObserverBlock") || isA(b, "RedstoneLampBlock") || isA(b, "TargetBlock") ||
           b == TRIPWIRE_HOOK || b == TRIPWIRE;
}

bool tick(ServerLevel& level, BlockPos pos, BlockStateId s) {
    uint32_t b = bs::blockOf(s);
    if (isA(b, "DiodeBlock")) {
        diodeTick(level, pos, s);
    } else if (isA(b, "RedstoneTorchBlock")) {
        torchTick(level, pos, s);
    } else if (isA(b, "ButtonBlock")) {
        if (bs::getBool(s, "powered")) { // checkPressed: no arrows are simulated, so it pops back out
            level.setBlock(pos, bs::setBool(s, "powered", false));
            attachedUpdateNeighbours(level, pos, s);
            buttonSound(level, pos, s, false, -1);
        }
    } else if (isA(b, "BasePressurePlateBlock")) {
        int signal = plateSignalForState(s);
        if (signal > 0) plateCheckPressed(level, pos, s, signal);
    } else if (isA(b, "ObserverBlock")) {
        if (bs::getBool(s, "powered")) {
            level.setBlock(pos, bs::setBool(s, "powered", false), ServerLevel::kUpdateClients);
        } else {
            level.setBlock(pos, bs::setBool(s, "powered", true), ServerLevel::kUpdateClients);
            level.scheduleTick(pos, b, 2);
        }
        updateNeighborsInFront(level, pos, s);
    } else if (isA(b, "RedstoneLampBlock")) {
        if (bs::getBool(s, "lit") && !rs::hasNeighborSignal(level, pos)) level.setBlock(pos, bs::setBool(s, "lit", false), ServerLevel::kUpdateClients);
    } else if (isA(b, "TargetBlock")) {
        if (intValue(s, "power") != 0) level.setBlock(pos, bs::setValue(s, "power", "0"));
    } else if (b == TRIPWIRE_HOOK) {
        hookCalculateState(level, pos, s, false, true, -1, std::nullopt);
    } else if (b == TRIPWIRE) {
        if (bs::getBool(level.getBlockState(pos), "powered")) tripwireCheckPressed(level, pos, anyEntityIn(level, tripwireBox(pos, s)));
    } else {
        return false;
    }
    return true;
}

std::optional<BlockStateId> updateShape(BlockStateId s, ServerLevel& level, BlockPos pos, Direction toNeighbour, BlockStateId neighbour) {
    uint32_t b = bs::blockOf(s);
    if (isA(b, "DiodeBlock")) {
        if (toNeighbour == Direction::Down && !(bs::flags(neighbour) & bs::kRigidSupportUp)) return bs::defaultState(AIR);
        if (isA(b, "RepeaterBlock") && direction::axis(toNeighbour) != direction::axis(dirValue(s)))
            return bs::setBool(s, "locked", diodeIsLocked(level, pos, s));
        return s;
    }
    if (isA(b, "ObserverBlock")) {
        if (dirValue(s) == toNeighbour && !bs::getBool(s, "powered") && !level.hasScheduledTick(pos, b)) level.scheduleTick(pos, b, 2);
        return s;
    }
    return std::nullopt;
}

BlockStateId placementState(uint32_t b, BlockStateId s, ServerLevel& level, BlockPos pos) {
    if (isA(b, "DoorBlock")) {
        bool powered = rs::hasNeighborSignal(level, pos) || rs::hasNeighborSignal(level, pos.above());
        return bs::setBool(bs::setBool(s, "powered", powered), "open", powered);
    }
    if (isA(b, "TrapDoorBlock")) {
        if (rs::hasNeighborSignal(level, pos)) s = bs::setBool(bs::setBool(s, "open", true), "powered", true);
        return s;
    }
    if (isA(b, "FenceGateBlock")) {
        bool open = rs::hasNeighborSignal(level, pos);
        return bs::setBool(bs::setBool(s, "open", open), "powered", open);
    }
    if (isA(b, "RedstoneLampBlock")) return bs::setBool(s, "lit", rs::hasNeighborSignal(level, pos));
    if (isA(b, "RepeaterBlock")) return bs::setBool(s, "locked", diodeIsLocked(level, pos, s));
    if (isA(b, "HopperBlock")) return bs::setBool(s, "enabled", true);
    return s;
}

void setPlacedBy(ServerLevel& level, BlockPos pos, BlockStateId s) {
    uint32_t b = bs::blockOf(s);
    if (isA(b, "DiodeBlock") && diodeShouldTurnOn(level, pos, s)) level.scheduleTick(pos, b, 1);
    if (b == TRIPWIRE_HOOK) hookCalculateState(level, pos, s, false, false, -1, std::nullopt);
}

std::optional<bool> use(ServerLevel& level, BlockPos pos, BlockStateId s, int playerId, bool mayBuild) {
    uint32_t b = bs::blockOf(s);
    if (isA(b, "LeverBlock")) { // LeverBlock.pull (with no player: everyone hears the click)
        s = bs::setBool(s, "powered", !bs::getBool(s, "powered"));
        level.setBlock(pos, s);
        attachedUpdateNeighbours(level, pos, s);
        static const int32_t kClick = sound("minecraft:block.lever.click");
        level.playBlockSound(-1, pos, kClick, 0.3f, bs::getBool(s, "powered") ? 0.6f : 0.5f);
        return true;
    }
    if (isA(b, "ButtonBlock")) {
        if (bs::getBool(s, "powered")) return true; // CONSUME
        level.setBlock(pos, bs::setBool(s, "powered", true)); // ButtonBlock.press
        attachedUpdateNeighbours(level, pos, s);
        level.scheduleTick(pos, b, bs::behaviour(b).redstoneParam);
        buttonSound(level, pos, s, true, playerId);
        return true;
    }
    if (!mayBuild) {
        if (isA(b, "RepeaterBlock") || isA(b, "ComparatorBlock") || isA(b, "DaylightDetectorBlock") || b == REDSTONE_WIRE) return false;
        return std::nullopt;
    }
    if (isA(b, "RepeaterBlock")) {
        level.setBlock(pos, bs::setValue(s, "delay", std::to_string(intValue(s, "delay") % 4 + 1)));
        return true;
    }
    if (isA(b, "ComparatorBlock")) {
        bool subtract = bs::getValue(s, "mode") != "subtract";
        s = bs::setValue(s, "mode", subtract ? "subtract" : "compare");
        static const int32_t kClick = sound("minecraft:block.comparator.click");
        level.playBlockSound(playerId, pos, kClick, 0.3f, subtract ? 0.55f : 0.5f);
        level.setBlock(pos, s, ServerLevel::kUpdateClients);
        if (bs::is(level.getBlockState(pos), b)) comparatorRefreshOutputState(level, pos, s);
        return true;
    }
    if (isA(b, "DaylightDetectorBlock")) {
        BlockStateId n = bs::setBool(s, "inverted", !bs::getBool(s, "inverted"));
        level.setBlock(pos, n, ServerLevel::kUpdateClients);
        daylightUpdate(level, pos, n);
        return true;
    }
    if (b == REDSTONE_WIRE) {
        if (!rs::wire::isCross(s) && !rs::wire::isDot(s)) return false;
        BlockStateId n = rs::wire::isCross(s) ? bs::setValue(bs::defaultState(b), "power", std::to_string(intValue(s, "power"))) : rs::wire::crossState(s);
        n = rs::wire::connectionState(level, n, pos);
        if (n == s) return false;
        level.setBlock(pos, n);
        for (Direction d : direction::kHorizontalPlane) { // updatesOnShapeChange
            BlockPos rp = pos.relative(d);
            if (rs::wire::connected(s, d) != rs::wire::connected(n, d) && conductor(level.getBlockState(rp)))
                level.updateNeighborsAtExceptFromFacing(rp, b, direction::opposite(d));
        }
        return true;
    }
    return std::nullopt;
}

void entityInside(ServerLevel& level, BlockPos pos, BlockStateId s, const AABB& entityBox) {
    uint32_t b = bs::blockOf(s);
    if (isA(b, "BasePressurePlateBlock") && plateSignalForState(s) == 0) plateCheckPressed(level, pos, s, 0);
    // TripWireBlock.entityInside (its entity-inside shape is its outline)
    if (b == TRIPWIRE && !bs::getBool(s, "powered") && !level.hasScheduledTick(pos, b) && entityBox.intersects(tripwireBox(pos, s)))
        tripwireCheckPressed(level, pos, true);
}

void lecternTick(ServerLevel& level, BlockPos pos, BlockStateId s) { // LecternBlock.tick -> changePowered(false)
    level.setBlock(pos, bs::setBool(s, "powered", false));
    lecternUpdateBelow(level, pos, s);
}

void lecternSignalPageChange(ServerLevel& level, BlockPos pos, BlockStateId s) {
    level.setBlock(pos, bs::setBool(s, "powered", true)); // changePowered(true)
    lecternUpdateBelow(level, pos, s);
    level.scheduleTick(pos, bs::blockOf(s), 2);
    level.levelEvent(1043, pos, 0);
}

void forgetLevel(const ServerLevel& level) { gRecentToggles.erase(&level); }

void daylightUpdate(ServerLevel& level, BlockPos pos, BlockStateId s) {
    constexpr float kPi = 3.14159265358979323846f;
    int target = level.brightness(light::LightLayer::Sky, pos) - level.skyDarken(); // getEffectiveSkyBrightness
    float sunAngle = ServerClockManager::sunAngle(level.dayTime()) * (kPi / 180.0f);
    if (bs::getBool(s, "inverted")) {
        target = 15 - target;
    } else if (target > 0) {
        float offset = sunAngle < kPi ? 0.0f : kPi * 2.0f;
        sunAngle += (offset - sunAngle) * 0.2f;
        target = static_cast<int>(std::floor(target * mth::cos(sunAngle) + 0.5f)); // Math.round
    }
    target = std::clamp(target, 0, 15);
    if (intValue(s, "power") != target) level.setBlock(pos, bs::setValue(s, "power", std::to_string(target)));
}

} // namespace mc::block_behaviour::redstone_blocks
