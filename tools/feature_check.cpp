// Developer tool: places a world-gen feature (a tree, a huge mushroom) with a given seed on an empty
// superflat level and prints what changed, so tools/feature_check.sh can diff it against vanilla's
// own feature code placing the same feature with the same seed (tools/extractor/FeatureCheck.java).
//
//   cmc-feature-check <feature id> <first seed> <count>
#include <cstdio>
#include <string>

#include "core/component/data_components.hpp"
#include "core/registries.hpp"
#include "generated/blocks.hpp"
#include "server/level/server_level.hpp"
#include "world/item/item_logic.hpp"
#include "world/level/biome/biome_climate.hpp"
#include "world/level/levelgen/feature/tree_feature.hpp"

using namespace mc;

namespace {
// "minecraft:oak_log[axis=y]" (properties sorted by name).
std::string describe(BlockStateId s) {
    const auto& info = gen::kBlocks[blocks::blockOf(s)];
    std::string out(info.name);
    if (info.properties.empty()) return out;
    out += '[';
    for (size_t i = 0; i < info.properties.size(); i++) {
        if (i) out += ',';
        out += info.properties[i].name;
        out += '=';
        out += *blocks::getValue(s, info.properties[i].name);
    }
    return out + ']';
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <feature> <first seed> <count>\n", argv[0]);
        return 2;
    }
    registries::bootstrap();
    components::bootstrap();
    blocks::bootstrap();
    items::bootstrap();
    std::string id = argv[1];
    if (id == "--climate") { // Biome temperatures on a grid, for comparing the noise with vanilla's
        for (const char* name : {"minecraft:frozen_ocean", "minecraft:snowy_slopes", "minecraft:plains"}) {
            auto biome = static_cast<uint32_t>(*registries::id("minecraft:worldgen/biome", name));
            for (int x = -300; x <= 300; x += 37)
                for (int z = -300; z <= 300; z += 41)
                    for (int y : {60, 100, 200}) std::printf("%s %d %d %d %.9g\n", name, x, y, z, biome::temperature(biome, {x, y, z}, 63));
        }
        return 0;
    }
    int64_t first = std::stoll(argv[2]);
    int count = std::stoi(argv[3]);
    int nextId = 1;
    for (int64_t seed = first; seed < first + count; seed++) {
        ServerLevel level("minecraft:overworld", "minecraft:overworld", DimensionInfo{-64, 384}, FlatLevelSource::classicFlat(), nullptr, nullptr,
                          true, 0, [&] { return nextId++; });
        LegacyRandom random(seed);
        BlockPos origin{0, -60, 0};
        bool placed = feature::place(id, level, random, origin);
        std::printf("SEED %lld placed=%d next=%d\n", static_cast<long long>(seed), placed ? 1 : 0, random.nextInt());
        auto flat = [](int y) {
            using namespace gen::block;
            return blocks::defaultState(y == -64 ? BEDROCK : y <= -62 ? DIRT : y == -61 ? GRASS_BLOCK : AIR);
        };
        for (int x = -24; x <= 24; x++)
            for (int y = -64; y <= 40; y++)
                for (int z = -24; z <= 24; z++) {
                    BlockStateId s = level.getBlockState(x, y, z);
                    if (s != flat(y)) std::printf("%d %d %d %s\n", x, y, z, describe(s).c_str());
                    if (blocks::is(s, gen::block::BEE_NEST)) {
                        auto* be = level.getChunk({x >> 4, z >> 4}).blockEntityAt(x, y, z);
                        const nbt::List* bees = be ? be->tag.getList("bees") : nullptr;
                        std::printf("BEES %d %d %d %zu\n", x, y, z, bees ? bees->items.size() : size_t{0});
                    }
                }
    }
    return 0;
}
