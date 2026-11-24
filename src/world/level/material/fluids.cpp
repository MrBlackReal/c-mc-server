#include "world/level/material/fluids.hpp"

#include <array>
#include <cmath>
#include <string>
#include <unordered_map>

#include "core/registries.hpp"
#include "generated/blocks.hpp"
#include "server/level/server_level.hpp"
#include "world/level/block/block_behaviour.hpp"

namespace mc::fluids {

namespace bs = blocks;
using namespace gen::block;

namespace {
constexpr std::string_view kNames[] = {"minecraft:empty", "minecraft:flowing_water", "minecraft:water", "minecraft:flowing_lava", "minecraft:lava"};

// FlowingFluid.getLegacyLevel: LiquidBlock's level property for a fluid state.
int legacyLevel(FluidState f) { return f.isSource() ? 0 : 8 - std::min<int>(f.amount, 8) + (f.falling ? 8 : 0); }

// The fluid a FlowingFluid method runs for: water or lava (`this`), and what it reads from the level.
struct Flowing {
    ServerLevel& level;
    bool lava;

    uint8_t sourceType() const { return lava ? kLava : kWater; }
    uint8_t flowingType() const { return lava ? kFlowingLava : kFlowingWater; }
    bool isSame(uint8_t t) const { return lava ? isLava(t) : isWater(t); }
    // LavaFluid.isFastLava (the dimension's minecraft:gameplay/fast_lava)
    bool fastLava() const { return lava && level.fastLava(); }
    int dropOff() const { return lava && !fastLava() ? 2 : 1; }
    int slopeFindDistance() const { return lava && !fastLava() ? 2 : 4; }
    bool canConvertToSource() const {
        return level.gameRules().getBool(lava ? GameRules::LavaSourceConversion : GameRules::WaterSourceConversion);
    }
    bool isSourceBlockOfThisType(FluidState f) const { return isSame(f.type) && f.isSource(); }

    // FlowingFluid.canPassThroughWall: whether the collision shapes of the two blocks leave a gap
    // in the face between them (Shapes.mergedFaceOccludes, from the extractor's table).
    static bool canPassThroughWall(Direction d, BlockStateId sourceState, BlockStateId targetState) {
        if (info::collisionIsBlock(targetState) || info::collisionIsBlock(sourceState)) return false;
        size_t a = gen::kStateShape[sourceState], b = gen::kStateShape[targetState];
        if (gen::kShapes[a].empty() && gen::kShapes[b].empty()) return true;
        return !(gen::kShapeFaceOccludes[a * gen::kShapes.size() + b] >> static_cast<int>(d) & 1);
    }
    bool canMaybePassThrough(BlockStateId sourceState, Direction d, BlockStateId testState, FluidState testFluid) const {
        return !isSourceBlockOfThisType(testFluid) && info::canHoldAnyFluid(testState) && canPassThroughWall(d, sourceState, testState);
    }
    bool canPassThrough(uint8_t fluid, BlockStateId sourceState, Direction d, BlockStateId testState, FluidState testFluid) const {
        return canMaybePassThrough(sourceState, d, testState, testFluid) && info::canHoldSpecificFluid(testState, fluid);
    }
    bool isWaterHole(BlockStateId topState, BlockStateId bottomState) const {
        if (!canPassThroughWall(Direction::Down, topState, bottomState)) return false;
        return isSame(stateOf(bottomState).type) || info::canHoldFluid(bottomState, flowingType());
    }

    // FluidState.canBeReplacedWith (WaterFluid: from above by anything but water; LavaFluid: by
    // water when at least 4/9 high).
    bool canBeReplacedWith(BlockPos pos, FluidState current, uint8_t by, Direction d) const {
        if (current.isEmpty()) return true;
        if (isWater(current.type)) return d == Direction::Down && !isWater(by);
        return height(level, pos, current) >= 0.44444445f && isWater(by);
    }

    FluidState getNewLiquid(BlockPos pos, BlockStateId state) const;
    void spread(BlockPos pos, BlockStateId state, FluidState fluid) const;
    void spreadToSides(BlockPos pos, FluidState fluid, BlockStateId state) const;
    void spreadTo(BlockPos pos, BlockStateId state, Direction d, FluidState target) const;
    void beforeDestroyingBlock(BlockPos pos, BlockStateId state) const;
    int sourceNeighborCount(BlockPos pos) const;
    struct SpreadContext;
    int getSlopeDistance(BlockPos pos, int pass, Direction from, BlockStateId state, SpreadContext& context) const;
    std::array<std::optional<FluidState>, 6> getSpread(BlockPos pos, BlockStateId state) const;
};

FluidState Flowing::getNewLiquid(BlockPos pos, BlockStateId state) const {
    int highestNeighbor = 0, neighbourSources = 0;
    for (Direction d : direction::kHorizontalPlane) {
        BlockPos testPos = pos.relative(d);
        BlockStateId testState = level.getBlockState(testPos);
        FluidState testFluid = stateOf(testState);
        if (isSame(testFluid.type) && canPassThroughWall(d, state, testState)) {
            if (testFluid.isSource()) neighbourSources++;
            highestNeighbor = std::max<int>(highestNeighbor, testFluid.amount);
        }
    }
    if (neighbourSources >= 2 && canConvertToSource()) {
        BlockStateId below = level.getBlockState(pos.below());
        if (bs::isSolid(below) || isSourceBlockOfThisType(stateOf(below))) return source(sourceType());
    }
    BlockPos abovePos = pos.above();
    BlockStateId above = level.getBlockState(abovePos);
    FluidState aboveFluid = stateOf(above);
    if (!aboveFluid.isEmpty() && isSame(aboveFluid.type) && canPassThroughWall(Direction::Up, state, above))
        return flowing(flowingType(), 8, true);
    int amount = highestNeighbor - dropOff();
    return amount <= 0 ? FluidState{} : flowing(flowingType(), amount, false);
}

void Flowing::spread(BlockPos pos, BlockStateId state, FluidState fluid) const {
    if (fluid.isEmpty()) return;
    BlockPos belowPos = pos.below();
    BlockStateId belowState = level.getBlockState(belowPos);
    FluidState belowFluid = stateOf(belowState);
    if (canMaybePassThrough(state, Direction::Down, belowState, belowFluid)) {
        FluidState newFluid = getNewLiquid(belowPos, belowState);
        if (canBeReplacedWith(belowPos, belowFluid, newFluid.type, Direction::Down) && info::canHoldSpecificFluid(belowState, newFluid.type)) {
            spreadTo(belowPos, belowState, Direction::Down, newFluid);
            if (sourceNeighborCount(pos) >= 3) spreadToSides(pos, fluid, state);
            return;
        }
    }
    if (fluid.isSource() || !isWaterHole(state, belowState)) spreadToSides(pos, fluid, state);
}

void Flowing::spreadToSides(BlockPos pos, FluidState fluid, BlockStateId state) const {
    int neighbor = fluid.amount - dropOff();
    if (fluid.falling) neighbor = 7;
    if (neighbor <= 0) return;
    auto spreads = getSpread(pos, state);
    for (int d = 0; d < 6; d++) { // (an EnumMap: in Direction order)
        if (!spreads[d]) continue;
        BlockPos np = pos.relative(static_cast<Direction>(d));
        spreadTo(np, level.getBlockState(np), static_cast<Direction>(d), *spreads[d]);
    }
}

void Flowing::spreadTo(BlockPos pos, BlockStateId state, Direction d, FluidState target) const {
    if (lava && d == Direction::Down && isWater(stateOf(state).type)) { // LavaFluid.spreadTo: lava falling into water
        if (bs::isInstance(bs::blockOf(state), "LiquidBlock")) level.setBlock(pos, bs::defaultState(STONE), ServerLevel::kUpdateAll);
        level.levelEvent(1501, pos, 0); // fizz
        return;
    }
    if (info::isLiquidBlockContainer(state)) {
        placeLiquid(level, pos, state, target);
        return;
    }
    if (!bs::isAir(state)) beforeDestroyingBlock(pos, state);
    level.setBlock(pos, legacyBlock(target), ServerLevel::kUpdateAll);
}

void Flowing::beforeDestroyingBlock(BlockPos pos, BlockStateId state) const {
    if (lava) { // LavaFluid.fizz
        level.levelEvent(1501, pos, 0);
        return;
    }
    // WaterFluid: Block.dropResources(state, level, pos, blockEntity)
    LevelChunk& chunk = level.getChunk({pos.x >> 4, pos.z >> 4});
    std::optional<nbt::Compound> be;
    if (auto* b = chunk.blockEntityAt(pos.x, pos.y, pos.z); b && b->blockAtLoad == bs::blockOf(state)) be = b->tag;
    level.dropResources(state, pos, be ? &*be : nullptr, nullptr, nullptr);
}

int Flowing::sourceNeighborCount(BlockPos pos) const {
    int count = 0;
    for (Direction d : direction::kHorizontalPlane)
        if (isSourceBlockOfThisType(stateOf(level.getBlockState(pos.relative(d))))) count++;
    return count;
}

// FlowingFluid.SpreadContext: block states and holes around the origin, read once per getSpread.
struct Flowing::SpreadContext {
    const Flowing& fluid;
    BlockPos origin;
    std::unordered_map<int, BlockStateId> states;
    std::unordered_map<int, bool> holes;

    int key(BlockPos p) const { return ((p.x - origin.x + 128) & 0xFF) << 8 | ((p.z - origin.z + 128) & 0xFF); }
    BlockStateId getBlockState(BlockPos p) {
        auto [it, added] = states.try_emplace(key(p), 0);
        if (added) it->second = fluid.level.getBlockState(p);
        return it->second;
    }
    bool isHole(BlockPos p) {
        int k = key(p);
        if (auto it = holes.find(k); it != holes.end()) return it->second;
        bool hole = fluid.isWaterHole(getBlockState(p), fluid.level.getBlockState(p.below()));
        holes.emplace(k, hole);
        return hole;
    }
};

int Flowing::getSlopeDistance(BlockPos pos, int pass, Direction from, BlockStateId state, SpreadContext& context) const {
    int lowest = 1000;
    for (Direction d : direction::kHorizontalPlane) {
        if (d == from) continue;
        BlockPos testPos = pos.relative(d);
        BlockStateId testState = context.getBlockState(testPos);
        if (!canPassThrough(flowingType(), state, d, testState, stateOf(testState))) continue;
        if (context.isHole(testPos)) return pass;
        if (pass < slopeFindDistance()) lowest = std::min(lowest, getSlopeDistance(testPos, pass + 1, direction::opposite(d), testState, context));
    }
    return lowest;
}

std::array<std::optional<FluidState>, 6> Flowing::getSpread(BlockPos pos, BlockStateId state) const {
    int lowest = 1000;
    std::array<std::optional<FluidState>, 6> result;
    std::optional<SpreadContext> context;
    for (Direction d : direction::kHorizontalPlane) {
        BlockPos testPos = pos.relative(d);
        BlockStateId testState = level.getBlockState(testPos);
        FluidState testFluid = stateOf(testState);
        if (!canMaybePassThrough(state, d, testState, testFluid)) continue;
        FluidState newFluid = getNewLiquid(testPos, testState);
        if (!info::canHoldSpecificFluid(testState, newFluid.type)) continue;
        if (!context) context.emplace(SpreadContext{*this, pos, {}, {}});
        int distance = context->isHole(testPos) ? 0 : getSlopeDistance(testPos, 1, direction::opposite(d), testState, *context);
        if (distance < lowest) result = {};
        if (distance <= lowest) {
            if (canBeReplacedWith(testPos, testFluid, newFluid.type, d)) result[static_cast<int>(d)] = newFluid;
            lowest = distance;
        }
    }
    return result;
}

// LiquidBlock.shouldSpreadLiquid: lava touching water becomes obsidian (source) or cobblestone;
// over soul soil next to blue ice it becomes basalt. False if that happened.
bool shouldSpreadLiquid(ServerLevel& level, BlockPos pos, BlockStateId state) {
    if (!isLava(stateOf(state).type)) return true;
    bool overSoulSoil = bs::is(level.getBlockState(pos.below()), SOUL_SOIL);
    // LiquidBlock.POSSIBLE_FLOW_DIRECTIONS, each looked at from the opposite side
    for (Direction d : {Direction::Down, Direction::South, Direction::North, Direction::East, Direction::West}) {
        BlockPos neighbour = pos.relative(direction::opposite(d));
        if (isWater(stateOf(level.getBlockState(neighbour)).type)) {
            uint32_t block = stateOf(level.getBlockState(pos)).isSource() ? OBSIDIAN : COBBLESTONE;
            level.setBlock(pos, bs::defaultState(block), ServerLevel::kUpdateAll);
            level.levelEvent(1501, pos, 0); // fizz
            return false;
        }
        if (overSoulSoil && bs::is(level.getBlockState(neighbour), BLUE_ICE)) {
            level.setBlock(pos, bs::defaultState(BASALT), ServerLevel::kUpdateAll);
            level.levelEvent(1501, pos, 0);
            return false;
        }
    }
    return true;
}
} // namespace

std::string_view name(uint8_t type) { return type < std::size(kNames) ? kNames[type] : kNames[0]; }

std::optional<uint8_t> byName(std::string_view n) {
    for (uint8_t i = 0; i < std::size(kNames); i++)
        if (kNames[i] == n || kNames[i].substr(10) == n) return i;
    return std::nullopt;
}

FluidState source(uint8_t type) { return {static_cast<uint8_t>(isLava(type) ? kLava : kWater), 8, false}; }
FluidState flowing(uint8_t type, int amount, bool falling) {
    return {static_cast<uint8_t>(isLava(type) ? kFlowingLava : kFlowingWater), static_cast<uint8_t>(amount), falling};
}

BlockStateId legacyBlock(FluidState f) {
    if (f.isEmpty()) return bs::defaultState(AIR);
    return bs::setValue(bs::defaultState(isLava(f.type) ? LAVA : WATER), "level", std::to_string(legacyLevel(f)));
}

int tickDelay(ServerLevel& level, uint8_t type) {
    if (isWater(type)) return 5;
    if (isLava(type)) return level.fastLava() ? 10 : 30;
    return 0;
}

float ownHeight(FluidState f) { return f.isEmpty() ? 0.0f : f.amount / 9.0f; }

float height(ServerLevel& level, BlockPos pos, FluidState f) {
    if (f.isEmpty()) return 0.0f;
    return isSame(f.type, stateOf(level.getBlockState(pos.above())).type) ? 1.0f : ownHeight(f); // hasSameAbove
}

std::array<double, 3> flow(ServerLevel& level, BlockPos pos, FluidState fluid) {
    auto normalize = [](double x, double y, double z) -> std::array<double, 3> { // Vec3.normalize
        double d = std::sqrt(x * x + y * y + z * z);
        return d < 1.0e-5f ? std::array<double, 3>{0, 0, 0} : std::array<double, 3>{x / d, y / d, z / d};
    };
    if (fluid.isEmpty()) return {0, 0, 0}; // (EmptyFluid)
    auto affectsFlow = [&](FluidState n) { return n.isEmpty() || isSame(n.type, fluid.type); };
    double flowX = 0, flowZ = 0;
    for (Direction d : direction::kHorizontalPlane) {
        BlockPos np = pos.relative(d);
        FluidState neighbour = stateOf(level.getBlockState(np));
        if (!affectsFlow(neighbour)) continue;
        float neighbourHeight = ownHeight(neighbour);
        float distance = 0.0f;
        if (neighbourHeight == 0.0f) {
            if (!bs::stateHasTag(level.getBlockState(np), "minecraft:blocks_fluid_flow")) {
                FluidState below = stateOf(level.getBlockState(np.below()));
                if (affectsFlow(below)) {
                    neighbourHeight = ownHeight(below);
                    if (neighbourHeight > 0.0f) distance = ownHeight(fluid) - (neighbourHeight - 0.8888889f);
                }
            }
        } else if (neighbourHeight > 0.0f) {
            distance = ownHeight(fluid) - neighbourHeight;
        }
        if (distance != 0.0f) flowX += direction::stepX(d) * distance, flowZ += direction::stepZ(d) * distance;
    }
    std::array<double, 3> result{flowX, 0.0, flowZ};
    if (fluid.falling) {
        // isSolidFace: a sturdy face of something that isn't this fluid (ice doesn't count; up always does)
        auto isSolidFace = [&](BlockPos p, Direction d) {
            BlockStateId st = level.getBlockState(p);
            if (isSame(stateOf(st).type, fluid.type)) return false;
            if (d == Direction::Up) return true;
            return !bs::isInstance(bs::blockOf(st), "IceBlock") && bs::isFaceSturdy(st, d);
        };
        for (Direction d : direction::kHorizontalPlane) {
            BlockPos np = pos.relative(d);
            if (isSolidFace(np, d) || isSolidFace(np.above(), d)) {
                auto n = normalize(result[0], result[1], result[2]);
                result = {n[0], n[1] - 6.0, n[2]};
                break;
            }
        }
    }
    return normalize(result[0], result[1], result[2]);
}

void tick(ServerLevel& level, BlockPos pos, BlockStateId state, FluidState fluid) {
    // FlowingFluid.tick
    Flowing self{level, isLava(fluid.type)};
    if (!fluid.isSource()) {
        FluidState newFluid = self.getNewLiquid(pos, level.getBlockState(pos));
        // getSpreadDelay (LavaFluid: rising lava usually waits four times as long)
        int delay = tickDelay(level, fluid.type);
        if (self.lava && !fluid.isEmpty() && !newFluid.isEmpty() && !fluid.falling && !newFluid.falling &&
            height(level, pos, newFluid) > height(level, pos, fluid) && level.levelRandom().nextInt(4) != 0)
            delay *= 4;
        if (newFluid.isEmpty()) {
            fluid = newFluid;
            state = bs::defaultState(AIR);
            level.setBlock(pos, state, ServerLevel::kUpdateAll);
        } else if (newFluid != fluid) {
            fluid = newFluid;
            state = legacyBlock(newFluid);
            level.setBlock(pos, state, ServerLevel::kUpdateAll);
            level.scheduleFluidTick(pos, newFluid.type, delay);
        }
    }
    self.spread(pos, state, fluid);
}

namespace {
// LiquidBlock.shouldBubbleColumnOccupy: a full source of a fluid in minecraft:bubble_column_can_occupy (water).
bool shouldBubbleColumnOccupy(BlockStateId state) {
    FluidState f = stateOf(state);
    return f.type == kWater && f.amount == 8;
}

bool enablesBubbleColumn(BlockStateId below) {
    return bs::stateHasTag(below, "minecraft:enables_bubble_column_push_up") || bs::stateHasTag(below, "minecraft:enables_bubble_column_drag_down");
}

// LiquidBlock.tryScheduleBubbleBlockColumn
void tryScheduleBubbleBlockColumn(ServerLevel& level, BlockPos pos, BlockStateId state, BlockStateId below) {
    if (enablesBubbleColumn(below)) level.scheduleTick(pos, bs::blockOf(state), 20);
}

// BubbleColumnBlock.canOccupy: a bubble column, or a full water source block
bool canOccupy(BlockStateId s) {
    if (bs::is(s, BUBBLE_COLUMN)) return true;
    FluidState f = stateOf(s);
    return f.type == kWater && bs::isInstance(bs::blockOf(s), "LiquidBlock") && f.amount >= 8;
}

// BubbleColumnBlock.getColumnState
BlockStateId columnState(BlockStateId below, BlockStateId occupy) {
    if (bs::is(below, BUBBLE_COLUMN)) return below;
    if (bs::stateHasTag(below, "minecraft:enables_bubble_column_push_up")) return bs::setBool(bs::defaultState(BUBBLE_COLUMN), "drag", false);
    if (bs::stateHasTag(below, "minecraft:enables_bubble_column_drag_down")) return bs::setBool(bs::defaultState(BUBBLE_COLUMN), "drag", true);
    return bs::is(occupy, BUBBLE_COLUMN) ? bs::defaultState(WATER) : occupy;
}

// BubbleColumnBlock.updateColumn: the block at `at` and the water above it follow what's below.
void updateColumn(ServerLevel& level, BlockPos at, BlockStateId occupy, BlockStateId below) {
    if (!canOccupy(occupy)) return;
    BlockStateId column = columnState(below, occupy);
    level.setBlock(at, column, ServerLevel::kUpdateClients);
    for (BlockPos p = at.above(); p.y <= level.maxY() && canOccupy(level.getBlockState(p)); p = p.above())
        if (!level.setBlock(p, column, ServerLevel::kUpdateClients)) return;
}
} // namespace

void liquidOnPlace(ServerLevel& level, BlockPos pos, BlockStateId state) {
    if (shouldSpreadLiquid(level, pos, state)) level.scheduleFluidTick(pos, stateOf(state).type, tickDelay(level, stateOf(state).type));
    if (shouldBubbleColumnOccupy(state)) tryScheduleBubbleBlockColumn(level, pos, state, level.getBlockState(pos.below()));
}

void liquidNeighborChanged(ServerLevel& level, BlockPos pos, BlockStateId state) { liquidOnPlace(level, pos, state); }

void liquidTick(ServerLevel& level, BlockPos pos, BlockStateId state) {
    if (shouldBubbleColumnOccupy(state)) updateColumn(level, pos, level.getBlockState(pos), level.getBlockState(pos.below()));
}

void bubbleColumnTick(ServerLevel& level, BlockPos pos, BlockStateId state) { updateColumn(level, pos, state, level.getBlockState(pos.below())); }

bool bubbleColumnCanSurvive(ServerLevel& level, BlockPos pos) {
    BlockStateId below = level.getBlockState(pos.below());
    return bs::is(below, BUBBLE_COLUMN) || enablesBubbleColumn(below);
}

void bubbleColumnShapeUpdate(ServerLevel& level, BlockPos pos, BlockStateId state, Direction toNeighbour, BlockStateId neighbour) {
    // (the water tick is scheduled with every other water-holding block's, in scheduleOnShapeUpdate)
    if (!bubbleColumnCanSurvive(level, pos) || toNeighbour == Direction::Down ||
        (toNeighbour == Direction::Up && !bs::is(neighbour, BUBBLE_COLUMN) && canOccupy(neighbour)))
        level.scheduleTick(pos, bs::blockOf(state), 5);
}

void scheduleOnShapeUpdate(ServerLevel& level, BlockPos pos, BlockStateId state, Direction toNeighbour, BlockStateId neighbour) {
    FluidState fluid = stateOf(state);
    if (fluid.isEmpty()) return;
    if (bs::isInstance(bs::blockOf(state), "LiquidBlock")) { // LiquidBlock.updateShape
        if (fluid.isSource() || stateOf(neighbour).isSource()) level.scheduleFluidTick(pos, fluid.type, tickDelay(level, fluid.type));
        if (toNeighbour == Direction::Down && shouldBubbleColumnOccupy(state)) tryScheduleBubbleBlockColumn(level, pos, state, neighbour);
        return;
    }
    level.scheduleFluidTick(pos, fluid.type, tickDelay(level, fluid.type)); // SimpleWaterloggedBlock & co.
}

bool placeLiquid(ServerLevel& level, BlockPos pos, BlockStateId state, FluidState fluid) {
    // SimpleWaterloggedBlock.placeLiquid (other containers, like kelp, take nothing)
    if (!info::isSimpleWaterlogged(state) || bs::getBool(state, "waterlogged") || fluid.type != kWater) return false;
    BlockStateId waterlogged = bs::setBool(state, "waterlogged", true);
    uint32_t block = bs::blockOf(state);
    if (bs::isInstance(block, "CampfireBlock")) { // CampfireBlock.placeLiquid: put out
        if (bs::getBool(state, "lit"))
            level.playBlockSound(-1, pos, registries::id("minecraft:sound_event", "minecraft:entity.generic.extinguish_fire").value_or(-1), 1.0f, 1.0f);
        waterlogged = bs::setBool(waterlogged, "lit", false);
    } else if (bs::isInstance(block, "AbstractCandleBlock") && bs::getBool(state, "lit")) { // CandleBlock.placeLiquid: extinguish
        level.setBlock(pos, bs::setBool(waterlogged, "lit", false), ServerLevel::kUpdateAll | 8);
        level.playBlockSound(-1, pos, registries::id("minecraft:sound_event", "minecraft:block.candle.extinguish").value_or(-1), 1.0f, 1.0f);
        level.scheduleFluidTick(pos, fluid.type, tickDelay(level, fluid.type));
        return true;
    }
    level.setBlock(pos, waterlogged, ServerLevel::kUpdateAll);
    level.scheduleFluidTick(pos, fluid.type, tickDelay(level, fluid.type));
    return true;
}

std::optional<std::string_view> pickupBlock(ServerLevel& level, BlockPos pos, BlockStateId state, std::string_view* pickupSound) {
    uint32_t block = bs::blockOf(state);
    auto sound = [&](std::string_view s) {
        if (pickupSound) *pickupSound = s;
    };
    constexpr int kUpdateAllImmediate = ServerLevel::kUpdateAll | 8;
    if (bs::isInstance(block, "LiquidBlock")) {
        if (bs::getValue(state, "level") != "0") return std::nullopt;
        level.setBlock(pos, bs::defaultState(AIR), kUpdateAllImmediate);
        bool lava = isLava(stateOf(state).type);
        sound(lava ? "minecraft:item.bucket.fill_lava" : "minecraft:item.bucket.fill");
        return lava ? "minecraft:lava_bucket" : "minecraft:water_bucket";
    }
    if (block == POWDER_SNOW) {
        level.setBlock(pos, bs::defaultState(AIR), kUpdateAllImmediate);
        level.levelEvent(2001, pos, static_cast<int>(state));
        sound("minecraft:item.bucket.fill_powder_snow");
        return "minecraft:powder_snow_bucket";
    }
    if (block == BUBBLE_COLUMN) {
        level.setBlock(pos, bs::defaultState(AIR), kUpdateAllImmediate);
        sound("minecraft:item.bucket.fill");
        return "minecraft:water_bucket";
    }
    if (info::isSimpleWaterlogged(state) && bs::getBool(state, "waterlogged")) { // SimpleWaterloggedBlock.pickupBlock
        level.setBlock(pos, bs::setBool(state, "waterlogged", false), ServerLevel::kUpdateAll);
        if (!block_behaviour::canSurvive(state, level, pos)) level.destroyBlock(pos, true);
        sound("minecraft:item.bucket.fill");
        return "minecraft:water_bucket";
    }
    return std::nullopt;
}

} // namespace mc::fluids
