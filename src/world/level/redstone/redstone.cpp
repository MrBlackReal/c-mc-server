#include "world/level/redstone/redstone.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include "server/level/server_level.hpp"
#include "world/inventory/container_menu.hpp"
#include "world/item/item_logic.hpp"
#include "world/item/item_nbt.hpp"
#include "world/level/block/entity/block_entity_container.hpp"
#include "world/level/block/entity/block_entity_ticker.hpp"

namespace mc::redstone {

using namespace gen::block;
namespace bs = blocks;

namespace {

// RedstoneWireBlock.shouldSignal: off while a wire asks its neighbours for their power, so wires
// don't power each other through blocks.
bool gShouldSignal = true;

bool isA(uint32_t block, std::string_view cls) { return bs::isInstance(block, cls); }
Direction dirValue(BlockStateId s, std::string_view prop) { return direction::byName(bs::getValue(s, prop).value_or("north")); }
int intValue(BlockStateId s, std::string_view prop) {
    auto v = bs::getValue(s, prop);
    return v ? std::stoi(std::string(*v)) : 0;
}
bool conductor(BlockStateId s) { return bs::flags(s) & bs::kRedstoneConductor; }

nbt::Compound* blockEntity(ServerLevel& level, BlockPos pos) {
    LevelChunk* chunk = level.getChunkIfLoaded({pos.x >> 4, pos.z >> 4});
    auto* be = chunk ? chunk->blockEntityAt(pos.x, pos.y, pos.z) : nullptr;
    return be && be->blockAtLoad == bs::blockOf(level.getBlockState(pos)) ? &be->tag : nullptr;
}

// FaceAttachedHorizontalDirectionalBlock.getConnectedDirection: towards the block it hangs on, reversed.
Direction connectedDirection(BlockStateId s) {
    auto face = bs::getValue(s, "face");
    if (face == "ceiling") return Direction::Down;
    if (face == "floor") return Direction::Up;
    return dirValue(s, "facing");
}

// Mth.lerpDiscrete(t, 0, 15)
int lerpDiscrete15(float t) { return static_cast<int>(std::floor(t * 14.0f)) + (t > 0.0f ? 1 : 0); }

// Items saved in a block entity's "Items" list (slot -> stack), for containers without a live one.
int signalFromSavedItems(const nbt::Compound& be, int size) {
    float total = 0.0f;
    if (const nbt::List* list = be.getList("Items"))
        for (const auto& e : list->items)
            if (const nbt::Compound* c = e.asCompound())
                if (auto stack = item_nbt::read(*c); stack && !stack->isEmpty())
                    total += static_cast<float>(stack->count) / std::min(99, stack->maxStackSize());
    return lerpDiscrete15(total / size);
}

// ChestBlock.getContainer(..., ignoreBlocked = false): both halves of a double chest; nothing if a
// half has a redstone conductor on top (ChestBlock.isChestBlockedAt; cats aren't simulated).
int chestSignal(ServerLevel& level, BlockPos pos, BlockStateId state) {
    auto blocked = [&](BlockPos p) { return conductor(level.getBlockState(p.above())); };
    if (blocked(pos)) return 0;
    auto self = level.blockEntityContainer(pos);
    if (!self) return 0;
    std::string_view type = bs::getValue(state, "type").value_or("single");
    if (type != "single") {
        Direction facing = dirValue(state, "facing");
        BlockPos other = pos.relative(type == "left" ? direction::clockWise(facing) : direction::counterClockWise(facing));
        BlockStateId os = level.getBlockState(other);
        std::string_view otherType = bs::getValue(os, "type").value_or("single");
        if (bs::blockOf(os) == bs::blockOf(state) && otherType != "single" && otherType != type && dirValue(os, "facing") == facing) {
            if (blocked(other)) return 0;
            if (auto second = level.blockEntityContainer(other)) {
                CompoundContainer both(self, second);
                return signalFromContainer(&both);
            }
        }
    }
    return signalFromContainer(self.get());
}

int pageCount(const ItemStack& book) { // LecternBlockEntity.getPageCount
    for (const char* type : {"minecraft:written_book_content", "minecraft:writable_book_content"}) {
        auto v = items::effectiveComponent(book, type);
        if (!v || !v->asCompound()) continue;
        const nbt::Tag* pages = v->asCompound()->get("pages");
        return pages && pages->asList() ? static_cast<int>(pages->asList()->items.size()) : 0;
    }
    return 0;
}

} // namespace

bool isDiode(BlockStateId state) { return isA(bs::blockOf(state), "DiodeBlock"); }

// ---------------------------------------------------------------- RedstoneWireBlock connections

namespace wire {

bool canSurviveOn(BlockStateId below) { return bs::isFaceSturdy(below, Direction::Up) || bs::is(below, HOPPER); }

namespace {
std::string_view connectingSide(ServerLevel& level, BlockPos pos, Direction dir, bool canConnectUp) {
    BlockPos rp = pos.relative(dir);
    BlockStateId rs = level.getBlockState(rp);
    if (canConnectUp) {
        bool placeableAbove = isA(bs::blockOf(rs), "TrapDoorBlock") || canSurviveOn(rs);
        if (placeableAbove && shouldWireConnectTo(level.getBlockState(rp.above()), std::nullopt))
            return bs::isFaceSturdy(rs, direction::opposite(dir)) ? "up" : "side";
    }
    bool none = !shouldWireConnectTo(rs, dir) && (conductor(rs) || !shouldWireConnectTo(level.getBlockState(rp.below()), std::nullopt));
    return none ? "none" : "side";
}
} // namespace

std::string_view connectingSide(ServerLevel& level, BlockPos pos, Direction dir) {
    return connectingSide(level, pos, dir, !conductor(level.getBlockState(pos.above())));
}
bool connected(BlockStateId s, Direction d) { return bs::getValue(s, direction::name(d)) != "none"; }
bool isDot(BlockStateId s) {
    for (Direction d : direction::kHorizontal)
        if (connected(s, d)) return false;
    return true;
}
bool isCross(BlockStateId s) {
    for (Direction d : direction::kHorizontal)
        if (!connected(s, d)) return false;
    return true;
}
BlockStateId crossState(BlockStateId s) {
    for (Direction d : direction::kHorizontal) s = bs::setValue(s, direction::name(d), "side");
    return s;
}
BlockStateId connectionState(ServerLevel& level, BlockStateId s, BlockPos pos) { // getConnectionState
    bool wasDot = isDot(s);
    BlockStateId st = bs::setValue(bs::defaultState(bs::blockOf(s)), "power", std::string(bs::getValue(s, "power").value_or("0")));
    bool canConnectUp = !conductor(level.getBlockState(pos.above()));
    for (Direction d : direction::kHorizontalPlane) // getMissingConnections
        if (!connected(st, d)) st = bs::setValue(st, direction::name(d), connectingSide(level, pos, d, canConnectUp));
    if (wasDot && isDot(st)) return st;
    bool n = connected(st, Direction::North), so = connected(st, Direction::South);
    bool e = connected(st, Direction::East), w = connected(st, Direction::West);
    bool nsEmpty = !n && !so, ewEmpty = !e && !w;
    if (!w && nsEmpty) st = bs::setValue(st, "west", "side");
    if (!e && nsEmpty) st = bs::setValue(st, "east", "side");
    if (!n && ewEmpty) st = bs::setValue(st, "north", "side");
    if (!so && ewEmpty) st = bs::setValue(st, "south", "side");
    return st;
}

} // namespace wire

// ---------------------------------------------------------------- BlockBehaviour signal methods

bool isSignalSource(BlockStateId state) {
    if (bs::is(state, REDSTONE_WIRE)) return gShouldSignal;
    return bs::flags(state) & bs::kSignalSource;
}

bool shouldWireConnectTo(BlockStateId st, std::optional<Direction> dir) {
    uint32_t b = bs::blockOf(st);
    if (b == REDSTONE_WIRE) return true;
    if (isA(b, "RepeaterBlock")) {
        Direction f = dirValue(st, "facing");
        return dir && (f == *dir || direction::opposite(f) == *dir);
    }
    if (isA(b, "ObserverBlock")) return dir && dirValue(st, "facing") == *dir;
    return isSignalSource(st) && dir.has_value();
}

int ownSignal(ServerLevel& level, BlockPos pos, BlockStateId s) {
    uint32_t b = bs::blockOf(s);
    if (b == REDSTONE_WIRE || isA(b, "WeightedPressurePlateBlock") || isA(b, "DaylightDetectorBlock") || isA(b, "SculkSensorBlock"))
        return intValue(s, "power");
    if (isA(b, "TargetBlock")) return intValue(s, "power");
    if (isA(b, "ComparatorBlock")) {
        if (!bs::getBool(s, "powered")) return 0;
        nbt::Compound* be = blockEntity(level, pos);
        return be ? be->getInt("OutputSignal") : 0;
    }
    if (isA(b, "RedstoneTorchBlock")) return bs::getBool(s, "lit") ? 15 : 0;
    if (isA(b, "PoweredBlock")) return 15;
    if (isA(b, "TrappedChestBlock")) {
        auto c = level.blockEntityContainer(pos); // ChestBlockEntity.getOpenCount
        return c ? std::clamp(c->openers, 0, 15) : 0;
    }
    if (isA(b, "JukeboxBlock")) {
        nbt::Compound* be = blockEntity(level, pos);
        return be && be->contains("ticks_since_song_started") ? 15 : 0; // JukeboxSongPlayer.isPlaying
    }
    // Repeaters, levers, buttons, pressure plates, observers, lecterns, tripwire hooks, detector
    // rails, lightning rods: on or off.
    if (bs::hasProperty(s, "powered") && isSignalSource(s)) return bs::getBool(s, "powered") ? 15 : 0;
    return 0;
}

int getSignal(ServerLevel& level, BlockPos pos, BlockStateId s, Direction dir) {
    uint32_t b = bs::blockOf(s);
    if (b == REDSTONE_WIRE) {
        if (!gShouldSignal || dir == Direction::Down) return 0;
        int power = intValue(s, "power");
        if (power == 0) return 0;
        if (dir != Direction::Up && !wire::connected(wire::connectionState(level, s, pos), direction::opposite(dir))) return 0;
        return power;
    }
    if (isA(b, "DiodeBlock") || isA(b, "ObserverBlock")) return dirValue(s, "facing") == dir ? ownSignal(level, pos, s) : 0;
    if (isA(b, "RedstoneWallTorchBlock")) return dirValue(s, "facing") != dir ? ownSignal(level, pos, s) : 0;
    if (isA(b, "RedstoneTorchBlock")) return dir != Direction::Up ? ownSignal(level, pos, s) : 0;
    if (isA(b, "CalibratedSculkSensorBlock")) return dir != dirValue(s, "facing") ? ownSignal(level, pos, s) : 0;
    if (!isSignalSource(s)) return 0;
    return ownSignal(level, pos, s);
}

int getDirectSignal(ServerLevel& level, BlockPos pos, BlockStateId s, Direction dir) {
    uint32_t b = bs::blockOf(s);
    if (b == REDSTONE_WIRE) return gShouldSignal ? getSignal(level, pos, s, dir) : 0;
    if (isA(b, "DiodeBlock") || isA(b, "ObserverBlock")) return getSignal(level, pos, s, dir);
    if (isA(b, "RedstoneTorchBlock")) return dir == Direction::Down ? getSignal(level, pos, s, dir) : 0;
    if (isA(b, "LeverBlock") || isA(b, "ButtonBlock")) return bs::getBool(s, "powered") && connectedDirection(s) == dir ? 15 : 0;
    if (isA(b, "BasePressurePlateBlock")) return dir == Direction::Up ? ownSignal(level, pos, s) : 0;
    if (isA(b, "TrappedChestBlock") || isA(b, "SculkSensorBlock")) return dir == Direction::Up ? getSignal(level, pos, s, dir) : 0;
    if (isA(b, "LecternBlock") || isA(b, "DetectorRailBlock")) return dir == Direction::Up && bs::getBool(s, "powered") ? 15 : 0;
    if (isA(b, "TripWireHookBlock") || isA(b, "LightningRodBlock")) return bs::getBool(s, "powered") && dirValue(s, "facing") == dir ? 15 : 0;
    return 0;
}

bool hasAnalogOutputSignal(BlockStateId state) {
    static constexpr std::string_view kClasses[] = {
        "AbstractCauldronBlock", "BarrelBlock", "AbstractFurnaceBlock", "BeehiveBlock", "CakeBlock", "ChestBlock", "CommandBlock",
        "CopperGolemStatueBlock", "BrewingStandBlock", "CreakingHeartBlock", "CandleCakeBlock", "CopperBulbBlock", "CrafterBlock",
        "ChiseledBookShelfBlock", "DecoratedPotBlock", "ComposterBlock", "ComparatorBlock", "HopperBlock", "DispenserBlock",
        "EndPortalFrameBlock", "JukeboxBlock", "LecternBlock", "DetectorRailBlock", "RespawnAnchorBlock", "SculkSensorBlock",
        "ShulkerBoxBlock", "ShelfBlock"};
    uint32_t b = bs::blockOf(state);
    static std::vector<int8_t> cache(gen::kBlocks.size(), -1);
    if (cache[b] < 0) {
        cache[b] = 0;
        for (auto cls : kClasses)
            if (isA(b, cls)) cache[b] = 1;
    }
    return cache[b];
}

int signalFromContainer(Container* container) {
    if (!container) return 0;
    float total = 0.0f;
    for (int i = 0; i < container->size(); i++) {
        const ItemStack& stack = container->item(i);
        if (!stack.isEmpty()) total += static_cast<float>(stack.count) / container->maxStackSize(stack);
    }
    return lerpDiscrete15(total / container->size());
}

int getAnalogOutputSignal(ServerLevel& level, BlockPos pos, BlockStateId s, Direction dir) {
    uint32_t b = bs::blockOf(s);
    if (isA(b, "LayeredCauldronBlock")) return intValue(s, "level");
    if (isA(b, "LavaCauldronBlock")) return 3;
    if (isA(b, "CakeBlock")) return (7 - intValue(s, "bites")) * 2;
    if (isA(b, "CandleCakeBlock")) return 14;
    if (isA(b, "ComposterBlock")) return intValue(s, "level");
    if (isA(b, "BeehiveBlock")) return intValue(s, "honey_level");
    if (isA(b, "EndPortalFrameBlock")) return bs::getBool(s, "eye") ? 15 : 0;
    if (isA(b, "RespawnAnchorBlock")) return static_cast<int>(std::floor(intValue(s, "charges") / 4.0f * 15));
    if (isA(b, "CopperGolemStatueBlock")) {
        static constexpr std::string_view kPoses[] = {"standing", "sitting", "running", "star"};
        auto pose = bs::getValue(s, "copper_golem_pose").value_or("standing");
        for (int i = 0; i < 4; i++)
            if (kPoses[i] == pose) return i + 1;
        return 1;
    }
    if (isA(b, "CopperBulbBlock")) return bs::getBool(level.getBlockState(pos), "lit") ? 15 : 0;
    if (isA(b, "ChestBlock")) return chestSignal(level, pos, s);
    if (isA(b, "ComparatorBlock") || isA(b, "CreakingHeartBlock") || isA(b, "DetectorRailBlock") || isA(b, "SculkSensorBlock")) {
        // (comparators: their output; the rest need entities or vibrations we don't simulate)
        if (!isA(b, "ComparatorBlock")) return 0;
    }
    nbt::Compound* be = blockEntity(level, pos);
    if (!be) return 0;
    if (isA(b, "ComparatorBlock")) return be->getInt("OutputSignal");
    if (isA(b, "JukeboxBlock")) {
        const nbt::Compound* record = be->getCompound("RecordItem");
        auto stack = record ? item_nbt::read(*record) : std::nullopt;
        return stack ? block_entity::jukebox::comparatorOutput(*stack) : 0;
    }
    if (isA(b, "LecternBlock")) {
        if (!bs::getBool(s, "has_book")) return 0;
        const nbt::Compound* book = be->getCompound("Book");
        auto stack = book ? item_nbt::read(*book) : std::nullopt;
        int pages = stack ? pageCount(*stack) : 0;
        float progress = pages > 1 ? be->getInt("Page") / (pages - 1.0f) : 1.0f;
        return static_cast<int>(std::floor(progress * 14.0f)) + (stack && !stack->isEmpty() ? 1 : 0);
    }
    if (isA(b, "ChiseledBookShelfBlock")) return be->getInt("last_interacted_slot", -1) + 1;
    if (isA(b, "CommandBlock")) return be->getInt("SuccessCount");
    if (isA(b, "CrafterBlock")) { // CrafterBlockEntity.getRedstoneSignal: filled or disabled slots
        auto c = level.blockEntityContainer(pos);
        int count = 0;
        std::vector<int> disabled;
        if (const auto* arr = be->getIntArray("disabled_slots")) disabled = *arr;
        for (int i = 0; i < 9; i++)
            if ((c && !c->item(i).isEmpty()) || std::ranges::find(disabled, i) != disabled.end()) count++;
        return count;
    }
    if (isA(b, "ShelfBlock")) { // a bit per filled slot, only out of the back
        if (dir != direction::opposite(dirValue(s, "facing"))) return 0;
        int bits = 0;
        if (const nbt::List* list = be->getList("Items"))
            for (const auto& e : list->items)
                if (const nbt::Compound* c = e.asCompound()) {
                    int slot = c->getInt("Slot") & 0xFF;
                    if (slot < 3 && item_nbt::read(*c)) bits |= 1 << slot;
                }
        return bits;
    }
    if (isA(b, "DecoratedPotBlock")) { // a single item container
        const nbt::Compound* item = be->getCompound("item");
        auto stack = item ? item_nbt::read(*item) : std::nullopt;
        return stack && !stack->isEmpty() ? lerpDiscrete15(static_cast<float>(stack->count) / std::min(99, stack->maxStackSize())) : 0;
    }
    // Containers: barrels, furnaces, brewing stands, hoppers, dispensers, droppers, shulker boxes.
    if (auto c = level.blockEntityContainer(pos)) return signalFromContainer(c.get());
    return 0;
}

// ---------------------------------------------------------------- SignalGetter

int getDirectSignal(ServerLevel& level, BlockPos pos, Direction dir) { return getDirectSignal(level, pos, level.getBlockState(pos), dir); }

int getDirectSignalTo(ServerLevel& level, BlockPos pos) {
    int result = 0;
    constexpr Direction kOrder[] = {Direction::Down, Direction::Up, Direction::North, Direction::South, Direction::West, Direction::East};
    for (Direction d : kOrder) {
        result = std::max(result, getDirectSignal(level, pos.relative(d), d));
        if (result >= 15) return result;
    }
    return result;
}

int getControlInputSignal(ServerLevel& level, BlockPos pos, Direction dir, bool onlyDiodes) {
    BlockStateId s = level.getBlockState(pos);
    if (onlyDiodes) return isDiode(s) ? getDirectSignal(level, pos, dir) : 0;
    if (bs::is(s, REDSTONE_BLOCK)) return 15;
    if (bs::is(s, REDSTONE_WIRE)) return intValue(s, "power");
    return isSignalSource(s) ? getDirectSignal(level, pos, dir) : 0;
}

int getSignal(ServerLevel& level, BlockPos pos, Direction dir) {
    BlockStateId s = level.getBlockState(pos);
    int signal = getSignal(level, pos, s, dir);
    return conductor(s) ? std::max(signal, getDirectSignalTo(level, pos)) : signal;
}

bool hasNeighborSignal(ServerLevel& level, BlockPos pos) {
    constexpr Direction kOrder[] = {Direction::Down, Direction::Up, Direction::North, Direction::South, Direction::West, Direction::East};
    for (Direction d : kOrder)
        if (getSignal(level, pos.relative(d), d) > 0) return true;
    return false;
}

int getBestNeighborSignal(ServerLevel& level, BlockPos pos) {
    int best = 0;
    for (Direction d : direction::kAll) {
        int signal = getSignal(level, pos.relative(d), d);
        if (signal >= 15) return 15;
        best = std::max(best, signal);
    }
    return best;
}

// RedstoneWireBlock.getBlockSignal: the strongest signal into `pos`, not counting wires.
int wireBlockSignal(ServerLevel& level, BlockPos pos) {
    gShouldSignal = false;
    int signal = getBestNeighborSignal(level, pos);
    gShouldSignal = true;
    return signal;
}

} // namespace mc::redstone
