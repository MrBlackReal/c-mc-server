#pragma once

#include <cstdint>

#include "core/direction.hpp"

namespace mc {
class ServerLevel;

// Biome.ClimateSettings and what depends on it: temperatures (with the height and frozen-ocean
// noise adjustments), rain versus snow, and water freezing.
namespace biome {

// Biome.getTemperature for the minecraft:worldgen/biome with this id.
float temperature(uint32_t biome, BlockPos pos, int seaLevel);
// Biome.warmEnoughToRain (below 0.15 it snows)
inline bool warmEnoughToRain(uint32_t biome, BlockPos pos, int seaLevel) { return temperature(biome, pos, seaLevel) >= 0.15f; }
// Biome.shouldFreeze: water here turns to ice (a water source block, not surrounded by water when
// `checkNeighbors`, in the cold and out of block light).
bool shouldFreeze(ServerLevel& level, uint32_t biome, BlockPos pos, bool checkNeighbors = true);

} // namespace biome
} // namespace mc
