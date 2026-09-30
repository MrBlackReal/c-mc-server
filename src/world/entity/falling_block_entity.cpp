#include "world/entity/falling_block_entity.hpp"

#include <cmath>
#include <random>

#include "server/level/server_level.hpp"
#include "world/entity/item_entity.hpp"
#include "world/level/block/block_behaviour.hpp"
#include "world/level/levelgen/block_predicate.hpp"

namespace mc {

namespace bs = blocks;

namespace {
constexpr double kGravity = 0.04;
constexpr float kAirDrag = 0.98f;

nbt::Compound saveState(BlockStateId state) { // BlockState.CODEC
    nbt::Compound c;
    uint32_t block = bs::blockOf(state);
    c.put("Name", std::string(gen::kBlocks[block].name));
    if (!gen::kBlocks[block].properties.empty()) {
        nbt::Compound props;
        for (const auto& p : gen::kBlocks[block].properties) props.put(std::string(p.name), std::string(bs::getValue(state, p.name).value_or("")));
        c.put("Properties", std::move(props));
    }
    return c;
}

std::optional<BlockStateId> loadState(const nbt::Compound& c) {
    auto block = bs::byName(c.getString("Name"));
    if (!block) return std::nullopt;
    BlockStateId state = bs::defaultState(*block);
    if (const nbt::Compound* props = c.getCompound("Properties"))
        for (const auto& [k, v] : props->entries)
            if (const std::string* s = v.asString()) state = bs::setValue(state, k, *s);
    return state;
}
} // namespace

FallingBlockEntity* FallingBlockEntity::fall(ServerLevel& level, BlockPos pos, BlockStateId state) {
    BlockStateId falling = bs::hasProperty(state, "waterlogged") ? bs::setBool(state, "waterlogged", false) : state;
    auto entity = std::make_unique<FallingBlockEntity>(level.nextEntityId(), UUID::random(), pos.x + 0.5, pos.y, pos.z + 0.5, falling);
    // FluidState.createLegacyBlock: water stays behind a waterlogged block.
    level.setBlock(pos, bs::getBool(state, "waterlogged") ? bs::defaultState(gen::block::WATER) : bs::defaultState(gen::block::AIR));
    if (bs::isInstance(bs::blockOf(state), "AnvilBlock")) { // AnvilBlock.falling
        entity->hurtEntities = true;
        entity->fallDamagePerDistance = 2.0f;
        entity->fallDamageMax = 40;
    }
    return level.addFreshEntity(std::move(entity));
}

void FallingBlockEntity::spawnItem(ServerLevel& level) { // Entity.spawnAtLocation(level, block)
    int32_t item = bs::behaviour(bs::blockOf(blockState)).item;
    if (item <= 0) return;
    if (ItemEntity* e = ItemEntity::spawn(level, x, y, z, ItemStack(item, 1), level.random())) e->pickupDelay = ItemEntity::kDefaultPickupDelay;
}

// FallingBlockEntity.causeFallDamage: only anvils care (they chip); hurting entities needs mobs.
void FallingBlockEntity::causeFallDamage(ServerLevel& level) {
    if (!hurtEntities) return;
    int distance = static_cast<int>(std::ceil(fallDistance - 1.0));
    if (distance < 0) return;
    float damage = std::min(static_cast<float>(std::floor(distance * fallDamagePerDistance)), static_cast<float>(fallDamageMax));
    // TODO: damage the living entities inside (players take fall damage from anvils).
    if (bs::stateHasTag(blockState, "minecraft:anvil") && damage > 0.0f &&
        std::uniform_real_distribution<float>(0.0f, 1.0f)(level.random()) < 0.05f + distance * 0.05f) {
        uint32_t b = bs::blockOf(blockState);
        std::string facing(bs::getValue(blockState, "facing").value_or("north"));
        if (b == gen::block::ANVIL) blockState = bs::setValue(bs::defaultState(gen::block::CHIPPED_ANVIL), "facing", facing);
        else if (b == gen::block::CHIPPED_ANVIL) blockState = bs::setValue(bs::defaultState(gen::block::DAMAGED_ANVIL), "facing", facing);
        else cancelDrop = true;
    }
}

void FallingBlockEntity::tick(ServerLevel& level) {
    if (bs::isAir(blockState)) {
        removed = true;
        return;
    }
    uint32_t block = bs::blockOf(blockState);
    time++;
    vy -= kGravity;
    double yBefore = y, xo = x, zo = z;
    move(level, vx, vy, vz);
    // Entity.checkFallDamage
    if (onGround) {
        if (fallDistance > 0) causeFallDamage(level);
        fallDistance = 0;
    } else if (y < yBefore) {
        fallDistance -= y - yBefore;
    }

    BlockPos pos{static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)), static_cast<int>(std::floor(z))};
    bool isConcrete = bs::isInstance(block, "ConcretePowderBlock");
    bool stuckInWater = isConcrete && (bs::flags(level.getBlockState(pos)) & bs::kWater);
    if (isConcrete && vx * vx + vy * vy + vz * vz > 1.0) { // falling fast: it may have passed through water this tick
        if (auto hit = level.clip(xo, yBefore, zo, x, y, z, true); hit && (bs::flags(level.getBlockState(*hit)) & bs::kWater)) {
            pos = *hit;
            stuckInWater = true;
        }
    }
    if (!onGround && !stuckInWater) {
        if ((time > 100 && (pos.y <= level.minY() || pos.y > level.maxY())) || time > 600) {
            if (dropItem && level.gameRules().getBool(GameRules::EntityDrops)) spawnItem(level);
            removed = true;
        }
    } else {
        BlockStateId current = level.getBlockState(pos);
        vx *= 0.7, vy *= -0.5, vz *= 0.7;
        if (bs::blockOf(current) != gen::block::MOVING_PISTON) {
            if (!cancelDrop) {
                // BlockState.canBeReplaced(DirectionalPlaceContext(pos, DOWN, EMPTY, UP))
                PlaceContext ctx{level, pos, Direction::Up, pos.x + 0.5, pos.y + 0.5, pos.z + 0.5, 0, 0, false, ItemStack{}, false, true, pos};
                bool mayReplace = block_behaviour::canBeReplaced(current, ctx);
                BlockStateId below = level.getBlockState(pos.below());
                bool free = bs::isAir(below) || bs::stateHasTag(below, "minecraft:fire") || bs::hasFluid(below) || bs::canBeReplaced(below);
                bool wouldContinueFalling = free && (!isConcrete || !stuckInWater);
                bool wouldSurvive = block_behaviour::canSurvive(blockState, level, pos) && !wouldContinueFalling;
                if (mayReplace && wouldSurvive) {
                    if (bs::hasProperty(blockState, "waterlogged") && (bs::flags(current) & bs::kWater))
                        blockState = bs::setBool(blockState, "waterlogged", true);
                    if (level.setBlock(pos, blockState)) {
                        // (clients get the block with the other changes; ClientboundAddTransientBlockPacket smooths
                        // the handover from entity to block and is sent by PlayerList with them)
                        level.transientBlocks.push_back(pos);
                        removed = true;
                        block_behaviour::onLand(level, pos, blockState, current);
                        if (blockData) {
                            LevelChunk& chunk = level.getChunk({pos.x >> 4, pos.z >> 4});
                            if (auto* be = chunk.blockEntityAt(pos.x, pos.y, pos.z)) {
                                for (const auto& [k, v] : blockData->entries) be->tag.put(k, v);
                                chunk.dirty = true;
                            }
                        }
                    } else if (dropItem && level.gameRules().getBool(GameRules::EntityDrops)) {
                        removed = true;
                        block_behaviour::onBrokenAfterFall(level, pos, blockState);
                        spawnItem(level);
                    }
                } else {
                    removed = true;
                    if (dropItem && level.gameRules().getBool(GameRules::EntityDrops)) {
                        block_behaviour::onBrokenAfterFall(level, pos, blockState);
                        spawnItem(level);
                    }
                }
            } else {
                removed = true;
                block_behaviour::onBrokenAfterFall(level, pos, blockState);
            }
        }
    }
    vx *= kAirDrag, vy *= kAirDrag, vz *= kAirDrag;
}

nbt::Compound FallingBlockEntity::save() const {
    nbt::Compound t;
    t.put("id", std::string("minecraft:falling_block"));
    saveBase(t);
    t.put("BlockState", saveState(blockState));
    t.put("Time", time);
    t.put("DropItem", nbt::byteTag(dropItem));
    t.put("HurtEntities", nbt::byteTag(hurtEntities));
    t.put("FallHurtAmount", fallDamagePerDistance);
    t.put("FallHurtMax", fallDamageMax);
    if (blockData) t.put("TileEntityData", *blockData);
    t.put("CancelDrop", nbt::byteTag(cancelDrop));
    return t;
}

std::unique_ptr<FallingBlockEntity> FallingBlockEntity::load(int id, const nbt::Compound& t) {
    const nbt::List* pos = t.getList("Pos");
    if (!pos || pos->items.size() != 3) return nullptr;
    const nbt::Compound* stateTag = t.getCompound("BlockState");
    BlockStateId state = (stateTag ? loadState(*stateTag) : std::nullopt).value_or(bs::defaultState(gen::block::SAND));
    auto e = std::make_unique<FallingBlockEntity>(id, entity_nbt::uuidFrom(t.getIntArray("UUID")).value_or(UUID::random()), pos->items[0].asDouble(),
                                                  pos->items[1].asDouble(), pos->items[2].asDouble(), state);
    e->loadBase(t);
    e->time = t.getInt("Time");
    e->hurtEntities = t.getBool("HurtEntities", bs::stateHasTag(state, "minecraft:anvil"));
    e->fallDamagePerDistance = static_cast<float>(t.getDouble("FallHurtAmount"));
    e->fallDamageMax = t.getInt("FallHurtMax", 40);
    e->dropItem = t.getBool("DropItem", true);
    if (const nbt::Compound* data = t.getCompound("TileEntityData")) e->blockData = *data;
    e->cancelDrop = t.getBool("CancelDrop");
    return e;
}

} // namespace mc
