#include "world/entity/entity.hpp"

#include <algorithm>

#include <cmath>

#include "server/level/server_level.hpp"
#include "core/registries.hpp"
#include "world/level/block/block_behaviour.hpp"
#include "world/level/material/fluids.hpp"

namespace mc {

namespace entity_nbt {
std::vector<int32_t> uuidInts(const UUID& u) {
    return {static_cast<int32_t>(u.most >> 32), static_cast<int32_t>(u.most), static_cast<int32_t>(u.least >> 32),
            static_cast<int32_t>(u.least)};
}
std::optional<UUID> uuidFrom(const std::vector<int32_t>* v) {
    if (!v || v->size() != 4) return std::nullopt;
    UUID u;
    u.most = static_cast<uint64_t>(static_cast<uint32_t>((*v)[0])) << 32 | static_cast<uint32_t>((*v)[1]);
    u.least = static_cast<uint64_t>(static_cast<uint32_t>((*v)[2])) << 32 | static_cast<uint32_t>((*v)[3]);
    return u;
}
} // namespace entity_nbt

namespace {
nbt::List doubles(std::initializer_list<double> v) {
    nbt::List l;
    for (double d : v) l.items.push_back(d);
    return l;
}
int floorInt(double v) { return static_cast<int>(std::floor(v)); }
} // namespace

BlockPos Entity::onPos(double offset) const { return {floorInt(x), floorInt(y - offset), floorInt(z)}; }

bool Entity::collides(ServerLevel& level, const AABB& box) const {
    for (const auto& b : level.collisionBoxes(box))
        if (b.intersects(box)) return true;
    return false;
}

void Entity::move(ServerLevel& level, double dx, double dy, double dz) {
    if (noPhysics) { // no collisions at all
        x += dx, y += dy, z += dz;
        horizontalCollision = verticalCollision = verticalCollisionBelow = false;
        onGround = false;
        return;
    }
    AABB box = boundingBox();
    std::vector<AABB> boxes = level.collisionBoxes(box.expandTowards(dx, dy, dz));
    double cx = dx, cy = dy, cz = dz;
    collision::collide(box, boxes, cx, cy, cz);
    x += cx, y += cy, z += cz;
    horizontalCollision = cx != dx || cz != dz;
    verticalCollision = cy != dy;
    verticalCollisionBelow = verticalCollision && dy < 0;
    onGround = verticalCollisionBelow;
    if (cx != dx) vx = 0;
    if (cz != dz) vz = 0;
    // The block under the entity (getOnPosLegacy: 0.2 below its feet) reacts to landing and stepping.
    uint32_t on = blocks::blockOf(level.getBlockState(onPos(0.2)));
    if (verticalCollision) {
        // Block.updateEntityMovementAfterFallOn: stop, or bounce back up (0.8: not a LivingEntity).
        if (vy < 0 && blocks::isInstance(on, "SlimeBlock")) vy = -vy * 0.8;
        else if (vy < 0 && blocks::isInstance(on, "BedBlock")) vy = -vy * 0.66f * 0.8;
        else vy = 0;
    }
    if (onGround && blocks::isInstance(on, "SlimeBlock") && std::abs(vy) < 0.1) {
        // SlimeBlock.stepOn: sticky.
        double slow = 0.4 + std::abs(vy) * 0.2;
        vx *= slow;
        vz *= slow;
    }
}

void Entity::moveTowardsClosestSpace(ServerLevel& level, double px, double py, double pz) {
    BlockPos pos{floorInt(px), floorInt(py), floorInt(pz)};
    double delta[3] = {px - pos.x, py - pos.y, pz - pos.z};
    Direction closestDirection = Direction::Up;
    double closest = 1.7976931348623157E308;
    for (Direction d : {Direction::North, Direction::South, Direction::West, Direction::East, Direction::Up}) {
        if (blocks::isCollisionFullBlock(level.getBlockState(pos.relative(d)))) continue;
        double v = delta[static_cast<int>(direction::axis(d))];
        bool positive = direction::stepX(d) + direction::stepY(d) + direction::stepZ(d) > 0;
        double oriented = positive ? 1.0 - v : v;
        if (oriented < closest) closest = oriented, closestDirection = d;
    }
    float speed = std::uniform_real_distribution<float>(0.0f, 1.0f)(level.random()) * 0.2f + 0.1f;
    float step = static_cast<float>(direction::stepX(closestDirection) + direction::stepY(closestDirection) + direction::stepZ(closestDirection));
    vx *= 0.75, vy *= 0.75, vz *= 0.75;
    switch (direction::axis(closestDirection)) {
    case Axis::X: vx = step * speed; break;
    case Axis::Y: vy = step * speed; break;
    case Axis::Z: vz = step * speed; break;
    }
}

void Entity::saveBase(nbt::Compound& t) const {
    t.put("Pos", doubles({x, y, z}));
    t.put("Motion", doubles({vx, vy, vz}));
    nbt::List rot;
    rot.items.push_back(yRot);
    rot.items.push_back(xRot);
    t.put("Rotation", std::move(rot));
    t.put("fall_distance", 0.0);
    t.put("Fire", static_cast<int16_t>(remainingFireTicks));
    t.put("Air", static_cast<int16_t>(300));
    t.put("OnGround", nbt::byteTag(onGround));
    t.put("Invulnerable", nbt::byteTag(false));
    t.put("PortalCooldown", 0);
    t.put("UUID", entity_nbt::uuidInts(uuid_));
}

void Entity::loadBase(const nbt::Compound& t) {
    if (const nbt::List* m = t.getList("Motion"); m && m->items.size() == 3)
        vx = m->items[0].asDouble(), vy = m->items[1].asDouble(), vz = m->items[2].asDouble();
    if (const nbt::List* r = t.getList("Rotation"); r && r->items.size() == 2)
        yRot = static_cast<float>(r->items[0].asDouble()), xRot = static_cast<float>(r->items[1].asDouble());
    onGround = t.getBool("OnGround");
    if (t.contains("Fire")) remainingFireTicks = t.getInt("Fire");
}

std::optional<std::array<double, 3>> Entity::findFreePosition(ServerLevel& level, const AABB& allowed, double px, double py, double pz, double sx,
                                                              double sy, double sz) {
    // The block collisions around, grown by half the entity's size: centres inside them collide.
    std::vector<AABB> blocked;
    for (const AABB& b : level.collisionBoxes(allowed.inflate(sx, sy, sz))) blocked.push_back(b.inflate(sx / 2, sy / 2, sz / 2));
    // Discretize like VoxelShape's IndexMerger: every edge inside the allowed box splits it.
    auto axis = [&](double lo, double hi, auto minOf, auto maxOf) {
        std::vector<double> c{lo, hi};
        for (const AABB& b : blocked)
            for (double v : {minOf(b), maxOf(b)})
                if (v > lo && v < hi) c.push_back(v);
        std::sort(c.begin(), c.end());
        c.erase(std::unique(c.begin(), c.end()), c.end());
        return c;
    };
    auto xs = axis(allowed.minX, allowed.maxX, [](const AABB& b) { return b.minX; }, [](const AABB& b) { return b.maxX; });
    auto ys = axis(allowed.minY, allowed.maxY, [](const AABB& b) { return b.minY; }, [](const AABB& b) { return b.maxY; });
    auto zs = axis(allowed.minZ, allowed.maxZ, [](const AABB& b) { return b.minZ; }, [](const AABB& b) { return b.maxZ; });
    std::optional<std::array<double, 3>> best;
    double bestDist = 0;
    for (size_t i = 0; i + 1 < xs.size(); i++)
        for (size_t j = 0; j + 1 < ys.size(); j++)
            for (size_t k = 0; k + 1 < zs.size(); k++) {
                double mx = (xs[i] + xs[i + 1]) / 2, my = (ys[j] + ys[j + 1]) / 2, mz = (zs[k] + zs[k + 1]) / 2;
                bool free = true;
                for (const AABB& b : blocked)
                    if (mx > b.minX && mx < b.maxX && my > b.minY && my < b.maxY && mz > b.minZ && mz < b.maxZ) {
                        free = false;
                        break;
                    }
                if (!free) continue;
                // closestPointTo: the point clamped into the cell; the first nearest one wins
                double cx = std::clamp(px, xs[i], xs[i + 1]), cy = std::clamp(py, ys[j], ys[j + 1]), cz = std::clamp(pz, zs[k], zs[k + 1]);
                double d = (cx - px) * (cx - px) + (cy - py) * (cy - py) + (cz - pz) * (cz - pz);
                if (!best || d < bestDist) best = std::array<double, 3>{cx, cy, cz}, bestDist = d;
            }
    return best;
}

// ---------------------------------------------------------------- fluids and fire

void Entity::playSound(ServerLevel& level, std::string_view sound, float volume, float pitch) {
    level.playSound(-1, x, y, z, registries::id("minecraft:sound_event", sound).value_or(-1), soundSource(), volume, pitch);
}

namespace {
float nextFloat(std::mt19937& r) { return std::uniform_real_distribution<float>(0.0f, 1.0f)(r); }
} // namespace

void Entity::doWaterSplashEffect(ServerLevel& level) {
    // (no passengers here)
    float speed = std::min(1.0f, static_cast<float>(std::sqrt(vx * vx * 0.2f + vy * vy + vz * vz * 0.2f)) * 0.2f);
    auto& r = level.random();
    playSound(level, "minecraft:entity.generic.splash", speed, 1.0f + (nextFloat(r) - nextFloat(r)) * 0.4f);
}

bool Entity::updateFluidInteraction(ServerLevel& level) {
    // EntityFluidInteraction.update: the fluids in the (slightly shrunk) bounding box, how high
    // they reach above its bottom, whether the eyes are in them, and the sum of their flows.
    fluidHeight_[0] = fluidHeight_[1] = 0.0;
    eyeInFluid_[0] = eyeInFluid_[1] = false;
    double typeHeight[5] = {0, 0, 0, 0, 0}; // (a tracker per fluid type, as vanilla keeps them)
    struct Current {
        double height = 0, x = 0, y = 0, z = 0;
        int count = 0;
    } current[2];
    AABB box = boundingBox().inflate(-0.001, -0.001, -0.001);
    int x0 = floorInt(box.minX), y0 = floorInt(box.minY), z0 = floorInt(box.minZ);
    int x1 = static_cast<int>(std::ceil(box.maxX)) - 1, y1 = static_cast<int>(std::ceil(box.maxY)) - 1, z1 = static_cast<int>(std::ceil(box.maxZ)) - 1;
    bool loaded = true; // hasFluidAndLoaded (the chunks around must be loaded)
    for (int cx = (x0 - 1) >> 4; cx <= (x1 + 1) >> 4 && loaded; cx++)
        for (int cz = (z0 - 1) >> 4; cz <= (z1 + 1) >> 4 && loaded; cz++) loaded = level.getChunkIfLoaded({cx, cz}) != nullptr;
    bool any = false;
    if (loaded) {
        double eyeY = y + eyeHeight();
        int eyeX = floorInt(x), eyeZ = floorInt(z);
        for (int bx = x0; bx <= x1; bx++)
            for (int by = y0; by <= y1; by++)
                for (int bz = z0; bz <= z1; bz++) {
                    if (by < level.minY() || by > level.maxY()) continue;
                    BlockPos p{bx, by, bz};
                    fluids::FluidState fluid = level.getFluidState(p);
                    if (fluid.isEmpty()) continue;
                    double bottom = by, top = bottom + fluids::height(level, p, fluid);
                    if (top < box.minY) continue;
                    any = true;
                    int family = fluids::isLava(fluid.type) ? 1 : 0;
                    if (bx == eyeX && bz == eyeZ && eyeY >= bottom) {
                        // FluidState.getHeightForCamera: a source under a sturdy bottom face fills the block
                        BlockStateId above = level.getBlockState(p.above());
                        double cameraTop = fluid.isSource() && blocks::isFaceSturdy(above, Direction::Down) ? bottom + 1.0 : top;
                        if (eyeY <= cameraTop) eyeInFluid_[family] = true;
                    }
                    typeHeight[fluid.type] = std::max(top - box.minY, typeHeight[fluid.type]);
                    fluidHeight_[family] = std::max(fluidHeight_[family], typeHeight[fluid.type]);
                    if (isPushedByFluid()) {
                        auto flow = fluids::flow(level, p, fluid);
                        Current& c = current[family];
                        c.height = std::max(typeHeight[fluid.type], c.height);
                        double scale = c.height < 0.4 ? c.height : 1.0;
                        c.x += flow[0] * scale, c.y += flow[1] * scale, c.z += flow[2] * scale;
                        c.count++;
                    }
                }
    }
    // Entity.updateFluidInteraction
    bool inWater = fluidHeight_[0] > 0.0, inLava = fluidHeight_[1] > 0.0;
    if (inWater && !wasTouchingWater_ && !firstTick_) doWaterSplashEffect(level);
    wasTouchingWater_ = inWater;
    auto applyCurrent = [&](const Current& c, double scale) { // CurrentAccumulator.applyTo (not a player)
        if (c.count == 0 || c.x * c.x + c.y * c.y + c.z * c.z < 1.0e-5f) return;
        double len = std::sqrt(c.x * c.x + c.y * c.y + c.z * c.z); // (normalize)
        double ix = c.x / len * scale, iy = c.y / len * scale, iz = c.z / len * scale;
        if (std::abs(vx) < 0.003 && std::abs(vz) < 0.003) {
            double ilen = std::sqrt(ix * ix + iy * iy + iz * iz);
            if (ilen < 0.0045000000000000005 && ilen >= 1.0e-5f) {
                double k = 0.0045000000000000005 / ilen;
                ix *= k, iy *= k, iz *= k;
            } else if (ilen < 0.0045000000000000005) {
                ix = iy = iz = 0;
            }
        }
        vx += ix, vy += iy, vz += iz;
    };
    if (isPushedByFluid()) {
        if (inWater) applyCurrent(current[0], 0.014);
        if (inLava) applyCurrent(current[1], level.fastLava() ? 0.007 : 0.0023333333333333335);
    }
    return any;
}

void Entity::baseTick(ServerLevel& level) {
    updateFluidInteraction(level);
    if (remainingFireTicks > 0) {
        if (fireImmune()) {
            remainingFireTicks = std::min(0, remainingFireTicks); // clearFire
        } else {
            if (remainingFireTicks % 20 == 0 && !isInLava()) hurt(level, "minecraft:on_fire", 1.0f);
            remainingFireTicks--;
        }
    }
    if (y < level.minY() - 64) removed = true; // checkBelowWorld -> onBelowWorld
    firstTick_ = false;
}

void Entity::applyEffectsFromBlocks(ServerLevel& level, double fx, double fy, double fz) {
    if (removed || noPhysics) return; // isAffectedByBlocks
    bool wasOnFire = isOnFire();
    int fireTicksBefore = remainingFireTicks;
    // checkInsideBlocks with the StepBasedCollector, as one step: what each block does, then the
    // effects in InsideBlockEffectType order with the hurts registered after them.
    bool fireIgnite = false, lavaIgnite = false, extinguish = false;
    std::vector<float> fireHurts;
    int lavaHurts = 0;
    AABB box = boundingBox().inflate(-1.0e-5, -1.0e-5, -1.0e-5);
    AABB from = box.move(fx - x, fy - y, fz - z);
    AABB swept{std::min(box.minX, from.minX), std::min(box.minY, from.minY), std::min(box.minZ, from.minZ),
               std::max(box.maxX, from.maxX), std::max(box.maxY, from.maxY), std::max(box.maxZ, from.maxZ)};
    for (int bx = floorInt(swept.minX); bx <= floorInt(swept.maxX); bx++)
        for (int by = std::max(level.minY(), floorInt(swept.minY)); by <= std::min(level.maxY(), floorInt(swept.maxY)); by++)
            for (int bz = floorInt(swept.minZ); bz <= floorInt(swept.maxZ); bz++) {
                BlockPos p{bx, by, bz};
                BlockStateId state = level.getBlockState(p);
                if (blocks::isAir(state)) continue;
                uint32_t b = blocks::blockOf(state);
                AABB blockBox{static_cast<double>(bx), static_cast<double>(by), static_cast<double>(bz), bx + 1.0, by + 1.0, bz + 1.0};
                if (blocks::isInstance(b, "BaseFireBlock") && blockBox.intersects(swept)) { // BaseFireBlock.entityInside
                    fireIgnite = true;
                    fireHurts.push_back(b == gen::block::SOUL_FIRE ? 2.0f : 1.0f);
                }
                fluids::FluidState fluid = fluids::stateOf(state);
                if (fluid.isEmpty()) continue;
                AABB fluidBox{static_cast<double>(bx), static_cast<double>(by), static_cast<double>(bz), bx + 1.0, by + fluids::height(level, p, fluid), bz + 1.0};
                if (!fluidBox.intersects(swept)) continue; // collidedWithFluid
                if (fluids::isLava(fluid.type)) { // LavaFluid.entityInside
                    lavaIgnite = true;
                    lavaHurts++;
                } else {
                    extinguish = true; // WaterFluid.entityInside
                }
            }
    auto& r = level.random();
    if (fireIgnite && !fireImmune()) { // BaseFireBlock.fireIgnite
        if (remainingFireTicks < 0) remainingFireTicks++;
        if (remainingFireTicks >= 0 && remainingFireTicks < 160) remainingFireTicks = 160; // igniteForSeconds(8)
    }
    for (float damage : fireHurts)
        if (!removed) hurt(level, "minecraft:in_fire", damage);
    if (lavaIgnite && !removed && !fireImmune() && remainingFireTicks < 300) remainingFireTicks = 300; // lavaIgnite: igniteForSeconds(15)
    for (int i = 0; i < lavaHurts && !removed; i++) { // Entity.lavaHurt
        if (fireImmune()) break;
        if (hurt(level, "minecraft:lava", 4.0f) && shouldPlayLavaHurtSound()) playSound(level, "minecraft:entity.generic.burn", 0.4f, 2.0f + nextFloat(r) * 0.4f);
    }
    if (extinguish && !removed) remainingFireTicks = std::min(0, remainingFireTicks); // clearFire
    if (wasOnFire && !isOnFire()) playSound(level, "minecraft:entity.generic.extinguish_fire", 0.7f, 1.6f + (nextFloat(r) - nextFloat(r)) * 0.4f);
    bool ignitedThisTick = remainingFireTicks > fireTicksBefore;
    if (!isOnFire() && !ignitedThisTick) remainingFireTicks = -1; // -getFireImmuneTicks()
    if (wasOnFire != isOnFire()) dataChanged = true; // (the shared on-fire flag)
    if (isOnFire()) everOnFire = true;
}

} // namespace mc
