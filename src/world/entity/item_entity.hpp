#pragma once

#include <optional>
#include <random>

#include "world/entity/entity.hpp"
#include "world/item/item_stack.hpp"

namespace mc {

class ServerLevel;

// Port of net.minecraft.world.entity.item.ItemEntity.
class ItemEntity : public Entity {
public:
    static constexpr double kWidth = 0.25, kHeight = 0.25;
    static constexpr int kDefaultPickupDelay = 10;
    static constexpr int kLifetime = 6000;

    ItemEntity(int id, UUID uuid, double x, double y, double z, ItemStack item);
    // ItemEntity(level, x, y, z, stack): random horizontal kick, 0.2 upwards.
    static ItemEntity* spawn(ServerLevel& level, double x, double y, double z, ItemStack item, std::mt19937& random);

    std::string_view type() const override { return "minecraft:item"; }
    double width() const override { return kWidth; }
    double height() const override { return kHeight; }
    void tick(ServerLevel& level) override;
    nbt::Compound save() const override;
    // ItemEntity.DATA_ITEM (index 8, EntityDataSerializers.ITEM_STACK).
    bool writeEntityData(ByteWriter& out) const override;
    static std::unique_ptr<ItemEntity> load(int id, const nbt::Compound& tag);

    bool isMergable() const;
    // ItemEntity.hurtServer: fire, lava, ... take health away; at 0 it's destroyed (and a
    // shulker box or bundle spills its contents). Items resistant to the damage aren't hurt.
    bool hurt(ServerLevel& level, std::string_view type, float amount) override;
    bool fireImmune() const override; // not hurt by minecraft:in_fire (netherite)

    ItemStack item;
    int pickupDelay = 0;
    int age = 0;
    std::optional<UUID> thrower;
    std::optional<UUID> target; // "Owner": only this player may pick it up
    int health = 5;

private:
    void mergeWithNeighbours(ServerLevel& level);
    double eyeHeight() const override { return 0.2125; }
    int soundSource() const override { return 8; } // AMBIENT
    bool shouldPlayLavaHurtSound() const override { return health <= 0 || tickCount % 10 == 0; }
};

} // namespace mc
