#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "nbt/tag.hpp"

namespace mc {

// Port of net.minecraft.world.level.gamerules.GameRules (the rules enabled without experimental
// features), saved as data/minecraft/game_rules.dat.
class GameRules {
public:
    // In GameRules' declaration order.
    enum Rule : int {
        AdvanceTime,
        AdvanceWeather,
        AllowEnteringNetherUsingPortals,
        BlockDrops,
        BlockExplosionDropDecay,
        CommandBlocksWork,
        CommandBlockOutput,
        DrowningDamage,
        ElytraMovementCheck,
        EnderPearlsVanishOnDeath,
        EntityDrops,
        FallDamage,
        FireDamage,
        FireSpreadRadiusAroundPlayer,
        ForgiveDeadPlayers,
        FreezeDamage,
        GlobalSoundEvents,
        ImmediateRespawn,
        KeepInventory,
        LavaSourceConversion,
        LimitedCrafting,
        LocatorBar,
        LogAdminCommands,
        MaxBlockModifications,
        MaxCommandForks,
        MaxCommandSequenceLength,
        MaxEntityCramming,
        MaxSnowAccumulationHeight,
        MobDrops,
        MobExplosionDropDecay,
        MobGriefing,
        NaturalHealthRegeneration,
        PlayerMovementCheck,
        PlayersNetherPortalCreativeDelay,
        PlayersNetherPortalDefaultDelay,
        PlayersSleepingPercentage,
        ProjectilesCanBreakBlocks,
        Pvp,
        Raids,
        RandomTickSpeed,
        ReducedDebugInfo,
        RespawnRadius,
        SendCommandFeedback,
        ShowAdvancementMessages,
        ShowDeathMessages,
        SpawnerBlocksWork,
        SpawnMobs,
        SpawnMonsters,
        SpawnPatrols,
        SpawnPhantoms,
        SpawnWanderingTraders,
        SpawnWardens,
        SpectatorsGenerateChunks,
        SpreadVines,
        TntExplodes,
        TntExplosionDropDecay,
        UniversalAnger,
        WaterSourceConversion,
        Count
    };
    struct Info {
        std::string_view id; // without the minecraft: namespace
        bool isBool;
        int defaultValue;
        int min, max; // integer rules: IntegerArgumentType bounds
    };
    static std::span<const Info> infos();
    static const Info& info(Rule rule) { return infos()[rule]; }
    // By "name" or "minecraft:name".
    static std::optional<Rule> byName(std::string_view name);

    GameRules();
    bool getBool(Rule rule) const { return values_[rule] != 0; }
    int getInt(Rule rule) const { return values_[rule]; }
    void set(Rule rule, int value) { values_[rule] = value; }
    // GameRule.serialize: "true"/"false" or the number.
    std::string serialize(Rule rule) const;

    // GameRuleMap.CODEC: {"minecraft:<id>": value}. Invalid values keep the default; rules we don't
    // know (experimental ones) are kept as loaded.
    void load(const nbt::Compound& data);
    nbt::Compound save() const;

private:
    std::array<int, Count> values_;
    nbt::Compound unknown_;
};

} // namespace mc
