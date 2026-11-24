#pragma once

#include <array>
#include <optional>

#include <optional>
#include <string_view>

#include "nbt/tag.hpp"
#include "util/uuid.hpp"
#include "world/level/chunk/level_chunk.hpp"
#include "world/phys/aabb.hpp"

namespace mc {

class ServerLevel;

// The parts of net.minecraft.world.entity.Entity that the simulated non-player entities share:
// position and motion, collision movement, the common saved fields and what ServerEntity needs
// to track them for clients.
class Entity {
public:
    Entity(int id, UUID uuid, double x, double y, double z) : x(x), y(y), z(z), sentX(x), sentY(y), sentZ(z), id_(id), uuid_(uuid) {}
    virtual ~Entity() = default;

    virtual std::string_view type() const = 0; // minecraft:entity_type id
    virtual double width() const = 0;
    virtual double height() const = 0;
    virtual void tick(ServerLevel& level) = 0;
    // Entity.saveWithoutId plus the type's own fields, with "id".
    virtual nbt::Compound save() const = 0;
    // ClientboundAddEntityPacket's data field (block state of a falling block, ...).
    virtual int spawnData() const { return 0; }
    // SynchedEntityData entries clients need beyond the defaults, as written in
    // ClientboundSetEntityDataPacket (without the 0xff terminator). Returns false if none.
    virtual bool writeEntityData(ByteWriter& /*out*/) const { return false; }
    // EntityType.clientTrackingRange, in chunks.
    virtual int trackingRange() const { return 6; }

    int id() const { return id_; }
    const UUID& uuid() const { return uuid_; }
    AABB boundingBox() const { return {x - width() / 2, y, z - width() / 2, x + width() / 2, y + height(), z + width() / 2}; }
    ChunkPos chunkPos() const { return ChunkPos::ofBlock(x, z); }

    double x, y, z;
    double vx = 0, vy = 0, vz = 0;
    float yRot = 0, xRot = 0;
    bool onGround = false;
    bool horizontalCollision = false, verticalCollision = false, verticalCollisionBelow = false;
    bool noPhysics = false;
    int tickCount = 0;
    bool removed = false;

    // Fire (Entity.remainingFireTicks: below 0 while fire can't take hold yet).
    int remainingFireTicks = -1;
    bool isOnFire() const { return !fireImmune() && remainingFireTicks > 0; }
    // SynchedEntityData sends only values that differ from the default: the shared flags (on
    // fire) once the entity has burned.
    bool everOnFire = false;
    void writeSharedFlags(ByteWriter& out) const {
        if (!isOnFire() && !everOnFire) return;
        out.writeByte(0); // Entity.DATA_SHARED_FLAGS_ID, EntityDataSerializers.BYTE
        out.writeVarInt(0);
        out.writeByte(isOnFire() ? 1 : 0);
    }
    virtual bool fireImmune() const { return false; }
    // Entity.hurtServer with a damage source of minecraft:damage_type `type` (no attacker).
    virtual bool hurt(ServerLevel& /*level*/, std::string_view /*type*/, float /*amount*/) { return false; }
    // EntityFluidInteraction: how deep in water / lava the entity is (as of the last update).
    bool isInWater() const { return wasTouchingWater_; }
    bool isInLava() const { return !firstTick_ && fluidHeight_[1] > 0.0; }
    double fluidHeight(bool lava) const { return fluidHeight_[lava ? 1 : 0]; }
    bool isEyeInFluid(bool lava) const { return eyeInFluid_[lava ? 1 : 0]; }

    // ServerEntity bookkeeping.
    bool needsSync = false;   // velocity changed a lot since the last update
    bool dataChanged = false; // entity data changed since it was sent
    double sentX = 0, sentY = 0, sentZ = 0;

protected:
    // Entity.move(MoverType.SELF): collide with blocks, then the landing block's reaction
    // (Block.updateEntityMovementAfterFallOn) and SlimeBlock.stepOn.
    void move(ServerLevel& level, double dx, double dy, double dz);
    // !Level.noCollision(this, box)
    bool collides(ServerLevel& level, const AABB& box) const;
    // CollisionGetter.findFreePosition: the point in `allowedCenters` closest to `preferred` where a
    // box of the given size doesn't overlap blocks (VoxelShape join ONLY_FIRST + closestPointTo).
    static std::optional<std::array<double, 3>> findFreePosition(ServerLevel& level, const AABB& allowedCenters, double px, double py, double pz,
                                                                 double sizeX, double sizeY, double sizeZ);
    // Entity.moveTowardsClosestSpace
    void moveTowardsClosestSpace(ServerLevel& level, double px, double py, double pz);
    // Entity.getOnPos(offset): the block at the feet, `offset` below them.
    BlockPos onPos(double offset) const;

    // Entity.baseTick: fluids (with their currents), burning, falling out of the world.
    void baseTick(ServerLevel& level);
    // Entity.updateFluidInteraction: measures the fluids the entity is in and lets their current
    // push it. True if it's in any fluid.
    bool updateFluidInteraction(ServerLevel& level);
    // Entity.applyEffectsFromBlocks after moving from (fx, fy, fz): fire and lava set it alight
    // and hurt it, water puts it out. (Only the blocks at the end of the move are looked at, not
    // the whole path.)
    void applyEffectsFromBlocks(ServerLevel& level, double fx, double fy, double fz);
    virtual double eyeHeight() const { return height() * 0.85; } // EntityDimensions' default
    virtual bool isPushedByFluid() const { return true; }
    virtual int soundSource() const { return 6; }                 // Entity.getSoundSource: NEUTRAL
    virtual bool shouldPlayLavaHurtSound() const { return true; }
    virtual void doWaterSplashEffect(ServerLevel& level);
    void playSound(ServerLevel& level, std::string_view sound, float volume, float pitch);

    bool firstTick_ = true;
    bool wasTouchingWater_ = false;
    double fluidHeight_[2] = {0, 0}; // water, lava
    bool eyeInFluid_[2] = {false, false};

    // The fields Entity.saveWithoutId writes for every entity, and reading them back.
    void saveBase(nbt::Compound& tag) const;
    void loadBase(const nbt::Compound& tag);

    int id_;
    UUID uuid_;
};

namespace entity_nbt {
std::vector<int32_t> uuidInts(const UUID& u);
std::optional<UUID> uuidFrom(const std::vector<int32_t>* v);
} // namespace entity_nbt

} // namespace mc
