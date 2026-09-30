// Prints "<slot> <component> <hash>" for every component of every item in a saved player file,
// hashed like HashOps (for HashedPatchMap); tools/extractor/HashCheck.java prints vanilla's.
//   cmc-codec-check world/players/data/<uuid>.dat
#include <cstdio>

#include "core/component/data_components.hpp"
#include "core/registries.hpp"
#include "nbt/tag.hpp"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <player.dat>\n", argv[0]);
        return 2;
    }
    mc::registries::bootstrap();
    mc::components::bootstrap();
    auto root = mc::nbt::readCompressedFile(argv[1]);
    if (!root) return 1;
    const mc::nbt::List* inv = root->getList("Inventory");
    if (!inv) return 0;
    for (const auto& e : inv->items) {
        const mc::nbt::Compound* item = e.asCompound();
        const mc::nbt::Compound* comps = item ? item->getCompound("components") : nullptr;
        if (!comps) continue;
        for (const auto& [name, value] : comps->entries) {
            if (name.starts_with("!")) continue;
            const auto* type = mc::components::byName(name);
            auto bytes = type ? mc::components::toNetwork(type->id, value) : std::nullopt;
            auto h = bytes ? mc::components::hash(type->id, *bytes) : std::nullopt;
            if (h) std::printf("%d %s %d\n", item->getInt("Slot"), name.c_str(), *h);
            else std::printf("%d %s ?\n", item->getInt("Slot"), name.c_str());
        }
    }
}
