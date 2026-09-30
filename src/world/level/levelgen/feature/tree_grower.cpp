#include "world/level/levelgen/feature/tree_grower.hpp"

#include <array>
#include <optional>
#include <string_view>
#include <vector>

#include "server/level/server_level.hpp"
#include "util/log.hpp"
#include "world/level/levelgen/feature/tree_feature.hpp"

namespace mc::tree_grower {
namespace {
namespace bs = blocks;

struct Weighted {
    std::string_view feature;
    int weight;
};
using List = std::vector<Weighted>;

// TreeGrower's growers (by name, as SaplingBlock's extracted tree_grower).
struct Grower {
    std::string_view name;
    List trees, megaTrees, flowerTrees;
    std::string_view shortestTreeType;
};
const std::vector<Grower>& growers() {
    static const std::vector<Grower> g = {
        {"oak", {{"minecraft:oak", 9}, {"minecraft:fancy_oak", 1}}, {}, {{"minecraft:oak_bees_005", 9}, {"minecraft:fancy_oak_bees_005", 1}}, "minecraft:oak"},
        {"spruce", {{"minecraft:spruce", 1}}, {{"minecraft:mega_spruce", 1}, {"minecraft:mega_pine", 1}}, {}, "minecraft:spruce"},
        {"mangrove", {{"minecraft:mangrove", 15}, {"minecraft:tall_mangrove", 85}}, {}, {}, "minecraft:mangrove"},
        {"azalea", {{"minecraft:azalea_tree", 1}}, {}, {}, "minecraft:azalea_tree"},
        {"birch", {{"minecraft:birch", 1}}, {}, {{"minecraft:birch_bees_005", 1}}, "minecraft:birch"},
        {"jungle", {{"minecraft:jungle_tree_no_vine", 1}}, {{"minecraft:mega_jungle_tree", 1}}, {}, "minecraft:jungle_tree_no_vine"},
        {"acacia", {{"minecraft:acacia", 1}}, {}, {}, "minecraft:acacia"},
        {"cherry", {{"minecraft:cherry", 1}}, {}, {{"minecraft:cherry_bees_005", 1}}, "minecraft:cherry"},
        {"dark_oak", {}, {{"minecraft:dark_oak", 1}}, {}, ""},
        {"pale_oak", {}, {{"minecraft:pale_oak_bonemeal", 1}}, {}, ""},
        {"poplar", {{"minecraft:red_poplar", 1}, {"minecraft:orange_poplar", 1}, {"minecraft:yellow_poplar", 1}}, {}, {}, "minecraft:red_poplar"},
    };
    return g;
}
const Grower* growerFor(BlockStateId sapling) {
    std::string_view name = bs::behaviour(bs::blockOf(sapling)).treeGrower;
    if (bs::is(sapling, gen::block::AZALEA) || bs::is(sapling, gen::block::FLOWERING_AZALEA)) name = "azalea"; // (AzaleaBlock's grower)
    for (const auto& g : growers())
        if (g.name == name) return &g;
    return nullptr;
}

// WeightedList.getRandom: a draw only when the list isn't empty.
std::optional<std::string_view> pick(const List& list, LegacyRandom& random) {
    if (list.empty()) return std::nullopt;
    int total = 0;
    for (const auto& w : list) total += w.weight;
    int selection = random.nextInt(total);
    for (const auto& w : list) {
        selection -= w.weight;
        if (selection < 0) return w.feature;
    }
    return std::nullopt;
}

// TreeGrower.hasFlowers
bool hasFlowers(ServerLevel& level, BlockPos pos) {
    for (int x = -2; x <= 2; x++)
        for (int y = -1; y <= 1; y++)
            for (int z = -2; z <= 2; z++)
                if (bs::stateHasTag(level.getBlockState(pos.offset(x, y, z)), "minecraft:flowers")) return true;
    return false;
}

struct Placed {
    BlockStateId state;
    BlockPos pos;
};
// TreeGrower.removeSapling: its fluid (a waterlogged propagule leaves water) with flags 818.
void removeSapling(ServerLevel& level, BlockPos pos) {
    BlockStateId s = level.getBlockState(pos);
    level.setBlock(pos, bs::getBool(s, "waterlogged") ? bs::defaultState(gen::block::WATER) : bs::defaultState(gen::block::AIR), 818);
}
void resetSaplings(ServerLevel& level, const std::vector<Placed>& saplings) {
    for (const auto& p : saplings) level.setBlock(p.pos, p.state, 260);
}

// TreeGrower.findTwoByTwoSaplingPos
std::optional<std::pair<BlockPos, std::vector<Placed>>> findTwoByTwo(ServerLevel& level, BlockStateId state, BlockPos pos) {
    uint32_t block = bs::blockOf(state);
    for (int dx = 0; dx >= -1; dx--)
        for (int dz = 0; dz >= -1; dz--) {
            std::vector<Placed> around;
            for (auto [ox, oz] : {std::pair{0, 0}, {1, 0}, {0, 1}, {1, 1}}) {
                BlockPos p = pos.offset(dx + ox, 0, dz + oz);
                around.push_back({level.getBlockState(p), p});
            }
            bool all = true;
            for (const auto& a : around) all = all && bs::blockOf(a.state) == block;
            if (all) return std::pair{BlockPos{dx, 0, dz}, around};
        }
    return std::nullopt;
}

} // namespace

void advanceTree(ServerLevel& level, BlockPos pos, BlockStateId sapling, LegacyRandom& random) {
    if (bs::getValue(sapling, "stage") == "0") {
        level.setBlock(pos, bs::setValue(sapling, "stage", "1"), 260); // UPDATE_NONE
        return;
    }
    growTree(level, pos, sapling, random);
}

bool growTree(ServerLevel& level, BlockPos pos, BlockStateId sapling, LegacyRandom& random) {
    const Grower* grower = growerFor(sapling);
    if (!grower) {
        log::warn("no tree grower for {}", gen::kBlocks[bs::blockOf(sapling)].name);
        return false;
    }
    if (auto mega = pick(grower->megaTrees, random)) {
        if (auto twoByTwo = findTwoByTwo(level, sapling, pos)) {
            auto& [offset, saplings] = *twoByTwo;
            for (const auto& s : saplings) removeSapling(level, s.pos);
            if (feature::place(*mega, level, random, pos.offset(offset.x, 0, offset.z))) return true;
            resetSaplings(level, saplings);
            return false;
        }
    }
    bool flowers = hasFlowers(level, pos);
    auto tree = flowers && !grower->flowerTrees.empty() ? pick(grower->flowerTrees, random) : pick(grower->trees, random);
    if (!tree) return false;
    removeSapling(level, pos);
    if (feature::place(*tree, level, random, pos)) return true;
    resetSaplings(level, {{sapling, pos}});
    return false;
}

std::optional<int> minimumHeight(BlockStateId sapling) {
    const Grower* grower = growerFor(sapling);
    if (!grower || grower->shortestTreeType.empty()) return std::nullopt;
    return feature::treeBaseHeight(grower->shortestTreeType);
}

bool canGrow(ServerLevel& level, BlockPos pos, BlockStateId sapling) {
    const Grower* grower = growerFor(sapling);
    if (!grower) return false;
    LegacyRandom& random = level.levelRandom();
    bool flowers = hasFlowers(level, pos);
    auto tree = flowers && !grower->flowerTrees.empty() ? pick(grower->flowerTrees, random) : pick(grower->trees, random);
    auto mega = pick(grower->megaTrees, random);
    return !tree && mega ? findTwoByTwo(level, sapling, pos).has_value() : true;
}

} // namespace mc::tree_grower
