// BlockBehaviour.randomTick (and the scheduled ticks that go with it) for the blocks that grow,
// spread, decay, melt or weather on their own: net.minecraft.world.level.block.*.randomTick.
#include <cmath>
#include <optional>
#include <string>

#include "core/registries.hpp"
#include "generated/blocks.hpp"
#include "server/level/server_level.hpp"
#include "world/level/block/block_behaviour.hpp"
#include "world/level/block/block_behaviour_internal.hpp"
#include "world/level/block/entity/block_entity_container.hpp"
#include "world/level/levelgen/feature/tree_feature.hpp"
#include "world/level/levelgen/feature/tree_grower.hpp"
#include "world/timeline/timeline.hpp"

namespace mc::block_behaviour {
namespace bs = blocks;
using namespace gen::block;
using detail::intValue;

namespace {
constexpr int kUpdateClients = ServerLevel::kUpdateClients;
constexpr int kUpdateNone = 260; // UPDATE_INVISIBLE | UPDATE_SKIP_BLOCK_ENTITY_SIDEEFFECTS: nobody is told

bool isA(uint32_t block, std::string_view cls) { return bs::isInstance(block, cls); }
std::string_view dirName(Direction d) { return direction::name(d); }
BlockStateId withInt(BlockStateId s, std::string_view prop, int v) { return bs::setValue(s, prop, std::to_string(v)); }
int32_t soundId(std::string_view name) { return registries::id("minecraft:sound_event", name).value_or(-1); }

// Direction.Plane.HORIZONTAL (vanilla's order, for random picks) and Direction.values().
constexpr Direction kPlane[] = {Direction::North, Direction::East, Direction::South, Direction::West};
Direction randomHorizontal(LegacyRandom& random) { return kPlane[random.nextInt(4)]; }
Direction randomDirection(LegacyRandom& random) { return direction::kAll[random.nextInt(6)]; }

void setBlockAndUpdate(ServerLevel& level, BlockPos pos, BlockStateId state) { level.setBlock(pos, state, ServerLevel::kUpdateAll); }
// Level.removeBlock: the block's fluid (waterlogged: water) or air.
void removeBlock(ServerLevel& level, BlockPos pos) {
    BlockStateId s = level.getBlockState(pos);
    setBlockAndUpdate(level, pos, bs::getBool(s, "waterlogged") ? bs::defaultState(WATER) : bs::defaultState(AIR));
}
bool isWater(BlockStateId s) { return (bs::flags(s) & bs::kWater) != 0; }
// FluidState.isFull: a source, or a falling flow (liquid block levels 8-15).
bool fluidIsFull(BlockStateId s) {
    if (!(bs::flags(s) & (bs::kWater | bs::kLava))) return false;
    if (bs::flags(s) & bs::kFluidSource) return true;
    return (bs::is(s, WATER) || bs::is(s, LAVA)) && intValue(s, "level") >= 8;
}
// The column (-dx..dx, dy0..dy1, -dz..dz) around `pos` has a block matching `test` (findBlocksIn).
template <class Test>
int countBlocks(ServerLevel& level, BlockPos from, BlockPos to, Test test, int stopAt) {
    int n = 0;
    for (int x = from.x; x <= to.x; x++)
        for (int y = from.y; y <= to.y; y++)
            for (int z = from.z; z <= to.z; z++)
                if (test(level.getBlockState({x, y, z})) && ++n >= stopAt) return n;
    return n;
}
bool isInsideBuildHeight(ServerLevel& level, BlockPos pos) { return pos.y >= level.minY() && pos.y <= level.maxY(); }

// ---------------------------------------------------------------- crops (CropBlock and friends)

int cropMaxAge(uint32_t b) { return isA(b, "TorchflowerCropBlock") ? 2 : isA(b, "BeetrootBlock") ? 3 : 7; }
BlockStateId cropStateForAge(uint32_t b, int age) {
    if (isA(b, "TorchflowerCropBlock") && age == 2) return bs::defaultState(TORCHFLOWER);
    return withInt(bs::defaultState(b), "age", age);
}

// CropBlock.getGrowthSpeed
float growthSpeed(uint32_t type, ServerLevel& level, BlockPos pos) {
    float speed = 1.0f;
    BlockPos below = pos.below();
    for (int xx = -1; xx <= 1; xx++)
        for (int zz = -1; zz <= 1; zz++) {
            float blockSpeed = 0.0f;
            BlockStateId st = level.getBlockState(below.offset(xx, 0, zz));
            if (bs::stateHasTag(st, "minecraft:grows_crops")) {
                blockSpeed = 1.0f;
                if (intValue(st, "moisture") > 0) blockSpeed = 3.0f;
            }
            if (xx != 0 || zz != 0) blockSpeed /= 4.0f;
            speed += blockSpeed;
        }
    auto is = [&](BlockPos p) { return bs::blockOf(level.getBlockState(p)) == type; };
    BlockPos north = pos.north(), south = pos.south(), west = pos.west(), east = pos.east();
    bool horizontal = is(west) || is(east);
    bool vertical = is(north) || is(south);
    if (horizontal && vertical) {
        speed /= 2.0f;
    } else if (is(west.north()) || is(east.north()) || is(east.south()) || is(west.south())) {
        speed /= 2.0f;
    }
    return speed;
}
bool growthRoll(uint32_t type, ServerLevel& level, BlockPos pos, LegacyRandom& random) {
    float speed = growthSpeed(type, level, pos);
    return random.nextInt(static_cast<int>(25.0f / speed) + 1) == 0;
}

void cropTick(ServerLevel& level, BlockPos pos, BlockStateId s, LegacyRandom& random) {
    uint32_t b = bs::blockOf(s);
    if ((isA(b, "BeetrootBlock") || isA(b, "TorchflowerCropBlock")) && random.nextInt(3) == 0) return;
    if (level.rawBrightness(pos, 0) < 9) return;
    int age = intValue(s, "age");
    if (age < cropMaxAge(b) && growthRoll(b, level, pos, random)) level.setBlock(pos, cropStateForAge(b, age + 1), kUpdateClients);
}

// PitcherCropBlock.grow
void pitcherGrow(ServerLevel& level, BlockStateId lower, BlockPos lowerPos, int increase) {
    int age = intValue(lower, "age");
    int updated = std::min(age + increase, 4);
    BlockPos above = lowerPos.above();
    auto canGrowInto = [&] {
        BlockStateId st = level.getBlockState(above);
        return bs::isAir(st) || bs::is(st, PITCHER_CROP);
    };
    bool canGrow = age < 4 && level.rawBrightness(lowerPos, 0) >= 8 && isInsideBuildHeight(level, above) && (updated < 3 || canGrowInto());
    if (!canGrow) return;
    BlockStateId newLower = withInt(lower, "age", updated);
    level.setBlock(lowerPos, newLower, kUpdateClients);
    if (updated >= 3) setBlockAndUpdate(level, above, bs::setValue(newLower, "half", "upper"));
}

// StemBlock.randomTick
void stemTick(ServerLevel& level, BlockPos pos, BlockStateId s, LegacyRandom& random) {
    uint32_t b = bs::blockOf(s);
    if (level.rawBrightness(pos, 0) < 9 || !growthRoll(b, level, pos, random)) return;
    int age = intValue(s, "age");
    if (age < 7) {
        level.setBlock(pos, withInt(s, "age", age + 1), kUpdateClients);
        return;
    }
    Direction d = randomHorizontal(random);
    BlockPos target = pos.relative(d);
    bool melon = b == MELON_STEM;
    if (bs::isAir(level.getBlockState(target)) &&
        bs::stateHasTag(level.getBlockState(target.below()), melon ? "minecraft:supports_melon_stem_fruit" : "minecraft:supports_pumpkin_stem_fruit")) {
        setBlockAndUpdate(level, target, bs::defaultState(melon ? MELON : PUMPKIN));
        setBlockAndUpdate(level, pos, bs::setValue(bs::defaultState(melon ? ATTACHED_MELON_STEM : ATTACHED_PUMPKIN_STEM), "facing", dirName(d)));
    }
}

// ---------------------------------------------------------------- tall plants

void sugarCaneTick(ServerLevel& level, BlockPos pos, BlockStateId s) {
    if (!level.isEmptyBlock(pos.above())) return;
    uint32_t b = bs::blockOf(s);
    int height = 1;
    while (bs::blockOf(level.getBlockState(pos.below(height))) == b) height++;
    if (height >= 3) return;
    int age = intValue(s, "age");
    if (age == 15) {
        setBlockAndUpdate(level, pos.above(), bs::defaultState(b));
        level.setBlock(pos, withInt(s, "age", 0), kUpdateNone);
    } else {
        level.setBlock(pos, withInt(s, "age", age + 1), kUpdateNone);
    }
}

void cactusTick(ServerLevel& level, BlockPos pos, BlockStateId s, LegacyRandom& random) {
    BlockPos above = pos.above();
    if (!level.isEmptyBlock(above)) return;
    uint32_t b = bs::blockOf(s);
    int height = 1, age = intValue(s, "age");
    while (bs::blockOf(level.getBlockState(pos.below(height))) == b)
        if (++height == 3 && age == 15) return;
    if (age == 8 && canSurvive(bs::defaultState(b), level, above)) {
        double chance = height >= 3 ? 0.25 : 0.1;
        if (random.nextDouble() <= chance) setBlockAndUpdate(level, above, bs::defaultState(CACTUS_FLOWER));
    } else if (age == 15 && height < 3) {
        setBlockAndUpdate(level, above, bs::defaultState(b));
        level.setBlock(pos, withInt(s, "age", 0), kUpdateNone);
        // (level.neighborChanged for the new cactus: TODO neighbour block updates)
    }
    if (age < 15) level.setBlock(pos, withInt(s, "age", age + 1), kUpdateNone);
}

// BambooStalkBlock.growBamboo
void growBamboo(ServerLevel& level, BlockStateId s, BlockPos pos, LegacyRandom& random, int height) {
    BlockStateId below = level.getBlockState(pos.below());
    BlockPos twoBelowPos = pos.below(2);
    BlockStateId twoBelow = level.getBlockState(twoBelowPos);
    std::string_view leaves = "none";
    if (height >= 1) {
        if (!bs::is(below, BAMBOO) || bs::getValue(below, "leaves") == "none") {
            leaves = "small";
        } else {
            leaves = "large";
            if (bs::is(twoBelow, BAMBOO)) {
                setBlockAndUpdate(level, pos.below(), bs::setValue(below, "leaves", "small"));
                setBlockAndUpdate(level, twoBelowPos, bs::setValue(twoBelow, "leaves", "none"));
            }
        }
    }
    int age = intValue(s, "age") != 1 && !bs::is(twoBelow, BAMBOO) ? 0 : 1;
    int stage = (height < 11 || !(random.nextFloat() < 0.25f)) && height != 15 ? 0 : 1;
    BlockStateId grown = bs::setValue(withInt(withInt(bs::defaultState(BAMBOO), "age", age), "stage", stage), "leaves", leaves);
    setBlockAndUpdate(level, pos.above(), grown);
}

void bambooTick(ServerLevel& level, BlockPos pos, BlockStateId s, LegacyRandom& random) {
    if (intValue(s, "stage") != 0) return;
    if (random.nextInt(3) != 0 || !level.isEmptyBlock(pos.above()) || level.rawBrightness(pos.above(), 0) < 9) return;
    int below = 0; // getHeightBelowUpToMax
    while (below < 16 && bs::is(level.getBlockState(pos.below(below + 1)), BAMBOO)) below++;
    int height = below + 1;
    if (height < 16) growBamboo(level, s, pos, random, height);
}

// ChorusFlowerBlock
bool allNeighborsEmpty(ServerLevel& level, BlockPos pos, std::optional<Direction> ignore) {
    for (Direction d : kPlane)
        if (d != ignore && !level.isEmptyBlock(pos.relative(d))) return false;
    return true;
}
void chorusFlowerTick(ServerLevel& level, BlockPos pos, BlockStateId s, LegacyRandom& random) {
    uint32_t flower = bs::blockOf(s);
    BlockPos above = pos.above();
    if (!level.isEmptyBlock(above) || above.y > level.maxY()) return;
    int age = intValue(s, "age");
    if (age >= 5) return;
    auto placeGrownFlower = [&](BlockPos at, int a) {
        level.setBlock(at, withInt(bs::defaultState(flower), "age", a), kUpdateClients);
        level.levelEvent(1033, at, 0);
    };
    auto placeDeadFlower = [&](BlockPos at) {
        level.setBlock(at, withInt(bs::defaultState(flower), "age", 5), kUpdateClients);
        level.levelEvent(1034, at, 0);
    };
    auto becomePlant = [&] { level.setBlock(pos, detail::chorusConnections(bs::defaultState(CHORUS_PLANT), level, pos), kUpdateClients); };
    bool growUpwards = false, pillarOnSupportBlock = false;
    BlockStateId below = level.getBlockState(pos.below());
    if (bs::stateHasTag(below, "minecraft:supports_chorus_flower")) {
        growUpwards = true;
    } else if (bs::is(below, CHORUS_PLANT)) {
        int height = 1;
        for (int i = 0; i < 4; i++) {
            BlockStateId test = level.getBlockState(pos.below(height + 1));
            if (!bs::is(test, CHORUS_PLANT)) {
                if (bs::stateHasTag(test, "minecraft:supports_chorus_flower")) pillarOnSupportBlock = true;
                break;
            }
            height++;
        }
        if (height < 2 || height <= random.nextInt(pillarOnSupportBlock ? 5 : 4)) growUpwards = true;
    } else if (bs::isAir(below)) {
        growUpwards = true;
    }
    if (growUpwards && allNeighborsEmpty(level, above, std::nullopt) && level.isEmptyBlock(pos.above(2))) {
        becomePlant();
        placeGrownFlower(above, age);
    } else if (age < 4) {
        int attempts = random.nextInt(4);
        if (pillarOnSupportBlock) attempts++;
        bool created = false;
        for (int i = 0; i < attempts; i++) {
            Direction d = randomHorizontal(random);
            BlockPos target = pos.relative(d);
            if (level.isEmptyBlock(target) && level.isEmptyBlock(target.below()) && allNeighborsEmpty(level, target, direction::opposite(d))) {
                placeGrownFlower(target, age + 1);
                created = true;
            }
        }
        if (created) becomePlant();
        else placeDeadFlower(pos);
    } else {
        placeDeadFlower(pos);
    }
}

// GrowingPlantHeadBlock (kelp, weeping/twisting/cave vines)
Direction growthDirection(uint32_t b) {
    return isA(b, "WeepingVinesBlock") || isA(b, "WeepingVinesPlantBlock") || isA(b, "CaveVinesBlock") || isA(b, "CaveVinesPlantBlock") ? Direction::Down
                                                                                                                                         : Direction::Up;
}
bool plantCanGrowInto(uint32_t head, BlockStateId st) { return isA(head, "KelpBlock") ? bs::is(st, WATER) : bs::isAir(st); }

void growingPlantHeadTick(ServerLevel& level, BlockPos pos, BlockStateId s, LegacyRandom& random) {
    uint32_t b = bs::blockOf(s);
    double probability = isA(b, "KelpBlock") ? 0.14 : 0.1;
    if (intValue(s, "age") >= 25 || !(random.nextDouble() < probability)) return;
    BlockPos growthPos = pos.relative(growthDirection(b));
    if (!plantCanGrowInto(b, level.getBlockState(growthPos))) return;
    BlockStateId grown = withInt(s, "age", intValue(s, "age") + 1); // getGrowIntoState: cycle(AGE)
    if (isA(b, "CaveVinesBlock")) grown = bs::setBool(grown, "berries", level.levelRandom().nextFloat() < 0.11f);
    setBlockAndUpdate(level, growthPos, grown);
}

// VineBlock
bool vineAcceptableNeighbour(ServerLevel& level, BlockPos neighbourPos, Direction towardsNeighbour) {
    return detail::canAttachFace(level, neighbourPos.relative(direction::opposite(towardsNeighbour)), towardsNeighbour);
}
bool vineHasHorizontalConnection(BlockStateId s) {
    return bs::getBool(s, "north") || bs::getBool(s, "east") || bs::getBool(s, "south") || bs::getBool(s, "west");
}
bool vineCanSpread(ServerLevel& level, BlockPos pos) {
    return countBlocks(level, pos.offset(-4, -1, -4), pos.offset(4, 1, 4), [](BlockStateId st) { return bs::is(st, VINE); }, 5) <= 4;
}
void vineTick(ServerLevel& level, BlockPos pos, BlockStateId s, LegacyRandom& random) {
    if (!level.gameRules().getBool(GameRules::SpreadVines) || random.nextInt(4) != 0) return;
    BlockStateId vine = bs::defaultState(VINE);
    Direction test = randomDirection(random);
    BlockPos abovePos = pos.above();
    if (direction::isHorizontal(test) && !bs::getBool(s, dirName(test))) {
        if (!vineCanSpread(level, pos)) return;
        BlockPos testPos = pos.relative(test);
        BlockStateId edge = level.getBlockState(testPos);
        if (bs::isAir(edge)) {
            Direction cw = direction::clockWise(test), ccw = direction::counterClockWise(test);
            bool cwFace = bs::getBool(s, dirName(cw)), ccwFace = bs::getBool(s, dirName(ccw));
            BlockPos cwTest = testPos.relative(cw), ccwTest = testPos.relative(ccw);
            if (cwFace && vineAcceptableNeighbour(level, cwTest, cw)) {
                level.setBlock(testPos, bs::setBool(vine, dirName(cw), true), kUpdateClients);
            } else if (ccwFace && vineAcceptableNeighbour(level, ccwTest, ccw)) {
                level.setBlock(testPos, bs::setBool(vine, dirName(ccw), true), kUpdateClients);
            } else {
                Direction opposite = direction::opposite(test);
                if (cwFace && level.isEmptyBlock(cwTest) && vineAcceptableNeighbour(level, pos.relative(cw), opposite)) {
                    level.setBlock(cwTest, bs::setBool(vine, dirName(opposite), true), kUpdateClients);
                } else if (ccwFace && level.isEmptyBlock(ccwTest) && vineAcceptableNeighbour(level, pos.relative(ccw), opposite)) {
                    level.setBlock(ccwTest, bs::setBool(vine, dirName(opposite), true), kUpdateClients);
                } else if (random.nextFloat() < 0.05 && vineAcceptableNeighbour(level, testPos.above(), Direction::Up)) {
                    level.setBlock(testPos, bs::setBool(vine, "up", true), kUpdateClients);
                }
            }
        } else if (vineAcceptableNeighbour(level, testPos, test)) {
            level.setBlock(pos, bs::setBool(s, dirName(test), true), kUpdateClients);
        }
        return;
    }
    if (test == Direction::Up && pos.y < level.maxY()) {
        if (detail::vineSupportsFace(level, pos, test)) {
            level.setBlock(pos, bs::setBool(s, "up", true), kUpdateClients);
            return;
        }
        if (level.isEmptyBlock(abovePos)) {
            if (!vineCanSpread(level, pos)) return;
            BlockStateId aboveState = s;
            for (Direction d : kPlane)
                if (random.nextBoolean() || !vineAcceptableNeighbour(level, abovePos.relative(d), d)) aboveState = bs::setBool(aboveState, dirName(d), false);
            if (vineHasHorizontalConnection(aboveState)) level.setBlock(abovePos, aboveState, kUpdateClients);
            return;
        }
    }
    if (pos.y > level.minY()) {
        BlockPos belowPos = pos.below();
        BlockStateId below = level.getBlockState(belowPos);
        if (bs::isAir(below) || bs::is(below, VINE)) {
            BlockStateId before = bs::isAir(below) ? vine : below;
            BlockStateId after = before; // copyRandomFaces
            for (Direction d : kPlane)
                if (random.nextBoolean() && bs::getBool(s, dirName(d))) after = bs::setBool(after, dirName(d), true);
            if (before != after && vineHasHorizontalConnection(after)) level.setBlock(belowPos, after, kUpdateClients);
        }
    }
}

// ---------------------------------------------------------------- ground

// SpreadingSnowyBlock (grass, mycelium)
bool canStayAlive(BlockStateId s, ServerLevel& level, BlockPos pos) {
    BlockStateId above = level.getBlockState(pos.above());
    if (bs::is(above, SNOW) && intValue(above, "layers") == 1) return true;
    if (fluidIsFull(above)) return false;
    return light::lightDampeningInto(s, above, static_cast<int>(Direction::Up), bs::lightDampening(above)) < 15;
}
void spreadingSnowyTick(ServerLevel& level, BlockPos pos, BlockStateId s, LegacyRandom& random) {
    uint32_t b = bs::blockOf(s);
    if (!canStayAlive(s, level, pos)) {
        setBlockAndUpdate(level, pos, bs::defaultState(DIRT));
        return;
    }
    if (level.maxLocalRawBrightness(pos.above()) < 9) return;
    BlockStateId def = bs::defaultState(b);
    for (int i = 0; i < 4; i++) {
        int dx = random.nextInt(3) - 1, dy = random.nextInt(5) - 3, dz = random.nextInt(3) - 1;
        BlockPos test = pos.offset(dx, dy, dz);
        if (bs::is(level.getBlockState(test), DIRT) && canStayAlive(def, level, test) && !isWater(level.getBlockState(test.above())))
            setBlockAndUpdate(level, test, bs::setBool(def, "snowy", bs::stateHasTag(level.getBlockState(test.above()), "minecraft:snow")));
    }
}

// FarmlandBlock
bool shouldMaintainFarmland(ServerLevel& level, BlockPos pos) { return bs::stateHasTag(level.getBlockState(pos.above()), "minecraft:maintains_farmland"); }
void farmlandTick(ServerLevel& level, BlockPos pos, BlockStateId s) {
    int moisture = intValue(s, "moisture");
    bool nearWater = countBlocks(level, pos.offset(-4, 0, -4), pos.offset(4, 1, 4), isWater, 1) > 0;
    if (!nearWater && !level.isRainingAt(pos.above())) {
        if (moisture > 0) level.setBlock(pos, withInt(s, "moisture", moisture - 1), kUpdateClients);
        else if (!shouldMaintainFarmland(level, pos)) setBlockAndUpdate(level, pos, bs::defaultState(DIRT)); // turnToBaseBlock
    } else if (moisture < 7) {
        level.setBlock(pos, withInt(s, "moisture", 7), kUpdateClients);
    }
}

// MushroomBlock
void mushroomTick(ServerLevel& level, BlockPos pos, BlockStateId s, LegacyRandom& random) {
    if (random.nextInt(25) != 0) return;
    uint32_t b = bs::blockOf(s);
    if (countBlocks(level, pos.offset(-4, -1, -4), pos.offset(4, 1, 4), [&](BlockStateId st) { return bs::blockOf(st) == b; }, 5) > 4) return;
    auto nextOffset = [&](BlockPos from) {
        int dx = random.nextInt(3) - 1;
        int dy = random.nextInt(2);
        dy -= random.nextInt(2);
        int dz = random.nextInt(3) - 1;
        return from.offset(dx, dy, dz);
    };
    BlockPos offset = nextOffset(pos);
    for (int i = 0; i < 4; i++) {
        if (level.isEmptyBlock(offset) && canSurvive(s, level, offset)) pos = offset;
        offset = nextOffset(pos);
    }
    if (level.isEmptyBlock(offset) && canSurvive(s, level, offset)) level.setBlock(offset, s, kUpdateClients);
}

// BuddingAmethystBlock
void buddingAmethystTick(ServerLevel& level, BlockPos pos, LegacyRandom& random) {
    if (random.nextInt(5) != 0) return;
    Direction d = randomDirection(random);
    BlockPos growPos = pos.relative(d);
    BlockStateId rel = level.getBlockState(growPos);
    auto facing = [&](BlockStateId st) { return bs::getValue(st, "facing") == dirName(d); };
    std::optional<uint32_t> next;
    if (bs::isAir(rel) || (bs::is(rel, WATER) && fluidIsFull(rel))) next = SMALL_AMETHYST_BUD;
    else if (bs::is(rel, SMALL_AMETHYST_BUD) && facing(rel)) next = MEDIUM_AMETHYST_BUD;
    else if (bs::is(rel, MEDIUM_AMETHYST_BUD) && facing(rel)) next = LARGE_AMETHYST_BUD;
    else if (bs::is(rel, LARGE_AMETHYST_BUD) && facing(rel)) next = AMETHYST_CLUSTER;
    if (!next) return;
    BlockStateId target = bs::setBool(bs::setValue(bs::defaultState(*next), "facing", dirName(d)), "waterlogged", isWater(rel));
    setBlockAndUpdate(level, growPos, target);
}

// ---------------------------------------------------------------- speleothems (pointed dripstone, sulfur spikes)

bool isSpeleothem(BlockStateId s) { return bs::stateHasTag(s, "minecraft:speleothems"); }
bool isStalactite(BlockStateId s) { return detail::isSpeleothemWithDirection(s, Direction::Down); }
bool isTip(BlockStateId s, bool includeMerged) {
    if (!isSpeleothem(s)) return false;
    auto t = bs::getValue(s, "thickness");
    return t == "tip" || (includeMerged && t == "tip_merge");
}
bool isFreeHangingStalactite(BlockStateId s) { return isStalactite(s) && bs::getValue(s, "thickness") == "tip" && !bs::getBool(s, "waterlogged"); }
bool isStalactiteStartPos(BlockStateId s, ServerLevel& level, BlockPos pos) {
    return isStalactite(s) && bs::blockOf(level.getBlockState(pos.above())) != bs::blockOf(s);
}
// SpeleothemBlock.findBlockVertical
template <class Path, class Target>
std::optional<BlockPos> findBlockVertical(ServerLevel& level, BlockPos pos, int step, Path path, Target target, int maxSteps) {
    for (int i = 1; i < maxSteps; i++) {
        pos = pos.offset(0, step, 0);
        BlockStateId st = level.getBlockState(pos);
        if (target(st)) return pos;
        if (pos.y < level.minY() || pos.y > level.maxY() || !path(pos, st)) return std::nullopt;
    }
    return std::nullopt;
}
std::optional<BlockPos> findTip(BlockStateId s, ServerLevel& level, BlockPos pos, int maxSearch, bool includeMerged) {
    if (isTip(s, includeMerged)) return pos;
    Direction dir = bs::getValue(s, "vertical_direction") == "down" ? Direction::Down : Direction::Up;
    uint32_t b = bs::blockOf(s);
    return findBlockVertical(
        level, pos, direction::stepY(dir), [&](BlockPos, BlockStateId st) { return bs::blockOf(st) == b && bs::getValue(st, "vertical_direction") == dirName(dir); },
        [&](BlockStateId st) { return isTip(st, includeMerged); }, maxSearch);
}
// PointedDripstoneBlock.canDripThrough
bool canDripThrough(BlockStateId st) {
    if (bs::isAir(st)) return true;
    uint32_t f = bs::flags(st);
    if ((f & bs::kCanOcclude) && (f & bs::kFullCollision)) return false; // isSolidRender
    if (f & (bs::kWater | bs::kLava)) return false;
    for (const auto& box : gen::kShapes[gen::kStateShape[st]]) // REQUIRED_SPACE_TO_DRIP_THROUGH_NON_SOLID_BLOCK: a 4x16x4 column
        if (box.minX < 0.625 && box.maxX > 0.375 && box.minZ < 0.625 && box.maxZ > 0.375 && box.maxY > 0.0 && box.minY < 1.0) return false;
    return true;
}
struct FluidAbove {
    BlockPos pos;
    int fluid; // 0 none, 1 water, 2 lava
    BlockStateId source;
};
std::optional<FluidAbove> fluidAboveStalactite(ServerLevel& level, BlockPos pos, BlockStateId s) {
    if (!isStalactite(s)) return std::nullopt;
    uint32_t b = bs::blockOf(s);
    auto root = findBlockVertical( // findRootBlock
        level, pos, 1, [&](BlockPos, BlockStateId st) { return bs::blockOf(st) == b && bs::getValue(st, "vertical_direction") == "down"; },
        [&](BlockStateId st) { return bs::blockOf(st) != b; }, 11);
    if (!root) return std::nullopt;
    BlockPos above = root->above();
    BlockStateId aboveState = level.getBlockState(above);
    int fluid = bs::is(aboveState, MUD) ? 1 : (bs::flags(aboveState) & bs::kWater) ? 1 : (bs::flags(aboveState) & bs::kLava) ? 2 : 0;
    return FluidAbove{above, fluid, aboveState};
}

void speleothemGrow(ServerLevel& level, uint32_t self, BlockPos from, Direction to);
bool isUnmergedTipWithDirection(uint32_t self, BlockStateId s, Direction tip) {
    return isTip(s, false) && bs::getValue(s, "vertical_direction") == dirName(tip) && bs::blockOf(s) == self;
}
bool canTipGrow(uint32_t self, BlockStateId tip, ServerLevel& level, BlockPos tipPos) {
    Direction dir = bs::getValue(tip, "vertical_direction") == "down" ? Direction::Down : Direction::Up;
    BlockStateId at = level.getBlockState(tipPos.relative(dir));
    if (bs::flags(at) & (bs::kWater | bs::kLava)) return false;
    return bs::isAir(at) || isUnmergedTipWithDirection(self, at, direction::opposite(dir));
}
void createSpeleothem(ServerLevel& level, uint32_t self, BlockPos pos, Direction dir, std::string_view thickness) {
    BlockStateId st = bs::setValue(bs::setValue(bs::defaultState(self), "vertical_direction", dirName(dir)), "thickness", thickness);
    setBlockAndUpdate(level, pos, bs::setBool(st, "waterlogged", isWater(level.getBlockState(pos))));
}
void speleothemGrow(ServerLevel& level, uint32_t self, BlockPos from, Direction to) {
    BlockPos target = from.relative(to);
    BlockStateId existing = level.getBlockState(target);
    if (isUnmergedTipWithDirection(self, existing, direction::opposite(to))) { // createMergedTips
        bool up = bs::getValue(existing, "vertical_direction") == "up";
        BlockPos stalagmite = up ? target : target.below(), stalactite = up ? target.above() : target;
        createSpeleothem(level, self, stalactite, Direction::Down, "tip_merge");
        createSpeleothem(level, self, stalagmite, Direction::Up, "tip_merge");
    } else if (bs::isAir(existing) || bs::is(existing, WATER)) {
        createSpeleothem(level, self, target, to, "tip");
    }
}
void growStalagmiteBelow(ServerLevel& level, uint32_t self, BlockPos from) {
    bool dripstone = self == POINTED_DRIPSTONE;
    BlockPos pos = from;
    for (int i = 0; i < 10; i++) {
        pos = pos.below();
        BlockStateId st = level.getBlockState(pos);
        if (bs::flags(st) & (bs::kWater | bs::kLava)) return;
        if (isUnmergedTipWithDirection(self, st, Direction::Up) && canTipGrow(self, st, level, pos)) {
            speleothemGrow(level, self, pos, Direction::Up);
            return;
        }
        if (detail::isValidSpeleothemPlacement(self, level, pos, Direction::Up) && !isWater(level.getBlockState(pos.below()))) {
            speleothemGrow(level, self, pos.below(), Direction::Up);
            return;
        }
        if (dripstone && !canDripThrough(st)) return; // blocksStalagmiteScan
    }
}

void speleothemTick(ServerLevel& level, BlockPos pos, BlockStateId s, LegacyRandom& random) {
    uint32_t self = bs::blockOf(s);
    bool dripstone = self == POINTED_DRIPSTONE;
    if (dripstone) { // PointedDripstoneBlock.maybeTransferFluid
        float value = random.nextFloat();
        if ((value <= 0.17578125f || value <= 0.05859375f) && isStalactiteStartPos(s, level, pos)) {
            if (auto info = fluidAboveStalactite(level, pos, s); info && info->fluid != 0) {
                float probability = info->fluid == 1 ? 0.17578125f : 0.05859375f;
                if (value < probability) {
                    if (auto tip = findTip(s, level, pos, 11, false)) {
                        if (bs::is(info->source, MUD) && info->fluid == 1) {
                            setBlockAndUpdate(level, info->pos, bs::defaultState(CLAY));
                            level.levelEvent(1504, *tip, 0);
                        } else {
                            int fluid = info->fluid;
                            auto cauldron = findBlockVertical( // findFillableCauldronBelowStalactiteTip
                                level, *tip, -1, [](BlockPos, BlockStateId st) { return canDripThrough(st); },
                                [&](BlockStateId st) { return bs::is(st, CAULDRON) || (fluid == 1 && bs::is(st, WATER_CAULDRON)); }, 11);
                            if (cauldron) {
                                level.levelEvent(1504, *tip, 0);
                                level.scheduleTick(*cauldron, bs::blockOf(level.getBlockState(*cauldron)), 50 + tip->y - cauldron->y);
                            }
                        }
                    }
                }
            }
        }
    }
    // SpeleothemBlock.randomTick
    if (!(random.nextFloat() < 0.011377778f) || !isStalactiteStartPos(s, level, pos)) return;
    BlockStateId above = level.getBlockState(pos.above());
    bool canGrow = bs::is(above, dripstone ? DRIPSTONE_BLOCK : SULFUR);
    if (dripstone) { // PointedDripstoneBlock.canGrow: water source two above
        BlockStateId twoAbove = level.getBlockState(pos.above(2));
        canGrow = canGrow && isWater(twoAbove) && (bs::flags(twoAbove) & bs::kFluidSource);
    }
    if (!canGrow) return;
    auto tip = findTip(s, level, pos, dripstone ? 7 : 2, false);
    if (!tip) return;
    BlockStateId tipState = level.getBlockState(*tip);
    if (!isFreeHangingStalactite(tipState) || !canTipGrow(self, tipState, level, *tip)) return;
    if (random.nextBoolean()) speleothemGrow(level, self, *tip, Direction::Down);
    else growStalagmiteBelow(level, self, *tip);
}

// AbstractCauldronBlock.tick: a drip from a stalactite above lands.
void cauldronTick(ServerLevel& level, BlockPos pos, BlockStateId s) {
    auto tip = findBlockVertical(level, pos, 1, [](BlockPos, BlockStateId st) { return canDripThrough(st); }, isFreeHangingStalactite, 11);
    if (!tip) return;
    auto info = fluidAboveStalactite(level, *tip, level.getBlockState(*tip));
    int fluid = info ? info->fluid : 0;
    if (fluid == 0) return;
    if (bs::is(s, CAULDRON)) {
        setBlockAndUpdate(level, pos, bs::defaultState(fluid == 1 ? WATER_CAULDRON : LAVA_CAULDRON));
        level.levelEvent(fluid == 1 ? 1047 : 1046, pos, 0);
    } else if (bs::is(s, WATER_CAULDRON) && fluid == 1 && intValue(s, "level") < 3) { // LayeredCauldronBlock (rain type)
        setBlockAndUpdate(level, pos, withInt(s, "level", intValue(s, "level") + 1));
        level.levelEvent(1047, pos, 0);
    }
}

// ---------------------------------------------------------------- copper (WeatheringCopper / ChangeOverTimeBlock)

void weatheringTick(ServerLevel& level, BlockPos pos, BlockStateId s, LegacyRandom& random) {
    uint32_t b = bs::blockOf(s);
    const auto& info = bs::behaviour(b);
    if (random.nextFloat() >= 0.05688889f) return; // changeOverTime
    int ownAge = info.weatherAge;
    int same = 0, older = 0;
    for (int dx = -4; dx <= 4; dx++) // findBlocksInManhattan(pos, 4)
        for (int dy = -4; dy <= 4; dy++)
            for (int dz = -4; dz <= 4; dz++) {
                if (std::abs(dx) + std::abs(dy) + std::abs(dz) > 4 || (dx == 0 && dy == 0 && dz == 0)) continue;
                int age = bs::behaviour(bs::blockOf(level.getBlockState(pos.offset(dx, dy, dz)))).weatherAge;
                if (age < 0) continue;
                if (age < ownAge) return;
                if (age > ownAge) older++;
                else same++;
            }
    float chance = static_cast<float>(older + 1) / static_cast<float>(older + same + 1);
    float modifier = ownAge == 0 ? 0.75f : 1.0f;
    if (random.nextFloat() < chance * chance * modifier && info.weathersTo >= 0)
        setBlockAndUpdate(level, pos, withPropertiesOf(static_cast<uint32_t>(info.weathersTo), s));
}

// ---------------------------------------------------------------- environment-driven blocks

// The day timeline's value for an environment attribute here (the overworld's clock).
std::optional<json::Value> dayAttribute(ServerLevel& level, std::string_view attribute) {
    if (level.dimensionType() != "minecraft:overworld") return std::nullopt;
    return timeline::sample("minecraft:day", attribute, level.dayTime());
}

// EyeblossomBlock.tryChangingState
bool eyeblossomTryChange(ServerLevel& level, BlockPos pos, BlockStateId s, LegacyRandom& random) {
    bool open = bs::is(s, OPEN_EYEBLOSSOM);
    auto v = dayAttribute(level, "minecraft:gameplay/eyeblossom_open");
    bool shouldBeOpen = v && v->isBool() ? v->boolean() : open; // TriState.toBoolean(open)
    if (shouldBeOpen == open) return false;
    setBlockAndUpdate(level, pos, bs::defaultState(open ? CLOSED_EYEBLOSSOM : OPEN_EYEBLOSSOM));
    // TODO: the transform particle (minecraft:trail has options; sendParticles doesn't do those yet).
    random.nextDouble(), random.nextDouble(), random.nextDouble(), random.nextDouble(); // (its lifetime and velocity)
    for (int x = pos.x - 3; x <= pos.x + 3; x++)
        for (int y = pos.y - 2; y <= pos.y + 2; y++)
            for (int z = pos.z - 3; z <= pos.z + 3; z++) {
                if (level.getBlockState({x, y, z}) != s) continue;
                double dx = x - pos.x, dy = y - pos.y, dz = z - pos.z;
                double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                int lo = static_cast<int>(distance * 5.0), hi = static_cast<int>(distance * 10.0);
                level.scheduleTick({x, y, z}, bs::blockOf(s), random.nextInt(hi - lo + 1) + lo); // nextIntBetweenInclusive
            }
    return true;
}
void eyeblossomSound(ServerLevel& level, BlockPos pos, bool nowOpen, bool longSound) {
    std::string name = std::string("minecraft:block.eyeblossom.") + (nowOpen ? "open" : "close") + (longSound ? "_long" : "");
    level.playBlockSound(-1, pos, soundId(name), 1.0f, 1.0f);
}

void flowerPotEyeblossomTick(ServerLevel& level, BlockPos pos, BlockStateId s, LegacyRandom& random) {
    bool open = bs::is(s, POTTED_OPEN_EYEBLOSSOM);
    auto v = dayAttribute(level, "minecraft:gameplay/eyeblossom_open");
    bool shouldBeOpen = v && v->isBool() ? v->boolean() : open;
    if (open == shouldBeOpen) return;
    setBlockAndUpdate(level, pos, bs::defaultState(open ? POTTED_CLOSED_EYEBLOSSOM : POTTED_OPEN_EYEBLOSSOM));
    random.nextDouble(), random.nextDouble(), random.nextDouble(), random.nextDouble(); // (the transform particle, see above)
    eyeblossomSound(level, pos, !open, true);
}

void turtleEggTick(ServerLevel& level, BlockPos pos, BlockStateId s, LegacyRandom& random) {
    auto chance = dayAttribute(level, "minecraft:gameplay/turtle_egg_hatch_chance"); // (modifier maximum over 0)
    float c = chance ? std::max(0.0f, static_cast<float>(chance->number())) : 0.0f;
    if (!(c > 0.0f && level.levelRandom().nextFloat() < c)) return;
    if (!bs::stateHasTag(level.getBlockState(pos.below()), "minecraft:sand")) return;
    int hatch = intValue(s, "hatch");
    if (hatch < 2) {
        static const int32_t kCrack = soundId("minecraft:entity.turtle.egg_crack");
        level.playBlockSound(-1, pos, kCrack, 0.7f, 0.9f + random.nextFloat() * 0.2f);
        level.setBlock(pos, withInt(s, "hatch", hatch + 1), kUpdateClients);
    }
    // TODO: hatching (turtles are mobs); until then a fully cracked egg waits.
}

// ---------------------------------------------------------------- lava (LavaFluid.randomTick)

bool ignitedByLava(ServerLevel& level, BlockPos pos) {
    if (pos.y >= level.minY() && pos.y <= level.maxY() && !level.getChunkIfLoaded({pos.x >> 4, pos.z >> 4})) return false;
    return bs::behaviour(bs::blockOf(level.getBlockState(pos))).ignitedByLava;
}

} // namespace

void randomTick(ServerLevel& level, BlockPos pos, BlockStateId s, LegacyRandom& random) {
    uint32_t b = bs::blockOf(s);
    const auto& info = bs::behaviour(b);
    if (info.weatherAge >= 0) { // WeatheringCopper*: doors weather from the lower half, chests while closed
        if (isA(b, "DoorBlock") && bs::getValue(s, "half") != "lower") return;
        if (isA(b, "ChestBlock")) {
            if (bs::getValue(s, "type") == "right") return;
            if (auto c = level.blockEntityContainer(pos); c && c->openers > 0) return;
        }
        return weatheringTick(level, pos, s, random);
    }
    if (isA(b, "CropBlock")) return cropTick(level, pos, s, random);
    if (isA(b, "PitcherCropBlock")) {
        if (growthRoll(b, level, pos, random)) pitcherGrow(level, s, pos, 1);
        return;
    }
    if (isA(b, "StemBlock")) return stemTick(level, pos, s, random);
    if (isA(b, "SugarCaneBlock")) return sugarCaneTick(level, pos, s);
    if (isA(b, "CactusBlock")) return cactusTick(level, pos, s, random);
    if (isA(b, "FarmlandBlock")) return farmlandTick(level, pos, s);
    if (isA(b, "LeavesBlock")) { // decaying: dropResources, then removeBlock
        if (!bs::getBool(s, "persistent") && intValue(s, "distance") == 7) {
            level.dropResources(s, pos, nullptr, nullptr, nullptr);
            removeBlock(level, pos);
        }
        return;
    }
    if (isA(b, "IceBlock")) { // melt (no water_evaporates in the overworld)
        if (level.brightness(light::LightLayer::Block, pos) > 11 - bs::lightDampening(s)) setBlockAndUpdate(level, pos, bs::defaultState(WATER));
        return;
    }
    if (isA(b, "SnowLayerBlock")) {
        if (level.brightness(light::LightLayer::Block, pos) > 11) {
            level.dropResources(s, pos, nullptr, nullptr, nullptr);
            removeBlock(level, pos);
        }
        return;
    }
    if (isA(b, "SpreadingSnowyBlock")) return spreadingSnowyTick(level, pos, s, random);
    if (isA(b, "NyliumBlock")) {
        BlockStateId above = level.getBlockState(pos.above());
        if (light::lightDampeningInto(s, above, static_cast<int>(Direction::Up), bs::lightDampening(above)) >= 15)
            setBlockAndUpdate(level, pos, bs::defaultState(NETHERRACK));
        return;
    }
    if (isA(b, "RedStoneOreBlock")) {
        if (bs::getBool(s, "lit")) setBlockAndUpdate(level, pos, bs::setBool(s, "lit", false));
        return;
    }
    if (isA(b, "MushroomBlock")) return mushroomTick(level, pos, s, random);
    if (isA(b, "NetherWartBlock")) {
        int age = intValue(s, "age");
        if (age < 3 && random.nextInt(10) == 0) level.setBlock(pos, withInt(s, "age", age + 1), kUpdateClients);
        return;
    }
    if (isA(b, "SweetBerryBushBlock")) {
        int age = intValue(s, "age");
        if (age < 3 && random.nextInt(5) == 0 && level.rawBrightness(pos.above(), 0) >= 9) level.setBlock(pos, withInt(s, "age", age + 1), kUpdateClients);
        return;
    }
    if (isA(b, "CocoaBlock")) {
        int age = intValue(s, "age");
        if (level.levelRandom().nextInt(5) == 0 && age < 2) level.setBlock(pos, withInt(s, "age", age + 1), kUpdateClients);
        return;
    }
    if (isA(b, "BuddingAmethystBlock")) return buddingAmethystTick(level, pos, random);
    if (isA(b, "GrowingPlantHeadBlock")) return growingPlantHeadTick(level, pos, s, random);
    if (isA(b, "VineBlock")) return vineTick(level, pos, s, random);
    if (isA(b, "BambooSaplingBlock")) {
        if (random.nextInt(3) == 0 && level.isEmptyBlock(pos.above()) && level.rawBrightness(pos.above(), 0) >= 9)
            setBlockAndUpdate(level, pos.above(), bs::setValue(bs::defaultState(BAMBOO), "leaves", "small"));
        return;
    }
    if (isA(b, "BambooStalkBlock")) return bambooTick(level, pos, s, random);
    if (isA(b, "ChorusFlowerBlock")) return chorusFlowerTick(level, pos, s, random);
    if (isA(b, "SpeleothemBlock")) return speleothemTick(level, pos, s, random);
    if (isA(b, "MangrovePropaguleBlock")) {
        if (!bs::getBool(s, "hanging")) {
            if (random.nextInt(7) == 0) tree_grower::advanceTree(level, pos, s, random);
        } else if (intValue(s, "age") < 4) {
            level.setBlock(pos, withInt(s, "age", intValue(s, "age") + 1), kUpdateClients);
        }
        return;
    }
    if (isA(b, "SaplingBlock")) {
        if (level.maxLocalRawBrightness(pos.above()) >= 9 && random.nextInt(7) == 0) tree_grower::advanceTree(level, pos, s, random);
        return;
    }
    if (isA(b, "EyeblossomBlock")) {
        if (eyeblossomTryChange(level, pos, s, random)) eyeblossomSound(level, pos, !bs::is(s, OPEN_EYEBLOSSOM), true);
        return;
    }
    if (isA(b, "FlowerPotBlock")) {
        if (bs::is(s, POTTED_OPEN_EYEBLOSSOM) || bs::is(s, POTTED_CLOSED_EYEBLOSSOM)) flowerPotEyeblossomTick(level, pos, s, random);
        return;
    }
    if (isA(b, "TurtleEggBlock")) return turtleEggTick(level, pos, s, random);
    if (isA(b, "DriedGhastBlock")) {
        if ((bs::getBool(s, "waterlogged") || intValue(s, "hydration") > 0) && !level.hasScheduledTick(pos, b)) level.scheduleTick(pos, b, 5000);
        return;
    }
    if (isA(b, "LiquidBlock")) return fluidRandomTick(level, pos, s, random); // LiquidBlock.randomTick: the fluid's
    // TODO: NetherPortalBlock (zombified piglins spawning: mobs).
}

void fluidRandomTick(ServerLevel& level, BlockPos pos, BlockStateId s, LegacyRandom& random) {
    if (!(bs::flags(s) & bs::kLava) || !level.canSpreadFireAround(pos)) return; // LavaFluid.randomTick
    int passes = random.nextInt(3);
    if (passes > 0) {
        BlockPos test = pos;
        for (int pass = 0; pass < passes; pass++) {
            int dx = random.nextInt(3) - 1;
            int dz = random.nextInt(3) - 1;
            test = test.offset(dx, 1, dz);
            if (!level.getChunkIfLoaded({test.x >> 4, test.z >> 4})) return;
            BlockStateId st = level.getBlockState(test);
            if (bs::isAir(st)) {
                for (Direction d : direction::kAll) // hasFlammableNeighbours
                    if (ignitedByLava(level, test.relative(d))) {
                        setBlockAndUpdate(level, test, fireStateFor(level, test));
                        return;
                    }
            } else if (bs::stateHasTag(st, "minecraft:blocks_lava_fire_spread")) {
                return;
            }
        }
    } else {
        for (int i = 0; i < 3; i++) {
            int dx = random.nextInt(3) - 1;
            int dz = random.nextInt(3) - 1;
            BlockPos test = pos.offset(dx, 0, dz);
            if (!level.getChunkIfLoaded({test.x >> 4, test.z >> 4})) return;
            if (level.isEmptyBlock(test.above()) && ignitedByLava(level, test)) setBlockAndUpdate(level, test.above(), fireStateFor(level, test));
        }
    }
}

bool growCrop(ServerLevel& level, BlockPos pos) {
    BlockStateId s = level.getBlockState(pos);
    uint32_t b = bs::blockOf(s);
    LegacyRandom& random = level.levelRandom();
    auto insideBuildHeight = [&](BlockPos p) { return isInsideBuildHeight(level, p); };
    int age = intValue(s, "age");
    // Each: isValidBonemealTarget, then (server side) isBonemealSuccess -> performBonemeal.
    if (isA(b, "CropBlock")) {
        int max = cropMaxAge(b);
        if (age >= max) return false;
        int increase = isA(b, "TorchflowerCropBlock") ? 1 : random.nextInt(4) + 2; // getBonemealAgeIncrease: Mth.nextInt(2, 5)
        if (isA(b, "BeetrootBlock")) increase /= 3;
        level.setBlock(pos, cropStateForAge(b, std::min(max, age + increase)), kUpdateClients); // growCrops
        return true;
    }
    if (isA(b, "StemBlock")) {
        if (age == 7) return false;
        int grown = std::min(7, age + random.nextInt(4) + 2);
        BlockStateId newState = withInt(s, "age", grown);
        level.setBlock(pos, newState, kUpdateClients);
        if (grown == 7) stemTick(level, pos, newState, random);
        return true;
    }
    if (isA(b, "MangrovePropaguleBlock") && bs::getBool(s, "hanging")) {
        if (age >= 4) return false;
        level.setBlock(pos, withInt(s, "age", age + 1), kUpdateClients);
        return true;
    }
    if (isA(b, "SaplingBlock")) {
        if (!tree_grower::canGrow(level, pos, s) || !insideBuildHeight(pos.above(tree_grower::minimumHeight(s).value_or(0)))) return false;
        if (level.levelRandom().nextFloat() < 0.45f) tree_grower::advanceTree(level, pos, s, random);
        return true;
    }
    if (isA(b, "AzaleaBlock")) {
        if (!insideBuildHeight(pos.above(tree_grower::minimumHeight(s).value_or(0) + 2)) || bs::hasFluid(level.getBlockState(pos.above()))) return false;
        if (level.levelRandom().nextFloat() < 0.45f) tree_grower::growTree(level, pos, s, random);
        return true;
    }
    if (isA(b, "MushroomBlock")) {
        std::string_view feature = bs::behaviour(b).growsInto;
        auto radius = feature::hugeMushroomFoliageRadius(feature);
        if (!radius || !insideBuildHeight(pos.above(4 + *radius))) return false;
        if (random.nextFloat() < 0.4f) { // growMushroom
            removeBlock(level, pos);
            if (!feature::place(feature, level, random, pos)) setBlockAndUpdate(level, pos, s);
        }
        return true;
    }
    if (isA(b, "SweetBerryBushBlock") || isA(b, "CocoaBlock")) {
        if (age >= (isA(b, "CocoaBlock") ? 2 : 3)) return false;
        level.setBlock(pos, withInt(s, "age", age + 1), kUpdateClients);
        return true;
    }
    if (isA(b, "BambooSaplingBlock")) {
        if (!bs::isAir(level.getBlockState(pos.above())) || !insideBuildHeight(pos.above())) return false;
        setBlockAndUpdate(level, pos.above(), bs::setValue(bs::defaultState(BAMBOO), "leaves", "small"));
        return true;
    }
    if (isA(b, "BambooStalkBlock")) {
        auto heightAbove = [&] {
            int h = 0;
            while (h < 16 && bs::is(level.getBlockState(pos.above(h + 1)), BAMBOO)) h++;
            return h;
        };
        int above = heightAbove(), below = 0;
        while (below < 16 && bs::is(level.getBlockState(pos.below(below + 1)), BAMBOO)) below++;
        BlockPos growthPos = pos.above(above + 1);
        if (!(above + below + 1 < 16 && intValue(level.getBlockState(pos.above(above)), "stage") != 1 && insideBuildHeight(growthPos) &&
              level.isEmptyBlock(growthPos)))
            return false;
        int total = above + below + 1;
        int newBamboo = 1 + random.nextInt(2);
        for (int i = 0; i < newBamboo; i++) {
            BlockPos topPos = pos.above(above);
            BlockStateId top = level.getBlockState(topPos);
            BlockPos grow = topPos.above();
            if (total >= 16 || intValue(top, "stage") == 1 || !level.isEmptyBlock(grow) || !insideBuildHeight(grow)) break;
            growBamboo(level, top, topPos, random, total);
            above++, total++;
        }
        return true;
    }
    if (isA(b, "CaveVinesBlock") || isA(b, "CaveVinesPlantBlock")) {
        if (bs::getBool(s, "berries")) return false;
        level.setBlock(pos, bs::setBool(s, "berries", true), kUpdateClients);
        return true;
    }
    if (isA(b, "GrowingPlantBlock")) { // a body grows its head (BlockUtil.getTopConnectedBlock)
        Direction growth = growthDirection(b);
        BlockPos headPos = pos;
        uint32_t head = b;
        if (!isA(b, "GrowingPlantHeadBlock")) {
            std::string_view name = gen::kBlocks[b].name;
            head = *bs::byName(name.substr(0, name.size() - 6)); // "..._plant"
            do headPos = headPos.relative(growth);
            while (bs::blockOf(level.getBlockState(headPos)) == b);
            if (bs::blockOf(level.getBlockState(headPos)) != head) return false;
        }
        BlockPos forward = headPos.relative(growth);
        if (!plantCanGrowInto(head, level.getBlockState(forward)) || !insideBuildHeight(forward)) return false;
        BlockStateId headState = level.getBlockState(headPos);
        int nextAge = std::min(intValue(headState, "age") + 1, 25);
        int blocks = 1; // getBlocksToGrowWhenBonemealed (kelp: 1; nether vines: NetherVines')
        if (isA(head, "WeepingVinesBlock") || isA(head, "TwistingVinesBlock")) {
            double probability = 1.0;
            for (blocks = 0; random.nextDouble() < probability; blocks++) probability *= 0.826;
        }
        for (int i = 0; i < blocks && plantCanGrowInto(head, level.getBlockState(forward)) && insideBuildHeight(forward); i++) {
            setBlockAndUpdate(level, forward, withInt(headState, "age", nextAge));
            forward = forward.relative(growth);
            nextAge = std::min(nextAge + 1, 25);
        }
        return true;
    }
    if (isA(b, "PitcherCropBlock")) {
        BlockPos lowerPos = bs::getValue(s, "half") == "upper" ? pos.below() : pos; // getLowerHalf
        BlockStateId lower = level.getBlockState(lowerPos);
        if (!bs::is(lower, PITCHER_CROP) || bs::getValue(lower, "half") != "lower") return false;
        int lowerAge = intValue(lower, "age"), next = lowerAge + 1;
        auto canGrowInto = [&] {
            BlockStateId st = level.getBlockState(lowerPos.above());
            return bs::isAir(st) || bs::is(st, PITCHER_CROP);
        };
        if (!(lowerAge < 4 && level.rawBrightness(lowerPos, 0) >= 8 && insideBuildHeight(lowerPos.above()) && (next < 3 || canGrowInto()))) return false;
        pitcherGrow(level, lower, lowerPos, 1);
        return true;
    }
    if (isA(b, "MangroveLeavesBlock")) {
        if (!bs::isAir(level.getBlockState(pos.below()))) return false;
        level.setBlock(pos.below(), bs::setBool(withInt(bs::defaultState(MANGROVE_PROPAGULE), "age", 0), "hanging", true), kUpdateClients);
        return true;
    }
    // TODO: bone meal on grass, moss, nylium, fungi, flowers, dripleaves, sea pickles, lichen and
    // underwater plants (they place world-gen features or spread in their own ways).
    return false;
}

namespace detail {

bool hasPlantTick(uint32_t b) {
    return isA(b, "CactusBlock") || isA(b, "SugarCaneBlock") || isA(b, "FarmlandBlock") || isA(b, "LeavesBlock") || isA(b, "BambooStalkBlock") ||
           isA(b, "GrowingPlantBlock") || isA(b, "ChorusFlowerBlock") || isA(b, "EyeblossomBlock") || isA(b, "DriedGhastBlock") ||
           isA(b, "AbstractCauldronBlock") || isA(b, "FrostedIceBlock");
}

bool plantTick(ServerLevel& level, BlockPos pos, BlockStateId s) {
    uint32_t b = bs::blockOf(s);
    LegacyRandom& random = level.levelRandom();
    if (isA(b, "CactusBlock") || isA(b, "SugarCaneBlock") || isA(b, "BambooStalkBlock") || isA(b, "GrowingPlantBlock") || isA(b, "ChorusFlowerBlock")) {
        if (!canSurvive(s, level, pos)) level.destroyBlock(pos, true);
        return true;
    }
    if (isA(b, "FarmlandBlock")) {
        if (!canSurvive(s, level, pos)) setBlockAndUpdate(level, pos, bs::defaultState(DIRT)); // turnToBaseBlock
        return true;
    }
    if (isA(b, "LeavesBlock")) {
        setBlockAndUpdate(level, pos, detail::leavesDistance(s, level, pos));
        return true;
    }
    if (isA(b, "EyeblossomBlock")) {
        if (eyeblossomTryChange(level, pos, s, random)) eyeblossomSound(level, pos, !bs::is(s, OPEN_EYEBLOSSOM), false);
        return true;
    }
    if (isA(b, "DriedGhastBlock")) {
        int hydration = intValue(s, "hydration");
        if (bs::getBool(s, "waterlogged")) {
            if (hydration < 3) {
                static const int32_t kTransition = soundId("minecraft:block.dried_ghast.transition");
                level.playBlockSound(-1, pos, kTransition, 1.0f, 1.0f);
                level.setBlock(pos, withInt(s, "hydration", hydration + 1), kUpdateClients);
            }
            // TODO: a fully hydrated dried ghast becomes a ghastling (mobs).
        } else if (hydration > 0) {
            level.setBlock(pos, withInt(s, "hydration", hydration - 1), kUpdateClients);
        }
        return true;
    }
    if (isA(b, "AbstractCauldronBlock")) {
        cauldronTick(level, pos, s);
        return true;
    }
    if (isA(b, "FrostedIceBlock")) {
        auto fewerNeighboursThan = [&](int limit) {
            int n = 0;
            for (Direction d : direction::kAll)
                if (bs::blockOf(level.getBlockState(pos.relative(d))) == b && ++n >= limit) return false;
            return true;
        };
        auto slightlyMelt = [&](BlockStateId st, BlockPos at) {
            int age = intValue(st, "age");
            if (age < 3) {
                level.setBlock(at, withInt(st, "age", age + 1), kUpdateClients);
                return false;
            }
            setBlockAndUpdate(level, at, bs::defaultState(WATER)); // IceBlock.melt
            return true;
        };
        if (random.nextInt(3) == 0 || fewerNeighboursThan(4)) {
            int brightness = level.maxLocalRawBrightness(pos);
            if (brightness > 11 - intValue(s, "age") - bs::lightDampening(s) && slightlyMelt(s, pos)) {
                for (Direction d : direction::kAll) {
                    BlockPos n = pos.relative(d);
                    BlockStateId ns = level.getBlockState(n);
                    if (bs::blockOf(ns) == b && !slightlyMelt(ns, n)) level.scheduleTick(n, b, random.nextInt(21) + 20);
                }
                return true;
            }
        }
        level.scheduleTick(pos, b, random.nextInt(21) + 20);
        return true;
    }
    return false;
}

} // namespace detail
} // namespace mc::block_behaviour
