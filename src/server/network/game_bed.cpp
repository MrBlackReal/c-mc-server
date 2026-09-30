// Beds: BedBlock.useWithoutItem, ServerPlayer.startSleepInBed / stopSleepInBed, the spawn point
// and (in PlayerList) ServerLevel's night skip once everyone sleeps.
#include <cmath>
#include <numbers>

#include "core/registries.hpp"
#include "generated/behaviour.hpp"
#include "generated/packet_ids.hpp"
#include "network/chat/component.hpp"
#include "server/minecraft_server.hpp"
#include "server/network/listeners.hpp"
#include "util/json.hpp"
#include "world/level/block/block_state.hpp"

namespace mc {

namespace cb = packets::play_cb;
namespace bs = blocks;

namespace {

} // namespace

// Level.isDarkOutside for the overworld: skyDarken = 15 - sky light level, bright below 4.
bool ServerGamePacketListener::isDarkOutside() {
    auto* clock = server_.clockManager().byName("minecraft:overworld");
    if (!clock) return false;
    int skyDarken = static_cast<int>(15.0f - 15.0f * ServerClockManager::skyLightMultiplier(clock->totalTicks));
    return skyDarken >= 4; // (no weather yet: rain would darken it)
}

// BedBlock.useWithoutItem
UseResult ServerGamePacketListener::useBed(BlockPos pos, BlockStateId state) {
    ServerLevel& level = player_.level();
    uint32_t block = bs::blockOf(state);
    Direction facing = direction::byName(bs::getValue(state, "facing").value_or("north"));
    bool head = bs::getValue(state, "part") == "head";
    BlockPos otherPos = pos.relative(head ? direction::opposite(facing) : facing);
    BlockStateId other = level.getBlockState(otherPos);
    if (bs::blockOf(other) != block || bs::getValue(other, "part") == bs::getValue(state, "part")) return UseResult::Consume;
    if (!head) {
        pos = otherPos;
        state = other;
    }
    // The overworld's bed rule (EnvironmentAttributes.BED_RULE): sleep when dark, always set the spawn.
    // TODO: other dimensions (beds explode in the Nether and the End).
    if (bs::getBool(state, "occupied")) {
        sendOverlay(Component::translatable("block.minecraft.bed.occupied"));
        return UseResult::SuccessServer;
    }
    if (player_.sleepingPos) return UseResult::SuccessServer; // (already sleeping: OTHER_PROBLEM)
    // ServerPlayer.startSleepInBed: close enough, room above, then the spawn point, then darkness.
    auto reachable = [&](BlockPos b) {
        return std::abs(player_.x - (b.x + 0.5)) <= 3.0 && std::abs(player_.y - b.y) <= 2.0 && std::abs(player_.z - (b.z + 0.5)) <= 3.0;
    };
    if (!reachable(pos) && !reachable(pos.relative(direction::opposite(facing)))) {
        sendOverlay(Component::translatable("block.minecraft.bed.too_far_away"));
        return UseResult::SuccessServer;
    }
    auto freeAt = [&](BlockPos b) { return !bs::isCollisionFullBlock(level.getBlockState(b)); }; // !isSuffocating
    if (!freeAt(pos.above()) || !freeAt(pos.above().relative(direction::opposite(facing)))) {
        sendOverlay(Component::translatable("block.minecraft.bed.obstructed"));
        return UseResult::SuccessServer;
    }
    ServerPlayer::RespawnConfig spawn{std::string(level.dimension()), pos, player_.yRot, player_.xRot, false};
    if (!player_.respawn || !player_.respawn->samePosition(spawn)) sendSystemMessage(Component::translatable("block.minecraft.set_spawn"));
    player_.respawn = spawn;
    if (!isDarkOutside()) {
        sendOverlay(Component::translatable("block.minecraft.bed.no_sleep"));
        return UseResult::SuccessServer;
    }
    // (monsters nearby would keep a survival player awake: there are no monsters yet)
    // LivingEntity.startSleeping: lie down on the bed.
    double sleepHeight = 0;
    for (const auto& b : gen::kShapes[gen::kStateShape[state]]) sleepHeight = std::max<double>(sleepHeight, b.maxY);
    player_.x = pos.x + 0.5, player_.y = pos.y + sleepHeight + 0.125, player_.z = pos.z + 0.5;
    level.setBlock(pos, bs::setBool(state, "occupied", true));
    player_.sleepingPos = pos;
    player_.sleepCounter = 0;
    server_.playerList().broadcastPlayerData(player_);
    server_.playerList().updateSleepingPlayerList();
    return UseResult::SuccessServer;
}

// ServerPlayer.stopSleepInBed: out of bed, standing next to it.
void ServerGamePacketListener::stopSleepInBed(bool forcefulWakeUp, bool updateLevelList) {
    if (!player_.sleepingPos) return;
    Packet anim(cb::ANIMATE); // ClientboundAnimatePacket WAKE_UP, to everyone who sees the player and the player
    anim.writeVarInt(player_.id());
    anim.writeByte(0);
    server_.playerList().broadcastAll(anim);
    ServerLevel& level = player_.level();
    BlockPos bed = *player_.sleepingPos;
    BlockStateId state = level.getBlockState(bed);
    if (bs::isInstance(bs::blockOf(state), "BedBlock")) { // LivingEntity.stopSleeping
        Direction facing = direction::byName(bs::getValue(state, "facing").value_or("north"));
        level.setBlock(bed, bs::setBool(state, "occupied", false));
        auto standUp = findStandUpPosition(bed, facing, player_.yRot);
        double sx = standUp ? standUp->x : bed.x + 0.5, sy = standUp ? standUp->y : bed.y + 1.1, sz = standUp ? standUp->z : bed.z + 0.5;
        double lx = bed.x + 0.5 - sx, lz = bed.z + 0.5 - sz;
        double len = std::sqrt(lx * lx + (bed.y - sy) * (bed.y - sy) + lz * lz);
        float yaw = static_cast<float>(std::atan2(lz / len, lx / len) * 57.2957763671875 - 90.0);
        yaw = std::fmod(yaw, 360.0f); // Mth.wrapDegrees
        if (yaw >= 180.0f) yaw -= 360.0f;
        if (yaw < -180.0f) yaw += 360.0f;
        player_.x = sx, player_.y = sy, player_.z = sz;
        player_.yRot = yaw;
        player_.xRot = 0.0f;
    }
    player_.sleepingPos.reset();
    server_.playerList().broadcastPlayerData(player_);
    player_.sleepCounter = forcefulWakeUp ? 0 : 100; // Player.stopSleepInBed
    if (updateLevelList) server_.playerList().updateSleepingPlayerList();
    teleport(player_.x, player_.y, player_.z, player_.yRot, player_.xRot);
}

// Player.tick while sleeping: counts up, and wakes at daybreak.
void ServerGamePacketListener::tickSleeping() {
    if (player_.sleepingPos) {
        if (++player_.sleepCounter > 100) player_.sleepCounter = 100;
        if (!isDarkOutside()) stopSleepInBed(false, true);
    } else if (player_.sleepCounter > 0 && ++player_.sleepCounter >= 110) {
        player_.sleepCounter = 0;
    }
}

// AbstractBedBlock.findStandUpPosition (DismountHelper.findSafeDismountLocation for a player).
std::optional<ServerGamePacketListener::Vec3> ServerGamePacketListener::findStandUpPosition(BlockPos bed, Direction forward, float yaw) {
    ServerLevel& level = player_.level();
    Direction right = direction::clockWise(forward);
    float rad = yaw * std::numbers::pi_v<float> / 180.0f; // Direction.isFacingAngle
    bool facing = direction::stepX(right) * -std::sin(rad) + direction::stepZ(right) * std::cos(rad) > 0;
    Direction side = facing ? direction::opposite(right) : right;
    int sx = direction::stepX(side), sz = direction::stepZ(side), fx = direction::stepX(forward), fz = direction::stepZ(forward);
    std::vector<std::pair<int, int>> offsets = {{sx, sz}, {sx - fx, sz - fz}, {sx - fx * 2, sz - fz * 2}, {-fx * 2, -fz * 2},
                                                {-sx - fx * 2, -sz - fz * 2}, {-sx - fx, -sz - fz}, {-sx, -sz}, {-sx + fx, -sz + fz},
                                                {fx, fz}, {sx + fx, sz + fz}};
    bool bunk = bs::isInstance(bs::blockOf(level.getBlockState(bed.below())), "BedBlock");
    std::vector<std::pair<int, int>> above = {{0, 0}, {-fx, -fz}};
    if (!bunk) offsets.insert(offsets.end(), above.begin(), above.end()); // bedStandUpOffsets
    auto dangerous = [](BlockStateId st) { // EntityType.isBlockDangerous for a player
        uint32_t b = bs::blockOf(st);
        bool burning = bs::stateHasTag(st, "minecraft:fire") || (bs::flags(st) & bs::kLava) || b == gen::block::MAGMA_BLOCK ||
                       (bs::stateHasTag(st, "minecraft:campfires") && bs::getBool(st, "lit")) || b == gen::block::LAVA_CAULDRON;
        return burning || b == gen::block::WITHER_ROSE || b == gen::block::SWEET_BERRY_BUSH || b == gen::block::CACTUS || b == gen::block::POWDER_SNOW;
    };
    auto maxY = [](BlockStateId st) { // (climbable blocks and open trapdoors count as empty)
        if (bs::stateHasTag(st, "minecraft:climbable") || (bs::isInstance(bs::blockOf(st), "TrapDoorBlock") && bs::getBool(st, "open")))
            return -1.0;
        double m = -1.0;
        for (const auto& b : gen::kShapes[gen::kStateShape[st]]) m = std::max<double>(m, b.maxY);
        return m;
    };
    auto safeAt = [&](BlockPos p, bool checkDangerous) -> std::optional<Vec3> {
        BlockStateId st = level.getBlockState(p);
        if (checkDangerous && dangerous(st)) return std::nullopt;
        double floor = maxY(st); // CollisionGetter.getBlockFloorHeight
        if (floor < 0) {
            double below = maxY(level.getBlockState(p.below()));
            floor = below >= 1.0 ? below - 1.0 : -INFINITY;
        }
        if (std::isinf(floor) || floor >= 1.0) return std::nullopt;
        if (checkDangerous && floor <= 0.0 && dangerous(level.getBlockState(p.below()))) return std::nullopt;
        Vec3 at{p.x + 0.5, p.y + floor, p.z + 0.5};
        AABB box{at.x - 0.3, at.y, at.z - 0.3, at.x + 0.3, at.y + 1.8, at.z + 0.3};
        for (const auto& b : level.collisionBoxes(box))
            if (b.intersects(box)) return std::nullopt;
        if (bs::stateHasTag(st, "minecraft:invalid_spawn_inside") || bs::stateHasTag(level.getBlockState(p.above()), "minecraft:invalid_spawn_inside"))
            return std::nullopt;
        return at;
    };
    auto atOffsets = [&](BlockPos base, const std::vector<std::pair<int, int>>& offs, bool checkDangerous) -> std::optional<Vec3> {
        for (auto [ox, oz] : offs)
            if (auto v = safeAt({base.x + ox, base.y, base.z + oz}, checkDangerous)) return v;
        return std::nullopt;
    };
    if (!bunk) {
        if (auto v = atOffsets(bed, offsets, true)) return v;
        return atOffsets(bed, offsets, false);
    }
    for (auto [base, offs, check] : {std::tuple{bed, offsets, true}, std::tuple{bed.below(), offsets, true}, std::tuple{bed, above, true},
                                     std::tuple{bed, offsets, false}, std::tuple{bed.below(), offsets, false}, std::tuple{bed, above, false}})
        if (auto v = atOffsets(base, offs, check)) return v;
    return std::nullopt;
}

// Player.sendOverlayMessage: a system chat message shown above the hotbar.
void ServerGamePacketListener::sendOverlay(const Component& message) {
    Packet p(cb::SYSTEM_CHAT);
    nbt::writeNetwork(p, message.toNbt());
    p.writeBool(true); // overlay
    conn_.send(p);
}

} // namespace mc
