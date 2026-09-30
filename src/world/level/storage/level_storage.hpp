#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "nbt/tag.hpp"
#include "util/uuid.hpp"
#include "world/level/block/block_state.hpp"
#include "world/level/gamerules/game_rules.hpp"

namespace mc {

// Port of the parts of LevelStorageSource / PrimaryLevelData / WorldGenSettings the server uses,
// in the 26.x layout:
//   level.dat                          Data{...}: time, spawn, difficulty, version, ...
//   data/minecraft/world_gen_settings.dat, world_clocks.dat, game_rules.dat
//   dimensions/minecraft/<dim>/region/r.X.Z.mca
//   players/data/<uuid>.dat
class LevelStorage {
public:
    struct FlatLayer {
        BlockStateId state;
        int height;
    };
    struct WorldGen {
        int64_t seed = 0;
        std::string overworldGenerator = "minecraft:flat"; // generator "type"
        std::vector<FlatLayer> layers;                    // when flat
        std::string biome = "minecraft:plains";           // when flat
        nbt::Compound raw;                                // world_gen_settings "data", passed through
    };
    struct LevelData {
        int64_t gameTime = 0;
        int spawnX = 0, spawnY = 64, spawnZ = 0;
        float spawnYaw = 0, spawnPitch = 0;
        std::string difficulty = "easy";
        bool hardcore = false;
        nbt::Compound raw; // "Data", passed through
    };
    struct Clock {
        std::string name;
        int64_t totalTicks = 0;
        float partialTick = 0, rate = 1;
        bool paused = false;
    };

    // Opens (creating if needed) and locks the world folder. Null + `error` on failure.
    static std::unique_ptr<LevelStorage> open(const std::filesystem::path& dir, std::string& error);
    ~LevelStorage();

    const std::filesystem::path& dir() const { return dir_; }
    bool exists() const;
    std::filesystem::path dimensionPath(std::string_view dimension) const; // e.g. minecraft:overworld

    // Loading an existing world. Returns an error message if the world can't be used.
    std::optional<std::string> load(LevelData& level, WorldGen& gen, std::vector<Clock>& clocks, GameRules& rules);
    // Creating a new world: fills defaults for a superflat world with the given seed.
    static WorldGen defaultFlat(int64_t seed);
    void saveLevel(const LevelData& level);
    void saveWorldGen(const WorldGen& gen);
    void saveClocks(const std::vector<Clock>& clocks);
    void saveGameRules(const GameRules& rules);

    std::optional<nbt::Compound> loadPlayer(const UUID& id);
    bool savePlayer(const UUID& id, const nbt::Compound& tag);

private:
    explicit LevelStorage(std::filesystem::path dir) : dir_(std::move(dir)) {}
    std::filesystem::path dataFile(std::string_view name) const { return dir_ / "data" / "minecraft" / (std::string(name) + ".dat"); }

    std::filesystem::path dir_;
    int lockFd_ = -1;
};

} // namespace mc
