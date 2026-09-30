#pragma once

#include <algorithm>
#include <vector>

namespace mc {

// Port of net.minecraft.world.phys.AABB (the parts entity movement needs).
struct AABB {
    double minX, minY, minZ, maxX, maxY, maxZ;

    AABB inflate(double x, double y, double z) const { return {minX - x, minY - y, minZ - z, maxX + x, maxY + y, maxZ + z}; }
    AABB move(double x, double y, double z) const { return {minX + x, minY + y, minZ + z, maxX + x, maxY + y, maxZ + z}; }
    AABB expandTowards(double x, double y, double z) const {
        return {x < 0 ? minX + x : minX, y < 0 ? minY + y : minY, z < 0 ? minZ + z : minZ,
                x > 0 ? maxX + x : maxX, y > 0 ? maxY + y : maxY, z > 0 ? maxZ + z : maxZ};
    }
    bool intersects(const AABB& o) const {
        return minX < o.maxX && maxX > o.minX && minY < o.maxY && maxY > o.minY && minZ < o.maxZ && maxZ > o.minZ;
    }
};

namespace collision {

constexpr double kEpsilon = 1.0e-7; // Shapes/VoxelShape collide tolerance

// Shapes.collide along one axis: how far `box` can move by `d` before hitting any of `boxes`.
inline double collideX(const AABB& box, const std::vector<AABB>& boxes, double d) {
    for (const auto& b : boxes) {
        if (b.maxY <= box.minY + kEpsilon || b.minY >= box.maxY - kEpsilon || b.maxZ <= box.minZ + kEpsilon || b.minZ >= box.maxZ - kEpsilon)
            continue;
        if (d > 0 && b.minX >= box.maxX - kEpsilon) d = std::min(d, b.minX - box.maxX);
        else if (d < 0 && b.maxX <= box.minX + kEpsilon) d = std::max(d, b.maxX - box.minX);
    }
    return d;
}
inline double collideY(const AABB& box, const std::vector<AABB>& boxes, double d) {
    for (const auto& b : boxes) {
        if (b.maxX <= box.minX + kEpsilon || b.minX >= box.maxX - kEpsilon || b.maxZ <= box.minZ + kEpsilon || b.minZ >= box.maxZ - kEpsilon)
            continue;
        if (d > 0 && b.minY >= box.maxY - kEpsilon) d = std::min(d, b.minY - box.maxY);
        else if (d < 0 && b.maxY <= box.minY + kEpsilon) d = std::max(d, b.maxY - box.minY);
    }
    return d;
}
inline double collideZ(const AABB& box, const std::vector<AABB>& boxes, double d) {
    for (const auto& b : boxes) {
        if (b.maxX <= box.minX + kEpsilon || b.minX >= box.maxX - kEpsilon || b.maxY <= box.minY + kEpsilon || b.minY >= box.maxY - kEpsilon)
            continue;
        if (d > 0 && b.minZ >= box.maxZ - kEpsilon) d = std::min(d, b.minZ - box.maxZ);
        else if (d < 0 && b.maxZ <= box.minZ + kEpsilon) d = std::max(d, b.maxZ - box.minZ);
    }
    return d;
}

inline AABB deflate(const AABB& a, double d) { return a.inflate(-d, -d, -d); }

// Entity.collideWithShapes: Y first, then the horizontal axes, larger movement last.
inline void collide(const AABB& box, const std::vector<AABB>& boxes, double& dx, double& dy, double& dz) {
    AABB b = box;
    if (dy != 0) {
        dy = collideY(b, boxes, dy);
        b = b.move(0, dy, 0);
    }
    bool zFirst = std::abs(dx) < std::abs(dz);
    if (zFirst && dz != 0) {
        dz = collideZ(b, boxes, dz);
        b = b.move(0, 0, dz);
    }
    if (dx != 0) {
        dx = collideX(b, boxes, dx);
        b = b.move(dx, 0, 0);
    }
    if (!zFirst && dz != 0) dz = collideZ(b, boxes, dz);
}

// Entity.collide for an entity with a step height (players: 0.6): if a horizontal move is
// blocked while on the ground, try stepping up onto what's in the way. `collidersFor` returns the
// block collision boxes intersecting a box.
template <class CollidersFor>
void collideWithStepUp(const AABB& box, bool onGround, double maxUpStep, double& dx, double& dy, double& dz, CollidersFor collidersFor) {
    double mx = dx, my = dy, mz = dz;
    double cx = dx, cy = dy, cz = dz;
    if (mx * mx + my * my + mz * mz != 0) collide(box, collidersFor(box.expandTowards(mx, my, mz)), cx, cy, cz);
    bool xc = mx != cx, yc = my != cy, zc = mz != cz;
    bool onGroundAfter = yc && my < 0;
    if (maxUpStep > 0 && (onGroundAfter || onGround) && (xc || zc)) {
        AABB grounded = onGroundAfter ? box.move(0, cy, 0) : box;
        AABB stepUp = grounded.expandTowards(mx, maxUpStep, mz);
        if (!onGroundAfter) stepUp = stepUp.expandTowards(0, -9.999999747378752E-6, 0);
        std::vector<AABB> colliders = collidersFor(stepUp);
        // collectCandidateStepUpHeights: box edges between the entity's feet and the step height.
        float skip = static_cast<float>(cy);
        std::vector<float> heights;
        for (const auto& c : colliders)
            for (double coord : {c.minY, c.maxY}) {
                float rel = static_cast<float>(coord - grounded.minY);
                if (rel < 0.0f || rel == skip || rel > static_cast<float>(maxUpStep)) continue;
                if (std::find(heights.begin(), heights.end(), rel) == heights.end()) heights.push_back(rel);
            }
        std::sort(heights.begin(), heights.end());
        for (float h : heights) {
            double sx = mx, sy = h, sz = mz;
            collide(grounded, colliders, sx, sy, sz);
            if (sx * sx + sz * sz > cx * cx + cz * cz) {
                dx = sx, dy = sy - (box.minY - grounded.minY), dz = sz;
                return;
            }
        }
    }
    dx = cx, dy = cy, dz = cz;
}

} // namespace collision
} // namespace mc
