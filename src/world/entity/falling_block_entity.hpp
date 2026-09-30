#pragma once

#include <memory>

#include "world/entity/entity.hpp"
#include "world/level/block/block_state.hpp"

namespace mc {

// Port of net.minecraft.world.entity.item.FallingBlockEntity: sand, gravel, concrete powder,
// anvils and dragon eggs in the air.
class FallingBlockEntity : public Entity {
public:
    FallingBlockEntity(int id, UUID uuid, double x, double y, double z, BlockStateId state) : Entity(id, uuid, x, y, z), blockState(state) {}

    // FallingBlockEntity.fall: the block at `pos` turns into a falling entity (its fluid stays).
    static FallingBlockEntity* fall(ServerLevel& level, BlockPos pos, BlockStateId state);

    std::string_view type() const override { return "minecraft:falling_block"; }
    double width() const override { return 0.98; }
    double height() const override { return 0.98; }
    int spawnData() const override { return static_cast<int>(blockState); } // Block.getId
    int trackingRange() const override { return 10; }
    void tick(ServerLevel& level) override;
    nbt::Compound save() const override;
    static std::unique_ptr<FallingBlockEntity> load(int id, const nbt::Compound& tag);

    BlockStateId blockState;
    int time = 0;
    bool dropItem = true;
    bool cancelDrop = false;
    bool hurtEntities = false;
    int fallDamageMax = 40;
    float fallDamagePerDistance = 0.0f;
    double fallDistance = 0;
    std::optional<nbt::Compound> blockData; // "TileEntityData"

private:
    void causeFallDamage(ServerLevel& level);
    void spawnItem(ServerLevel& level);
};

} // namespace mc
