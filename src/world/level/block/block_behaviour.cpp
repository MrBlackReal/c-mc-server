#include "world/level/block/block_behaviour.hpp"
#include "world/level/block/block_behaviour_internal.hpp"

#include <algorithm>
#include <deque>
#include <unordered_set>
#include <cmath>
#include <random>
#include <string>

#include "core/registries.hpp"
#include "generated/behaviour.hpp"
#include "server/level/server_level.hpp"
#include "world/level/material/fluids.hpp"
#include "util/json.hpp"
#include "world/entity/falling_block_entity.hpp"
#include "world/item/item_logic.hpp"

namespace mc {

using namespace gen::block;
namespace bs = blocks;

std::array<Direction, 6> PlaceContext::nearestLookingDirections() const {
    auto dirs = direction::orderedByNearest(yRot, xRot);
    if (replaceClicked) return dirs;
    // BlockPlaceContext.getNearestLookingDirections: the face opposite the clicked one goes first.
    Direction first = direction::opposite(face);
    size_t i = 0;
    while (i < dirs.size() && dirs[i] != first) i++;
    for (; i > 0; i--) dirs[i] = dirs[i - 1];
    dirs[0] = first;
    return dirs;
}

namespace block_behaviour {
namespace {

bool isA(uint32_t block, std::string_view cls) { return bs::isInstance(block, cls); }
bool isA(BlockStateId state, std::string_view cls, int) { return bs::isInstance(bs::blockOf(state), cls); }

std::string_view dirName(Direction d) { return direction::name(d); }
Direction dirValue(BlockStateId s, std::string_view prop) { return direction::byName(bs::getValue(s, prop).value_or("north")); }

BlockStateId withWaterlogged(BlockStateId s, const PlaceContext& ctx) {
    if (!bs::hasProperty(s, "waterlogged")) return s;
    return bs::setBool(s, "waterlogged", bs::isWaterSource(ctx.level.getBlockState(ctx.pos)));
}

// Common "click on the upper half or underside -> top" rule used by slabs, stairs and trapdoors.
bool placeOnTop(const PlaceContext& ctx) {
    return ctx.face == Direction::Down || (ctx.face != Direction::Up && ctx.clickYWithinBlock() > 0.5);
}

// Block.canSupportCenter: the face is sturdy for SupportType.CENTER (extracted for up and down).
bool canSupportCenter(ServerLevel& level, BlockPos pos, Direction face) {
    BlockStateId s = level.getBlockState(pos);
    if (face == Direction::Up) return bs::flags(s) & bs::kCenterSupportUp;
    if (face == Direction::Down) return bs::flags(s) & bs::kCenterSupportDown;
    return bs::isFaceSturdy(s, face);
}

bool canAttachTo(ServerLevel& level, BlockPos pos, Direction towardsSupport) {
    BlockPos support = pos.relative(towardsSupport);
    return bs::isFaceSturdy(level.getBlockState(support), direction::opposite(towardsSupport));
}

// Block.isExceptionForConnection
bool isExceptionForConnection(BlockStateId s) {
    uint32_t b = bs::blockOf(s);
    return isA(b, "LeavesBlock") || b == BARRIER || b == CARVED_PUMPKIN || b == JACK_O_LANTERN || b == MELON || b == PUMPKIN ||
           bs::hasTag(b, "minecraft:shulker_boxes");
}

bool fenceConnectsTo(uint32_t self, BlockStateId neighbour, bool faceSturdy, Direction dirFromNeighbour) {
    uint32_t nb = bs::blockOf(neighbour);
    bool sameFence = bs::hasTag(nb, "minecraft:fences") &&
                     bs::hasTag(nb, "minecraft:wooden_fences") == bs::hasTag(self, "minecraft:wooden_fences");
    bool gate = isA(nb, "FenceGateBlock") &&
                direction::axis(dirValue(neighbour, "facing")) == direction::axis(direction::clockWise(dirFromNeighbour));
    return (!isExceptionForConnection(neighbour) && faceSturdy) || sameFence || gate;
}

bool barsAttachTo(BlockStateId neighbour, bool faceSturdy) {
    uint32_t nb = bs::blockOf(neighbour);
    return (!isExceptionForConnection(neighbour) && faceSturdy) || isA(nb, "IronBarsBlock") || bs::hasTag(nb, "minecraft:walls");
}

bool wallConnectsTo(BlockStateId neighbour, bool faceSturdy, Direction dirFromNeighbour) {
    uint32_t nb = bs::blockOf(neighbour);
    bool gate = isA(nb, "FenceGateBlock") &&
                direction::axis(dirValue(neighbour, "facing")) == direction::axis(direction::clockWise(dirFromNeighbour));
    return bs::hasTag(nb, "minecraft:walls") || (!isExceptionForConnection(neighbour) && faceSturdy) || isA(nb, "IronBarsBlock") || gate;
}

// Connection state for fences, panes and walls towards `dir`.
BlockStateId connectSide(BlockStateId s, ServerLevel& level, BlockPos pos, Direction dir) {
    uint32_t b = bs::blockOf(s);
    BlockPos np = pos.relative(dir);
    BlockStateId n = level.getBlockState(np);
    bool sturdy = bs::isFaceSturdy(n, direction::opposite(dir));
    if (isA(b, "FenceBlock")) return bs::setBool(s, dirName(dir), fenceConnectsTo(b, n, sturdy, direction::opposite(dir)));
    if (isA(b, "IronBarsBlock")) return bs::setBool(s, dirName(dir), barsAttachTo(n, sturdy));
    return s;
}

// An axis-aligned rectangle in the XZ plane, in block units.
struct RectXZ {
    double minX, minZ, maxX, maxZ;
};

// VoxelShape.getFaceShape(DOWN) of a state's collision shape, as XZ rectangles: the parts of
// its boxes that touch the bottom of the block.
std::vector<RectXZ> bottomFace(BlockStateId state) {
    std::vector<RectXZ> out;
    for (const auto& b : gen::kShapes[gen::kStateShape[state]])
        if (b.minY <= 1.0e-7 && b.maxY > 1.0e-7) out.push_back({b.minX, b.minZ, b.maxX, b.maxZ});
    return out;
}

// WallBlock.isCovered: `test` lies entirely within the union of `face`.
bool isCovered(const std::vector<RectXZ>& face, RectXZ test) {
    // Split the test rectangle along every edge inside it; each cell must be inside some rectangle.
    std::vector<double> xs{test.minX, test.maxX}, zs{test.minZ, test.maxZ};
    for (const auto& r : face) {
        for (double x : {r.minX, r.maxX})
            if (x > test.minX && x < test.maxX) xs.push_back(x);
        for (double z : {r.minZ, r.maxZ})
            if (z > test.minZ && z < test.maxZ) zs.push_back(z);
    }
    std::sort(xs.begin(), xs.end());
    std::sort(zs.begin(), zs.end());
    for (size_t i = 0; i + 1 < xs.size(); i++) {
        for (size_t j = 0; j + 1 < zs.size(); j++) {
            double cx = (xs[i] + xs[i + 1]) / 2, cz = (zs[j] + zs[j + 1]) / 2;
            if (xs[i + 1] - xs[i] < 1.0e-9 || zs[j + 1] - zs[j] < 1.0e-9) continue;
            bool inside = std::any_of(face.begin(), face.end(),
                                      [&](const RectXZ& r) { return cx > r.minX && cx < r.maxX && cz > r.minZ && cz < r.maxZ; });
            if (!inside) return false;
        }
    }
    return true;
}

// WallBlock.updateShape(level, state, topPos, topNeighbour, north, east, south, west).
BlockStateId wallState(BlockStateId s, ServerLevel& level, BlockPos pos, bool north, bool east, bool south, bool west) {
    BlockStateId top = level.getBlockState(pos.above());
    std::vector<RectXZ> above = bottomFace(top);
    // TEST_SHAPES_WALL: a 2-wide strip from the center to each side; TEST_SHAPE_POST: the center.
    constexpr double lo = 7.0 / 16, hi = 9.0 / 16;
    auto side = [&](bool connected, RectXZ test) -> std::string_view {
        return !connected ? "none" : isCovered(above, test) ? "tall" : "low";
    };
    std::string_view n = side(north, {lo, 0.0, hi, hi}), so = side(south, {lo, lo, hi, 1.0});
    std::string_view e = side(east, {lo, lo, 1.0, hi}), w = side(west, {0.0, lo, hi, hi});
    s = bs::setValue(bs::setValue(bs::setValue(bs::setValue(s, "north", n), "south", so), "east", e), "west", w);
    // shouldRaisePost
    bool up;
    if (isA(bs::blockOf(top), "WallBlock") && bs::getBool(top, "up")) {
        up = true;
    } else {
        bool nn = n == "none", sn = so == "none", en = e == "none", wn = w == "none";
        bool corner = (nn && sn && wn && en) || nn != sn || wn != en;
        if (corner) up = true;
        else if ((n == "tall" && so == "tall") || (e == "tall" && w == "tall")) up = false;
        else up = bs::hasTag(bs::blockOf(top), "minecraft:wall_post_override") || isCovered(above, {lo, lo, hi, hi});
    }
    return bs::setBool(s, "up", up);
}

bool wallConnects(ServerLevel& level, BlockPos pos, Direction dir) {
    BlockPos np = pos.relative(dir);
    BlockStateId n = level.getBlockState(np);
    return wallConnectsTo(n, bs::isFaceSturdy(n, direction::opposite(dir)), direction::opposite(dir));
}

// StairBlock.getStairsShape
std::string_view stairsShape(BlockStateId s, ServerLevel& level, BlockPos pos) {
    auto isStairs = [](BlockStateId st) { return isA(st, "StairBlock", 0); };
    Direction facing = dirValue(s, "facing");
    auto half = bs::getValue(s, "half");
    auto canTakeShape = [&](Direction neighbour) {
        BlockStateId n = level.getBlockState(pos.relative(neighbour));
        return !isStairs(n) || dirValue(n, "facing") != facing || bs::getValue(n, "half") != half;
    };
    BlockStateId behind = level.getBlockState(pos.relative(facing));
    if (isStairs(behind) && bs::getValue(behind, "half") == half) {
        Direction bf = dirValue(behind, "facing");
        if (direction::axis(bf) != direction::axis(facing) && canTakeShape(direction::opposite(bf)))
            return bf == direction::counterClockWise(facing) ? "outer_left" : "outer_right";
    }
    BlockStateId front = level.getBlockState(pos.relative(direction::opposite(facing)));
    if (isStairs(front) && bs::getValue(front, "half") == half) {
        Direction ff = dirValue(front, "facing");
        if (direction::axis(ff) != direction::axis(facing) && canTakeShape(ff))
            return ff == direction::counterClockWise(facing) ? "inner_left" : "inner_right";
    }
    return "straight";
}

// DoorBlock.getHinge
std::string_view doorHinge(const PlaceContext& ctx) {
    ServerLevel& level = ctx.level;
    BlockPos pos = ctx.pos, above = pos.above();
    Direction place = ctx.horizontalDirection();
    Direction left = direction::counterClockWise(place), right = direction::clockWise(place);
    BlockStateId ls = level.getBlockState(pos.relative(left)), las = level.getBlockState(above.relative(left));
    BlockStateId rs = level.getBlockState(pos.relative(right)), ras = level.getBlockState(above.relative(right));
    int balance = (bs::isCollisionFullBlock(ls) ? -1 : 0) + (bs::isCollisionFullBlock(las) ? -1 : 0) +
                  (bs::isCollisionFullBlock(rs) ? 1 : 0) + (bs::isCollisionFullBlock(ras) ? 1 : 0);
    bool doorLeft = isA(ls, "DoorBlock", 0) && bs::getValue(ls, "half") == "lower";
    bool doorRight = isA(rs, "DoorBlock", 0) && bs::getValue(rs, "half") == "lower";
    if ((doorLeft && !doorRight) || balance > 0) return "right";
    if ((doorRight && !doorLeft) || balance < 0) return "left";
    int sx = direction::stepX(place), sz = direction::stepZ(place);
    double cx = ctx.hitX - pos.x, cz = ctx.hitZ - pos.z;
    bool left_ = (sx >= 0 || !(cz < 0.5)) && (sx <= 0 || !(cz > 0.5)) && (sz >= 0 || !(cx > 0.5)) && (sz <= 0 || !(cx < 0.5));
    return left_ ? "left" : "right";
}

// Classes whose survival depends on a support block (updateShape breaks them when it goes away;
// cactus, sugar cane, bamboo, kelp and vines, chorus flowers break a tick later instead).
bool needsSupport(uint32_t b) {
    return isA(b, "VegetationBlock") || isA(b, "BaseTorchBlock") || isA(b, "FaceAttachedHorizontalDirectionalBlock") ||
           isA(b, "LadderBlock") || isA(b, "CarpetBlock") || isA(b, "DoorBlock") || isA(b, "BasePressurePlateBlock") ||
           isA(b, "BaseRailBlock") || isA(b, "SignBlock") || isA(b, "BannerBlock") || isA(b, "WallBannerBlock") ||
           isA(b, "SnowLayerBlock") || isA(b, "DiodeBlock") || isA(b, "RedstoneWireBlock") || isA(b, "CandleBlock") ||
           isA(b, "AmethystClusterBlock") || isA(b, "BaseCoralWallFanBlock") || isA(b, "BaseCoralPlantTypeBlock") ||
           isA(b, "BellBlock") || isA(b, "CocoaBlock") || isA(b, "TripWireHookBlock") || isA(b, "ShelfMushroomBlock") || isA(b, "CakeBlock") ||
           isA(b, "CandleCakeBlock") || isA(b, "CeilingHangingSignBlock") || isA(b, "HangingRootsBlock") || isA(b, "SporeBlossomBlock") ||
           isA(b, "MultifaceBlock") || isA(b, "VineBlock") || isA(b, "SpeleothemBlock") ||
           isA(b, "LeafLitterBlock") || isA(b, "MossyCarpetBlock") || isA(b, "BambooSaplingBlock") ||
           isA(b, "BigDripleafBlock") || isA(b, "BigDripleafStemBlock") || isA(b, "ChorusPlantBlock") ||
           isA(b, "FrogspawnBlock") || isA(b, "LanternBlock");
}

// Rotation for standing signs/banners/skulls (RotationSegment.convertToSegment(yRot + 180)).
std::string rotationSegment(float yRot) {
    int seg = static_cast<int>(std::floor((yRot + 180.0f) * 16.0f / 360.0f + 0.5f)) & 15;
    return std::to_string(seg);
}

} // namespace

bool canBeReplaced(BlockStateId state, const PlaceContext& ctx) {
    uint32_t b = bs::blockOf(state);
    // SlabBlock.canBeReplaced: a slab can take a second slab of the same kind.
    if (isA(b, "SlabBlock") && !ctx.item.isEmpty() && ctx.item.info().block == static_cast<int32_t>(b)) {
        auto type = bs::getValue(state, "type");
        if (type == "double") return false;
        if (ctx.replaceClicked) {
            bool upper = ctx.hitY - ctx.hitPos.y > 0.5;
            if (type == "bottom") return ctx.face == Direction::Up || (upper && direction::isHorizontal(ctx.face));
            return ctx.face == Direction::Down || (!upper && direction::isHorizontal(ctx.face));
        }
        return true;
    }
    bool holdingThis = !ctx.item.isEmpty() && ctx.item.info().block == static_cast<int32_t>(b);
    // Candles, sea pickles, turtle eggs, flower beds and leaf litter take up to four.
    auto count = [&](std::string_view prop) { return std::stoi(std::string(bs::getValue(state, prop).value_or("4"))); };
    if (isA(b, "ScaffoldingBlock")) return holdingThis;
    if (isA(b, "CandleBlock") && !ctx.secondaryUse && holdingThis && count("candles") < 4) return true;
    if (isA(b, "SeaPickleBlock") && !ctx.secondaryUse && holdingThis && count("pickles") < 4) return true;
    if (isA(b, "TurtleEggBlock") && !ctx.secondaryUse && holdingThis && count("eggs") < 4) return true;
    if (isA(b, "FlowerBedBlock") && !ctx.secondaryUse && holdingThis && count("flower_amount") < 4) return true;
    if (isA(b, "LeafLitterBlock") && !ctx.secondaryUse && holdingThis && count("segment_amount") < 4) return true;
    if (isA(b, "SnowLayerBlock")) {
        int layers = count("layers");
        if (!holdingThis || layers >= 8) return layers == 1;
        return ctx.replaceClicked ? ctx.face == Direction::Up : true;
    }
    if (isA(b, "VineBlock")) {
        BlockStateId clicked = ctx.level.getBlockState(ctx.pos);
        if (bs::blockOf(clicked) == b) {
            int faces = 0;
            for (auto f : {"up", "north", "east", "south", "west"}) faces += bs::getBool(clicked, f);
            return faces < 5;
        }
    }
    if (isA(b, "MultifaceBlock")) {
        if (!holdingThis) return bs::canBeReplaced(state);
        for (Direction d : direction::kAll)
            if (bs::hasProperty(state, dirName(d)) && !bs::getBool(state, dirName(d))) return true;
        return false;
    }
    if (!bs::canBeReplaced(state)) return false;
    return ctx.item.isEmpty() || ctx.item.item != bs::behaviour(b).item;
}

// NoteBlock.setInstrument: a mob head above decides; otherwise the block below (heads below: harp).
BlockStateId noteBlockInstrument(ServerLevel& level, BlockPos pos, BlockStateId s) {
    const auto& above = gen::kInstruments[bs::behaviour(bs::blockOf(level.getBlockState(pos.above()))).instrument];
    if (above.worksAboveNoteBlock) return bs::setValue(s, "instrument", above.name);
    const auto& below = gen::kInstruments[bs::behaviour(bs::blockOf(level.getBlockState(pos.below()))).instrument];
    return bs::setValue(s, "instrument", below.worksAboveNoteBlock ? std::string_view("harp") : below.name);
}

const gen::InstrumentInfo& instrumentOf(BlockStateId s) {
    auto name = bs::getValue(s, "instrument").value_or("harp");
    for (const auto& i : gen::kInstruments)
        if (i.name == name) return i;
    return gen::kInstruments[0];
}

// NoteBlock.playNote: only with air above (or a head, which is the instrument).
void playNote(ServerLevel& level, BlockPos pos, BlockStateId s) {
    if (instrumentOf(s).worksAboveNoteBlock || bs::isAir(level.getBlockState(pos.above()))) level.blockEvent(pos, bs::blockOf(s), 0, 0);
}


// ---------------------------------------------------------------- per-block placement helpers

// Block.withPropertiesOf: `block`'s default state with every property it shares with `from`.
BlockStateId withPropertiesOf(uint32_t block, BlockStateId from) {
    BlockStateId out = bs::defaultState(block);
    for (const auto& p : gen::kBlocks[block].properties)
        if (auto v = bs::getValue(from, p.name)) out = bs::setValue(out, p.name, *v);
    return out;
}

int intValue(BlockStateId s, std::string_view prop, int def = 0) {
    auto v = bs::getValue(s, prop);
    return v ? std::stoi(std::string(*v)) : def;
}

// Block.isFaceFull(state.getCollisionShape(), face): the collision boxes cover the whole face.
bool collisionFaceFull(BlockStateId st, Direction face) {
    const auto& boxes = gen::kShapes[gen::kStateShape[st]];
    if (boxes.empty()) return false;
    constexpr float kEps = 1.0e-7f;
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++) {
            float u = (i + 0.5f) / 16, v = (j + 0.5f) / 16;
            bool covered = false;
            for (const auto& b : boxes) {
                bool touches, in;
                switch (face) {
                case Direction::Down: touches = b.minY <= kEps, in = u > b.minX && u < b.maxX && v > b.minZ && v < b.maxZ; break;
                case Direction::Up: touches = b.maxY >= 1 - kEps, in = u > b.minX && u < b.maxX && v > b.minZ && v < b.maxZ; break;
                case Direction::North: touches = b.minZ <= kEps, in = u > b.minX && u < b.maxX && v > b.minY && v < b.maxY; break;
                case Direction::South: touches = b.maxZ >= 1 - kEps, in = u > b.minX && u < b.maxX && v > b.minY && v < b.maxY; break;
                case Direction::West: touches = b.minX <= kEps, in = u > b.minZ && u < b.maxZ && v > b.minY && v < b.maxY; break;
                default: touches = b.maxX >= 1 - kEps, in = u > b.minZ && u < b.maxZ && v > b.minY && v < b.maxY; break;
                }
                if (touches && in) {
                    covered = true;
                    break;
                }
            }
            if (!covered) return false;
        }
    return true;
}

// MultifaceBlock.canAttachTo: the neighbour's support or collision face towards us is full (leaves
// have no support shape but a full collision box).
bool canAttachFace(ServerLevel& level, BlockPos pos, Direction towardsNeighbour) {
    BlockStateId n = level.getBlockState(pos.relative(towardsNeighbour));
    Direction face = direction::opposite(towardsNeighbour);
    return bs::isFaceSturdy(n, face) || collisionFaceFull(n, face);
}

// ChestBlock: which chests pair up (CopperChestBlock: any copper chest).
bool chestCanConnectTo(uint32_t self, BlockStateId other) {
    if (bs::hasTag(self, "minecraft:copper_chests")) return bs::stateHasTag(other, "minecraft:copper_chests") && bs::hasProperty(other, "type");
    return bs::blockOf(other) == self;
}
Direction chestConnectedDirection(BlockStateId s) { // ChestBlock.getConnectedDirection
    Direction facing = dirValue(s, "facing");
    return bs::getValue(s, "type") == "left" ? direction::clockWise(facing) : direction::counterClockWise(facing);
}
std::optional<Direction> chestPartnerFacing(uint32_t self, ServerLevel& level, BlockPos pos, Direction neighbour) {
    BlockStateId n = level.getBlockState(pos.relative(neighbour));
    if (chestCanConnectTo(self, n) && bs::getValue(n, "type") == "single") return dirValue(n, "facing");
    return std::nullopt;
}

// Copper chest weathering: 0 unaffected .. 3 oxidized, from the block name.
int copperChestAge(uint32_t b) {
    std::string_view n = gen::kBlocks[b].name;
    if (n.find("oxidized") != std::string_view::npos) return 3;
    if (n.find("weathered") != std::string_view::npos) return 2;
    if (n.find("exposed") != std::string_view::npos) return 1;
    return 0;
}
uint32_t unwaxed(uint32_t b) {
    std::string_view n = gen::kBlocks[b].name;
    if (n.starts_with("minecraft:waxed_")) {
        if (auto u = bs::byName("minecraft:" + std::string(n.substr(16)))) return *u;
    }
    return b;
}

bool isSpeleothemWithDirection(BlockStateId s, Direction tip) {
    return bs::stateHasTag(s, "minecraft:speleothems") && bs::getValue(s, "vertical_direction") == dirName(tip);
}
bool isValidSpeleothemPlacement(uint32_t self, ServerLevel& level, BlockPos pos, Direction tip) {
    BlockPos behind = pos.relative(direction::opposite(tip));
    BlockStateId b = level.getBlockState(behind);
    return bs::isFaceSturdy(b, tip) || (isSpeleothemWithDirection(b, tip) && bs::blockOf(b) == self);
}
std::string_view speleothemThickness(uint32_t self, ServerLevel& level, BlockPos pos, Direction tip, bool mergeOpposingTips) {
    Direction base = direction::opposite(tip);
    BlockStateId front = level.getBlockState(pos.relative(tip));
    if (isSpeleothemWithDirection(front, base) && bs::blockOf(front) == self)
        return !mergeOpposingTips && bs::getValue(front, "thickness") != "tip_merge" ? "tip" : "tip_merge";
    if (!isSpeleothemWithDirection(front, tip)) return "tip";
    auto frontThickness = bs::getValue(front, "thickness");
    if (frontThickness != "tip" && frontThickness != "tip_merge")
        return !isSpeleothemWithDirection(level.getBlockState(pos.relative(base)), tip) ? "base" : "middle";
    return "frustum";
}

// LeavesBlock.updateDistance
BlockStateId leavesDistance(BlockStateId s, ServerLevel& level, BlockPos pos) {
    int d = 7;
    for (Direction dir : direction::kAll) {
        BlockStateId n = level.getBlockState(pos.relative(dir));
        int nd = bs::stateHasTag(n, "minecraft:prevents_nearby_leaf_decay") ? 0 : bs::hasProperty(n, "distance") && isA(n, "LeavesBlock", 0) ? intValue(n, "distance", 7) : 7;
        d = std::min(d, nd + 1);
        if (d == 1) break;
    }
    return bs::setValue(s, "distance", std::to_string(d));
}

// ScaffoldingBlock.getDistance
int scaffoldingDistance(ServerLevel& level, BlockPos pos) {
    BlockPos below = pos.below();
    BlockStateId b = level.getBlockState(below);
    int d = 7;
    if (bs::is(b, SCAFFOLDING)) d = intValue(b, "distance", 7);
    else if (bs::isFaceSturdy(b, Direction::Up)) return 0;
    for (Direction dir : direction::kHorizontal) {
        BlockStateId n = level.getBlockState(pos.relative(dir));
        if (bs::is(n, SCAFFOLDING)) {
            d = std::min(d, intValue(n, "distance", 7) + 1);
            if (d == 1) break;
        }
    }
    return d;
}
BlockStateId scaffoldingState(BlockStateId s, ServerLevel& level, BlockPos pos) {
    int d = scaffoldingDistance(level, pos);
    s = bs::setValue(s, "distance", std::to_string(d));
    return bs::setBool(s, "bottom", d > 0 && !bs::is(level.getBlockState(pos.below()), SCAFFOLDING));
}

// ChorusPlantBlock.getStateWithConnections
BlockStateId chorusConnections(BlockStateId s, ServerLevel& level, BlockPos pos) {
    uint32_t self = bs::blockOf(s);
    for (Direction d : direction::kAll) {
        BlockStateId n = level.getBlockState(pos.relative(d));
        bool c = bs::blockOf(n) == self || bs::is(n, CHORUS_FLOWER) || (d == Direction::Down && bs::stateHasTag(n, "minecraft:supports_chorus_plant"));
        s = bs::setBool(s, dirName(d), c);
    }
    return s;
}

// TripWireBlock.shouldConnectTo
bool tripwireConnects(BlockStateId n, Direction d) {
    if (bs::is(n, TRIPWIRE_HOOK)) return dirValue(n, "facing") == direction::opposite(d);
    return bs::is(n, TRIPWIRE);
}

// ConcretePowderBlock.touchesLiquid
bool concreteTouchesLiquid(ServerLevel& level, BlockPos pos) {
    auto water = [](BlockStateId st) { return (bs::flags(st) & bs::kWater) != 0; };
    for (Direction d : direction::kAll) {
        if (d == Direction::Down && !water(level.getBlockState(pos))) continue;
        BlockStateId n = level.getBlockState(pos.relative(d));
        if (water(n) && !bs::isFaceSturdy(n, direction::opposite(d))) return true;
    }
    return false;
}
uint32_t concreteOf(uint32_t powder) { // white_concrete_powder -> white_concrete
    std::string n(gen::kBlocks[powder].name);
    auto at = n.rfind("_powder");
    if (at == std::string::npos) return powder;
    return bs::byName(n.substr(0, at)).value_or(powder);
}

// VineBlock.canSupportAtFace
bool vineSupportsFace(ServerLevel& level, BlockPos pos, Direction d) {
    if (d == Direction::Down) return false;
    if (canAttachFace(level, pos, d)) return true;
    if (d == Direction::Up) return false;
    BlockStateId above = level.getBlockState(pos.above());
    return bs::is(above, VINE) && bs::getBool(above, dirName(d));
}

// MossyCarpetBlock.getUpdatedState
BlockStateId mossyCarpetState(BlockStateId s, ServerLevel& level, BlockPos pos, bool createSides) {
    createSides |= bs::getBool(s, "bottom");
    std::optional<BlockStateId> above, below;
    for (Direction d : direction::kHorizontal) {
        std::string_view side = canAttachFace(level, pos, d) ? (createSides ? "low" : bs::getValue(s, dirName(d)).value_or("none")) : "none";
        if (side == "low") {
            if (!above) above = level.getBlockState(pos.above());
            if (bs::is(*above, PALE_MOSS_CARPET) && bs::getValue(*above, dirName(d)) != "none" && !bs::getBool(*above, "bottom")) side = "tall";
            if (!bs::getBool(s, "bottom")) {
                if (!below) below = level.getBlockState(pos.below());
                if (bs::is(*below, PALE_MOSS_CARPET) && bs::getValue(*below, dirName(d)) == "none") side = "none";
            }
        }
        s = bs::setValue(s, dirName(d), side);
    }
    return s;
}

// HugeMushroomBlock: a face shows unless the same block is next to it.
BlockStateId hugeMushroomFaces(BlockStateId s, ServerLevel& level, BlockPos pos) {
    for (Direction d : direction::kAll) s = bs::setBool(s, dirName(d), bs::blockOf(level.getBlockState(pos.relative(d))) != bs::blockOf(s));
    return s;
}

// CreakingHeartBlock.hasRequiredLogs
bool creakingHeartHasLogs(BlockStateId s, ServerLevel& level, BlockPos pos) {
    auto axis = bs::getValue(s, "axis").value_or("y");
    for (Direction d : direction::kAll) {
        if (direction::axisName(direction::axis(d)) != axis) continue;
        BlockStateId n = level.getBlockState(pos.relative(d));
        if (!bs::stateHasTag(n, "minecraft:pale_oak_logs") || bs::getValue(n, "axis") != axis) return false;
    }
    return true;
}

// PotentSulfurBlock.validBlockState
BlockStateId potentSulfurState(BlockStateId s, ServerLevel& level, BlockPos pos) {
    if (!bs::isWaterSource(level.getBlockState(pos.above()))) return bs::setValue(s, "state", "dry");
    BlockStateId below = level.getBlockState(pos.below());
    auto sourceIfFluid = [](BlockStateId st) { return !(bs::flags(st) & (bs::kWater | bs::kLava)) || (bs::flags(st) & bs::kFluidSource); };
    if (bs::stateHasTag(below, "minecraft:causes_continuous_geyser_eruptions") && sourceIfFluid(below)) return bs::setValue(s, "state", "continuous");
    if (bs::stateHasTag(below, "minecraft:causes_periodic_geyser_eruptions") && sourceIfFluid(below))
        return bs::getValue(s, "state") == "erupting" ? s : bs::setValue(s, "state", "dormant");
    return bs::setValue(s, "state", "wet");
}

// FrontAndTop for crafters and jigsaws: "<front>_<top>".
std::string frontAndTop(Direction front, Direction top) { return std::string(dirName(front)) + "_" + std::string(dirName(top)); }

// BellBlock.canSurvive
bool bellCanSurvive(BlockStateId s, ServerLevel& level, BlockPos pos) {
    auto att = bs::getValue(s, "attachment");
    Direction connected = att == "floor" ? Direction::Down : att == "ceiling" ? Direction::Up : dirValue(s, "facing");
    // getConnectedDirection(...).getOpposite(): floor bells hang from below, ceiling ones from above.
    if (connected == Direction::Up) return canSupportCenter(level, pos.above(), Direction::Down);
    return canAttachTo(level, pos, connected);
}


// ---------------------------------------------------------------- redstone wire connections (RedstoneWireBlock)

bool wireShouldConnectTo(BlockStateId st, std::optional<Direction> dir) { // shouldRedstoneWireConnectTo
    uint32_t b = bs::blockOf(st);
    if (isA(b, "RedstoneWireBlock")) return true;
    if (isA(b, "RepeaterBlock")) {
        Direction f = dirValue(st, "facing");
        return dir && (f == *dir || direction::opposite(f) == *dir);
    }
    if (isA(b, "ObserverBlock")) return dir && dirValue(st, "facing") == *dir;
    return (bs::flags(st) & bs::kSignalSource) && dir.has_value();
}
bool wireCanSurviveOn(BlockStateId st) { return bs::isFaceSturdy(st, Direction::Up) || bs::is(st, HOPPER); }
bool conductor(BlockStateId st) { return bs::flags(st) & bs::kRedstoneConductor; }

std::string_view wireConnectingSide(ServerLevel& level, BlockPos pos, Direction dir, bool canConnectUp) {
    BlockPos rp = pos.relative(dir);
    BlockStateId rs = level.getBlockState(rp);
    if (canConnectUp) {
        bool placeableAbove = isA(rs, "TrapDoorBlock", 0) || wireCanSurviveOn(rs);
        if (placeableAbove && wireShouldConnectTo(level.getBlockState(rp.above()), std::nullopt))
            return bs::isFaceSturdy(rs, direction::opposite(dir)) ? "up" : "side";
    }
    bool none = !wireShouldConnectTo(rs, dir) && (conductor(rs) || !wireShouldConnectTo(level.getBlockState(rp.below()), std::nullopt));
    return none ? "none" : "side";
}
std::string_view wireConnectingSide(ServerLevel& level, BlockPos pos, Direction dir) {
    return wireConnectingSide(level, pos, dir, !conductor(level.getBlockState(pos.above())));
}
bool wireConnected(BlockStateId s, Direction d) { return bs::getValue(s, dirName(d)) != "none"; }
bool wireIsDot(BlockStateId s) {
    for (Direction d : direction::kHorizontal)
        if (wireConnected(s, d)) return false;
    return true;
}
bool wireIsCross(BlockStateId s) {
    for (Direction d : direction::kHorizontal)
        if (!wireConnected(s, d)) return false;
    return true;
}
BlockStateId wireCross(BlockStateId s) { // crossState with the same power
    for (Direction d : direction::kHorizontal) s = bs::setValue(s, dirName(d), "side");
    return s;
}
BlockStateId wireConnectionState(ServerLevel& level, BlockStateId s, BlockPos pos) { // getConnectionState
    bool wasDot = wireIsDot(s);
    BlockStateId st = bs::setValue(bs::defaultState(bs::blockOf(s)), "power", std::string(bs::getValue(s, "power").value_or("0")));
    bool canConnectUp = !conductor(level.getBlockState(pos.above()));
    for (Direction d : direction::kHorizontal) // getMissingConnections
        if (!wireConnected(st, d)) st = bs::setValue(st, dirName(d), wireConnectingSide(level, pos, d, canConnectUp));
    if (wasDot && wireIsDot(st)) return st;
    bool n = wireConnected(st, Direction::North), so = wireConnected(st, Direction::South);
    bool e = wireConnected(st, Direction::East), w = wireConnected(st, Direction::West);
    bool nsEmpty = !n && !so, ewEmpty = !e && !w;
    if (!w && nsEmpty) st = bs::setValue(st, "west", "side");
    if (!e && nsEmpty) st = bs::setValue(st, "east", "side");
    if (!n && ewEmpty) st = bs::setValue(st, "north", "side");
    if (!so && ewEmpty) st = bs::setValue(st, "south", "side");
    return st;
}

// ---------------------------------------------------------------- rails (RailState)

bool isRailState(BlockStateId st) { return bs::stateHasTag(st, "minecraft:rails"); }

struct RailState {
    ServerLevel& level;
    BlockPos pos;
    BlockStateId state;
    bool straight; // powered, detector and activator rails can't curve
    std::vector<BlockPos> connections;

    RailState(ServerLevel& l, BlockPos p, BlockStateId s) : level(l), pos(p), state(s), straight(!isA(s, "RailBlock", 0)) {
        updateConnections(bs::getValue(s, "shape").value_or("north_south"));
    }
    void updateConnections(std::string_view shape) {
        connections.clear();
        BlockPos n = pos.relative(Direction::North), so = pos.relative(Direction::South);
        BlockPos w = pos.relative(Direction::West), e = pos.relative(Direction::East);
        if (shape == "north_south") connections = {n, so};
        else if (shape == "east_west") connections = {w, e};
        else if (shape == "ascending_east") connections = {w, e.above()};
        else if (shape == "ascending_west") connections = {w.above(), e};
        else if (shape == "ascending_north") connections = {n.above(), so};
        else if (shape == "ascending_south") connections = {n, so.above()};
        else if (shape == "south_east") connections = {e, so};
        else if (shape == "south_west") connections = {w, so};
        else if (shape == "north_west") connections = {w, n};
        else if (shape == "north_east") connections = {e, n};
    }
    std::optional<RailState> getRail(BlockPos p) {
        for (BlockPos q : {p, p.above(), p.below()}) {
            BlockStateId st = level.getBlockState(q);
            if (isRailState(st)) return RailState(level, q, st);
        }
        return std::nullopt;
    }
    bool hasRail(BlockPos p) { return isRailState(level.getBlockState(p)) || isRailState(level.getBlockState(p.above())) || isRailState(level.getBlockState(p.below())); }
    bool hasConnection(BlockPos p) const {
        for (const auto& c : connections)
            if (c.x == p.x && c.z == p.z) return true;
        return false;
    }
    void removeSoftConnections() {
        for (size_t i = 0; i < connections.size(); i++) {
            auto rail = getRail(connections[i]);
            if (rail && rail->hasConnection(pos)) connections[i] = rail->pos;
            else connections.erase(connections.begin() + static_cast<long>(i--));
        }
    }
    bool canConnectTo(const RailState& r) const { return hasConnection(r.pos) || connections.size() != 2; }
    bool hasNeighborRail(BlockPos p) {
        auto n = getRail(p);
        if (!n) return false;
        n->removeSoftConnections();
        return n->canConnectTo(*this);
    }
    std::string_view ascend(std::string_view shape) {
        BlockPos n = pos.relative(Direction::North), so = pos.relative(Direction::South);
        BlockPos w = pos.relative(Direction::West), e = pos.relative(Direction::East);
        if (shape == "north_south") {
            if (isRailState(level.getBlockState(n.above()))) shape = "ascending_north";
            if (isRailState(level.getBlockState(so.above()))) shape = "ascending_south";
        }
        if (shape == "east_west") {
            if (isRailState(level.getBlockState(e.above()))) shape = "ascending_east";
            if (isRailState(level.getBlockState(w.above()))) shape = "ascending_west";
        }
        return shape;
    }
    void connectTo(const RailState& r) {
        connections.push_back(r.pos);
        bool n = hasConnection(pos.relative(Direction::North)), so = hasConnection(pos.relative(Direction::South));
        bool w = hasConnection(pos.relative(Direction::West)), e = hasConnection(pos.relative(Direction::East));
        std::string_view shape;
        if (n || so) shape = "north_south";
        if (w || e) shape = "east_west";
        if (!straight) {
            if (so && e && !n && !w) shape = "south_east";
            if (so && w && !n && !e) shape = "south_west";
            if (n && w && !so && !e) shape = "north_west";
            if (n && e && !so && !w) shape = "north_east";
        }
        shape = ascend(shape);
        if (shape.empty()) shape = "north_south";
        state = bs::setValue(state, "shape", shape);
        level.setBlock(pos, state);
    }
    void place(bool hasSignal, bool first, std::string_view defaultShape) {
        bool n = hasNeighborRail(pos.relative(Direction::North)), so = hasNeighborRail(pos.relative(Direction::South));
        bool w = hasNeighborRail(pos.relative(Direction::West)), e = hasNeighborRail(pos.relative(Direction::East));
        std::string_view shape;
        bool ns = n || so, we = w || e;
        if (ns && !we) shape = "north_south";
        if (we && !ns) shape = "east_west";
        bool se = so && e, sw = so && w, ne = n && e, nw = n && w;
        if (!straight) {
            if (se && !n && !w) shape = "south_east";
            if (sw && !n && !e) shape = "south_west";
            if (nw && !so && !e) shape = "north_west";
            if (ne && !so && !w) shape = "north_east";
        }
        if (shape.empty()) {
            if (ns && we) shape = defaultShape;
            else if (ns) shape = "north_south";
            else if (we) shape = "east_west";
            if (!straight) {
                if (hasSignal) {
                    if (se) shape = "south_east";
                    if (sw) shape = "south_west";
                    if (ne) shape = "north_east";
                    if (nw) shape = "north_west";
                } else {
                    if (nw) shape = "north_west";
                    if (ne) shape = "north_east";
                    if (sw) shape = "south_west";
                    if (se) shape = "south_east";
                }
            }
        }
        shape = ascend(shape);
        if (shape.empty()) shape = defaultShape;
        updateConnections(shape);
        state = bs::setValue(state, "shape", shape);
        if (first || level.getBlockState(pos) != state) {
            level.setBlock(pos, state);
            for (size_t i = 0; i < connections.size(); i++) {
                auto nb = getRail(connections[i]);
                if (!nb) continue;
                nb->removeSoftConnections();
                if (nb->canConnectTo(*this)) nb->connectTo(*this);
            }
        }
    }
};

bool updatePlacementContext(PlaceContext& ctx) {
    if (ctx.item.isEmpty() || ctx.item.info().block != static_cast<int32_t>(SCAFFOLDING)) return true;
    ServerLevel& level = ctx.level;
    if (!bs::is(level.getBlockState(ctx.pos), SCAFFOLDING)) return scaffoldingDistance(level, ctx.pos) != 7;
    // ScaffoldingBlockItem: sneaking places against the clicked face, otherwise the line grows
    // forwards (clicking its top) or upwards, up to 7 blocks sideways.
    Direction dir = ctx.secondaryUse ? ctx.face : ctx.face == Direction::Up ? ctx.horizontalDirection() : Direction::Up;
    BlockPos p = ctx.pos.relative(dir);
    for (int horizontal = 0; horizontal < 7;) {
        if (p.y < level.minY() || p.y > level.maxY()) return false;
        BlockStateId st = level.getBlockState(p);
        if (!bs::is(st, SCAFFOLDING)) {
            // BlockPlaceContext.at: as if the player had clicked `p`'s `dir` face.
            BlockPos oldHit = ctx.hitPos, oldPos = ctx.pos;
            Direction oldFace = ctx.face;
            bool oldReplace = ctx.replaceClicked;
            ctx.hitPos = ctx.pos = p;
            ctx.face = dir;
            ctx.replaceClicked = true;
            if (canBeReplaced(st, ctx)) return true;
            ctx.hitPos = oldHit, ctx.pos = oldPos, ctx.face = oldFace, ctx.replaceClicked = oldReplace;
            return false;
        }
        p = p.relative(dir);
        if (direction::isHorizontal(dir)) horizontal++;
    }
    return false;
}

std::optional<BlockStateId> getStateForPlacement(uint32_t block, const PlaceContext& ctx) {
    ServerLevel& level = ctx.level;
    BlockPos pos = ctx.pos;
    BlockStateId s = bs::defaultState(block);
    Direction horizontal = ctx.horizontalDirection();
    bool maxY = pos.y >= level.info().minY + level.info().height - 1;

    if (isA(block, "NoteBlock")) return noteBlockInstrument(level, pos, s);
    if (isA(block, "SlabBlock")) {
        BlockStateId existing = level.getBlockState(pos);
        if (bs::blockOf(existing) == block) return bs::setBool(bs::setValue(existing, "type", "double"), "waterlogged", false);
        s = withWaterlogged(bs::setValue(s, "type", "bottom"), ctx);
        return placeOnTop(ctx) ? bs::setValue(s, "type", "top") : s;
    }
    if (isA(block, "StairBlock")) {
        s = bs::setValue(s, "facing", dirName(horizontal));
        s = bs::setValue(s, "half", placeOnTop(ctx) ? "top" : "bottom");
        s = withWaterlogged(s, ctx);
        return bs::setValue(s, "shape", stairsShape(s, level, pos));
    }
    if (isA(block, "DoorBlock")) {
        if (maxY || !canBeReplaced(level.getBlockState(pos.above()), ctx)) return std::nullopt;
        s = bs::setValue(s, "facing", dirName(horizontal));
        s = bs::setValue(s, "hinge", doorHinge(ctx));
        s = bs::setValue(s, "half", "lower");
        return canSurvive(s, level, pos) ? std::optional(s) : std::nullopt;
    }
    if (isA(block, "BedBlock")) {
        BlockPos head = pos.relative(horizontal);
        if (!canBeReplaced(level.getBlockState(head), ctx)) return std::nullopt;
        return bs::setValue(s, "facing", dirName(horizontal));
    }
    if (isA(block, "DoublePlantBlock")) {
        if (maxY || !canBeReplaced(level.getBlockState(pos.above()), ctx)) return std::nullopt;
        return withWaterlogged(bs::setValue(s, "half", "lower"), ctx);
    }
    if (isA(block, "TrapDoorBlock")) {
        if (!ctx.replaceClicked && direction::isHorizontal(ctx.face)) {
            s = bs::setValue(s, "facing", dirName(ctx.face));
            s = bs::setValue(s, "half", ctx.clickYWithinBlock() > 0.5 ? "top" : "bottom");
        } else {
            s = bs::setValue(s, "facing", dirName(direction::opposite(horizontal)));
            s = bs::setValue(s, "half", ctx.face == Direction::Up ? "bottom" : "top");
        }
        return withWaterlogged(s, ctx);
    }
    if (isA(block, "FenceGateBlock")) {
        auto isWall = [&](BlockPos p) { return bs::stateHasTag(level.getBlockState(p), "minecraft:walls"); };
        Axis ax = direction::axis(horizontal);
        bool inWall = (ax == Axis::Z && (isWall(pos.relative(Direction::West)) || isWall(pos.relative(Direction::East)))) ||
                      (ax == Axis::X && (isWall(pos.relative(Direction::North)) || isWall(pos.relative(Direction::South))));
        return bs::setBool(bs::setValue(s, "facing", dirName(horizontal)), "in_wall", inWall);
    }
    if (isA(block, "WallBlock")) {
        s = wallState(s, level, pos, wallConnects(level, pos, Direction::North), wallConnects(level, pos, Direction::East),
                      wallConnects(level, pos, Direction::South), wallConnects(level, pos, Direction::West));
        return withWaterlogged(s, ctx);
    }
    if (isA(block, "FenceBlock") || isA(block, "IronBarsBlock")) {
        for (Direction d : direction::kHorizontal) s = connectSide(s, level, pos, d);
        return withWaterlogged(s, ctx);
    }
    if (isA(block, "FaceAttachedHorizontalDirectionalBlock")) {
        for (Direction d : ctx.nearestLookingDirections()) {
            BlockStateId c;
            if (direction::axis(d) == Axis::Y)
                c = bs::setValue(bs::setValue(s, "face", d == Direction::Up ? "ceiling" : "floor"), "facing", dirName(horizontal));
            else
                c = bs::setValue(bs::setValue(s, "face", "wall"), "facing", dirName(direction::opposite(d)));
            if (canSurvive(c, level, pos)) return c;
        }
        return std::nullopt;
    }
    if (isA(block, "WallTorchBlock") || isA(block, "RedstoneWallTorchBlock") || isA(block, "LadderBlock") ||
        isA(block, "WallSignBlock") || isA(block, "WallBannerBlock") ||
        isA(block, "WallSkullBlock") || isA(block, "BaseCoralWallFanBlock")) {
        if (isA(block, "LadderBlock") && !ctx.replaceClicked) {
            BlockStateId behind = level.getBlockState(pos.relative(direction::opposite(ctx.face)));
            if (bs::blockOf(behind) == block && dirValue(behind, "facing") == ctx.face) return std::nullopt;
        }
        for (Direction d : ctx.nearestLookingDirections()) {
            if (!direction::isHorizontal(d)) continue;
            BlockStateId c = withWaterlogged(bs::setValue(s, "facing", dirName(direction::opposite(d))), ctx);
            if (canSurvive(c, level, pos)) return c;
        }
        return std::nullopt;
    }
    if (isA(block, "SkullBlock") && bs::hasProperty(s, "rotation")) { // no +180, unlike signs and banners
        int seg = static_cast<int>(std::floor(ctx.yRot * 16.0f / 360.0f + 0.5f)) & 15;
        return bs::setValue(s, "rotation", std::to_string(seg));
    }
    if (bs::hasProperty(s, "rotation")) { // standing signs, banners
        return withWaterlogged(bs::setValue(s, "rotation", rotationSegment(ctx.yRot)), ctx);
    }
    if (isA(block, "ObserverBlock")) return bs::setValue(s, "facing", dirName(ctx.nearestLookingDirection()));
    if (isA(block, "AnvilBlock")) return bs::setValue(s, "facing", dirName(direction::clockWise(horizontal)));
    if (isA(block, "EndRodBlock") || isA(block, "LightningRodBlock") || isA(block, "AmethystClusterBlock") ||
        isA(block, "ShulkerBoxBlock")) {
        return withWaterlogged(bs::setValue(s, "facing", dirName(ctx.face)), ctx);
    }
    if (isA(block, "HopperBlock")) {
        Direction d = direction::opposite(ctx.face);
        return bs::setValue(s, "facing", dirName(direction::axis(d) == Axis::Y ? Direction::Down : d));
    }
    if (isA(block, "LanternBlock")) {
        for (Direction d : ctx.nearestLookingDirections()) {
            if (direction::axis(d) != Axis::Y) continue;
            BlockStateId c = withWaterlogged(bs::setBool(s, "hanging", d == Direction::Up), ctx);
            if (canSurvive(c, level, pos)) return c;
        }
        return std::nullopt;
    }
    if (isA(block, "ChainBlock") || isA(block, "RotatedPillarBlock") || isA(block, "HayBlock") || isA(block, "BaseCoralPlantTypeBlock")) {
        if (bs::hasProperty(s, "axis")) s = bs::setValue(s, "axis", direction::axisName(direction::axis(ctx.face)));
        return withWaterlogged(s, ctx);
    }

    if (isA(block, "RedstoneWireBlock")) return wireConnectionState(level, wireCross(s), pos);
    if (isA(block, "BaseRailBlock")) { // the shape is settled by onPlace (RailState.place)
        bool ew = horizontal == Direction::East || horizontal == Direction::West;
        return withWaterlogged(bs::setValue(s, "shape", ew ? "east_west" : "north_south"), ctx);
    }
    if (isA(block, "WallHangingSignBlock")) {
        // Faces across the clicked face and hangs from a sturdy side (or another wall hanging sign).
        auto canAttach = [&](BlockStateId st, BlockPos at, Direction face) {
            BlockStateId n = level.getBlockState(at);
            if (bs::stateHasTag(n, "minecraft:wall_hanging_signs")) return direction::axis(dirValue(n, "facing")) == direction::axis(dirValue(st, "facing"));
            return bs::isFaceSturdy(n, face);
        };
        for (Direction d : ctx.nearestLookingDirections()) {
            if (!direction::isHorizontal(d) || direction::axis(d) == direction::axis(ctx.face)) continue;
            BlockStateId c = bs::setValue(s, "facing", dirName(direction::opposite(d)));
            Direction cw = direction::clockWise(dirValue(c, "facing")), ccw = direction::counterClockWise(dirValue(c, "facing"));
            if (canAttach(c, pos.relative(cw), ccw) || canAttach(c, pos.relative(ccw), cw)) return withWaterlogged(c, ctx);
        }
        return std::nullopt;
    }
    // --- facing the player's direction (not towards the player)
    if (isA(block, "CampfireBlock")) {
        bool water = bs::isWaterSource(level.getBlockState(pos));
        s = bs::setBool(bs::setBool(s, "waterlogged", water), "lit", !water);
        s = bs::setBool(s, "signal_fire", bs::is(level.getBlockState(pos.below()), HAY_BLOCK));
        return bs::setValue(s, "facing", dirName(horizontal));
    }
    if (isA(block, "DecoratedPotBlock") || isA(block, "CalibratedSculkSensorBlock")) {
        s = bs::setValue(withWaterlogged(s, ctx), "facing", dirName(horizontal));
        return bs::hasProperty(s, "cracked") ? bs::setBool(s, "cracked", false) : s;
    }
    if (isA(block, "CeilingHangingSignBlock")) {
        BlockStateId above = level.getBlockState(pos.above());
        Direction dir = direction::fromYRot(ctx.yRot);
        bool attachedToMiddle = !bs::isFaceSturdy(above, Direction::Down) || ctx.secondaryUse;
        if (bs::stateHasTag(above, "minecraft:all_hanging_signs") && !ctx.secondaryUse) {
            if (bs::hasProperty(above, "facing")) {
                if (direction::axis(dirValue(above, "facing")) == direction::axis(dir)) attachedToMiddle = false;
            } else if (bs::hasProperty(above, "rotation")) {
                int r = intValue(above, "rotation");
                if (r % 4 == 0 && direction::axis(direction::fromYRot(r * 22.5f)) == direction::axis(dir)) attachedToMiddle = false;
            }
        }
        int seg = !attachedToMiddle ? static_cast<int>(std::floor(direction::toYRot(direction::opposite(dir)) * 16.0f / 360.0f + 0.5f)) & 15
                                    : static_cast<int>(std::floor((ctx.yRot + 180.0f) * 16.0f / 360.0f + 0.5f)) & 15;
        s = bs::setBool(bs::setValue(s, "rotation", std::to_string(seg)), "attached", attachedToMiddle);
        return withWaterlogged(s, ctx);
    }

    // --- stacking onto an existing block of the same kind
    if (isA(block, "CandleBlock") || isA(block, "SeaPickleBlock") || isA(block, "TurtleEggBlock") || isA(block, "SnowLayerBlock") ||
        isA(block, "FlowerBedBlock") || isA(block, "LeafLitterBlock")) {
        BlockStateId existing = level.getBlockState(pos);
        std::string_view prop = isA(block, "CandleBlock") ? "candles" : isA(block, "SeaPickleBlock") ? "pickles" : isA(block, "TurtleEggBlock") ? "eggs"
                              : isA(block, "SnowLayerBlock") ? "layers" : isA(block, "FlowerBedBlock") ? "flower_amount" : "segment_amount";
        int max = prop == "layers" ? 8 : 4;
        if (bs::blockOf(existing) == block) return bs::setValue(existing, prop, std::to_string(std::min(max, intValue(existing, prop) + 1)));
        if (isA(block, "FlowerBedBlock") || isA(block, "LeafLitterBlock")) return bs::setValue(s, "facing", dirName(direction::opposite(horizontal)));
        if (isA(block, "SnowLayerBlock")) return canSurvive(s, level, pos) ? std::optional(s) : std::nullopt;
        s = withWaterlogged(s, ctx);
        return needsSupport(block) && !canSurvive(s, level, pos) ? std::nullopt : std::optional(s);
    }

    // --- joining neighbours
    if (isA(block, "ChestBlock") && bs::hasProperty(s, "type")) {
        std::string_view type = "single";
        Direction facing = direction::opposite(horizontal);
        if (direction::isHorizontal(ctx.face) && ctx.secondaryUse) {
            auto nf = chestPartnerFacing(block, level, pos, direction::opposite(ctx.face));
            if (nf && direction::axis(*nf) != direction::axis(ctx.face)) {
                facing = *nf;
                type = direction::counterClockWise(*nf) == direction::opposite(ctx.face) ? "right" : "left";
            }
        }
        if (type == "single" && !ctx.secondaryUse) { // ChestBlock.getChestType
            if (chestPartnerFacing(block, level, pos, direction::clockWise(facing)) == facing) type = "left";
            else if (chestPartnerFacing(block, level, pos, direction::counterClockWise(facing)) == facing) type = "right";
        }
        s = withWaterlogged(bs::setValue(bs::setValue(s, "facing", dirName(facing)), "type", type), ctx);
        if (bs::hasTag(block, "minecraft:copper_chests") && type != "single") {
            // CopperChestBlock.getLeastOxidizedChestOfConnectedBlocks
            BlockStateId other = level.getBlockState(pos.relative(chestConnectedDirection(s)));
            uint32_t mine = block, theirs = bs::blockOf(other);
            if ((unwaxed(mine) != mine) != (unwaxed(theirs) != theirs)) mine = unwaxed(mine), theirs = unwaxed(theirs);
            s = withPropertiesOf(copperChestAge(block) <= copperChestAge(bs::blockOf(other)) ? mine : theirs, s);
        }
        return s;
    }
    if (isA(block, "ChorusPlantBlock")) return chorusConnections(s, level, pos);
    if (isA(block, "TripWireBlock")) {
        for (Direction d : direction::kHorizontal) s = bs::setBool(s, dirName(d), tripwireConnects(level.getBlockState(pos.relative(d)), d));
        return s;
    }
    if (isA(block, "HugeMushroomBlock")) return hugeMushroomFaces(s, level, pos);
    if (isA(block, "MossyCarpetBlock")) return mossyCarpetState(s, level, pos, true);

    // --- attached to a neighbour
    if (isA(block, "CocoaBlock") || isA(block, "ShelfMushroomBlock") || isA(block, "TripWireHookBlock")) {
        if (isA(block, "TripWireHookBlock")) s = bs::setBool(bs::setBool(s, "powered", false), "attached", false);
        for (Direction d : ctx.nearestLookingDirections()) {
            if (!direction::isHorizontal(d)) continue;
            BlockStateId c = bs::setValue(s, "facing", dirName(isA(block, "CocoaBlock") ? d : direction::opposite(d)));
            if (canSurvive(c, level, pos)) return c;
        }
        return std::nullopt;
    }
    if (isA(block, "BellBlock")) {
        Axis axis = direction::axis(ctx.face);
        if (axis == Axis::Y) {
            BlockStateId c = bs::setValue(bs::setValue(s, "attachment", ctx.face == Direction::Down ? "ceiling" : "floor"), "facing", dirName(horizontal));
            return bellCanSurvive(c, level, pos) ? std::optional(c) : std::nullopt;
        }
        bool twoWalls = axis == Axis::X ? bs::isFaceSturdy(level.getBlockState(pos.relative(Direction::West)), Direction::East) && bs::isFaceSturdy(level.getBlockState(pos.relative(Direction::East)), Direction::West)
                                         : bs::isFaceSturdy(level.getBlockState(pos.relative(Direction::North)), Direction::South) && bs::isFaceSturdy(level.getBlockState(pos.relative(Direction::South)), Direction::North);
        BlockStateId c = bs::setValue(bs::setValue(s, "facing", dirName(direction::opposite(ctx.face))), "attachment", twoWalls ? "double_wall" : "single_wall");
        if (bellCanSurvive(c, level, pos)) return c;
        c = bs::setValue(c, "attachment", bs::isFaceSturdy(level.getBlockState(pos.below()), Direction::Up) ? "floor" : "ceiling");
        return bellCanSurvive(c, level, pos) ? std::optional(c) : std::nullopt;
    }
    if (isA(block, "VineBlock")) {
        BlockStateId clicked = level.getBlockState(pos);
        bool clickedVine = bs::blockOf(clicked) == block;
        BlockStateId result = clickedVine ? clicked : s;
        for (Direction d : ctx.nearestLookingDirections()) {
            if (d == Direction::Down) continue;
            bool occupied = clickedVine && bs::getBool(clicked, dirName(d));
            if (!occupied && vineSupportsFace(level, pos, d)) return bs::setBool(result, dirName(d), true);
        }
        return clickedVine ? std::optional(result) : std::nullopt;
    }
    if (isA(block, "MultifaceBlock")) { // glow lichen, sculk veins, resin clumps
        BlockStateId old = level.getBlockState(pos);
        for (Direction d : ctx.nearestLookingDirections()) {
            if (!bs::hasProperty(s, dirName(d))) continue;
            if (bs::blockOf(old) == block && bs::getBool(old, dirName(d))) continue;
            if (!canAttachFace(level, pos, d)) continue;
            BlockStateId n = bs::blockOf(old) == block ? old : bs::isWaterSource(old) ? bs::setBool(s, "waterlogged", true) : s;
            return bs::setBool(n, dirName(d), true);
        }
        return std::nullopt;
    }
    if (isA(block, "SpeleothemBlock")) { // pointed dripstone
        Direction def = ctx.xRot < 0 ? Direction::Down : Direction::Up; // nearest looking vertical direction, reversed
        std::optional<Direction> tip;
        if (isValidSpeleothemPlacement(block, level, pos, def)) tip = def;
        else if (isValidSpeleothemPlacement(block, level, pos, direction::opposite(def))) tip = direction::opposite(def);
        if (!tip) return std::nullopt;
        s = bs::setValue(s, "vertical_direction", dirName(*tip));
        s = bs::setValue(s, "thickness", speleothemThickness(block, level, pos, *tip, !ctx.secondaryUse));
        return withWaterlogged(s, ctx);
    }

    // --- decided by the surroundings
    if (isA(block, "LeavesBlock")) return leavesDistance(withWaterlogged(bs::setBool(s, "persistent", true), ctx), level, pos);
    if (isA(block, "ScaffoldingBlock")) return scaffoldingState(withWaterlogged(s, ctx), level, pos);
    if (isA(block, "ConcretePowderBlock")) {
        bool water = (bs::flags(level.getBlockState(pos)) & bs::kWater) != 0;
        return water || concreteTouchesLiquid(level, pos) ? bs::defaultState(concreteOf(block)) : s;
    }
    if (isA(block, "FarmlandBlock") || isA(block, "DirtPathBlock") || isA(block, "PathBlock")) {
        // Turns back to dirt under a solid block (FarmlandBlock/PathBlock.canSurvive).
        BlockStateId above = level.getBlockState(pos.above());
        bool ok = !bs::isSolid(above) || isA(above, "FenceGateBlock", 0) || isA(above, "MovingPistonBlock", 0);
        return ok ? s : bs::defaultState(DIRT);
    }
    if (isA(block, "ConduitBlock")) return bs::setBool(s, "waterlogged", bs::isWaterSource(level.getBlockState(pos)));
    if (isA(block, "KelpBlock") || isA(block, "SeagrassBlock")) {
        if (!bs::isWaterSource(level.getBlockState(pos))) return std::nullopt;
        if (isA(block, "KelpBlock")) s = bs::setValue(s, "age", std::to_string(std::uniform_int_distribution<int>(0, 24)(level.random())));
        return canSurvive(s, level, pos) ? std::optional(s) : std::nullopt;
    }
    if (isA(block, "TallSeagrassBlock")) {
        if (!bs::isWaterSource(level.getBlockState(pos)) || !bs::isWaterSource(level.getBlockState(pos.above()))) return std::nullopt;
        return s;
    }
    if (isA(block, "GrowingPlantBlock")) { // weeping/twisting/cave vines, kelp bodies
        bool down = isA(block, "WeepingVinesBlock") || isA(block, "WeepingVinesPlantBlock") || isA(block, "CaveVinesBlock") || isA(block, "CaveVinesPlantBlock");
        BlockStateId next = level.getBlockState(down ? pos.below() : pos.above());
        std::string_view name = gen::kBlocks[block].name;
        std::string head(name), body(name);
        if (name.ends_with("_plant")) head = std::string(name.substr(0, name.size() - 6));
        else body = std::string(name) + "_plant";
        auto headId = bs::byName(head), bodyId = bs::byName(body);
        if (headId && bodyId && (bs::blockOf(next) == *headId || bs::blockOf(next) == *bodyId)) return bs::defaultState(*bodyId);
        if (bs::hasProperty(s, "age") && !isA(block, "KelpPlantBlock"))
            s = bs::setValue(s, "age", std::to_string(std::uniform_int_distribution<int>(0, 24)(level.random())));
        return canSurvive(s, level, pos) ? std::optional(s) : std::nullopt;
    }
    if (isA(block, "BambooStalkBlock")) {
        if (bs::flags(level.getBlockState(pos)) & (bs::kWater | bs::kLava)) return std::nullopt;
        BlockStateId below = level.getBlockState(pos.below());
        if (!bs::stateHasTag(below, "minecraft:supports_bamboo")) return std::nullopt;
        if (bs::is(below, BAMBOO_SAPLING)) return bs::setValue(s, "age", "0");
        if (bs::is(below, BAMBOO)) return bs::setValue(s, "age", intValue(below, "age") > 0 ? "1" : "0");
        BlockStateId above = level.getBlockState(pos.above());
        return bs::is(above, BAMBOO) ? bs::setValue(s, "age", std::string(bs::getValue(above, "age").value_or("0"))) : bs::defaultState(BAMBOO_SAPLING);
    }
    if (isA(block, "BigDripleafBlock")) {
        BlockStateId below = level.getBlockState(pos.below());
        bool part = bs::is(below, BIG_DRIPLEAF) || bs::is(below, BIG_DRIPLEAF_STEM);
        s = bs::setValue(withWaterlogged(s, ctx), "facing", part ? bs::getValue(below, "facing").value_or("north") : dirName(direction::opposite(horizontal)));
        return canSurvive(s, level, pos) ? std::optional(s) : std::nullopt;
    }
    if (isA(block, "MangrovePropaguleBlock")) {
        s = bs::setValue(withWaterlogged(s, ctx), "age", "4");
        return canSurvive(s, level, pos) ? std::optional(s) : std::nullopt;
    }
    if (isA(block, "CreakingHeartBlock")) {
        s = bs::setValue(s, "axis", direction::axisName(direction::axis(ctx.face)));
        // updateState: uprooted -> dormant/awake with the right logs (awake needs night; creaking_active).
        if (creakingHeartHasLogs(s, level, pos) && bs::getValue(s, "creaking_heart_state") == "uprooted")
            s = bs::setValue(s, "creaking_heart_state", "dormant");
        return s;
    }
    if (isA(block, "PotentSulfurBlock")) return potentSulfurState(s, level, pos);
    if (isA(block, "CrafterBlock")) {
        Direction front = direction::opposite(ctx.nearestLookingDirection());
        Direction top = front == Direction::Down ? direction::opposite(horizontal) : front == Direction::Up ? horizontal : Direction::Up;
        return bs::setValue(s, "orientation", frontAndTop(front, top));
    }
    if (isA(block, "JigsawBlock")) {
        Direction top = direction::axis(ctx.face) == Axis::Y ? direction::opposite(horizontal) : Direction::Up;
        return bs::setValue(s, "orientation", frontAndTop(ctx.face, top));
    }

    if (isA(block, "SnowyBlock")) { // grass, podzol, mycelium placed under snow
        s = bs::setBool(s, "snowy", bs::stateHasTag(level.getBlockState(pos.above()), "minecraft:snow"));
        return s;
    }
    if (isA(block, "LecternBlock")) {
        // has_book when an op places one whose block_entity_data has a Book (canUseGameMasterBlocks: creative).
        bool book = false;
        if (ctx.creative)
            if (auto data = items::componentValue(ctx.item, "minecraft:block_entity_data"))
                book = data->asCompound() && data->asCompound()->contains("Book");
        return bs::setBool(bs::setValue(s, "facing", dirName(direction::opposite(horizontal))), "has_book", book);
    }
    if (isA(block, "TestBlock")) { // mode from the item's block_state component
        if (auto props = items::componentValue(ctx.item, "minecraft:block_state"); props && props->asCompound())
            if (const nbt::Tag* mode = props->asCompound()->get("mode"); mode && mode->asString()) s = bs::setValue(s, "mode", *mode->asString());
        return s;
    }

    // Generic fallback, covering the many HorizontalDirectionalBlock/DirectionalBlock subclasses
    // that face the player (furnaces, pumpkins, dispensers, pistons, barrels, ...). Redstone-driven
    // placement states (powered, lit, locked, triggered) stay off until redstone is ported.
    if (auto facing = gen::kBlocks[block].properties; !facing.empty()) {
        for (const auto& p : facing) {
            if (p.name != "facing") continue;
            if (p.values.size() == 4) s = bs::setValue(s, "facing", dirName(direction::opposite(horizontal)));
            else if (p.values.size() >= 5) s = bs::setValue(s, "facing", dirName(direction::opposite(ctx.nearestLookingDirection())));
        }
    }
    if (bs::hasProperty(s, "axis")) s = bs::setValue(s, "axis", direction::axisName(direction::axis(ctx.face)));
    s = withWaterlogged(s, ctx);
    if (needsSupport(block) && !canSurvive(s, level, pos)) return std::nullopt;
    return s;
}

namespace {
// FallingBlock.getDelayAfterPlace
int fallDelay(uint32_t block) { return isA(block, "DragonEggBlock") ? 5 : 2; }
} // namespace

namespace { // fire helpers, defined with the rest of the fire code below
bool isValidFireLocation(ServerLevel& level, BlockPos pos);
BlockStateId fireWithAge(ServerLevel& level, BlockPos pos, int age);
} // namespace

bool canSurvive(BlockStateId s, ServerLevel& level, BlockPos pos) {
    uint32_t b = bs::blockOf(s);
    if (b == BUBBLE_COLUMN) return fluids::bubbleColumnCanSurvive(level, pos);
    BlockPos below = pos.below(), above = pos.above();
    BlockStateId belowState = level.getBlockState(below);
    BlockStateId aboveState = level.getBlockState(above);
    auto facing = [&] { return dirValue(s, "facing"); };
    // The neighbour behind a wall-mounted block, whose face towards us must be sturdy.
    auto sturdyBehind = [&] { return canAttachTo(level, pos, direction::opposite(facing())); };
    auto isFluid = [](BlockStateId st) { return (bs::flags(st) & (bs::kWater | bs::kLava)) != 0; };
    auto topFaceNotEmpty = [](BlockStateId st) {
        for (const auto& box : gen::kShapes[gen::kStateShape[st]])
            if (box.maxY >= 1.0 - 1.0e-7) return true;
        return false;
    };

    if (isA(b, "DoorBlock")) {
        return bs::getValue(s, "half") == "lower" ? bs::isFaceSturdy(belowState, Direction::Up) : bs::blockOf(belowState) == b;
    }
    if (isA(b, "SoulFireBlock")) return bs::stateHasTag(belowState, "minecraft:soul_fire_base_blocks");
    if (isA(b, "FireBlock")) return bs::isFaceSturdy(belowState, Direction::Up) || isValidFireLocation(level, pos);
    if (isA(b, "TallSeagrassBlock")) {
        if (bs::getValue(s, "half") == "upper") return bs::blockOf(belowState) == b && bs::getValue(belowState, "half") == "lower";
        return bs::isFaceSturdy(belowState, Direction::Up) && !bs::stateHasTag(belowState, "minecraft:cannot_support_seagrass") &&
               bs::isWaterSource(level.getBlockState(pos));
    }
    if (isA(b, "SmallDripleafBlock") && bs::getValue(s, "half") != "upper") {
        return bs::stateHasTag(belowState, "minecraft:supports_small_dripleaf") ||
               (bs::isWaterSource(level.getBlockState(pos)) && bs::stateHasTag(belowState, "minecraft:supports_vegetation"));
    }
    if (isA(b, "DoublePlantBlock") && bs::getValue(s, "half") == "upper") {
        return bs::blockOf(belowState) == b && bs::getValue(belowState, "half") == "lower";
    }
    // VegetationBlock.mayPlaceOn overrides; crops need light (CropBlock.hasSufficientLight).
    if (isA(b, "CropBlock")) return level.rawBrightness(pos, 0) >= 8 && bs::stateHasTag(belowState, "minecraft:supports_crops");
    if (isA(b, "PitcherCropBlock")) { // the lower half needs light
        if (b == PITCHER_CROP && bs::getValue(s, "half") == "lower" && level.rawBrightness(pos, 0) < 8) return false;
        return bs::stateHasTag(belowState, "minecraft:supports_crops");
    }
    if (isA(b, "LilyPadBlock")) {
        bool fluid = bs::isWaterSource(belowState) || bs::stateHasTag(belowState, "minecraft:supports_lily_pad");
        return fluid && !isFluid(aboveState);
    }
    if (isA(b, "NetherWartBlock")) return bs::stateHasTag(belowState, "minecraft:supports_nether_wart");
    if (isA(b, "DryVegetationBlock")) return bs::stateHasTag(belowState, "minecraft:supports_dry_vegetation");
    if (isA(b, "AzaleaBlock")) return bs::stateHasTag(belowState, "minecraft:supports_azalea");
    if (isA(b, "WitherRoseBlock")) return bs::stateHasTag(belowState, "minecraft:supports_wither_rose");
    if (isA(b, "NetherSproutsBlock")) return bs::stateHasTag(belowState, "minecraft:supports_nether_sprouts");
    if (isA(b, "MangrovePropaguleBlock")) {
        if (bs::getBool(s, "hanging")) return bs::stateHasTag(aboveState, "minecraft:supports_hanging_mangrove_propagule");
        return bs::stateHasTag(belowState, "minecraft:supports_mangrove_propagule");
    }
    if (isA(b, "StemBlock") || isA(b, "AttachedStemBlock")) {
        bool melon = gen::kBlocks[b].name.find("melon") != std::string_view::npos;
        return bs::stateHasTag(belowState, melon ? "minecraft:supports_melon_stem" : "minecraft:supports_pumpkin_stem");
    }
    if (isA(b, "NetherFungusBlock") || isA(b, "NetherRootsBlock")) {
        std::string_view n = gen::kBlocks[b].name;
        std::string tag = std::string("minecraft:supports_") + (n.find("crimson") != std::string_view::npos ? "crimson_" : "warped_") +
                          (isA(b, "NetherFungusBlock") ? "fungus" : "roots");
        return bs::stateHasTag(belowState, tag);
    }
    if (isA(b, "CactusFlowerBlock")) {
        return bs::stateHasTag(belowState, "minecraft:support_override_cactus_flower") || (bs::flags(belowState) & bs::kCenterSupportUp);
    }
    if (isA(b, "SeaPickleBlock")) return topFaceNotEmpty(belowState) || bs::isFaceSturdy(belowState, Direction::Up);
    if (isA(b, "SeagrassBlock")) {
        return bs::isFaceSturdy(belowState, Direction::Up) && !bs::stateHasTag(belowState, "minecraft:cannot_support_seagrass");
    }
    if (isA(b, "MushroomBlock")) { // dark enough, on something solid (or on mycelium, podzol, nylium, ...)
        if (bs::stateHasTag(belowState, "minecraft:overrides_mushroom_light_requirement")) return true;
        return level.rawBrightness(pos, 0) < 13 && (bs::flags(belowState) & bs::kCanOcclude) && (bs::flags(belowState) & bs::kFullCollision); // isSolidRender
    }
    if (isA(b, "VegetationBlock")) return bs::stateHasTag(belowState, "minecraft:supports_vegetation");

    // Attached to a wall.
    if (isA(b, "WallTorchBlock") || isA(b, "RedstoneWallTorchBlock") || isA(b, "LadderBlock") || isA(b, "AmethystClusterBlock") ||
        isA(b, "BaseCoralWallFanBlock") || isA(b, "ShelfMushroomBlock")) {
        if (isA(b, "AmethystClusterBlock")) return canAttachTo(level, pos, direction::opposite(facing()));
        return sturdyBehind();
    }
    if (isA(b, "TripWireHookBlock")) return direction::isHorizontal(facing()) && sturdyBehind();
    if (isA(b, "CocoaBlock")) return bs::stateHasTag(level.getBlockState(pos.relative(facing())), "minecraft:supports_cocoa");
    if (isA(b, "WallHangingSignBlock")) return true; // no canSurvive override: it hangs on
    if (isA(b, "WallSignBlock") || isA(b, "WallBannerBlock")) {
        return bs::isSolid(level.getBlockState(pos.relative(direction::opposite(facing()))));
    }
    if (isA(b, "BaseTorchBlock")) return canSupportCenter(level, below, Direction::Up);
    if (isA(b, "GrindstoneBlock")) return true;
    if (isA(b, "FaceAttachedHorizontalDirectionalBlock")) {
        auto face = bs::getValue(s, "face");
        Direction towards = face == "floor" ? Direction::Down : face == "ceiling" ? Direction::Up : direction::opposite(facing());
        return canAttachTo(level, pos, towards);
    }
    if (isA(b, "BellBlock")) return bellCanSurvive(s, level, pos);
    if (isA(b, "LanternBlock")) {
        return bs::getBool(s, "hanging") ? canSupportCenter(level, above, Direction::Down) : canSupportCenter(level, below, Direction::Up);
    }
    if (isA(b, "CeilingHangingSignBlock")) return canSupportCenter(level, above, Direction::Down);
    if (isA(b, "HangingRootsBlock")) return bs::isFaceSturdy(aboveState, Direction::Down);
    if (isA(b, "SporeBlossomBlock")) return canSupportCenter(level, above, Direction::Down) && !(bs::flags(level.getBlockState(pos)) & bs::kWater);
    if (isA(b, "MultifaceBlock")) {
        bool any = false;
        for (Direction d : direction::kAll) {
            if (!bs::hasProperty(s, dirName(d)) || !bs::getBool(s, dirName(d))) continue;
            if (!canAttachFace(level, pos, d)) return false;
            any = true;
        }
        return any;
    }
    if (isA(b, "VineBlock")) {
        for (Direction d : {Direction::Up, Direction::North, Direction::East, Direction::South, Direction::West})
            if (bs::getBool(s, dirName(d)) && vineSupportsFace(level, pos, d)) return true;
        return false;
    }
    if (isA(b, "SpeleothemBlock")) return isValidSpeleothemPlacement(b, level, pos, dirValue(s, "vertical_direction"));
    if (isA(b, "GrowingPlantBlock")) {
        bool down = isA(b, "WeepingVinesBlock") || isA(b, "WeepingVinesPlantBlock") || isA(b, "CaveVinesBlock") || isA(b, "CaveVinesPlantBlock");
        Direction growth = down ? Direction::Down : Direction::Up;
        BlockPos attached = pos.relative(direction::opposite(growth));
        BlockStateId att = level.getBlockState(attached);
        if (isA(b, "KelpBlock") || isA(b, "KelpPlantBlock")) {
            if (bs::stateHasTag(att, "minecraft:cannot_support_kelp")) return false; // canAttachTo
        }
        std::string_view name = gen::kBlocks[b].name;
        std::string head(name), body(name);
        if (name.ends_with("_plant")) head = std::string(name.substr(0, name.size() - 6));
        else body = std::string(name) + "_plant";
        uint32_t ab = bs::blockOf(att);
        return ab == bs::byName(head).value_or(~0u) || ab == bs::byName(body).value_or(~0u) || bs::isFaceSturdy(att, growth);
    }

    // Standing on the block below.
    if (isA(b, "CarpetBlock")) return !bs::isAir(belowState);
    if (isA(b, "MossyCarpetBlock")) return bs::getBool(s, "bottom") ? !bs::isAir(belowState) : bs::blockOf(belowState) == b && bs::getBool(belowState, "bottom");
    if (isA(b, "StandingSignBlock") || isA(b, "BannerBlock") || isA(b, "CakeBlock") || isA(b, "CandleCakeBlock")) return bs::isSolid(belowState);
    if (isA(b, "BaseRailBlock") || isA(b, "DiodeBlock")) return bs::flags(belowState) & bs::kRigidSupportUp;
    if (isA(b, "BasePressurePlateBlock")) return (bs::flags(belowState) & bs::kRigidSupportUp) || canSupportCenter(level, below, Direction::Up);
    if (isA(b, "RedstoneWireBlock")) return bs::isFaceSturdy(belowState, Direction::Up) || bs::is(belowState, HOPPER);
    if (isA(b, "CandleBlock")) return canSupportCenter(level, below, Direction::Up);
    if (isA(b, "LeafLitterBlock") || isA(b, "BaseCoralPlantTypeBlock")) return bs::isFaceSturdy(belowState, Direction::Up);
    if (isA(b, "SnowLayerBlock")) {
        if (bs::stateHasTag(belowState, "minecraft:cannot_support_snow_layer")) return false;
        if (bs::stateHasTag(belowState, "minecraft:support_override_snow_layer")) return true;
        return bs::isFaceSturdy(belowState, Direction::Up) || (bs::is(belowState, SNOW) && intValue(belowState, "layers") == 8);
    }
    if (isA(b, "BambooSaplingBlock") || isA(b, "BambooStalkBlock")) return bs::stateHasTag(belowState, "minecraft:supports_bamboo");
    if (isA(b, "BigDripleafBlock")) {
        return bs::blockOf(belowState) == b || bs::is(belowState, BIG_DRIPLEAF_STEM) || bs::stateHasTag(belowState, "minecraft:supports_big_dripleaf");
    }
    if (isA(b, "BigDripleafStemBlock")) {
        return (bs::blockOf(belowState) == b || bs::stateHasTag(belowState, "minecraft:supports_big_dripleaf")) &&
               (bs::blockOf(aboveState) == b || bs::is(aboveState, BIG_DRIPLEAF));
    }
    if (isA(b, "CactusBlock")) {
        for (Direction d : direction::kHorizontal) {
            BlockStateId n = level.getBlockState(pos.relative(d));
            if (bs::isSolid(n) || (bs::flags(n) & bs::kLava)) return false;
        }
        return (bs::blockOf(belowState) == b || bs::stateHasTag(belowState, "minecraft:supports_cactus")) && !isFluid(aboveState);
    }
    if (isA(b, "SugarCaneBlock")) {
        if (bs::blockOf(belowState) == b) return true;
        if (!bs::stateHasTag(belowState, "minecraft:supports_sugar_cane")) return false;
        for (Direction d : direction::kHorizontal) {
            BlockStateId n = level.getBlockState(below.relative(d));
            if ((bs::flags(n) & bs::kWater) || bs::stateHasTag(n, "minecraft:supports_sugar_cane_adjacently")) return true;
        }
        return false;
    }
    if (isA(b, "ChorusPlantBlock")) {
        bool blockAboveOrBelow = !bs::isAir(aboveState) && !bs::isAir(belowState);
        for (Direction d : direction::kHorizontal) {
            BlockPos np = pos.relative(d);
            if (bs::blockOf(level.getBlockState(np)) != b) continue;
            if (blockAboveOrBelow) return false;
            BlockStateId nb = level.getBlockState(np.below());
            if (bs::blockOf(nb) == b || bs::stateHasTag(nb, "minecraft:supports_chorus_plant")) return true;
        }
        return bs::blockOf(belowState) == b || bs::stateHasTag(belowState, "minecraft:supports_chorus_plant");
    }
    if (isA(b, "ChorusFlowerBlock")) {
        if (bs::is(belowState, CHORUS_PLANT) || bs::stateHasTag(belowState, "minecraft:supports_chorus_flower")) return true;
        if (!bs::isAir(belowState)) return false;
        bool one = false;
        for (Direction d : direction::kHorizontal) {
            BlockStateId n = level.getBlockState(pos.relative(d));
            if (bs::is(n, CHORUS_PLANT)) {
                if (one) return false;
                one = true;
            } else if (!bs::isAir(n)) {
                return false;
            }
        }
        return one;
    }
    if (isA(b, "ScaffoldingBlock")) return scaffoldingDistance(level, pos) < 7;
    if (isA(b, "FarmlandBlock")) return !bs::isSolid(aboveState) || bs::stateHasTag(aboveState, "minecraft:maintains_farmland");
    if (isA(b, "FrogspawnBlock")) {
        return (bs::isWaterSource(belowState) || bs::stateHasTag(belowState, "minecraft:supports_frogspawn")) && !isFluid(aboveState);
    }
    return true;
}

namespace {
// GrowingPlantHeadBlock / GrowingPlantBodyBlock.updateShape: a head with more plant beyond it
// becomes body, a body with nothing beyond it becomes a head; unsupported plants break a tick later.
BlockStateId growingPlantShape(BlockStateId s, ServerLevel& level, BlockPos pos, Direction toNeighbour, BlockStateId neighbour) {
    uint32_t b = bs::blockOf(s);
    std::string_view name = gen::kBlocks[b].name;
    bool body = name.ends_with("_plant");
    uint32_t head = body ? *bs::byName(name.substr(0, name.size() - 6)) : b;
    uint32_t bodyBlock = body ? b : *bs::byName(std::string(name) + "_plant");
    bool down = isA(b, "WeepingVinesBlock") || isA(b, "WeepingVinesPlantBlock") || isA(b, "CaveVinesBlock") || isA(b, "CaveVinesPlantBlock");
    Direction growth = down ? Direction::Down : Direction::Up;
    auto isPlant = [&](BlockStateId st) { return bs::blockOf(st) == head || bs::blockOf(st) == bodyBlock; };
    bool caveVines = isA(b, "CaveVinesBlock") || isA(b, "CaveVinesPlantBlock");
    if (toNeighbour == direction::opposite(growth) && !canSurvive(s, level, pos)) level.scheduleTick(pos, b, 1);
    if (body) {
        if (toNeighbour == growth && !isPlant(neighbour)) { // updateHeadAfterConvertedFromBody
            BlockStateId h = bs::setValue(bs::defaultState(head), "age", std::to_string(level.levelRandom().nextInt(25)));
            return caveVines ? bs::setBool(h, "berries", bs::getBool(s, "berries")) : h;
        }
        return s;
    }
    BlockStateId asBody = caveVines ? bs::setBool(bs::defaultState(bodyBlock), "berries", bs::getBool(s, "berries")) : bs::defaultState(bodyBlock);
    if (toNeighbour == direction::opposite(growth) && canSurvive(s, level, pos) && isPlant(level.getBlockState(pos.relative(growth)))) return asBody;
    if (toNeighbour == growth && isPlant(neighbour)) return asBody;
    return s;
}
} // namespace

namespace {
BlockStateId updateShapeOwn(BlockStateId s, ServerLevel& level, BlockPos pos, Direction toNeighbour, BlockPos neighbourPos, BlockStateId neighbour);
} // namespace

BlockStateId updateShape(BlockStateId s, ServerLevel& level, BlockPos pos, Direction toNeighbour, BlockPos neighbourPos,
                         BlockStateId neighbour) {
    // The fluid part: most blocks holding water schedule it first thing; these only if they survive.
    uint32_t b = bs::blockOf(s);
    bool scheduleAfter = b == SEAGRASS || b == BIG_DRIPLEAF || b == LADDER || b == HANGING_ROOTS || b == SEA_PICKLE;
    bool kelpConverts = (b == KELP && toNeighbour == Direction::Up && (bs::is(neighbour, KELP) || bs::is(neighbour, KELP_PLANT))) ||
                        (b == KELP_PLANT && toNeighbour == Direction::Up && !bs::is(neighbour, KELP) && !bs::is(neighbour, KELP_PLANT));
    if (!scheduleAfter && !kelpConverts && b != TALL_SEAGRASS) fluids::scheduleOnShapeUpdate(level, pos, s, toNeighbour, neighbour);
    if (b == BUBBLE_COLUMN) fluids::bubbleColumnShapeUpdate(level, pos, s, toNeighbour, neighbour);
    BlockStateId result = updateShapeOwn(s, level, pos, toNeighbour, neighbourPos, neighbour);
    if (scheduleAfter && !bs::isAir(result)) fluids::scheduleOnShapeUpdate(level, pos, s, toNeighbour, neighbour);
    return result;
}

namespace {
BlockStateId updateShapeOwn(BlockStateId s, ServerLevel& level, BlockPos pos, Direction toNeighbour, BlockPos neighbourPos, BlockStateId neighbour) {
    uint32_t b = bs::blockOf(s);
    BlockStateId air = bs::defaultState(AIR);

    if (isA(b, "FallingBlock")) level.scheduleTick(pos, b, fallDelay(b)); // (then the block's own updateShape)
    if (isA(b, "NoteBlock") && direction::axis(toNeighbour) == Axis::Y) return noteBlockInstrument(level, pos, s);
    if (isA(b, "FireBlock")) return canSurvive(s, level, pos) ? fireWithAge(level, pos, intValue(s, "age")) : air;
    if (isA(b, "SoulFireBlock")) return canSurvive(s, level, pos) ? bs::defaultState(b) : air;
    if (isA(b, "RedstoneWireBlock")) {
        if (toNeighbour == Direction::Down) return wireCanSurviveOn(neighbour) ? s : air;
        if (toNeighbour == Direction::Up) return wireConnectionState(level, s, pos);
        std::string_view side = wireConnectingSide(level, pos, toNeighbour);
        if ((side != "none") == wireConnected(s, toNeighbour) && !wireIsCross(s)) return bs::setValue(s, dirName(toNeighbour), side);
        return wireConnectionState(level, bs::setValue(wireCross(s), dirName(toNeighbour), side), pos);
    }
    if (isA(b, "ChestBlock") && bs::hasProperty(s, "type")) {
        // ChestBlock.updateShape: pair up with a neighbour that points at us, or split.
        if (chestCanConnectTo(b, neighbour) && direction::isHorizontal(toNeighbour)) {
            auto ntype = bs::getValue(neighbour, "type");
            if (bs::getValue(s, "type") == "single" && ntype != "single" && dirValue(s, "facing") == dirValue(neighbour, "facing") &&
                chestConnectedDirection(neighbour) == direction::opposite(toNeighbour)) {
                s = bs::setValue(s, "type", ntype == "left" ? "right" : "left");
                // CopperChestBlock: take on the partner's weathering.
                if (bs::hasTag(b, "minecraft:copper_chests")) s = withPropertiesOf(bs::blockOf(neighbour), s);
                return s;
            }
        } else if (bs::getValue(s, "type") != "single" && chestConnectedDirection(s) == toNeighbour) {
            return bs::setValue(s, "type", "single");
        }
        // CopperChestBlock: a paired chest follows its partner's weathering.
        if (bs::hasTag(b, "minecraft:copper_chests") && chestCanConnectTo(b, neighbour) && bs::getValue(s, "type") != "single" &&
            chestConnectedDirection(s) == toNeighbour)
            return withPropertiesOf(bs::blockOf(neighbour), s);
        return s;
    }
    if (isA(b, "ChorusPlantBlock")) {
        if (!canSurvive(s, level, pos)) return air; // vanilla breaks it a tick later
        bool c = bs::blockOf(neighbour) == b || bs::is(neighbour, CHORUS_FLOWER) ||
                 (toNeighbour == Direction::Down && bs::stateHasTag(neighbour, "minecraft:supports_chorus_plant"));
        return bs::setBool(s, dirName(toNeighbour), c);
    }
    if (isA(b, "TripWireBlock") && direction::isHorizontal(toNeighbour)) return bs::setBool(s, dirName(toNeighbour), tripwireConnects(neighbour, toNeighbour));
    if (isA(b, "HugeMushroomBlock")) return bs::blockOf(neighbour) == b ? bs::setBool(s, dirName(toNeighbour), false) : s;
    if (isA(b, "CampfireBlock") && toNeighbour == Direction::Down) return bs::setBool(s, "signal_fire", bs::is(neighbour, HAY_BLOCK));
    if (isA(b, "ConcretePowderBlock")) return concreteTouchesLiquid(level, pos) ? bs::defaultState(concreteOf(b)) : s;
    if (isA(b, "LeavesBlock")) { // the distance is updated a tick later (LeavesBlock.tick)
        int fromNeighbour = (bs::stateHasTag(neighbour, "minecraft:prevents_nearby_leaf_decay") ? 0
                             : bs::hasProperty(neighbour, "distance") && isA(neighbour, "LeavesBlock", 0) ? intValue(neighbour, "distance", 7)
                                                                                                          : 7) + 1;
        if (fromNeighbour != 1 || intValue(s, "distance") != fromNeighbour) level.scheduleTick(pos, b, 1);
        return s;
    }
    if (isA(b, "CactusBlock") || isA(b, "SugarCaneBlock") || (isA(b, "ChorusFlowerBlock") && toNeighbour != Direction::Up)) {
        if (!canSurvive(s, level, pos)) level.scheduleTick(pos, b, 1);
        return s;
    }
    if (isA(b, "ChorusFlowerBlock")) return s;
    if (isA(b, "BambooSaplingBlock")) {
        if (!canSurvive(s, level, pos)) return air;
        return toNeighbour == Direction::Up && bs::is(neighbour, BAMBOO) ? bs::defaultState(BAMBOO) : s;
    }
    if (isA(b, "BambooStalkBlock")) {
        if (!canSurvive(s, level, pos)) level.scheduleTick(pos, b, 1);
        if (toNeighbour == Direction::Up && bs::is(neighbour, BAMBOO) && intValue(neighbour, "age") > intValue(s, "age"))
            return bs::setValue(s, "age", intValue(s, "age") == 0 ? "1" : "0"); // cycle(AGE)
        return s;
    }
    if (isA(b, "GrowingPlantBlock")) return growingPlantShape(s, level, pos, toNeighbour, neighbour);
    if (isA(b, "AttachedStemBlock")) {
        bool melon = b == ATTACHED_MELON_STEM;
        if (!bs::is(neighbour, melon ? MELON : PUMPKIN) && toNeighbour == dirValue(s, "facing"))
            return bs::setValue(bs::defaultState(melon ? MELON_STEM : PUMPKIN_STEM), "age", "7");
    }
    if (isA(b, "ScaffoldingBlock")) return canSurvive(s, level, pos) ? scaffoldingState(s, level, pos) : air; // vanilla: falls
    if (isA(b, "MossyCarpetBlock")) {
        if (!canSurvive(s, level, pos)) return air;
        BlockStateId n = mossyCarpetState(s, level, pos, false);
        bool faces = bs::getBool(n, "bottom");
        for (Direction d : direction::kHorizontal) faces = faces || bs::getValue(n, dirName(d)) != "none";
        return faces ? n : air;
    }
    if (isA(b, "MultifaceBlock")) {
        // Faces lose support individually; with none left the block goes.
        if (bs::hasProperty(s, dirName(toNeighbour)) && bs::getBool(s, dirName(toNeighbour)) && !canAttachFace(level, pos, toNeighbour))
            s = bs::setBool(s, dirName(toNeighbour), false);
        for (Direction d : direction::kAll)
            if (bs::hasProperty(s, dirName(d)) && bs::getBool(s, dirName(d))) return s;
        return bs::getBool(s, "waterlogged") ? bs::defaultState(WATER) : air;
    }
    if (isA(b, "VineBlock")) {
        if (toNeighbour == Direction::Down) return s;
        // VineBlock.getUpdatedState: drop faces that lost support (the top face needs the block above).
        for (Direction d : {Direction::Up, Direction::North, Direction::East, Direction::South, Direction::West})
            if (bs::getBool(s, dirName(d)) && !vineSupportsFace(level, pos, d)) s = bs::setBool(s, dirName(d), false);
        for (auto f : {"up", "north", "east", "south", "west"})
            if (bs::getBool(s, f)) return s;
        return air;
    }
    if (isA(b, "SpeleothemBlock")) {
        if (direction::axis(toNeighbour) != Axis::Y) return s;
        Direction tip = dirValue(s, "vertical_direction");
        if (toNeighbour == direction::opposite(tip) && !canSurvive(s, level, pos)) return air; // vanilla: breaks / falls a tick later
        return bs::setValue(s, "thickness", speleothemThickness(b, level, pos, tip, bs::getValue(s, "thickness") == "tip_merge"));
    }
    if (isA(b, "BellBlock")) {
        auto att = bs::getValue(s, "attachment");
        Direction connected = att == "floor" ? Direction::Down : att == "ceiling" ? Direction::Up : dirValue(s, "facing");
        if (connected == toNeighbour && !bellCanSurvive(s, level, pos) && att != "double_wall") return air;
        if (direction::axis(toNeighbour) == direction::axis(dirValue(s, "facing"))) {
            if (att == "double_wall" && !bs::isFaceSturdy(neighbour, direction::opposite(toNeighbour)))
                return bs::setValue(bs::setValue(s, "attachment", "single_wall"), "facing", dirName(direction::opposite(toNeighbour)));
            if (att == "single_wall" && direction::opposite(connected) == toNeighbour && bs::isFaceSturdy(neighbour, dirValue(s, "facing")))
                return bs::setValue(s, "attachment", "double_wall");
        }
        return s;
    }
    if (isA(b, "FarmlandBlock")) { // turns to dirt a tick later (FarmlandBlock.tick)
        if (toNeighbour == Direction::Up && !canSurvive(s, level, pos)) level.scheduleTick(pos, b, 1);
        return s;
    }
    if (isA(b, "PathBlock") && toNeighbour == Direction::Up) {
        bool ok = !bs::isSolid(neighbour) || isA(neighbour, "FenceGateBlock", 0) || isA(neighbour, "MovingPistonBlock", 0);
        return ok ? s : bs::defaultState(DIRT); // vanilla: a tick later
    }
    if (isA(b, "CreakingHeartBlock")) { // updateState (a tick later in vanilla): only ever wakes up
        if (bs::getValue(s, "creaking_heart_state") == "uprooted" && creakingHeartHasLogs(s, level, pos))
            return bs::setValue(s, "creaking_heart_state", "dormant");
        return s;
    }
    if (isA(b, "PotentSulfurBlock") && direction::axis(toNeighbour) == Axis::Y) return potentSulfurState(s, level, pos);
    if (isA(b, "DoorBlock") || isA(b, "DoublePlantBlock")) {
        auto half = bs::getValue(s, "half");
        bool lower = half == "lower";
        if (direction::axis(toNeighbour) == Axis::Y && lower == (toNeighbour == Direction::Up)) {
            // The other half changed.
            if (isA(b, "DoorBlock")) {
                if (isA(neighbour, "DoorBlock", 0) && bs::getValue(neighbour, "half") != half) return bs::setValue(neighbour, "half", *half);
                return air;
            }
            if (!(bs::blockOf(neighbour) == b && bs::getValue(neighbour, "half") != half)) return air;
        }
        if (lower && toNeighbour == Direction::Down && !canSurvive(s, level, pos)) return air;
        return s;
    }
    if (isA(b, "BedBlock")) {
        Direction facing = dirValue(s, "facing");
        bool foot = bs::getValue(s, "part") == "foot";
        if (toNeighbour == (foot ? facing : direction::opposite(facing))) {
            bool partner = bs::blockOf(neighbour) == b && bs::getValue(neighbour, "part") != bs::getValue(s, "part");
            return partner ? bs::setBool(s, "occupied", bs::getBool(neighbour, "occupied")) : air;
        }
        return s;
    }
    if (isA(b, "StairBlock")) {
        return direction::isHorizontal(toNeighbour) ? bs::setValue(s, "shape", stairsShape(s, level, pos)) : s;
    }
    if (isA(b, "WallBlock")) {
        if (toNeighbour == Direction::Down) return s;
        // topUpdate / sideUpdate: only the changed side is re-checked.
        auto connected = [&](Direction d) {
            if (d == toNeighbour) return wallConnects(level, pos, d);
            return bs::getValue(s, dirName(d)).value_or("none") != "none";
        };
        return wallState(s, level, pos, connected(Direction::North), connected(Direction::East), connected(Direction::South),
                         connected(Direction::West));
    }
    if (isA(b, "FenceBlock") || isA(b, "IronBarsBlock")) {
        if (direction::isHorizontal(toNeighbour)) s = connectSide(s, level, pos, toNeighbour);
        return s;
    }
    if (isA(b, "FenceGateBlock")) {
        Direction facing = dirValue(s, "facing");
        if (direction::axis(direction::clockWise(facing)) != direction::axis(toNeighbour)) return s;
        auto isWall = [](BlockStateId x) { return bs::stateHasTag(x, "minecraft:walls"); };
        bool inWall = isWall(neighbour) || isWall(level.getBlockState(pos.relative(direction::opposite(toNeighbour))));
        return bs::setBool(s, "in_wall", inWall);
    }
    if (bs::hasProperty(s, "snowy") && toNeighbour == Direction::Up) { // SnowyBlock (grass, podzol, mycelium)
        return bs::setBool(s, "snowy", bs::stateHasTag(neighbour, "minecraft:snow"));
    }
    if (needsSupport(b) && !canSurvive(s, level, pos)) return air;
    return s;
}
} // namespace

void updateIndirectNeighbourShapes(ServerLevel& level, BlockPos pos, BlockStateId s, int flags, int depth) {
    if (!isA(s, "RedstoneWireBlock", 0)) return;
    uint32_t wire = bs::blockOf(s);
    for (Direction d : direction::kHorizontal) {
        if (!wireConnected(s, d) || bs::blockOf(level.getBlockState(pos.relative(d))) == wire) continue;
        for (BlockPos p : {pos.relative(d).below(), pos.relative(d).above()})
            if (bs::blockOf(level.getBlockState(p)) == wire) level.neighborShapeChanged(direction::opposite(d), p, p.relative(direction::opposite(d)), flags, depth + 1);
    }
}

bool isSmokeyPos(ServerLevel& level, BlockPos pos) {
    auto isLitCampfire = [](BlockStateId st) { return bs::stateHasTag(st, "minecraft:campfires") && bs::getBool(st, "lit"); };
    for (int i = 1; i <= 5; i++) {
        BlockStateId below = level.getBlockState({pos.x, pos.y - i, pos.z});
        if (isLitCampfire(below)) return true;
        // Shapes.joinIsNotEmpty(SHAPE_VIRTUAL_POST (a 4x16x4 post in the middle), collision shape, AND)
        bool blocked = false;
        for (const auto& b : gen::kShapes[gen::kStateShape[below]])
            blocked = blocked || (b.minX < 0.625 && b.maxX > 0.375 && b.minY < 1.0 && b.maxY > 0.0 && b.minZ < 0.625 && b.maxZ > 0.375);
        if (blocked) return isLitCampfire(level.getBlockState({pos.x, pos.y - i - 1, pos.z}));
    }
    return false;
}

// ---------------------------------------------------------------- fire (BaseFireBlock, FireBlock, SoulFireBlock)

namespace {
int fireIgniteOdds(BlockStateId st) { return bs::getBool(st, "waterlogged") ? 0 : bs::behaviour(bs::blockOf(st)).igniteOdds; }
int fireBurnOdds(BlockStateId st) { return bs::getBool(st, "waterlogged") ? 0 : bs::behaviour(bs::blockOf(st)).burnOdds; }
bool fireCanBurn(BlockStateId st) { return fireIgniteOdds(st) > 0; }

bool isValidFireLocation(ServerLevel& level, BlockPos pos) {
    for (Direction d : direction::kAll)
        if (fireCanBurn(level.getBlockState(pos.relative(d)))) return true;
    return false;
}

// FireBlock.getStateForPlacement: in the air, fire shows on the sides that can burn.
BlockStateId fireForPlacement(ServerLevel& level, BlockPos pos) {
    BlockStateId result = bs::defaultState(FIRE);
    BlockStateId below = level.getBlockState(pos.below());
    if (fireCanBurn(below) || bs::isFaceSturdy(below, Direction::Up)) return result;
    for (Direction d : {Direction::North, Direction::East, Direction::South, Direction::West, Direction::Up})
        result = bs::setBool(result, direction::name(d), fireCanBurn(level.getBlockState(pos.relative(d))));
    return result;
}

BlockStateId fireWithAge(ServerLevel& level, BlockPos pos, int age) { // FireBlock.getStateWithAge
    BlockStateId st = fireStateFor(level, pos);
    return bs::blockOf(st) == FIRE ? bs::setValue(st, "age", std::to_string(age)) : st;
}

bool increasedFireBurnout(ServerLevel& level, BlockPos pos) { // the biome's gameplay/increased_fire_burnout
    static const json::Value attributes = json::parse(gen::kBiomeAttributesJson);
    std::string_view biome = registries::name("minecraft:worldgen/biome", static_cast<int>(level.getBiome(pos)));
    return attributes[biome]["minecraft:gameplay/increased_fire_burnout"].boolean();
}

int nextInt(ServerLevel& level, int bound) { return std::uniform_int_distribution<int>(0, bound - 1)(level.random()); }

// FireBlock.checkBurnOut
void fireCheckBurnOut(ServerLevel& level, BlockPos pos, int chance, int age) {
    BlockStateId old = level.getBlockState(pos);
    if (nextInt(level, chance) >= fireBurnOdds(old)) return;
    if (nextInt(level, age + 10) < 5) level.setBlock(pos, fireWithAge(level, pos, std::min(age + nextInt(level, 5) / 4, 15))); // (no rain yet)
    else level.setBlock(pos, bs::defaultState(AIR));
    // TODO: TntBlock.prime (primed TNT is an entity).
}

// FireBlock.tick
void fireTick(ServerLevel& level, BlockPos pos, BlockStateId s) {
    level.scheduleTick(pos, FIRE, 30 + nextInt(level, 10));
    if (!level.canSpreadFireAround(pos)) return;
    if (!canSurvive(s, level, pos)) level.setBlock(pos, bs::defaultState(AIR));
    bool infiniBurn = bs::stateHasTag(level.getBlockState(pos.below()), "minecraft:infiniburn_overworld");
    int age = intValue(s, "age");
    // (no weather yet: rain never puts fire out)
    int newAge = std::min(15, age + nextInt(level, 3) / 2);
    if (age != newAge) {
        s = bs::setValue(s, "age", std::to_string(newAge));
        level.setBlock(pos, s, 260); // UPDATE_NONE: nobody is told, the age is invisible
    }
    if (!infiniBurn) {
        if (!isValidFireLocation(level, pos)) {
            if (!bs::isFaceSturdy(level.getBlockState(pos.below()), Direction::Up) || age > 3) level.setBlock(pos, bs::defaultState(AIR));
            return;
        }
        if (age == 15 && nextInt(level, 4) == 0 && !fireCanBurn(level.getBlockState(pos.below()))) {
            level.setBlock(pos, bs::defaultState(AIR));
            return;
        }
    }
    bool increasedBurnout = increasedFireBurnout(level, pos);
    int extra = increasedBurnout ? -50 : 0;
    fireCheckBurnOut(level, {pos.x + 1, pos.y, pos.z}, 300 + extra, age);
    fireCheckBurnOut(level, {pos.x - 1, pos.y, pos.z}, 300 + extra, age);
    fireCheckBurnOut(level, pos.below(), 250 + extra, age);
    fireCheckBurnOut(level, pos.above(), 250 + extra, age);
    fireCheckBurnOut(level, {pos.x, pos.y, pos.z - 1}, 300 + extra, age);
    fireCheckBurnOut(level, {pos.x, pos.y, pos.z + 1}, 300 + extra, age);
    for (int xx = -1; xx <= 1; xx++) {
        for (int zz = -1; zz <= 1; zz++) {
            for (int yy = -1; yy <= 4; yy++) {
                if (xx == 0 && yy == 0 && zz == 0) continue;
                int rate = 100 + (yy > 1 ? (yy - 1) * 100 : 0);
                BlockPos test{pos.x + xx, pos.y + yy, pos.z + zz};
                // FireBlock.getIgniteOdds(level, pos): an empty block next to something flammable
                int ignite = 0;
                if (bs::isAir(level.getBlockState(test)))
                    for (Direction d : direction::kAll) ignite = std::max(ignite, fireIgniteOdds(level.getBlockState(test.relative(d))));
                if (ignite <= 0) continue;
                int odds = (ignite + 40 + level.difficulty * 7) / (age + 30);
                if (increasedBurnout) odds /= 2;
                if (odds > 0 && nextInt(level, rate) <= odds) level.setBlock(test, fireWithAge(level, test, std::min(15, age + nextInt(level, 5) / 4)));
            }
        }
    }
}
} // namespace

BlockStateId fireStateFor(ServerLevel& level, BlockPos pos) {
    if (bs::stateHasTag(level.getBlockState(pos.below()), "minecraft:soul_fire_base_blocks")) return bs::defaultState(SOUL_FIRE);
    return fireForPlacement(level, pos);
}

bool fireCanBePlacedAt(ServerLevel& level, BlockPos pos) {
    // TODO: fire in an obsidian frame lights a nether portal (BaseFireBlock.isPortal, PortalShape).
    return bs::isAir(level.getBlockState(pos)) && canSurvive(fireStateFor(level, pos), level, pos);
}

void onLand(ServerLevel& level, BlockPos pos, BlockStateId state, BlockStateId replaced) {
    uint32_t b = bs::blockOf(state);
    if (isA(b, "AnvilBlock")) level.levelEvent(1031, pos, 0);
    if (isA(b, "ConcretePowderBlock") && ((bs::flags(replaced) & bs::kWater) || concreteTouchesLiquid(level, pos)))
        level.setBlock(pos, bs::defaultState(concreteOf(b)));
}

void onBrokenAfterFall(ServerLevel& level, BlockPos pos, BlockStateId state) {
    if (isA(bs::blockOf(state), "AnvilBlock")) level.levelEvent(1029, pos, 0);
}

namespace {
// SpongeBlock.tryAbsorbWater / removeWaterBreadthFirstSearch (BlockPos.breadthFirstTraversal,
// depth 6, at most 65 positions): water around the sponge goes, and it turns wet.
void spongeTryAbsorbWater(ServerLevel& level, BlockPos start) {
    std::deque<std::pair<BlockPos, int>> nodes{{start, 0}};
    std::unordered_set<int64_t> visited;
    int count = 0;
    auto process = [&](BlockPos pos) {
        if (pos == start) return true;
        BlockStateId state = level.getBlockState(pos);
        if (!fluids::isWater(fluids::stateOf(state).type)) return false;
        if (fluids::info::isBucketPickup(state) && fluids::pickupBlock(level, pos, state)) return true;
        uint32_t b = bs::blockOf(state);
        if (isA(b, "LiquidBlock")) {
            level.setBlock(pos, bs::defaultState(AIR));
        } else {
            if (b != KELP && b != KELP_PLANT && b != SEAGRASS && b != TALL_SEAGRASS) return false;
            level.dropResources(state, pos, nullptr, nullptr, nullptr);
            level.setBlock(pos, bs::defaultState(AIR));
        }
        return true;
    };
    while (!nodes.empty()) {
        auto [pos, depth] = nodes.front();
        nodes.pop_front();
        if (!visited.insert(pos.asLong()).second) continue;
        if (pos.y < level.minY() || pos.y > level.maxY() || !process(pos)) continue; // SKIP
        if (++count >= 65) break;
        if (depth < 6)
            for (int d = 0; d < 6; d++) nodes.emplace_back(pos.relative(static_cast<Direction>(d)), depth + 1);
    }
    if (count > 1) {
        level.setBlock(start, bs::defaultState(WET_SPONGE), ServerLevel::kUpdateClients);
        level.playBlockSound(-1, start, registries::id("minecraft:sound_event", "minecraft:block.sponge.absorb").value_or(-1), 1.0f, 1.0f);
    }
}
} // namespace

bool isRedstoneWire(BlockStateId state) { return isA(state, "RedstoneWireBlock", 0); }

void neighborChanged(ServerLevel& level, BlockPos pos, BlockStateId state, uint32_t sourceBlock) {
    (void)sourceBlock;
    uint32_t b = bs::blockOf(state);
    if (isA(b, "LiquidBlock")) return fluids::liquidNeighborChanged(level, pos, state);
    if (b == SPONGE) return spongeTryAbsorbWater(level, pos);
    // TODO: redstone components (doors, trapdoors, gates, note blocks, rails, TNT, ...), frosted ice
}

void onPlace(ServerLevel& level, BlockPos pos, BlockStateId state, BlockStateId old) {
    uint32_t b = bs::blockOf(state);
    if (isA(b, "LiquidBlock")) return fluids::liquidOnPlace(level, pos, state);
    if (b == SPONGE && bs::blockOf(old) != b) return spongeTryAbsorbWater(level, pos);
    if (isA(b, "FallingBlock")) level.scheduleTick(pos, b, fallDelay(b));
    if (isA(b, "FrostedIceBlock")) level.scheduleTick(pos, b, level.levelRandom().nextInt(61) + 60); // Mth.nextInt(60, 120)
    if (isA(b, "BaseFireBlock")) {
        if (bs::blockOf(old) != b && !canSurvive(state, level, pos)) {
            level.setBlock(pos, bs::defaultState(AIR));
            return;
        }
        if (b == FIRE) level.scheduleTick(pos, FIRE, 30 + nextInt(level, 10)); // FireBlock.onPlace
    }
}

BlockStateId updateFromNeighbourShapes(BlockStateId state, ServerLevel& level, BlockPos pos) {
    for (Direction d : {Direction::West, Direction::East, Direction::North, Direction::South, Direction::Down, Direction::Up}) {
        BlockPos n = pos.relative(d);
        state = updateShape(state, level, pos, d, n, level.getBlockState(n));
    }
    return state;
}

bool hasTick(uint32_t block) {
    return isA(block, "ComposterBlock") || block == FIRE || isA(block, "FallingBlock") || isA(block, "LecternBlock") || isA(block, "LiquidBlock") ||
           block == BUBBLE_COLUMN || detail::hasPlantTick(block);
}

void tick(ServerLevel& level, BlockPos pos, BlockStateId s) {
    uint32_t b = bs::blockOf(s);
    if (detail::plantTick(level, pos, s)) return;
    if (isA(b, "LiquidBlock")) return fluids::liquidTick(level, pos, s);
    if (b == BUBBLE_COLUMN) return fluids::bubbleColumnTick(level, pos, s);
    if (b == FIRE) return fireTick(level, pos, s);
    if (isA(b, "FallingBlock")) { // FallingBlock.tick: nothing below any more
        BlockStateId below = level.getBlockState(pos.below());
        bool free = bs::isAir(below) || bs::stateHasTag(below, "minecraft:fire") || bs::hasFluid(below) || bs::canBeReplaced(below);
        if (free && pos.y >= level.minY()) FallingBlockEntity::fall(level, pos, s);
        return;
    }
    if (isA(b, "LecternBlock")) { // LecternBlock.tick: the page-turn pulse ends
        if (bs::getBool(s, "powered")) level.setBlock(pos, bs::setBool(s, "powered", false));
        return;
    }
    if (isA(b, "ComposterBlock") && bs::getValue(s, "level") == "7") { // the compost is ready
        level.setBlock(pos, bs::setValue(s, "level", "8"));
        static const int32_t kReady = registries::id("minecraft:sound_event", "minecraft:block.composter.ready").value_or(-1);
        level.playBlockSound(-1, pos, kReady, 1.0f, 1.0f);
    }
}

void setPlacedBy(ServerLevel& level, BlockPos pos, BlockStateId s) {
    uint32_t b = bs::blockOf(s);
    if (isA(b, "BaseRailBlock")) { // BaseRailBlock.onPlace -> updateDir (no redstone signals yet)
        RailState(level, pos, s).place(false, true, bs::getValue(s, "shape").value_or("north_south"));
        return;
    }
    if (isA(b, "DoorBlock") || isA(b, "DoublePlantBlock")) {
        BlockStateId upper = bs::setValue(s, "half", "upper");
        if (bs::hasProperty(upper, "waterlogged")) upper = bs::setBool(upper, "waterlogged", bs::isWaterSource(level.getBlockState(pos.above())));
        level.setBlock(pos.above(), upper);
    } else if (isA(b, "BedBlock")) {
        level.setBlock(pos.relative(dirValue(s, "facing")), bs::setValue(s, "part", "head"));
    }
}

bool useWithoutItem(ServerLevel& level, BlockPos pos, BlockStateId s, float playerYRot, int playerId, bool mayBuild) {
    uint32_t b = bs::blockOf(s);
    const auto& beh = bs::behaviour(b);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    // DoorBlock/TrapDoorBlock/FenceGateBlock.playSound: the BlockSetType/WoodType open or close sound.
    auto openCloseSound = [&](bool open) {
        level.playBlockSound(playerId, pos, open ? beh.openSound : beh.closeSound, 1.0f, unit(level.random()) * 0.1f + 0.9f);
    };
    // Iron doors/trapdoors can only be opened by redstone (BlockSetType.canOpenByHand).
    if ((isA(b, "DoorBlock") && b != IRON_DOOR) || (isA(b, "TrapDoorBlock") && b != IRON_TRAPDOOR)) {
        bool open = !bs::getBool(s, "open");
        level.setBlock(pos, bs::setBool(s, "open", open));
        openCloseSound(open);
        return true;
    }
    if (isA(b, "FenceGateBlock")) {
        if (bs::getBool(s, "open")) {
            s = bs::setBool(s, "open", false);
        } else {
            Direction player = direction::fromYRot(playerYRot);
            if (dirValue(s, "facing") == direction::opposite(player)) s = bs::setValue(s, "facing", dirName(player));
            s = bs::setBool(s, "open", true);
        }
        level.setBlock(pos, s);
        openCloseSound(bs::getBool(s, "open"));
        return true;
    }
    if (isA(b, "LeverBlock")) {
        bool powered = !bs::getBool(s, "powered");
        level.setBlock(pos, bs::setBool(s, "powered", powered));
        static const int32_t kLeverClick = registries::id("minecraft:sound_event", "minecraft:block.lever.click").value_or(-1);
        level.playBlockSound(playerId, pos, kLeverClick, 0.3f, powered ? 0.6f : 0.5f); // LeverBlock.playSound
        return true; // TODO: redstone power propagation
    }
    // Adjustable redstone components (their redstone behaviour isn't ported yet).
    if (isA(b, "RepeaterBlock") && mayBuild) {
        int delay = std::stoi(std::string(bs::getValue(s, "delay").value_or("1")));
        level.setBlock(pos, bs::setValue(s, "delay", std::to_string(delay % 4 + 1)));
        return true;
    }
    if (isA(b, "ComparatorBlock") && mayBuild) {
        bool subtract = bs::getValue(s, "mode") != "subtract";
        level.setBlock(pos, bs::setValue(s, "mode", subtract ? "subtract" : "compare"));
        static const int32_t kClick = registries::id("minecraft:sound_event", "minecraft:block.comparator.click").value_or(-1);
        level.playBlockSound(playerId, pos, kClick, 0.3f, subtract ? 0.55f : 0.5f);
        return true;
    }
    if (isA(b, "DaylightDetectorBlock") && mayBuild) {
        level.setBlock(pos, bs::setBool(s, "inverted", !bs::getBool(s, "inverted")));
        return true;
    }
    if (isA(b, "RedstoneWireBlock") && mayBuild && (wireIsCross(s) || wireIsDot(s))) { // toggle dot <-> cross
        BlockStateId n = wireIsCross(s) ? bs::setValue(bs::defaultState(b), "power", std::string(bs::getValue(s, "power").value_or("0"))) : wireCross(s);
        n = wireConnectionState(level, n, pos);
        if (n != s) {
            level.setBlock(pos, n);
            return true;
        }
    }
    if (isA(b, "NoteBlock")) { // cycle the note, then play it
        auto note = bs::getValue(s, "note").value_or("0");
        int next = (std::stoi(std::string(note)) + 1) % 25;
        s = bs::setValue(s, "note", std::to_string(next));
        level.setBlock(pos, s);
        playNote(level, pos, s);
        return true;
    }
    // Blocks with a use in vanilla that isn't ported yet: they still consume the click, so using
    // them doesn't place a block against them. (Container menus, beds, lecterns and the like are
    // handled by the packet listener before this.)
    // TODO: the cartography table (maps), bells, buttons (redstone) and command blocks.
    static constexpr std::string_view kInteractive[] = {"CartographyTableBlock", "BellBlock", "ButtonBlock", "CommandBlock"};
    for (auto cls : kInteractive)
        if (isA(b, cls)) return true;
    return false;
}

void attack(ServerLevel& level, BlockPos pos, BlockStateId s) {
    if (isA(bs::blockOf(s), "NoteBlock")) playNote(level, pos, s);
}

bool triggerEvent(ServerLevel& level, BlockPos pos, BlockStateId s, int b0, int b1) {
    // DecoratedPotBlockEntity.triggerEvent: a wobble (clients animate it).
    if (isA(bs::blockOf(s), "DecoratedPotBlock")) return b0 == 1 && b1 >= 0 && b1 < 2;
    // Chest, ender chest and shulker box lids (the viewer count).
    if (isA(bs::blockOf(s), "ChestBlock") || isA(bs::blockOf(s), "EnderChestBlock") || isA(bs::blockOf(s), "ShulkerBoxBlock")) return b0 == 1;
    if (!isA(bs::blockOf(s), "NoteBlock")) return false;
    const auto& inst = instrumentOf(s);
    float pitch = 1.0f;
    if (inst.tunable) {
        int note = std::stoi(std::string(bs::getValue(s, "note").value_or("0")));
        pitch = static_cast<float>(std::pow(2.0, (note - 12) / 12.0)); // getPitchFromNote; clients show the particle
    }
    if (inst.customSound) {
        // A player head's note_block_sound (SkullBlockEntity), played as an inline sound event.
        BlockPos above = pos.above();
        auto* be = level.getChunk({above.x >> 4, above.z >> 4}).blockEntityAt(above.x, above.y, above.z);
        std::string sound = be ? be->tag.getString("note_block_sound") : "";
        if (sound.empty()) return false;
        level.playDirectSound(pos.x + 0.5, pos.y + 0.5, pos.z + 0.5, std::move(sound), ServerLevel::kSoundSourceRecords, 3.0f, pitch);
        return true;
    }
    level.playSound(-1, pos.x + 0.5, pos.y + 0.5, pos.z + 0.5, inst.sound, ServerLevel::kSoundSourceRecords, 3.0f, pitch);
    return true;
}

namespace detail {
int intValue(BlockStateId s, std::string_view property, int def) { return block_behaviour::intValue(s, property, def); }
bool canAttachFace(ServerLevel& level, BlockPos pos, Direction d) { return block_behaviour::canAttachFace(level, pos, d); }
bool vineSupportsFace(ServerLevel& level, BlockPos pos, Direction d) { return block_behaviour::vineSupportsFace(level, pos, d); }
bool isSpeleothemWithDirection(BlockStateId s, Direction tip) { return block_behaviour::isSpeleothemWithDirection(s, tip); }
bool isValidSpeleothemPlacement(uint32_t self, ServerLevel& level, BlockPos pos, Direction tip) {
    return block_behaviour::isValidSpeleothemPlacement(self, level, pos, tip);
}
BlockStateId chorusConnections(BlockStateId s, ServerLevel& level, BlockPos pos) { return block_behaviour::chorusConnections(s, level, pos); }
BlockStateId leavesDistance(BlockStateId s, ServerLevel& level, BlockPos pos) { return block_behaviour::leavesDistance(s, level, pos); }
} // namespace detail

} // namespace block_behaviour
} // namespace mc
