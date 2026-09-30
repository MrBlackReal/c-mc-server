#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace mc {

// Port of net.minecraft.core.Direction; values are the 3D data values (network ids).
enum class Direction : uint8_t { Down = 0, Up = 1, North = 2, South = 3, West = 4, East = 5 };
enum class Axis : uint8_t { X, Y, Z };

namespace direction {

inline constexpr std::array<Direction, 6> kAll = {Direction::Down, Direction::Up, Direction::North,
                                                   Direction::South, Direction::West, Direction::East};
inline constexpr std::array<Direction, 4> kHorizontal = {Direction::North, Direction::South, Direction::West, Direction::East};

constexpr int stepX(Direction d) { return d == Direction::East ? 1 : d == Direction::West ? -1 : 0; }
constexpr int stepY(Direction d) { return d == Direction::Up ? 1 : d == Direction::Down ? -1 : 0; }
constexpr int stepZ(Direction d) { return d == Direction::South ? 1 : d == Direction::North ? -1 : 0; }

constexpr Direction opposite(Direction d) { return static_cast<Direction>(static_cast<uint8_t>(d) ^ 1); }
constexpr Axis axis(Direction d) {
    return d == Direction::West || d == Direction::East ? Axis::X : d == Direction::Up || d == Direction::Down ? Axis::Y : Axis::Z;
}
constexpr bool isHorizontal(Direction d) { return axis(d) != Axis::Y; }

constexpr Direction clockWise(Direction d) {
    switch (d) {
    case Direction::North: return Direction::East;
    case Direction::East: return Direction::South;
    case Direction::South: return Direction::West;
    case Direction::West: return Direction::North;
    default: return d;
    }
}
constexpr Direction counterClockWise(Direction d) { return opposite(clockWise(d)); }

constexpr std::string_view name(Direction d) {
    constexpr std::string_view names[] = {"down", "up", "north", "south", "west", "east"};
    return names[static_cast<int>(d)];
}
constexpr std::string_view axisName(Axis a) { return a == Axis::X ? "x" : a == Axis::Y ? "y" : "z"; }
inline Direction byName(std::string_view n) {
    for (Direction d : kAll)
        if (name(d) == n) return d;
    return Direction::North;
}

// Direction.toYRot: south 0, west 90, north 180, east 270 (horizontal directions only).
inline float toYRot(Direction d) {
    switch (d) {
    case Direction::West: return 90.0f;
    case Direction::North: return 180.0f;
    case Direction::East: return 270.0f;
    default: return 0.0f;
    }
}

// Direction.fromYRot
inline Direction fromYRot(double yRot) {
    constexpr Direction by2D[] = {Direction::South, Direction::West, Direction::North, Direction::East};
    return by2D[static_cast<int>(std::floor(yRot / 90.0 + 0.5)) & 3];
}

// Direction.orderedByNearest: the six directions ordered by how closely the view vector matches.
inline std::array<Direction, 6> orderedByNearest(float yRot, float xRot) {
    constexpr float kDeg = 3.14159265358979323846f / 180.0f;
    float pitch = xRot * kDeg;
    float yaw = -yRot * kDeg;
    float pitchSin = std::sin(pitch), pitchCos = std::cos(pitch);
    float yawSin = std::sin(yaw), yawCos = std::cos(yaw);
    bool xPos = yawSin > 0.0f, yPos = pitchSin < 0.0f, zPos = yawCos > 0.0f;
    float xYaw = xPos ? yawSin : -yawSin;
    float yMag = yPos ? -pitchSin : pitchSin;
    float zYaw = zPos ? yawCos : -yawCos;
    float xMag = xYaw * pitchCos, zMag = zYaw * pitchCos;
    Direction ax = xPos ? Direction::East : Direction::West;
    Direction ay = yPos ? Direction::Up : Direction::Down;
    Direction az = zPos ? Direction::South : Direction::North;
    auto make = [](Direction a, Direction b, Direction c) {
        return std::array<Direction, 6>{a, b, c, opposite(c), opposite(b), opposite(a)};
    };
    if (xYaw > zYaw) {
        if (yMag > xMag) return make(ay, ax, az);
        return zMag > yMag ? make(ax, az, ay) : make(ax, ay, az);
    }
    if (yMag > zMag) return make(ay, az, ax);
    return xMag > yMag ? make(az, ax, ay) : make(az, ay, ax);
}

} // namespace direction

struct BlockPos {
    int x = 0, y = 0, z = 0;

    BlockPos relative(Direction d, int n = 1) const {
        return {x + direction::stepX(d) * n, y + direction::stepY(d) * n, z + direction::stepZ(d) * n};
    }
    BlockPos above(int n = 1) const { return {x, y + n, z}; }
    BlockPos below(int n = 1) const { return {x, y - n, z}; }
    BlockPos north(int n = 1) const { return {x, y, z - n}; }
    BlockPos south(int n = 1) const { return {x, y, z + n}; }
    BlockPos west(int n = 1) const { return {x - n, y, z}; }
    BlockPos east(int n = 1) const { return {x + n, y, z}; }
    BlockPos offset(int dx, int dy, int dz) const { return {x + dx, y + dy, z + dz}; }
    bool operator==(const BlockPos&) const = default;

    int64_t asLong() const {
        return (static_cast<int64_t>(x) & 0x3FFFFFF) << 38 | (static_cast<int64_t>(z) & 0x3FFFFFF) << 12 | (static_cast<int64_t>(y) & 0xFFF);
    }
    static BlockPos fromLong(int64_t packed) {
        return {static_cast<int>(packed >> 38), static_cast<int>(packed << 52 >> 52), static_cast<int>(packed << 26 >> 38)};
    }
};

} // namespace mc
