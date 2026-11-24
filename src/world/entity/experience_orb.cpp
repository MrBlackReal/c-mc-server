#include "world/entity/experience_orb.hpp"

#include <cmath>
#include <random>

#include "server/level/server_level.hpp"
#include "server/level/server_player.hpp"
#include "world/item/enchantment/enchantment_helper.hpp"
#include "world/item/item_logic.hpp"

namespace mc {

namespace {
constexpr double kGravity = 0.03; // ExperienceOrb.getDefaultGravity
constexpr float kAirDrag = 0.98f;

double nextDouble(std::mt19937& r) { return std::uniform_real_distribution<double>(0.0, 1.0)(r); }
float nextFloat(std::mt19937& r) { return std::uniform_real_distribution<float>(0.0f, 1.0f)(r); }
} // namespace

int ExperienceOrb::experienceValue(int maxValue) {
    for (int v : {2477, 1237, 617, 307, 149, 73, 37, 17, 7, 3})
        if (maxValue >= v) return v;
    return 1;
}

void ExperienceOrb::award(ServerLevel& level, double x, double y, double z, int amount) {
    std::mt19937& random = level.random();
    while (amount > 0) {
        int value = experienceValue(amount);
        amount -= value;
        // tryMergeToExisting: an orb of the same value in the same group (id % 40) takes it.
        int group = std::uniform_int_distribution<int>(0, 39)(random);
        bool merged = false;
        for (ExperienceOrb* orb : level.entitiesIn<ExperienceOrb>({x - 0.5, y - 0.5, z - 0.5, x + 0.5, y + 0.5, z + 0.5})) {
            if ((orb->id() - group) % 40 == 0 && orb->value == value) {
                orb->count++;
                orb->age = 0;
                merged = true;
                break;
            }
        }
        if (merged) continue;
        auto orb = std::make_unique<ExperienceOrb>(level.nextEntityId(), UUID::random(), x, y, z, value);
        orb->yRot = nextFloat(random) * 360.0f;
        orb->vx = (nextDouble(random) * 0.2 - 0.1) * 2.0;
        orb->vy = nextDouble(random) * 0.2 * 2.0;
        orb->vz = (nextDouble(random) * 0.2 - 0.1) * 2.0;
        // unstuckIfPossible: an orb spawned inside blocks moves to the nearest free spot within
        // half its size (bounding box size 0.5).
        if (orb->collides(level, orb->boundingBox())) {
            double h = orb->height(), size = 0.5;
            double cy = orb->y + h / 2;
            AABB allowed{orb->x - size / 2, cy - size / 2, orb->z - size / 2, orb->x + size / 2, cy + size / 2, orb->z + size / 2};
            if (auto p = findFreePosition(level, allowed, orb->x, cy, orb->z, orb->width(), h, orb->width())) {
                orb->x = (*p)[0], orb->y = (*p)[1] - h / 2, orb->z = (*p)[2];
                orb->sentX = orb->x, orb->sentY = orb->y, orb->sentZ = orb->z;
            }
        }
        level.addFreshEntity(std::move(orb));
    }
}

bool ExperienceOrb::hurt(ServerLevel& level, std::string_view type, float amount) {
    (void)level, (void)type;
    if (removed) return false; // (isInvulnerableToBase)
    health = static_cast<int>(health - amount);
    if (health <= 0) removed = true;
    return true;
}

void ExperienceOrb::tick(ServerLevel& level) {
    bool wasOnFire = isOnFire();
    baseTick(level); // (Entity.tick)
    if (removed) return;
    AABB box = boundingBox();
    bool colliding = collides(level, box);
    if (isEyeInFluid(false)) { // setUnderwaterMovement
        vx *= 0.9900000095367432;
        vy = std::min(vy + 5.000000237487257E-4, 0.05999999865889549);
        vz *= 0.9900000095367432;
    } else if (!colliding) {
        vy -= kGravity;
    }
    if (blocks::flags(level.getBlockState(static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)), static_cast<int>(std::floor(z)))) &
        blocks::kLava) {
        std::mt19937& r = level.random();
        vx = (nextFloat(r) - nextFloat(r)) * 0.2f;
        vy = 0.20000000298023224;
        vz = (nextFloat(r) - nextFloat(r)) * 0.2f;
    }
    if (tickCount % 20 == 1) scanForMerges(level);
    followNearbyPlayer(level);
    if (followingPlayer_ < 0 && colliding && collides(level, box.move(vx, vy, vz))) {
        moveTowardsClosestSpace(level, x, (box.minY + box.maxY) / 2.0, z);
        needsSync = true;
    }
    double fallSpeed = vy;
    double fromX = x, fromY = y, fromZ = z;
    move(level, vx, vy, vz);
    applyEffectsFromBlocks(level, fromX, fromY, fromZ);
    if (removed) return;
    float friction = kAirDrag;
    if (onGround) friction *= blocks::behaviour(blocks::blockOf(level.getBlockState(onPos(0.999999f)))).friction;
    vx *= friction, vy *= friction, vz *= friction;
    if (verticalCollisionBelow && fallSpeed < -kGravity) vy = -fallSpeed * 0.4;
    if (++age >= kLifetime) removed = true;
    if (wasOnFire != isOnFire()) dataChanged = true;
}

void ExperienceOrb::followNearbyPlayer(ServerLevel& level) {
    ServerPlayer* following = nullptr;
    auto players = level.players();
    for (ServerPlayer* p : players)
        if (p->id() == followingPlayer_) following = p;
    auto distSqr = [&](ServerPlayer* p) { return (p->x - x) * (p->x - x) + (p->y - y) * (p->y - y) + (p->z - z) * (p->z - z); };
    if (!following || following->gameMode == GameType::Spectator || distSqr(following) > 64.0) {
        // Level.getNearestPlayer(this, 8): the closest non-spectator within 8 blocks.
        following = nullptr;
        double best = 64.0;
        for (ServerPlayer* p : players) {
            if (p->gameMode == GameType::Spectator) continue;
            double d = distSqr(p);
            if (d < best) best = d, following = p;
        }
    }
    followingPlayer_ = following ? following->id() : -1;
    if (!following) return;
    double dx = following->x - x, dy = following->y + (following->eyeY() - following->y) / 2.0 - y, dz = following->z - z;
    double length = std::sqrt(dx * dx + dy * dy + dz * dz);
    double power = 1.0 - length / 8.0;
    if (length < 1.0e-5) return; // Vec3.normalize of a zero vector
    double scale = power * power * 0.1 / length;
    vx += dx * scale, vy += dy * scale, vz += dz * scale;
}

void ExperienceOrb::scanForMerges(ServerLevel& level) {
    for (ExperienceOrb* orb : level.entitiesIn<ExperienceOrb>(boundingBox().inflate(0.5, 0.5, 0.5))) {
        if (orb == this || (orb->id() - id()) % 40 != 0 || orb->value != value) continue;
        count += orb->count; // merge
        age = std::min(age, orb->age);
        orb->removed = true;
    }
}

void ExperienceOrb::playerTouch(ServerLevel& level, ServerPlayer& player) {
    int remaining = repairPlayerItems(level, player, value);
    if (remaining > 0) player.giveExperiencePoints(remaining);
    if (--count == 0) removed = true;
}

// ExperienceOrb.repairPlayerItems: Mending (minecraft:repair_with_xp) on a random damaged item.
int ExperienceOrb::repairPlayerItems(ServerLevel& level, ServerPlayer& player, int amount) {
    std::vector<ItemStack*> candidates; // EnchantmentHelper.getRandomItemWith (once per matching enchantment)
    for (EquipmentSlot slot : {EquipmentSlot::MainHand, EquipmentSlot::OffHand, EquipmentSlot::Feet, EquipmentSlot::Legs,
                               EquipmentSlot::Chest, EquipmentSlot::Head}) {
        ItemStack& stack = player.inventory.equipment(slot);
        if (stack.isEmpty() || items::damage(stack) <= 0) continue;
        if (enchantments::hasEffectInSlot(stack, "minecraft:repair_with_xp", slot)) candidates.push_back(&stack);
    }
    if (candidates.empty()) return amount;
    ItemStack& stack = *candidates[std::uniform_int_distribution<size_t>(0, candidates.size() - 1)(level.random())];
    int toRepair = std::max(0, static_cast<int>(enchantments::processValue(level, stack, "minecraft:repair_with_xp", static_cast<float>(amount))));
    int repair = std::min(toRepair, items::damage(stack));
    items::setDamage(stack, items::damage(stack) - repair);
    if (repair > 0) {
        int remaining = amount - repair * amount / toRepair;
        if (remaining > 0) return repairPlayerItems(level, player, remaining);
    }
    return 0;
}

bool ExperienceOrb::writeEntityData(ByteWriter& out) const {
    writeSharedFlags(out);
    out.writeByte(8);
    out.writeVarInt(1); // EntityDataSerializers.INT
    out.writeVarInt(value);
    return true;
}

nbt::Compound ExperienceOrb::save() const {
    nbt::Compound t;
    t.put("id", std::string("minecraft:experience_orb"));
    saveBase(t);
    t.put("Health", static_cast<int16_t>(health));
    t.put("Age", static_cast<int16_t>(age));
    t.put("Value", static_cast<int16_t>(value));
    t.put("Count", count);
    return t;
}

std::unique_ptr<ExperienceOrb> ExperienceOrb::load(int id, const nbt::Compound& t) {
    const nbt::List* pos = t.getList("Pos");
    if (!pos || pos->items.size() != 3) return nullptr;
    auto orb = std::make_unique<ExperienceOrb>(id, entity_nbt::uuidFrom(t.getIntArray("UUID")).value_or(UUID::random()), pos->items[0].asDouble(),
                                               pos->items[1].asDouble(), pos->items[2].asDouble(), t.getInt("Value"));
    orb->loadBase(t);
    orb->health = t.getInt("Health", 5);
    orb->age = t.getInt("Age");
    orb->count = std::max(1, t.getInt("Count", 1));
    return orb;
}

} // namespace mc
