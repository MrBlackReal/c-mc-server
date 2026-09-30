#include "world/entity/item_entity.hpp"

#include <cmath>

#include "server/level/server_level.hpp"
#include "world/item/item_nbt.hpp"

namespace mc {
namespace {
constexpr double kGravity = 0.04;   // ItemEntity.getDefaultGravity
constexpr float kAirDrag = 0.98f;   // Entity.getAirDrag
} // namespace

ItemEntity::ItemEntity(int id, UUID uuid, double x, double y, double z, ItemStack item) : Entity(id, uuid, x, y, z), item(std::move(item)) {}

ItemEntity* ItemEntity::spawn(ServerLevel& level, double x, double y, double z, ItemStack item, std::mt19937& random) {
    std::uniform_real_distribution<double> d(0.0, 1.0);
    auto e = std::make_unique<ItemEntity>(level.nextEntityId(), UUID::random(), x, y, z, std::move(item));
    e->vx = d(random) * 0.2 - 0.1;
    e->vy = 0.2;
    e->vz = d(random) * 0.2 - 0.1;
    return level.addFreshEntity(std::move(e));
}

bool ItemEntity::isMergable() const {
    return !removed && pickupDelay != 32767 && age != -32768 && age < kLifetime && item.count < item.maxStackSize();
}

void ItemEntity::tick(ServerLevel& level) {
    if (item.isEmpty()) {
        removed = true;
        return;
    }
    if (pickupDelay > 0 && pickupDelay != 32767) pickupDelay--;
    double oldX = x, oldY = y, oldZ = z;
    double ovx = vx, ovy = vy, ovz = vz;

    // Fluid movement vs gravity (fluid height check approximated by the block at the item's center).
    BlockStateId inside = level.getBlockState(static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y + kHeight / 2)),
                                              static_cast<int>(std::floor(z)));
    uint32_t f = blocks::flags(inside);
    if (f & (blocks::kWater | blocks::kLava)) {
        double m = (f & blocks::kWater) ? 0.99 : 0.95; // setUnderwaterMovement / setUnderLavaMovement
        vx *= m;
        vy += vy < 0.06f ? 5.0e-4f : 0.0f;
        vz *= m;
    } else {
        vy -= kGravity;
    }

    // Stuck inside a block: no collisions, and a push towards the closest free space.
    AABB box = boundingBox();
    noPhysics = collides(level, box.inflate(-1.0e-7, -1.0e-7, -1.0e-7));
    if (noPhysics) moveTowardsClosestSpace(level, x, (box.minY + box.maxY) / 2.0, z);
    if (onGround && vx * vx + vz * vz <= 9.999999747378752E-6 && (tickCount + id_) % 4 != 0) {
        // Resting items only move every fourth tick.
    } else {
        move(level, vx, vy, vz);
        float friction = kAirDrag;
        if (onGround) friction = kAirDrag * blocks::behaviour(blocks::blockOf(level.getBlockState(onPos(0.999999f)))).friction;
        vx *= friction;
        vy *= kAirDrag;
        vz *= friction;
        if (onGround && vy < 0) vy *= -0.5;
    }

    bool movedBlock = std::floor(oldX) != std::floor(x) || std::floor(oldY) != std::floor(y) || std::floor(oldZ) != std::floor(z);
    if (tickCount % (movedBlock ? 2 : 40) == 0 && isMergable()) mergeWithNeighbours(level);
    if (age != -32768) age++;
    double dvx = vx - ovx, dvy = vy - ovy, dvz = vz - ovz;
    if (dvx * dvx + dvy * dvy + dvz * dvz > 0.01) needsSync = true;
    if (age >= kLifetime) removed = true;
}

void ItemEntity::mergeWithNeighbours(ServerLevel& level) {
    for (ItemEntity* other : level.itemsIn(boundingBox().inflate(0.5, 0.0, 0.5))) {
        if (other == this || !other->isMergable() || !isMergable()) continue;
        // ItemEntity.areMergable
        if (!ItemStack::isSameItemSameComponents(item, other->item) || item.count + other->item.count > item.maxStackSize()) continue;
        if (thrower != other->thrower && target != other->target) continue;
        ItemEntity* into = other->item.count < item.count ? this : other;
        ItemEntity* from = into == this ? other : this;
        int moved = std::min(into->item.maxStackSize() - into->item.count, from->item.count);
        into->item.grow(moved);
        from->item.shrink(moved);
        into->pickupDelay = std::max(into->pickupDelay, from->pickupDelay);
        into->age = std::min(into->age, from->age);
        into->dataChanged = true;
        if (from->item.count <= 0) from->removed = true;
        if (removed) break;
    }
}

nbt::Compound ItemEntity::save() const {
    nbt::Compound t;
    t.put("id", std::string("minecraft:item"));
    saveBase(t);
    t.put("Health", static_cast<int16_t>(5));
    t.put("Age", static_cast<int16_t>(age));
    t.put("PickupDelay", static_cast<int16_t>(pickupDelay));
    if (thrower) t.put("Thrower", entity_nbt::uuidInts(*thrower));
    if (target) t.put("Owner", entity_nbt::uuidInts(*target));
    t.put("Item", item_nbt::write(item));
    return t;
}

std::unique_ptr<ItemEntity> ItemEntity::load(int id, const nbt::Compound& t) {
    const nbt::Compound* itemTag = t.getCompound("Item");
    auto stack = itemTag ? item_nbt::read(*itemTag) : std::nullopt;
    const nbt::List* pos = t.getList("Pos");
    if (!stack || !pos || pos->items.size() != 3) return nullptr;
    auto e = std::make_unique<ItemEntity>(id, entity_nbt::uuidFrom(t.getIntArray("UUID")).value_or(UUID::random()), pos->items[0].asDouble(),
                                          pos->items[1].asDouble(), pos->items[2].asDouble(), std::move(*stack));
    e->loadBase(t);
    e->age = t.getInt("Age");
    e->pickupDelay = t.getInt("PickupDelay");
    e->thrower = entity_nbt::uuidFrom(t.getIntArray("Thrower"));
    e->target = entity_nbt::uuidFrom(t.getIntArray("Owner"));
    return e;
}

bool ItemEntity::writeEntityData(ByteWriter& out) const {
    out.writeByte(8);
    out.writeVarInt(7); // EntityDataSerializers.ITEM_STACK
    item.write(out);
    return true;
}

} // namespace mc
