#include "world/entity/item_entity.hpp"

#include <cmath>

#include "server/level/server_level.hpp"
#include "world/item/item_nbt.hpp"
#include "core/component/codec.hpp"
#include "core/registries.hpp"
#include "world/item/item_logic.hpp"

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
    bool wasOnFire = isOnFire();
    baseTick(level); // (Entity.tick)
    if (removed) return;
    if (pickupDelay > 0 && pickupDelay != 32767) pickupDelay--;
    double oldX = x, oldY = y, oldZ = z;
    double ovx = vx, ovy = vy, ovz = vz;

    auto setFluidMovement = [&](double multiplier) {
        vx *= multiplier;
        vy += vy < 0.06f ? 5.0e-4f : 0.0f;
        vz *= multiplier;
    };
    if (isInWater() && fluidHeight(false) > 0.1f) setFluidMovement(0.99f); // setUnderwaterMovement
    else if (isInLava() && fluidHeight(true) > 0.1f) setFluidMovement(0.95f); // setUnderLavaMovement
    else vy -= kGravity;

    // Stuck inside a block: no collisions, and a push towards the closest free space.
    AABB box = boundingBox();
    noPhysics = collides(level, box.inflate(-1.0e-7, -1.0e-7, -1.0e-7));
    if (noPhysics) moveTowardsClosestSpace(level, x, (box.minY + box.maxY) / 2.0, z);
    if (onGround && vx * vx + vz * vz <= 9.999999747378752E-6 && (tickCount + id_) % 4 != 0) {
        // Resting items only move every fourth tick (applyEffectsFromBlocksForLastMovements).
        applyEffectsFromBlocks(level, x, y, z);
    } else {
        move(level, vx, vy, vz);
        applyEffectsFromBlocks(level, oldX, oldY, oldZ);
        float friction = kAirDrag;
        if (onGround) friction = kAirDrag * blocks::behaviour(blocks::blockOf(level.getBlockState(onPos(0.999999f)))).friction;
        vx *= friction;
        vy *= kAirDrag;
        vz *= friction;
        if (onGround && vy < 0) vy *= -0.5;
    }
    if (removed) return;

    bool movedBlock = std::floor(oldX) != std::floor(x) || std::floor(oldY) != std::floor(y) || std::floor(oldZ) != std::floor(z);
    if (tickCount % (movedBlock ? 2 : 40) == 0 && isMergable()) mergeWithNeighbours(level);
    if (age != -32768) age++;
    if (updateFluidInteraction(level)) needsSync = true; // (a second time, as vanilla's ItemEntity does)
    double dvx = vx - ovx, dvy = vy - ovy, dvz = vz - ovz;
    if (dvx * dvx + dvy * dvy + dvz * dvz > 0.01) needsSync = true;
    if (age >= kLifetime) removed = true;
    if (wasOnFire != isOnFire()) dataChanged = true;
}

namespace {
// ItemStack.canBeHurtBy: unless its minecraft:damage_resistant types include the damage type.
bool resists(const ItemStack& stack, std::string_view type) {
    auto resistant = items::effectiveComponent(stack, "minecraft:damage_resistant");
    const nbt::Compound* c = resistant ? resistant->asCompound() : nullptr;
    const nbt::Tag* types = c ? c->get("types") : nullptr;
    if (!types) return false;
    auto matches = [&](std::string_view ref) {
        if (ref.starts_with('#')) return registries::hasTag("minecraft:damage_type", type, ref.substr(1));
        return ref == type;
    };
    if (const std::string* one = types->asString()) return matches(*one);
    if (const nbt::List* list = types->asList())
        for (const auto& e : list->items)
            if (const std::string* ref = e.asString(); ref && matches(*ref)) return true;
    return false;
}
} // namespace

bool ItemEntity::fireImmune() const { return resists(item, "minecraft:in_fire"); }

bool ItemEntity::hurt(ServerLevel& level, std::string_view type, float amount) {
    // isInvulnerableToBase: fire damage to fire-immune entities
    if (removed || (registries::hasTag("minecraft:damage_type", type, "minecraft:is_fire") && fireImmune())) return false;
    if (resists(item, type)) return false;
    health = static_cast<int>(health - amount);
    if (health <= 0) {
        // ItemStack.onDestroyed: BlockItem (a container's items) / BundleItem (its contents) spill out
        std::vector<ItemStack> contents;
        auto spill = [&](std::string_view component, bool slots) {
            auto value = items::componentValue(item, component);
            if (!value) return;
            std::vector<nbt::Tag> entries;
            try {
                entries = codec::listItems(*value);
            } catch (const std::exception&) {
                return;
            }
            for (const auto& e : entries) {
                const nbt::Compound* c = e.asCompound();
                if (slots && c) c = c->getCompound("item");
                if (auto stack = c ? item_nbt::read(*c) : std::nullopt; stack && !stack->isEmpty()) contents.push_back(std::move(*stack));
            }
            items::removeComponent(item, component);
        };
        if (item.info().block >= 0) spill("minecraft:container", true);
        if (item.info().itemClass == "BundleItem") spill("minecraft:bundle_contents", false);
        removed = true;
        for (auto& stack : contents) ItemEntity::spawn(level, x, y, z, std::move(stack), level.random());
    }
    return true;
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
    t.put("Health", static_cast<int16_t>(health));
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
    e->health = t.contains("Health") ? t.getInt("Health") : 5;
    e->pickupDelay = t.getInt("PickupDelay");
    e->thrower = entity_nbt::uuidFrom(t.getIntArray("Thrower"));
    e->target = entity_nbt::uuidFrom(t.getIntArray("Owner"));
    return e;
}

bool ItemEntity::writeEntityData(ByteWriter& out) const {
    writeSharedFlags(out);
    out.writeByte(8);
    out.writeVarInt(7); // EntityDataSerializers.ITEM_STACK
    item.write(out);
    return true;
}

} // namespace mc
