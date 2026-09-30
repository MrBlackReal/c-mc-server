#include "world/entity/entity.hpp"

#include <algorithm>

#include <cmath>

#include "server/level/server_level.hpp"

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
    t.put("Fire", static_cast<int16_t>(0));
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

} // namespace mc
