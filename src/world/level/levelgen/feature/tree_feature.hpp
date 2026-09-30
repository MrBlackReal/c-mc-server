#pragma once

#include <optional>
#include <string_view>

#include "core/direction.hpp"
#include "util/legacy_random.hpp"

namespace mc {
class ServerLevel;

// Data-driven world-gen features (data/features.json, the jar's worldgen/feature): so far the
// ones saplings and mushrooms grow into, TreeFeature (with its trunk, foliage and root placers
// and tree decorators) and the huge mushrooms.
namespace feature {

// Feature.place: false if the feature doesn't fit (or isn't ported: logged).
bool place(std::string_view id, ServerLevel& level, LegacyRandom& random, BlockPos origin);
// TreeFeature's trunkPlacer().getBaseHeight(), for TreeGrower.getMinimumHeight.
std::optional<int> treeBaseHeight(std::string_view id);
// AbstractHugeMushroomFeature.foliageRadius, if `id` is a huge mushroom.
std::optional<int> hugeMushroomFoliageRadius(std::string_view id);

} // namespace feature
} // namespace mc
