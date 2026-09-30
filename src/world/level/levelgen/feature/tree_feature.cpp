// Ports of TreeFeature (trunk placers, foliage placers, the mangrove root placer, feature sizes,
// tree decorators) and the huge mushroom features, interpreting their JSON the way the codecs in
// net.minecraft.world.level.levelgen.feature do. Random numbers are drawn in vanilla's order, so a
// seed grows the same tree as vanilla (checked by tools/feature_check).
#include "world/level/levelgen/feature/tree_feature.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include "generated/behaviour.hpp"
#include "generated/blocks.hpp"
#include "nbt/tag.hpp"
#include "server/level/server_level.hpp"
#include "util/json.hpp"
#include "util/log.hpp"
#include "world/level/block/block_behaviour.hpp"
#include "world/level/levelgen/block_predicate.hpp"
#include "world/level/levelgen/feature/java_hash_set.hpp"

namespace mc::feature {
namespace {
namespace bs = blocks;
using json::Value;
using namespace gen::block;

const Value& features() {
    static const Value v = json::parse(gen::kFeaturesJson);
    return v;
}
const Value& registeredProviders() { // minecraft:worldgen/block_state_provider
    static const Value v = json::parse(gen::kRegistryElementsJson)["minecraft:worldgen/block_state_provider"];
    return v;
}

// Direction.Plane.HORIZONTAL in vanilla's order (random picks index into it).
constexpr Direction kPlane[] = {Direction::North, Direction::East, Direction::South, Direction::West};
Direction randomHorizontal(LegacyRandom& random) { return kPlane[random.nextInt(4)]; }

int toInt(const Value& v) { return static_cast<int>(v.number()); }
int intOr(const Value& obj, std::string_view key, int def) { return obj.has(key) ? toInt(obj[key]) : def; }

// Mth.sin / Mth.cos: a 65536-entry table.
const std::vector<float>& sinTable() {
    static const std::vector<float> table = [] {
        std::vector<float> t(65536);
        for (int i = 0; i < 65536; i++) t[i] = static_cast<float>(std::sin(i / 10430.378350470453));
        return t;
    }();
    return table;
}
float mthSin(double v) { return sinTable()[static_cast<size_t>(static_cast<int64_t>(v * 10430.378350470453) & 65535)]; }
float mthCos(double v) { return sinTable()[static_cast<size_t>(static_cast<int64_t>(v * 10430.378350470453 + 16384.0) & 65535)]; }
// Mth.floor: (int) Math.floor(v), with Java's cast (NaN is 0, out of range saturates).
int floorInt(double v) {
    double f = std::floor(v);
    if (std::isnan(f)) return 0;
    return static_cast<int>(std::clamp(f, -2147483648.0, 2147483647.0));
}

// ---------------------------------------------------------------- IntProviders

int sampleInt(const Value& p, LegacyRandom& random) {
    if (p.isNumber()) return toInt(p);
    std::string_view type = p["type"].isString() ? std::string_view(p["type"].string()) : std::string_view("minecraft:uniform");
    if (type == "minecraft:constant") return toInt(p["value"]);
    if (type == "minecraft:uniform") { // Mth.randomBetweenInclusive
        int lo = toInt(p["min_inclusive"]), hi = toInt(p["max_inclusive"]);
        return random.nextInt(hi - lo + 1) + lo;
    }
    if (type == "minecraft:biased_to_bottom") {
        int lo = toInt(p["min_inclusive"]), hi = toInt(p["max_inclusive"]);
        return lo + random.nextInt(random.nextInt(hi - lo + 1) + 1);
    }
    if (type == "minecraft:clamped") return std::clamp(sampleInt(p["source"], random), toInt(p["min_inclusive"]), toInt(p["max_inclusive"]));
    if (type == "minecraft:weighted_list") { // WeightedList.getRandomOrThrow, then its sample
        const auto& entries = p["distribution"].array();
        int total = 0;
        for (const auto& e : entries) total += toInt(e["weight"]);
        int selection = random.nextInt(total);
        for (const auto& e : entries) {
            selection -= toInt(e["weight"]);
            if (selection < 0) return sampleInt(e["data"], random);
        }
    }
    log::warn("unsupported int provider {}", type);
    return 0;
}

// ---------------------------------------------------------------- BlockStateProviders

BlockStateId parseState(const Value& v) {
    if (v.isString()) return bs::defaultState(bs::byName(v.string()).value_or(AIR));
    return levelgen::parseBlockState(v).value_or(bs::defaultState(AIR));
}

BlockStateId getState(const Value& p, ServerLevel& level, LegacyRandom& random, BlockPos pos);

// BlockStateProvider.getOptionalState (a holder: a registered provider's id, or one inline).
std::optional<BlockStateId> optionalState(const Value& p, ServerLevel& level, LegacyRandom& random, BlockPos pos) {
    if (p.isString()) return optionalState(registeredProviders()[p.string()], level, random, pos);
    if (!p.has("type")) return parseState(p); // a plain BlockState: SimpleStateProvider
    std::string_view type = p["type"].string();
    if (type == "minecraft:simple") return parseState(p["state"]);
    if (type == "minecraft:weighted") {
        const auto& entries = p["entries"].array();
        int total = 0;
        for (const auto& e : entries) total += toInt(e["weight"]);
        int selection = random.nextInt(total);
        for (const auto& e : entries) {
            selection -= toInt(e["weight"]);
            if (selection < 0) return parseState(e["data"]);
        }
        return std::nullopt;
    }
    if (type == "minecraft:rule_based") {
        for (const auto& rule : p["rules"].array())
            if (levelgen::testBlockPredicate(rule["if_true"], level, pos))
                if (auto s = optionalState(rule["then"], level, random, pos)) return s;
        if (p.has("fallback")) return optionalState(p["fallback"], level, random, pos);
        return std::nullopt;
    }
    if (type == "minecraft:randomized_int") { // the source's state with a random value for an int property
        BlockStateId s = getState(p["source"], level, random, pos);
        if (!bs::hasProperty(s, p["property"].string())) return s;
        return bs::setValue(s, p["property"].string(), std::to_string(sampleInt(p["values"], random)));
    }
    if (type == "minecraft:rotated") { // a fixed or random direction: axis and facing
        Direction dir = p.has("direction") ? direction::byName(p["direction"].string()) : direction::kAll[random.nextInt(6)];
        BlockStateId s = getState(p["state"], level, random, pos);
        s = bs::setValue(bs::setValue(s, "axis", direction::axisName(direction::axis(dir))), "facing", direction::name(dir));
        return s;
    }
    log::warn("unsupported block state provider {}", type);
    return std::nullopt;
}
// BlockStateProvider.getState: RuleBasedStateProvider falls back to what's there.
BlockStateId getState(const Value& p, ServerLevel& level, LegacyRandom& random, BlockPos pos) {
    auto s = optionalState(p, level, random, pos);
    return s ? *s : level.getBlockState(pos);
}

BlockStateId withAxis(BlockStateId s, Axis a) { return bs::setValue(s, "axis", direction::axisName(a)); } // trySetValue

// ---------------------------------------------------------------- TreeFeature

constexpr int kTreeFlags = 19; // UPDATE_NEIGHBORS | UPDATE_CLIENTS | UPDATE_KNOWN_SHAPE

struct Attachment { // FoliagePlacer.FoliageAttachment
    BlockPos pos;
    int radiusOffsetXZ = 0;
    int foliageHeightOffset = 0;
    int sizeX = 1, sizeZ = 1;
    bool doubleTrunk() const { return sizeX == 2 && sizeZ == 2; }
};
Attachment attachment(BlockPos pos, int radiusOffset, bool doubleTrunk) {
    int size = doubleTrunk ? 2 : 1;
    return {pos, radiusOffset, 0, size, size};
}

class Tree {
public:
    Tree(const Value& cfg, ServerLevel& level, LegacyRandom& random) : cfg(cfg), level(level), random(random) {}

    const Value& cfg;
    ServerLevel& level;
    LegacyRandom& random;
    JavaBlockPosSet roots, trunks, foliage, decorations;

    const Value& trunkPlacer() const { return cfg["trunk_placer"]; }
    const Value& foliagePlacer() const { return cfg["foliage_placer"]; }
    std::string_view trunkType() const { return trunkPlacer()["type"].string(); }
    std::string_view foliageType() const { return foliagePlacer()["type"].string(); }

    BlockStateId stateAt(BlockPos p) { return level.getBlockState(p); }
    void setBlock(BlockPos p, BlockStateId s) { level.setBlock(p, s, kTreeFlags); }
    void setRoot(BlockPos p, BlockStateId s) { roots.add(p), setBlock(p, s); }
    void setTrunk(BlockPos p, BlockStateId s) { trunks.add(p), setBlock(p, s); }
    void setFoliage(BlockPos p, BlockStateId s) { foliage.add(p), setBlock(p, s); }
    void setDecoration(BlockPos p, BlockStateId s) { decorations.add(p), setBlock(p, s); }

    // TreeFeature.validTreePos / isVine / isAirOrLeaves
    bool validTreePos(BlockPos p) {
        BlockStateId s = stateAt(p);
        return bs::isAir(s) || bs::stateHasTag(s, "minecraft:replaceable_by_trees");
    }
    bool isAirOrLeaves(BlockPos p) {
        BlockStateId s = stateAt(p);
        return bs::isAir(s) || bs::stateHasTag(s, "minecraft:leaves");
    }
    // The trunk placer's validTreePos (UpwardsBranchingTrunkPlacer: also can_grow_through).
    bool trunkValidTreePos(BlockPos p) {
        if (validTreePos(p)) return true;
        return trunkType() == "minecraft:upwards_branching_trunk_placer" && levelgen::matchesBlocks(trunkPlacer()["can_grow_through"], bs::blockOf(stateAt(p)));
    }
    bool isFree(BlockPos p) { return trunkValidTreePos(p) || bs::stateHasTag(stateAt(p), "minecraft:logs"); } // TrunkPlacer.isFree

    bool placeLog(BlockPos p, const std::function<BlockStateId(BlockStateId)>& modifier = {}) {
        if (!trunkValidTreePos(p)) return false;
        BlockStateId s = getState(cfg["trunk_provider"], level, random, p);
        setTrunk(p, modifier ? modifier(s) : s);
        return true;
    }
    void placeLogIfFree(BlockPos p) {
        if (isFree(p)) placeLog(p);
    }
    void placeBelowTrunkBlock(BlockPos p) {
        if (auto s = optionalState(cfg["below_trunk_provider"], level, random, p)) setTrunk(p, *s);
    }

    int treeHeight() { // TrunkPlacer.getTreeHeight
        const Value& t = trunkPlacer();
        int base = toInt(t["base_height"]);
        int a = random.nextInt(toInt(t["height_rand_a"]) + 1);
        int b = random.nextInt(toInt(t["height_rand_b"]) + 1);
        return base + a + b;
    }

    std::vector<Attachment> placeTrunk(int treeHeight, BlockPos origin);
    int foliageHeight(int treeHeight);
    int foliageRadius(int trunkHeight) { return sampleInt(foliagePlacer()["radius"], random); }
    void createFoliage(int treeHeight, const Attachment& a, int foliageHeight, int leafRadius);
    bool placeRoots(BlockPos origin, BlockPos trunkOrigin);
    void decorate();
    bool place(BlockPos origin);

private:
    // FoliagePlacer helpers
    bool shouldSkipLocation(int dx, int y, int dz, int r, bool doubleTrunk);
    bool shouldSkipLocationSigned(int dx, int y, int dz, int r, bool doubleTrunk);
    void placeLeavesRow(BlockPos origin, int r, int y, bool doubleTrunk);
    void placeLeavesRowWithHangingLeavesBelow(BlockPos origin, int r, int y, bool doubleTrunk, float chance, float extensionChance);
    bool tryPlaceLeaf(BlockPos p);
    bool tryPlaceExtension(float chance, BlockPos logPos, BlockPos p);
    // PoplarFoliagePlacer
    void poplarLeavesRow(BlockPos origin, int r, int y, bool doubleTrunk, int foliageHeight, bool flip);
    // TrunkPlacers
    std::vector<Attachment> fancyTrunk(int treeHeight, BlockPos origin);
    bool makeLimb(BlockPos start, BlockPos end, bool doPlace);
    Attachment cherryBranch(int treeHeight, BlockPos origin, Direction branchDir, int offsetFromOrigin, bool middleContinuesUpwards, Axis sideways);
    void upwardsBranch(int treeHeight, std::vector<Attachment>& out, BlockPos& logPos, int currentHeight, Direction dir, int branchPos, int steps);
    // RootPlacer
    bool simulateRoots(BlockPos rootPos, Direction dir, BlockPos rootOrigin, std::vector<BlockPos>& out, int layer);
    bool canPlaceRoot(BlockPos p);
    void placeRoot(BlockPos p);
    BlockStateId waterlogged(BlockPos p, BlockStateId s) {
        return bs::hasProperty(s, "waterlogged") ? bs::setBool(s, "waterlogged", (bs::flags(stateAt(p)) & bs::kWater) != 0) : s;
    }
    int getMaxFreeTreeHeight(int maxTreeHeight, BlockPos treePos);
    int sizeAtHeight(int treeHeight, int yo);
    void updateLeavesAndEdges();
};

// ---- trunk placers

std::vector<Attachment> Tree::placeTrunk(int treeHeight, BlockPos origin) {
    std::string_view type = trunkType();
    const Value& t = trunkPlacer();
    if (type == "minecraft:straight_trunk_placer") {
        placeBelowTrunkBlock(origin.below());
        for (int y = 0; y < treeHeight; y++) placeLog(origin.above(y));
        return {attachment(origin.above(treeHeight), 0, false)};
    }
    if (type == "minecraft:forking_trunk_placer") {
        placeBelowTrunkBlock(origin.below());
        std::vector<Attachment> out;
        Direction lean = randomHorizontal(random);
        int leanHeight = treeHeight - random.nextInt(4) - 1;
        int leanSteps = 3 - random.nextInt(3);
        int tx = origin.x, tz = origin.z;
        std::optional<int> ey;
        for (int yo = 0; yo < treeHeight; yo++) {
            int yy = origin.y + yo;
            if (yo >= leanHeight && leanSteps > 0) {
                tx += direction::stepX(lean), tz += direction::stepZ(lean);
                leanSteps--;
            }
            if (placeLog({tx, yy, tz})) ey = yy + 1;
        }
        if (ey) out.push_back(attachment({tx, *ey, tz}, 1, false));
        tx = origin.x, tz = origin.z;
        Direction branch = randomHorizontal(random);
        if (branch != lean) {
            int branchPos = leanHeight - random.nextInt(2) - 1;
            int branchSteps = 1 + random.nextInt(3);
            ey.reset();
            for (int yo = branchPos; yo < treeHeight && branchSteps > 0; branchSteps--) {
                if (yo >= 1) {
                    int yy = origin.y + yo;
                    tx += direction::stepX(branch), tz += direction::stepZ(branch);
                    if (placeLog({tx, yy, tz})) ey = yy + 1;
                }
                yo++;
            }
            if (ey) out.push_back(attachment({tx, *ey, tz}, 0, false));
        }
        return out;
    }
    if (type == "minecraft:giant_trunk_placer" || type == "minecraft:mega_jungle_trunk_placer") {
        BlockPos below = origin.below();
        placeBelowTrunkBlock(below);
        placeBelowTrunkBlock(below.east());
        placeBelowTrunkBlock(below.south());
        placeBelowTrunkBlock(below.south().east());
        for (int hh = 0; hh < treeHeight; hh++) {
            placeLogIfFree(origin.offset(0, hh, 0));
            if (hh < treeHeight - 1) {
                placeLogIfFree(origin.offset(1, hh, 0));
                placeLogIfFree(origin.offset(1, hh, 1));
                placeLogIfFree(origin.offset(0, hh, 1));
            }
        }
        std::vector<Attachment> out{attachment(origin.above(treeHeight), 0, true)};
        if (type == "minecraft:mega_jungle_trunk_placer") {
            for (int branchHeight = treeHeight - 2 - random.nextInt(4); branchHeight > treeHeight / 2; branchHeight -= 2 + random.nextInt(4)) {
                float angle = random.nextFloat() * 6.2831855f;
                int bx = 0, bz = 0;
                for (int b = 0; b < 5; b++) {
                    bx = static_cast<int>(1.5f + mthCos(angle) * b);
                    bz = static_cast<int>(1.5f + mthSin(angle) * b);
                    placeLog(origin.offset(bx, branchHeight - 3 + b / 2, bz));
                }
                out.push_back(attachment(origin.offset(bx, branchHeight, bz), -2, false));
            }
        }
        return out;
    }
    if (type == "minecraft:dark_oak_trunk_placer") {
        std::vector<Attachment> out;
        BlockPos below = origin.below();
        placeBelowTrunkBlock(below);
        placeBelowTrunkBlock(below.east());
        placeBelowTrunkBlock(below.south());
        placeBelowTrunkBlock(below.south().east());
        Direction lean = randomHorizontal(random);
        int leanHeight = treeHeight - random.nextInt(4);
        int leanSteps = 2 - random.nextInt(3);
        int x = origin.x, y = origin.y, z = origin.z, tx = x, tz = z;
        int ey = y + treeHeight - 1;
        for (int dy = 0; dy < treeHeight; dy++) {
            if (dy >= leanHeight && leanSteps > 0) {
                tx += direction::stepX(lean), tz += direction::stepZ(lean);
                leanSteps--;
            }
            BlockPos p{tx, y + dy, tz};
            if (isAirOrLeaves(p)) {
                placeLog(p);
                placeLog(p.east());
                placeLog(p.south());
                placeLog(p.east().south());
            }
        }
        out.push_back(attachment({tx, ey, tz}, 0, true));
        for (int ox = -1; ox <= 2; ox++)
            for (int oz = -1; oz <= 2; oz++)
                if ((ox < 0 || ox > 1 || oz < 0 || oz > 1) && random.nextInt(3) <= 0) {
                    int length = random.nextInt(3) + 2;
                    for (int by = 0; by < length; by++) placeLog({x + ox, ey - by - 1, z + oz});
                    out.push_back(attachment({x + ox, ey, z + oz}, 0, false));
                }
        return out;
    }
    if (type == "minecraft:bending_trunk_placer") {
        Direction dir = randomHorizontal(random);
        int logHeight = treeHeight - 1;
        BlockPos pos = origin;
        placeBelowTrunkBlock(pos.below());
        std::vector<Attachment> out;
        int minHeightForLeaves = intOr(t, "min_height_for_leaves", 1);
        for (int i = 0; i <= logHeight; i++) {
            if (i + 1 >= logHeight + random.nextInt(2)) pos = pos.relative(dir);
            if (validTreePos(pos)) placeLog(pos);
            if (i >= minHeightForLeaves) out.push_back(attachment(pos, 0, false));
            pos = pos.above();
        }
        int dirLength = sampleInt(t["bend_length"], random);
        for (int i = 0; i <= dirLength; i++) {
            if (validTreePos(pos)) placeLog(pos);
            out.push_back(attachment(pos, 0, false));
            pos = pos.relative(dir);
        }
        return out;
    }
    if (type == "minecraft:fancy_trunk_placer") return fancyTrunk(treeHeight, origin);
    if (type == "minecraft:cherry_trunk_placer") {
        placeBelowTrunkBlock(origin.below());
        const Value& start = t["branch_start_offset_from_top"];
        int startLo = toInt(start["min_inclusive"]), startHi = toInt(start["max_inclusive"]);
        int first = std::max(0, treeHeight - 1 + (random.nextInt(startHi - startLo + 1) + startLo));
        int second = std::max(0, treeHeight - 1 + (random.nextInt(startHi - 1 - startLo + 1) + startLo));
        if (second >= first) second++;
        int branchCount = sampleInt(t["branch_count"], random);
        bool middle = branchCount == 3, both = branchCount >= 2;
        int trunkHeight = middle ? treeHeight : both ? std::max(first, second) + 1 : first + 1;
        for (int y = 0; y < trunkHeight; y++) placeLog(origin.above(y));
        std::vector<Attachment> out;
        if (middle) out.push_back(attachment(origin.above(trunkHeight), 0, false));
        Direction dir = randomHorizontal(random);
        Axis sideways = direction::axis(dir);
        out.push_back(cherryBranch(treeHeight, origin, dir, first, first < trunkHeight - 1, sideways));
        if (both) out.push_back(cherryBranch(treeHeight, origin, direction::opposite(dir), second, second < trunkHeight - 1, sideways));
        return out;
    }
    if (type == "minecraft:upwards_branching_trunk_placer") {
        std::vector<Attachment> out;
        float perLog = static_cast<float>(t["place_branch_per_log_probability"].number());
        BlockPos logPos;
        for (int h = 0; h < treeHeight; h++) {
            int currentHeight = origin.y + h;
            logPos = {origin.x, currentHeight, origin.z};
            if (placeLog(logPos) && h < treeHeight - 1 && random.nextFloat() < perLog) {
                Direction dir = randomHorizontal(random);
                int branchLen = sampleInt(t["extra_branch_length"], random);
                int branchPos = std::max(0, branchLen - sampleInt(t["extra_branch_length"], random) - 1);
                int steps = sampleInt(t["extra_branch_steps"], random);
                upwardsBranch(treeHeight, out, logPos, currentHeight, dir, branchPos, steps);
            }
            if (h == treeHeight - 1) {
                logPos = {origin.x, currentHeight + 1, origin.z};
                out.push_back(attachment(logPos, 0, false));
            }
        }
        return out;
    }
    if (type == "minecraft:poplar_trunk_placer") {
        placeBelowTrunkBlock(origin.below());
        int upToBranches = treeHeight - sampleInt(t["trunk_height_above_branches"], random);
        for (int y = 0; y < treeHeight; y++) {
            placeLog(origin.above(y));
            std::array<Direction, 6> all = direction::kAll; // Direction.allShuffled: Util.shuffle of values()
            for (int i = 6; i > 1; i--) std::swap(all[i - 1], all[random.nextInt(i)]);
            std::vector<Direction> dirs;
            for (Direction d : all)
                if (direction::isHorizontal(d)) dirs.push_back(d);
            if (upToBranches - 1 == y) {
                int branches = sampleInt(t["branch_amount"], random);
                for (int x = 0; x < branches; x++) {
                    Direction d = dirs[x];
                    placeLog(origin.above(y).relative(d), [d](BlockStateId s) { return withAxis(s, direction::axis(d)); });
                }
            }
        }
        return {attachment(origin.above(upToBranches), 0, false)};
    }
    log::warn("unsupported trunk placer {}", type);
    return {};
}

std::vector<Attachment> Tree::fancyTrunk(int treeHeight, BlockPos origin) {
    int height = treeHeight + 2;
    int trunkHeight = floorInt(height * 0.618);
    placeBelowTrunkBlock(origin.below());
    int clustersPerY = std::min(1, floorInt(1.382 + std::pow(1.0 * height / 13.0, 2.0)));
    int trunkTop = origin.y + trunkHeight;
    int relativeY = height - 5;
    struct Coords {
        Attachment a;
        int branchBase;
    };
    std::vector<Coords> coords{{attachment(origin.above(relativeY), 0, false), trunkTop}};
    auto treeShape = [](int h, int y) -> float {
        if (y < h * 0.3f) return -1.0f;
        float radius = h / 2.0f;
        float adjacent = radius - y;
        float distance = static_cast<float>(std::sqrt(radius * radius - adjacent * adjacent));
        if (adjacent == 0.0f) distance = radius;
        else if (std::abs(adjacent) >= radius) return 0.0f;
        return distance * 0.5f;
    };
    for (; relativeY >= 0; relativeY--) {
        float shape = treeShape(height, relativeY);
        if (shape < 0.0f) continue;
        for (int i = 0; i < clustersPerY; i++) {
            double radius = 1.0 * shape * (random.nextFloat() + 0.328);
            double angle = static_cast<double>(random.nextFloat() * 2.0f) * 3.141592653589793;
            double x = radius * std::sin(angle) + 0.5;
            double z = radius * std::cos(angle) + 0.5;
            BlockPos checkStart = origin.offset(floorInt(x), relativeY - 1, floorInt(z));
            BlockPos checkEnd = checkStart.above(5);
            if (makeLimb(checkStart, checkEnd, false)) {
                int dx = origin.x - checkStart.x, dz = origin.z - checkStart.z;
                double branchHeight = checkStart.y - std::sqrt(static_cast<double>(dx * dx + dz * dz)) * 0.381;
                int branchTop = branchHeight > trunkTop ? trunkTop : static_cast<int>(branchHeight);
                BlockPos base{origin.x, branchTop, origin.z};
                if (makeLimb(base, checkStart, false)) coords.push_back({attachment(checkStart, 0, false), base.y});
            }
        }
    }
    makeLimb(origin, origin.above(trunkHeight), true);
    auto trim = [&](int localY) { return localY >= height * 0.2; };
    for (const auto& c : coords) { // makeBranches
        BlockPos base{origin.x, c.branchBase, origin.z};
        if (!(base == c.a.pos) && trim(c.branchBase - origin.y)) makeLimb(base, c.a.pos, true);
    }
    std::vector<Attachment> out;
    for (const auto& c : coords)
        if (trim(c.branchBase - origin.y)) out.push_back(c.a);
    return out;
}

bool Tree::makeLimb(BlockPos start, BlockPos end, bool doPlace) {
    if (!doPlace && start == end) return true;
    int ddx = end.x - start.x, ddy = end.y - start.y, ddz = end.z - start.z;
    int steps = std::max(std::abs(ddx), std::max(std::abs(ddy), std::abs(ddz)));
    float dx = static_cast<float>(ddx) / steps, dy = static_cast<float>(ddy) / steps, dz = static_cast<float>(ddz) / steps;
    for (int i = 0; i <= steps; i++) {
        BlockPos p = start.offset(floorInt(0.5f + i * dx), floorInt(0.5f + i * dy), floorInt(0.5f + i * dz));
        if (doPlace) {
            int xd = std::abs(p.x - start.x), zd = std::abs(p.z - start.z), maxd = std::max(xd, zd);
            Axis axis = maxd > 0 ? (xd == maxd ? Axis::X : Axis::Z) : Axis::Y; // getLogAxis
            placeLog(p, [axis](BlockStateId s) { return withAxis(s, axis); });
        } else if (!isFree(p)) {
            return false;
        }
    }
    return true;
}

Attachment Tree::cherryBranch(int treeHeight, BlockPos origin, Direction dir, int offsetFromOrigin, bool middleContinuesUpwards, Axis sideways) {
    const Value& t = trunkPlacer();
    auto side = [sideways](BlockStateId s) { return withAxis(s, sideways); };
    BlockPos logPos = origin.above(offsetFromOrigin);
    int endOffset = treeHeight - 1 + sampleInt(t["branch_end_offset_from_top"], random);
    bool extend = middleContinuesUpwards || endOffset < offsetFromOrigin;
    int distanceToTrunk = sampleInt(t["branch_horizontal_length"], random) + (extend ? 1 : 0);
    BlockPos end = origin.relative(dir, distanceToTrunk).above(endOffset);
    int horizontalSteps = extend ? 2 : 1;
    for (int i = 0; i < horizontalSteps; i++) {
        logPos = logPos.relative(dir);
        placeLog(logPos, side);
    }
    Direction vertical = end.y > logPos.y ? Direction::Up : Direction::Down;
    while (true) {
        int distance = std::abs(logPos.x - end.x) + std::abs(logPos.y - end.y) + std::abs(logPos.z - end.z);
        if (distance == 0) return attachment(end.above(), 0, false);
        float chance = static_cast<float>(std::abs(end.y - logPos.y)) / distance;
        bool growVertically = random.nextFloat() < chance;
        logPos = logPos.relative(growVertically ? vertical : dir);
        if (growVertically) placeLog(logPos);
        else placeLog(logPos, side);
    }
}

void Tree::upwardsBranch(int treeHeight, std::vector<Attachment>& out, BlockPos& logPos, int currentHeight, Direction dir, int branchPos,
                         int steps) {
    int heightAlong = currentHeight + branchPos;
    int logX = logPos.x, logZ = logPos.z;
    for (int index = branchPos; index < treeHeight && steps > 0; index++, steps--) {
        if (index >= 1) {
            int placementHeight = currentHeight + index;
            logX += direction::stepX(dir), logZ += direction::stepZ(dir);
            heightAlong = placementHeight;
            logPos = {logX, placementHeight, logZ};
            if (placeLog(logPos)) heightAlong = placementHeight + 1;
            out.push_back(attachment(logPos, 0, false));
        }
    }
    if (heightAlong - currentHeight > 1) {
        BlockPos foliagePos{logX, heightAlong, logZ};
        out.push_back(attachment(foliagePos, 0, false));
        out.push_back(attachment(foliagePos.below(2), 0, false));
    }
}

// ---- foliage placers

int Tree::foliageHeight(int treeHeight) {
    std::string_view type = foliageType();
    const Value& f = foliagePlacer();
    if (type == "minecraft:blob_foliage_placer" || type == "minecraft:fancy_foliage_placer" || type == "minecraft:jungle_foliage_placer")
        return toInt(f["height"]);
    if (type == "minecraft:spruce_foliage_placer") return std::max(4, treeHeight - sampleInt(f["trunk_height"], random));
    if (type == "minecraft:acacia_foliage_placer") return 0;
    if (type == "minecraft:mega_pine_foliage_placer") return sampleInt(f["crown_height"], random);
    if (type == "minecraft:dark_oak_foliage_placer") return 4;
    if (type == "minecraft:random_spread_foliage_placer") return sampleInt(f["foliage_height"], random);
    if (type == "minecraft:cherry_foliage_placer" || type == "minecraft:poplar_foliage_placer") return sampleInt(f["height"], random);
    log::warn("unsupported foliage placer {}", type);
    return 0;
}

bool Tree::shouldSkipLocation(int dx, int y, int dz, int r, bool doubleTrunk) {
    std::string_view type = foliageType();
    const Value& f = foliagePlacer();
    if (type == "minecraft:blob_foliage_placer") return dx == r && dz == r && (random.nextInt(2) == 0 || y == 0);
    if (type == "minecraft:fancy_foliage_placer") return (dx + 0.5f) * (dx + 0.5f) + (dz + 0.5f) * (dz + 0.5f) > static_cast<float>(r * r);
    if (type == "minecraft:spruce_foliage_placer") return dx == r && dz == r && r > 0;
    if (type == "minecraft:acacia_foliage_placer") return y == 0 ? (dx > 1 || dz > 1) && dx != 0 && dz != 0 : dx == r && dz == r && r > 0;
    if (type == "minecraft:mega_pine_foliage_placer" || type == "minecraft:jungle_foliage_placer") return dx + dz >= 7 ? true : dx * dx + dz * dz > r * r;
    if (type == "minecraft:dark_oak_foliage_placer") {
        if (y == -1 && !doubleTrunk) return dx == r && dz == r;
        return y == 1 ? dx + dz > r * 2 - 2 : false;
    }
    if (type == "minecraft:cherry_foliage_placer") {
        float wideHole = static_cast<float>(f["wide_bottom_layer_hole_chance"].number());
        float cornerHole = static_cast<float>(f["corner_hole_chance"].number());
        if (y == -1 && (dx == r || dz == r) && random.nextFloat() < wideHole) return true;
        bool corner = dx == r && dz == r;
        bool wide = r > 2;
        return wide ? corner || (dx + dz > r * 2 - 2 && random.nextFloat() < cornerHole) : corner && random.nextFloat() < cornerHole;
    }
    return false; // random spread
}

bool Tree::shouldSkipLocationSigned(int dx, int y, int dz, int r, bool doubleTrunk) {
    if (foliageType() == "minecraft:dark_oak_foliage_placer" && y == 0 && doubleTrunk && (dx == -r || dx >= r) && (dz == -r || dz >= r)) return true;
    int minDx, minDz;
    if (doubleTrunk) {
        minDx = std::min(std::abs(dx), std::abs(dx - 1));
        minDz = std::min(std::abs(dz), std::abs(dz - 1));
    } else {
        minDx = std::abs(dx);
        minDz = std::abs(dz);
    }
    return shouldSkipLocation(minDx, y, minDz, r, doubleTrunk);
}

bool Tree::tryPlaceLeaf(BlockPos p) {
    BlockStateId there = stateAt(p);
    if (bs::getBool(there, "persistent") || !validTreePos(p)) return false;
    BlockStateId s = getState(cfg["foliage_provider"], level, random, p);
    if (bs::hasProperty(s, "waterlogged")) s = bs::setBool(s, "waterlogged", bs::isWaterSource(stateAt(p)));
    setFoliage(p, s);
    return true;
}

void Tree::placeLeavesRow(BlockPos origin, int r, int y, bool doubleTrunk) {
    int offset = doubleTrunk ? 1 : 0;
    for (int dx = -r; dx <= r + offset; dx++)
        for (int dz = -r; dz <= r + offset; dz++)
            if (!shouldSkipLocationSigned(dx, y, dz, r, doubleTrunk)) tryPlaceLeaf(origin.offset(dx, y, dz));
}

bool Tree::tryPlaceExtension(float chance, BlockPos logPos, BlockPos p) {
    if (std::abs(p.x - logPos.x) + std::abs(p.y - logPos.y) + std::abs(p.z - logPos.z) >= 7) return false;
    return random.nextFloat() > chance ? false : tryPlaceLeaf(p);
}

void Tree::placeLeavesRowWithHangingLeavesBelow(BlockPos origin, int r, int y, bool doubleTrunk, float chance, float extensionChance) {
    placeLeavesRow(origin, r, y, doubleTrunk);
    int offset = doubleTrunk ? 1 : 0;
    BlockPos logPos = origin.below();
    for (Direction along : kPlane) {
        Direction toEdge = direction::clockWise(along);
        bool positive = toEdge == Direction::East || toEdge == Direction::South;
        int offsetToEdge = positive ? r + offset : r;
        BlockPos pos = origin.offset(0, y - 1, 0).relative(toEdge, offsetToEdge).relative(along, -r);
        for (int alongEdge = -r; alongEdge < r + offset; alongEdge++) {
            bool leavesAbove = foliage.contains(pos.above());
            if (leavesAbove && tryPlaceExtension(chance, logPos, pos)) tryPlaceExtension(extensionChance, logPos, pos.below());
            pos = pos.relative(along);
        }
    }
}

void Tree::poplarLeavesRow(BlockPos origin, int r, int y, bool doubleTrunk, int foliageHeight, bool flip) {
    float sideHoleChance = static_cast<float>(foliagePlacer()["side_hole_chance"].number());
    int offset = doubleTrunk ? 1 : 0;
    bool partial = foliageHeight - 1 == y || foliageHeight - 2 == y;
    for (int dx = -r; dx <= r + offset; dx++)
        for (int dz = -r; dz <= r + offset; dz++) {
            bool smallCorner = flip ? (dx > 0 && dz > 0) || (dz < 0 && dx < 0) : (dx > 0 && dz < 0) || (dz > 0 && dx < 0);
            int cut = smallCorner ? r - 1 : (partial ? r + 1 : r);
            int adx = std::abs(dx), adz = std::abs(dz);
            bool skip;
            if (partial && (adx == r || adz == r)) {
                skip = true;
            } else {
                int extra = random.nextFloat() <= sideHoleChance ? 1 : 0;
                skip = !(adx + adz <= r * 2 - (cut + extra));
            }
            if (!skip) tryPlaceLeaf(origin.offset(dx, y, dz));
        }
}

void Tree::createFoliage(int treeHeight, const Attachment& a, int foliageHeight, int leafRadius) {
    std::string_view type = foliageType();
    const Value& f = foliagePlacer();
    int offset = sampleInt(f["offset"], random); // FoliagePlacer.createFoliage samples the offset first
    bool doubleTrunk = a.doubleTrunk();
    if (type == "minecraft:blob_foliage_placer") {
        int withOffset = foliageHeight + a.foliageHeightOffset;
        for (int yo = offset; yo >= offset - withOffset; yo--) {
            int r = std::max(leafRadius + a.radiusOffsetXZ - 1 - yo / 2, 0);
            placeLeavesRow(a.pos, r, yo, doubleTrunk);
        }
    } else if (type == "minecraft:fancy_foliage_placer") {
        for (int yo = offset; yo >= offset - foliageHeight; yo--) {
            int r = leafRadius + (yo != offset && yo != offset - foliageHeight ? 1 : 0);
            placeLeavesRow(a.pos, r, yo, doubleTrunk);
        }
    } else if (type == "minecraft:spruce_foliage_placer") {
        int r = random.nextInt(2), maxR = 1, minR = 0;
        int withOffset = foliageHeight + a.foliageHeightOffset;
        for (int yo = offset; yo >= -withOffset; yo--) {
            placeLeavesRow(a.pos, r, yo, doubleTrunk);
            if (r >= maxR) {
                r = minR;
                minR = 1;
                maxR = std::min(maxR + 1, leafRadius + a.radiusOffsetXZ);
            } else {
                r++;
            }
        }
    } else if (type == "minecraft:acacia_foliage_placer") {
        BlockPos pos = a.pos.above(offset);
        int withOffset = foliageHeight + a.foliageHeightOffset;
        placeLeavesRow(pos, leafRadius + a.radiusOffsetXZ, -1 - withOffset, doubleTrunk);
        placeLeavesRow(pos, leafRadius - 1, -withOffset, doubleTrunk);
        placeLeavesRow(pos, leafRadius + a.radiusOffsetXZ - 1, 0, doubleTrunk);
    } else if (type == "minecraft:jungle_foliage_placer") {
        int leafHeight = (doubleTrunk ? foliageHeight : 1 + random.nextInt(2)) + a.foliageHeightOffset;
        for (int yo = offset; yo >= offset - leafHeight; yo--) placeLeavesRow(a.pos, leafRadius + a.radiusOffsetXZ + 1 - yo, yo, doubleTrunk);
    } else if (type == "minecraft:mega_pine_foliage_placer") {
        int prevRadius = 0;
        int withOffset = foliageHeight + a.foliageHeightOffset;
        for (int yy = a.pos.y - withOffset + offset; yy <= a.pos.y + offset; yy++) {
            int yo = a.pos.y - yy;
            int smooth = leafRadius + a.radiusOffsetXZ + floorInt(static_cast<float>(yo) / withOffset * 3.5f);
            int jagged = yo > 0 && smooth == prevRadius && (yy & 1) == 0 ? smooth + 1 : smooth;
            placeLeavesRow({a.pos.x, yy, a.pos.z}, jagged, 0, doubleTrunk);
            prevRadius = smooth;
        }
    } else if (type == "minecraft:dark_oak_foliage_placer") {
        BlockPos pos = a.pos.above(offset);
        if (doubleTrunk) {
            placeLeavesRow(pos, leafRadius + 2, -1, true);
            placeLeavesRow(pos, leafRadius + 3, 0, true);
            placeLeavesRow(pos, leafRadius + 2, 1, true);
            if (random.nextBoolean()) placeLeavesRow(pos, leafRadius, 2, true);
        } else {
            placeLeavesRow(pos, leafRadius + 2, -1, false);
            placeLeavesRow(pos, leafRadius + 1, 0, false);
        }
    } else if (type == "minecraft:random_spread_foliage_placer") {
        int attempts = toInt(f["leaf_placement_attempts"]);
        for (int i = 0; i < attempts; i++) {
            int dx = random.nextInt(leafRadius);
            dx -= random.nextInt(leafRadius);
            int dy = random.nextInt(foliageHeight);
            dy -= random.nextInt(foliageHeight);
            int dz = random.nextInt(leafRadius);
            dz -= random.nextInt(leafRadius);
            tryPlaceLeaf(a.pos.offset(dx, dy, dz));
        }
    } else if (type == "minecraft:cherry_foliage_placer") {
        BlockPos pos = a.pos.above(offset);
        int r = leafRadius + a.radiusOffsetXZ - 1;
        int withOffset = foliageHeight + a.foliageHeightOffset;
        float hanging = static_cast<float>(f["hanging_leaves_chance"].number());
        float extension = static_cast<float>(f["hanging_leaves_extension_chance"].number());
        placeLeavesRow(pos, r - 2, withOffset - 3, doubleTrunk);
        placeLeavesRow(pos, r - 1, withOffset - 4, doubleTrunk);
        for (int y = withOffset - 5; y >= 0; y--) placeLeavesRow(pos, r, y, doubleTrunk);
        placeLeavesRowWithHangingLeavesBelow(pos, r, -1, doubleTrunk, hanging, extension);
        placeLeavesRowWithHangingLeavesBelow(pos, r - 1, -2, doubleTrunk, hanging, extension);
    } else if (type == "minecraft:poplar_foliage_placer") {
        BlockPos pos = a.pos.above(offset);
        int r = leafRadius + a.radiusOffsetXZ - 1;
        bool flip = random.nextBoolean();
        int h = foliageHeight + a.foliageHeightOffset;
        poplarLeavesRow(pos, r - 2, h - 1, doubleTrunk, h, flip);
        poplarLeavesRow(pos, r - 1, h - 2, doubleTrunk, h, flip);
        poplarLeavesRow(pos, r - 1, h - 3, doubleTrunk, h, flip);
        for (int y = h - 4; y >= 1; y--) poplarLeavesRow(pos, r, y, doubleTrunk, h, flip);
        { // replaceLeavesWithLog
            int y = h - 4;
            int off = doubleTrunk ? 1 : 0;
            bool partial = h - 1 == y || h - 2 == y;
            for (int dx = -r; dx <= r + off; dx++)
                for (int dz = -r; dz <= r + off; dz++) {
                    int adz = std::abs(dz), adx = std::abs(dx);
                    bool smallCorner = flip ? (dx > 0 && dz > 0) || (dz < 0 && dx < 0) : (dx > 0 && dz < 0) || (dz > 0 && dx < 0);
                    int cut = smallCorner ? r - 1 : (partial ? r + 1 : r);
                    if (adx + adz <= r * 2 - (cut + 2) && ((adz == 0 && r - adx >= 4) || (adx == 0 && r - adz >= 4))) {
                        BlockPos p = pos.offset(dx, y, dz);
                        Axis axis = adz == 0 ? Axis::X : Axis::Z;
                        // tryPlaceLog: only over the tree's own leaves
                        if (stateAt(p) == getState(cfg["foliage_provider"], level, random, p))
                            setFoliage(p, withAxis(getState(cfg["trunk_provider"], level, random, p), axis));
                    }
                }
        }
        poplarLeavesRow(pos, r - 1, 0, doubleTrunk, h, flip);
        poplarLeavesRow(pos, std::clamp(r - 2, 1, 2), -1, doubleTrunk, h, flip);
    }
}

// ---- roots (MangroveRootPlacer)

bool Tree::canPlaceRoot(BlockPos p) {
    if (validTreePos(p)) return true;
    return levelgen::matchesBlocks(cfg["root_placer"]["mangrove_root_placement"]["can_grow_through"], bs::blockOf(stateAt(p)));
}

void Tree::placeRoot(BlockPos p) {
    const Value& rp = cfg["root_placer"];
    const Value& placement = rp["mangrove_root_placement"];
    if (levelgen::matchesBlocks(placement["muddy_roots_in"], bs::blockOf(stateAt(p)))) {
        setRoot(p, waterlogged(p, getState(placement["muddy_roots_provider"], level, random, p)));
        return;
    }
    if (!canPlaceRoot(p)) return; // RootPlacer.placeRoot
    setRoot(p, waterlogged(p, getState(rp["root_provider"], level, random, p)));
    if (rp.has("above_root_placement")) {
        const Value& above = rp["above_root_placement"];
        BlockPos up = p.above();
        if (random.nextFloat() < static_cast<float>(above["above_root_placement_chance"].number()) && bs::isAir(stateAt(up)))
            setRoot(up, waterlogged(up, getState(above["above_root_provider"], level, random, up)));
    }
}

bool Tree::simulateRoots(BlockPos rootPos, Direction dir, BlockPos rootOrigin, std::vector<BlockPos>& out, int layer) {
    const Value& placement = cfg["root_placer"]["mangrove_root_placement"];
    int maxLength = toInt(placement["max_root_length"]);
    if (layer == maxLength || static_cast<int>(out.size()) > maxLength) return false;
    // potentialRootPositions
    BlockPos below = rootPos.below(), nextTo = rootPos.relative(dir);
    int width = std::abs(rootPos.x - rootOrigin.x) + std::abs(rootPos.y - rootOrigin.y) + std::abs(rootPos.z - rootOrigin.z);
    int maxWidth = toInt(placement["max_root_width"]);
    float skew = static_cast<float>(placement["random_skew_chance"].number());
    std::vector<BlockPos> candidates;
    if (width > maxWidth - 3 && width <= maxWidth) {
        candidates = random.nextFloat() < skew ? std::vector<BlockPos>{below, nextTo.below()} : std::vector<BlockPos>{below};
    } else if (width > maxWidth) {
        candidates = {below};
    } else if (random.nextFloat() < skew) {
        candidates = {below};
    } else {
        candidates = {random.nextBoolean() ? nextTo : below};
    }
    for (BlockPos p : candidates) {
        if (!canPlaceRoot(p)) continue;
        out.push_back(p);
        if (!simulateRoots(p, dir, rootOrigin, out, layer + 1)) return false;
    }
    return true;
}

bool Tree::placeRoots(BlockPos origin, BlockPos trunkOrigin) {
    for (BlockPos column = origin; column.y < trunkOrigin.y; column = column.above())
        if (!canPlaceRoot(column)) return false;
    std::vector<BlockPos> positions{trunkOrigin.below()};
    for (Direction dir : kPlane) {
        std::vector<BlockPos> inDirection;
        if (!simulateRoots(trunkOrigin.relative(dir), dir, trunkOrigin, inDirection, 0)) return false;
        positions.insert(positions.end(), inDirection.begin(), inDirection.end());
        positions.push_back(trunkOrigin.relative(dir));
    }
    for (BlockPos p : positions) placeRoot(p);
    return true;
}

// ---- TreeFeature.doPlace / place

int Tree::sizeAtHeight(int treeHeight, int yo) {
    const Value& s = cfg["minimum_size"];
    if (s["type"].string() == "minecraft:three_layers_feature_size") {
        if (yo < intOr(s, "limit", 1)) return intOr(s, "lower_size", 0);
        return yo >= treeHeight - intOr(s, "upper_limit", 1) ? intOr(s, "upper_size", 1) : intOr(s, "middle_size", 1);
    }
    return yo < intOr(s, "limit", 1) ? intOr(s, "lower_size", 0) : intOr(s, "upper_size", 1);
}

int Tree::getMaxFreeTreeHeight(int maxTreeHeight, BlockPos treePos) {
    bool ignoreVines = cfg["ignore_vines"].boolean(false);
    for (int y = 0; y <= maxTreeHeight + 1; y++) {
        int r = sizeAtHeight(maxTreeHeight, y);
        for (int x = -r; x <= r; x++)
            for (int z = -r; z <= r; z++) {
                BlockPos p = treePos.offset(x, y, z);
                if (!isFree(p) || (!ignoreVines && bs::is(stateAt(p), VINE))) return y - 2;
            }
    }
    return maxTreeHeight;
}

// TreeDecorator.Context: the sets as lists, stably sorted by y.
std::vector<BlockPos> sortedByY(const JavaBlockPosSet& set) {
    std::vector<BlockPos> v = set.ordered();
    std::stable_sort(v.begin(), v.end(), [](BlockPos a, BlockPos b) { return a.y < b.y; });
    return v;
}

void Tree::decorate() {
    std::vector<BlockPos> logs = sortedByY(trunks), leaves = sortedByY(foliage), rootList = sortedByY(roots);
    auto isAir = [&](BlockPos p) { return bs::isAir(stateAt(p)); };
    auto placeVine = [&](BlockPos p, Direction face) { setDecoration(p, bs::setBool(bs::defaultState(VINE), direction::name(face), true)); };
    for (const auto& d : cfg["decorators"].array()) {
        std::string_view type = d["type"].string();
        if (type == "minecraft:trunk_vine") {
            for (BlockPos p : logs) {
                if (random.nextInt(3) > 0 && isAir(p.west())) placeVine(p.west(), Direction::East);
                if (random.nextInt(3) > 0 && isAir(p.east())) placeVine(p.east(), Direction::West);
                if (random.nextInt(3) > 0 && isAir(p.north())) placeVine(p.north(), Direction::South);
                if (random.nextInt(3) > 0 && isAir(p.south())) placeVine(p.south(), Direction::North);
            }
        } else if (type == "minecraft:leave_vine") {
            float probability = static_cast<float>(d["probability"].number());
            auto hanging = [&](BlockPos p, Direction face) {
                placeVine(p, face);
                int maxDir = 4;
                for (BlockPos q = p.below(); isAir(q) && maxDir > 0; maxDir--) {
                    placeVine(q, face);
                    q = q.below();
                }
            };
            for (BlockPos p : leaves) {
                if (random.nextFloat() < probability && isAir(p.west())) hanging(p.west(), Direction::East);
                if (random.nextFloat() < probability && isAir(p.east())) hanging(p.east(), Direction::West);
                if (random.nextFloat() < probability && isAir(p.north())) hanging(p.north(), Direction::South);
                if (random.nextFloat() < probability && isAir(p.south())) hanging(p.south(), Direction::North);
            }
        } else if (type == "minecraft:beehive") {
            if (logs.empty()) continue;
            if (random.nextFloat() >= static_cast<float>(d["probability"].number())) continue;
            int hiveY = !leaves.empty() ? std::max(leaves.front().y - 1, logs.front().y + 1)
                                        : std::min(logs.front().y + 1 + random.nextInt(3), logs.back().y);
            std::vector<BlockPos> placements; // SPAWN_DIRECTIONS: the horizontal ones but north (behind a south-facing nest)
            for (BlockPos p : logs)
                if (p.y == hiveY)
                    for (Direction dir : {Direction::East, Direction::South, Direction::West}) placements.push_back(p.relative(dir));
            if (placements.empty()) continue;
            for (size_t i = placements.size(); i > 1; i--) std::swap(placements[i - 1], placements[random.nextInt(static_cast<int>(i))]); // Util.shuffle
            for (BlockPos p : placements) {
                if (!isAir(p) || !isAir(p.south())) continue;
                setDecoration(p, bs::setValue(bs::defaultState(BEE_NEST), "facing", "south"));
                LevelChunk& chunk = level.getChunk({p.x >> 4, p.z >> 4});
                if (auto* be = chunk.blockEntityAt(p.x, p.y, p.z)) { // BeehiveBlockEntity.storeBee(Occupant.create(...))
                    int bees = 2 + random.nextInt(2);
                    nbt::List list{nbt::TagType::Compound, {}};
                    if (const nbt::List* old = be->tag.getList("bees")) list = *old;
                    for (int i = 0; i < bees; i++) {
                        nbt::Compound occupant, entity;
                        entity.put("id", std::string("minecraft:bee"));
                        occupant.put("entity_data", std::move(entity));
                        occupant.put("ticks_in_hive", random.nextInt(599));
                        occupant.put("min_ticks_in_hive", 600);
                        list.items.push_back(std::move(occupant));
                    }
                    be->tag.put("bees", std::move(list));
                    chunk.dirty = true;
                }
                break;
            }
        } else if (type == "minecraft:alter_ground") {
            // TreeFeature.getLowestTrunkOrRootOfTree
            std::vector<BlockPos> base;
            if (rootList.empty()) base = logs;
            else if (!logs.empty() && rootList.front().y == logs.front().y) base = logs, base.insert(base.end(), rootList.begin(), rootList.end());
            else base = rootList;
            if (base.empty()) continue;
            int minY = base.front().y;
            auto placeBlockAt = [&](BlockPos p) {
                for (int dy = 2; dy >= -3; dy--) {
                    BlockPos cursor = p.above(dy);
                    if (auto s = optionalState(d["provider"], level, random, cursor)) {
                        setDecoration(cursor, *s);
                        break;
                    }
                    if (!isAir(cursor) && dy < 0) break;
                }
            };
            auto placeCircle = [&](BlockPos p) {
                for (int xx = -2; xx <= 2; xx++)
                    for (int zz = -2; zz <= 2; zz++)
                        if (std::abs(xx) != 2 || std::abs(zz) != 2) placeBlockAt(p.offset(xx, 0, zz));
            };
            for (BlockPos p : base) {
                if (p.y != minY) continue;
                placeCircle(p.west().north());
                placeCircle(p.east(2).north());
                placeCircle(p.west().south(2));
                placeCircle(p.east(2).south(2));
                for (int i = 0; i < 5; i++) {
                    int placement = random.nextInt(64);
                    int xx = placement % 8, zz = placement / 8;
                    if (xx == 0 || xx == 7 || zz == 0 || zz == 7) placeCircle(p.offset(-3 + xx, 0, -3 + zz));
                }
            }
        } else if (type == "minecraft:attached_to_leaves") {
            float probability = static_cast<float>(d["probability"].number());
            int exXZ = toInt(d["exclusion_radius_xz"]), exY = toInt(d["exclusion_radius_y"]), required = toInt(d["required_empty_blocks"]);
            std::vector<Direction> dirs;
            for (const auto& v : d["directions"].array()) dirs.push_back(direction::byName(v.string()));
            JavaBlockPosSet blacklist; // (only asked what it contains)
            std::vector<BlockPos> shuffled = leaves; // Util.shuffledCopy
            for (size_t i = shuffled.size(); i > 1; i--) std::swap(shuffled[i - 1], shuffled[random.nextInt(static_cast<int>(i))]);
            for (BlockPos leaf : shuffled) {
                Direction dir = dirs[random.nextInt(static_cast<int>(dirs.size()))];
                BlockPos placement = leaf.relative(dir);
                if (blacklist.contains(placement) || !(random.nextFloat() < probability)) continue;
                bool empty = true;
                for (int i = 1; i <= required && empty; i++) empty = isAir(leaf.relative(dir, i));
                if (!empty) continue;
                for (int x = -exXZ; x <= exXZ; x++)
                    for (int y = -exY; y <= exY; y++)
                        for (int z = -exXZ; z <= exXZ; z++) blacklist.add(placement.offset(x, y, z));
                setDecoration(placement, getState(d["block_provider"], level, random, placement));
            }
        } else if (type == "minecraft:shelf_mushroom") {
            if (random.nextFloat() >= static_cast<float>(d["probability"].number()) || logs.empty()) continue;
            auto replaceable = [&](BlockPos p) {
                auto water = [&](BlockPos q) { return bs::is(stateAt(q), WATER); };
                return bs::canBeReplaced(stateAt(p)) && !(water(p) || water(p.east()) || water(p.west()) || water(p.north()) || water(p.south()));
            };
            auto shelfAt = [&](BlockPos p) { return bs::is(stateAt(p), SHELF_MUSHROOM); };
            auto place = [&](BlockPos p, Direction facing) {
                setDecoration(p, bs::setValue(bs::setValue(bs::defaultState(SHELF_MUSHROOM), "age", std::to_string(random.nextInt(2))), "facing",
                                              direction::name(facing)));
            };
            if (logs.front().y == logs.back().y) { // placeOnFallenLog
                Axis logAxis = logs.front().x != logs.back().x ? Axis::X : Axis::Z;
                std::array<Direction, 2> dirs = logAxis == Axis::X ? std::array{Direction::North, Direction::South} : std::array{Direction::East, Direction::West};
                for (BlockPos log : logs)
                    for (Direction facing : dirs) {
                        if (random.nextFloat() > 0.25f) continue;
                        BlockPos p = log.offset(direction::stepX(facing), 0, direction::stepZ(facing));
                        auto adjacent = [&](BlockPos q) {
                            for (Direction h : kPlane)
                                if (shelfAt(q.relative(h))) return true;
                            return false;
                        };
                        if (replaceable(p) && !adjacent(p) && !adjacent(log)) place(p, facing);
                    }
            } else { // placeOnStandingTree
                Direction first = randomHorizontal(random);
                std::array<Direction, 2> dirs{first, direction::clockWise(first)};
                int baseY = logs.front().y;
                for (BlockPos log : logs) {
                    int dy = log.y - baseY;
                    if (dy < 1 || dy > 4) continue;
                    for (Direction facing : dirs) {
                        if (random.nextFloat() > 0.25f) continue;
                        BlockPos p = log.offset(direction::stepX(facing), 0, direction::stepZ(facing));
                        if (!replaceable(p) || shelfAt(p.below())) continue;
                        place(p, facing);
                        break;
                    }
                }
            }
        } else {
            log::warn("unsupported tree decorator {}", type);
        }
    }
}

// TreeFeature.updateLeaves and StructureTemplate.updateShapeAtEdge.
void Tree::updateLeavesAndEdges() {
    std::vector<BlockPos> all;
    for (const auto* set : {&roots, &trunks, &foliage, &decorations}) {
        auto v = set->ordered();
        all.insert(all.end(), v.begin(), v.end());
    }
    BlockPos lo = all.front(), hi = all.front(); // BoundingBox.encapsulatingPositions
    for (BlockPos p : all) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    int sx = hi.x - lo.x + 1, sy = hi.y - lo.y + 1, sz = hi.z - lo.z + 1;
    std::vector<bool> shape(static_cast<size_t>(sx) * sy * sz);
    auto inside = [&](BlockPos p) { return p.x >= lo.x && p.x <= hi.x && p.y >= lo.y && p.y <= hi.y && p.z >= lo.z && p.z <= hi.z; };
    auto cell = [&](int x, int y, int z) { return (static_cast<size_t>(x) * sy + y) * sz + z; };
    auto fill = [&](BlockPos p) { shape[cell(p.x - lo.x, p.y - lo.y, p.z - lo.z)] = true; };
    auto full = [&](BlockPos p) { return static_cast<bool>(shape[cell(p.x - lo.x, p.y - lo.y, p.z - lo.z)]); };
    for (const auto* set : {&decorations, &roots})
        for (BlockPos p : set->ordered())
            if (inside(p)) fill(p);

    auto distanceAt = [](BlockStateId s) -> std::optional<int> { // LeavesBlock.getOptionalDistanceAt
        if (bs::stateHasTag(s, "minecraft:prevents_nearby_leaf_decay")) return 0;
        if (bs::hasProperty(s, "distance") && bs::isInstance(bs::blockOf(s), "LeavesBlock"))
            return std::stoi(std::string(*bs::getValue(s, "distance")));
        return std::nullopt;
    };
    std::array<JavaBlockPosSet, 7> toCheck;
    for (BlockPos p : trunks.ordered()) toCheck[0].add(p);
    int smallest = 0;
    while (true) {
        while (smallest >= 7 || !toCheck[smallest].empty()) {
            if (smallest >= 7) goto edges;
            BlockPos p = toCheck[smallest].popFirst();
            if (!inside(p)) continue;
            if (smallest != 0) setBlock(p, bs::setValue(stateAt(p), "distance", std::to_string(smallest)));
            fill(p);
            for (Direction d : direction::kAll) {
                BlockPos n = p.relative(d);
                if (!inside(n) || full(n)) continue;
                if (auto dist = distanceAt(stateAt(n))) {
                    int newDistance = std::min(*dist, smallest + 1);
                    if (newDistance < 7) {
                        toCheck[newDistance].add(n);
                        smallest = std::min(smallest, newDistance);
                    }
                }
            }
        }
        smallest++;
    }
edges:
    // StructureTemplate.updateShapeAtEdge(level, 3, shape, bounds.min): each face of the shape,
    // in DiscreteVoxelShape.forAllFaces order (z runs, then y runs, then x runs).
    auto face = [&](Direction d, int x, int y, int z) {
        BlockPos p{lo.x + x, lo.y + y, lo.z + z};
        BlockPos n = p.relative(d);
        BlockStateId s = stateAt(p), ns = stateAt(n);
        BlockStateId updated = block_behaviour::updateShape(s, level, p, d, n, ns);
        if (updated != s) level.setBlock(p, updated, ServerLevel::kUpdateClients);
        BlockStateId nUpdated = block_behaviour::updateShape(ns, level, n, direction::opposite(d), p, updated);
        if (nUpdated != ns) level.setBlock(n, nUpdated, ServerLevel::kUpdateClients);
    };
    auto isFull = [&](int x, int y, int z) { return static_cast<bool>(shape[cell(x, y, z)]); };
    auto scan = [&](int aSize, int bSize, int cSize, auto coords, Direction neg, Direction pos) {
        for (int a = 0; a < aSize; a++)
            for (int b = 0; b < bSize; b++) {
                bool lastFull = false;
                for (int c = 0; c <= cSize; c++) {
                    bool isF = false;
                    if (c != cSize) {
                        auto [x, y, z] = coords(a, b, c);
                        isF = isFull(x, y, z);
                    }
                    if (!lastFull && isF) {
                        auto [x, y, z] = coords(a, b, c);
                        face(neg, x, y, z);
                    }
                    if (lastFull && !isF) {
                        auto [x, y, z] = coords(a, b, c - 1);
                        face(pos, x, y, z);
                    }
                    lastFull = isF;
                }
            }
    };
    scan(sx, sy, sz, [](int a, int b, int c) { return std::array{a, b, c}; }, Direction::North, Direction::South);
    scan(sz, sx, sy, [](int a, int b, int c) { return std::array{b, c, a}; }, Direction::Down, Direction::Up);
    scan(sy, sz, sx, [](int a, int b, int c) { return std::array{c, a, b}; }, Direction::West, Direction::East);
}

bool Tree::place(BlockPos origin) {
    // TreeFeature.doPlace
    int treeHeight = this->treeHeight();
    int foliageH = foliageHeight(treeHeight);
    int trunkHeight = treeHeight - foliageH;
    int leafRadius = foliageRadius(trunkHeight);
    bool hasRoots = cfg.has("root_placer");
    BlockPos trunkOrigin = hasRoots ? origin.above(sampleInt(cfg["root_placer"]["trunk_offset_y"], random)) : origin;
    int minY = std::min(origin.y, trunkOrigin.y);
    int maxY = std::max(origin.y, trunkOrigin.y) + treeHeight + 1;
    if (minY < level.minY() + 1 || maxY > level.maxY() + 1) return false;
    std::optional<int> minClipped;
    if (cfg["minimum_size"].has("min_clipped_height")) minClipped = toInt(cfg["minimum_size"]["min_clipped_height"]);
    int clipped = getMaxFreeTreeHeight(treeHeight, trunkOrigin);
    if (!(clipped >= treeHeight || (minClipped && clipped >= *minClipped))) return false;
    if (hasRoots && !placeRoots(origin, trunkOrigin)) return false;
    for (const Attachment& a : placeTrunk(clipped, trunkOrigin)) createFoliage(clipped, a, foliageH, leafRadius);
    // TreeFeature.place
    if (trunks.empty() && foliage.empty()) return false;
    if (!cfg["decorators"].array().empty()) decorate();
    updateLeavesAndEdges();
    return true;
}

// ---------------------------------------------------------------- huge mushrooms (AbstractHugeMushroomFeature)

bool placeHugeMushroom(const Value& cfg, ServerLevel& level, LegacyRandom& random, BlockPos origin) {
    bool red = cfg["type"].string() == "minecraft:huge_red_mushroom";
    int foliageRadius = intOr(cfg, "foliage_radius", 2);
    int treeHeight = random.nextInt(3) + 4;
    if (random.nextInt(12) == 0) treeHeight *= 2;
    auto radiusFor = [&](int yo) { // getTreeRadiusForHeight(-1, -1, foliageRadius, yo)
        if (red) return (yo < -1 && yo >= -4) || yo == -1 ? foliageRadius : 0;
        return yo <= 3 ? 0 : foliageRadius;
    };
    // isValidPosition
    if (origin.y < level.minY() + 1 || origin.y + treeHeight + 1 > level.maxY()) return false;
    if (!levelgen::testBlockPredicate(cfg["can_place_on"], level, origin.below())) return false;
    for (int dy = 0; dy <= treeHeight; dy++) {
        int r = radiusFor(dy);
        for (int dx = -r; dx <= r; dx++)
            for (int dz = -r; dz <= r; dz++) {
                BlockStateId s = level.getBlockState(origin.offset(dx, dy, dz));
                if (!bs::isAir(s) && !bs::stateHasTag(s, "minecraft:leaves")) return false;
            }
    }
    auto placeBlock = [&](BlockPos p, BlockStateId s) { // placeMushroomBlock, Feature.setBlock (setBlockAndUpdate)
        BlockStateId current = level.getBlockState(p);
        if (bs::isAir(current) || bs::stateHasTag(current, "minecraft:replaceable_by_mushrooms")) level.setBlock(p, s, ServerLevel::kUpdateAll);
    };
    auto hasFaces = [](BlockStateId s, bool up) {
        return bs::hasProperty(s, "west") && bs::hasProperty(s, "east") && bs::hasProperty(s, "north") && bs::hasProperty(s, "south") &&
               (!up || bs::hasProperty(s, "up"));
    };
    if (red) {
        for (int dy = treeHeight - 3; dy <= treeHeight; dy++) {
            int r = dy < treeHeight ? foliageRadius : foliageRadius - 1;
            int center = foliageRadius - 2;
            for (int dx = -r; dx <= r; dx++)
                for (int dz = -r; dz <= r; dz++) {
                    bool xEdge = dx == -r || dx == r, zEdge = dz == -r || dz == r;
                    if (!(dy >= treeHeight || xEdge != zEdge)) continue;
                    BlockStateId s = getState(cfg["cap_provider"], level, random, origin);
                    if (hasFaces(s, true))
                        s = bs::setBool(bs::setBool(bs::setBool(bs::setBool(bs::setBool(s, "up", dy >= treeHeight - 1), "west", dx < -center), "east", dx > center),
                                                    "north", dz < -center),
                                        "south", dz > center);
                    placeBlock(origin.offset(dx, dy, dz), s);
                }
        }
    } else {
        int r = foliageRadius;
        for (int dx = -r; dx <= r; dx++)
            for (int dz = -r; dz <= r; dz++) {
                bool minX = dx == -r, maxX = dx == r, minZ = dz == -r, maxZ = dz == r;
                bool xEdge = minX || maxX, zEdge = minZ || maxZ;
                if (xEdge && zEdge) continue;
                bool west = minX || (zEdge && dx == 1 - r), east = maxX || (zEdge && dx == r - 1);
                bool north = minZ || (xEdge && dz == 1 - r), south = maxZ || (xEdge && dz == r - 1);
                BlockStateId s = getState(cfg["cap_provider"], level, random, origin);
                if (hasFaces(s, false)) s = bs::setBool(bs::setBool(bs::setBool(bs::setBool(s, "west", west), "east", east), "north", north), "south", south);
                placeBlock(origin.offset(dx, treeHeight, dz), s);
            }
    }
    for (int dy = 0; dy < treeHeight; dy++) placeBlock(origin.above(dy), getState(cfg["stem_provider"], level, random, origin)); // placeTrunk
    return true;
}

} // namespace

bool place(std::string_view id, ServerLevel& level, LegacyRandom& random, BlockPos origin) {
    const Value& cfg = features()[id];
    std::string_view type = cfg["type"].isString() ? std::string_view(cfg["type"].string()) : std::string_view();
    if (type == "minecraft:tree") return Tree(cfg, level, random).place(origin);
    if (type == "minecraft:huge_red_mushroom" || type == "minecraft:huge_brown_mushroom") return placeHugeMushroom(cfg, level, random, origin);
    log::warn("feature {} ({}) isn't ported yet", id, type.empty() ? "unknown" : type);
    return false;
}

std::optional<int> treeBaseHeight(std::string_view id) {
    const Value& cfg = features()[id];
    if (!cfg.isObject() || cfg["type"].string() != "minecraft:tree") return std::nullopt;
    return toInt(cfg["trunk_placer"]["base_height"]);
}

std::optional<int> hugeMushroomFoliageRadius(std::string_view id) {
    const Value& cfg = features()[id];
    if (!cfg.isObject()) return std::nullopt;
    std::string_view type = cfg["type"].string();
    if (type != "minecraft:huge_red_mushroom" && type != "minecraft:huge_brown_mushroom") return std::nullopt;
    return intOr(cfg, "foliage_radius", 2);
}

} // namespace mc::feature
