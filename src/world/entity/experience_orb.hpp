#pragma once

#include <memory>

#include "world/entity/entity.hpp"

namespace mc {

class ServerPlayer;

// Port of net.minecraft.world.entity.ExperienceOrb.
class ExperienceOrb : public Entity {
public:
    static constexpr int kLifetime = 6000;

    ExperienceOrb(int id, UUID uuid, double x, double y, double z, int value) : Entity(id, uuid, x, y, z), value(value) {}

    // ExperienceOrb.award: `amount` split into orbs (ExperienceOrb.getExperienceValue), merged into
    // matching orbs nearby where possible.
    static void award(ServerLevel& level, double x, double y, double z, int amount);
    static int experienceValue(int maxValue);

    std::string_view type() const override { return "minecraft:experience_orb"; }
    double width() const override { return 0.5; }
    double height() const override { return 0.5; }
    void tick(ServerLevel& level) override;
    nbt::Compound save() const override;
    static std::unique_ptr<ExperienceOrb> load(int id, const nbt::Compound& tag);
    // ExperienceOrb.DATA_VALUE (index 8, EntityDataSerializers.INT)
    bool writeEntityData(ByteWriter& out) const override;

    // ExperienceOrb.playerTouch
    void playerTouch(ServerLevel& level, ServerPlayer& player);

    int value = 0;
    int age = 0;
    int health = 5;
    int count = 1;

private:
    void followNearbyPlayer(ServerLevel& level);
    void scanForMerges(ServerLevel& level);
    int repairPlayerItems(ServerLevel& level, ServerPlayer& player, int amount);
    int followingPlayer_ = -1; // entity id
};

} // namespace mc
