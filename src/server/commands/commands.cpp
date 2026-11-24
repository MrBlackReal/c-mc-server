#include "server/commands/commands.hpp"

#include <cmath>
#include <deque>
#include <format>
#include <limits>
#include <map>

#include "core/registries.hpp"
#include "generated/packet_ids.hpp"
#include "network/connection.hpp"
#include "server/commands/arguments.hpp"
#include "server/level/server_player.hpp"
#include "server/minecraft_server.hpp"
#include "server/network/listeners.hpp"
#include "util/log.hpp"
#include "util/mth.hpp"
#include "world/entity/entity.hpp"
#include "world/level/block/block_behaviour.hpp"
#include "world/timeline/timeline.hpp"

namespace mc::commands {

using namespace brigadier;

namespace {

Component lit(std::string s) { return Component::literal(std::move(s)); }
Component num(int64_t v) { return Component::literal(std::to_string(v)); }

// Commands.hasPermission(level)
Requirement perm(int level) {
    return [level](const CommandSourceStack& s) { return s.hasPermission(level); };
}

template <class T, class... A>
std::shared_ptr<ArgumentType> arg(A&&... a) {
    return std::make_shared<T>(std::forward<A>(a)...);
}

// ---------------------------------------------------------------- effect

int effectId(const CommandContext& c, std::string_view name) { return *registries::id("minecraft:mob_effect", c.get<std::string>(name)); }

Component effectName(int effect) {
    std::string id(registries::name("minecraft:mob_effect", effect));
    return Component::translatable("effect." + id.substr(0, id.find(':')) + "." + id.substr(id.find(':') + 1));
}

bool isInstantaneous(int effect) {
    std::string_view id = registries::name("minecraft:mob_effect", effect);
    return id == "minecraft:instant_health" || id == "minecraft:instant_damage" || id == "minecraft:saturation";
}

// CommandResponseTracker: counts the targets something happened to; one message names the target.
struct ResponseTracker {
    int elementCount = 0, nonZeroCount = 0, total = 0;
    std::optional<EntityRef> onlyElement, onlyNonZero;
    void track(const EntityRef& e, int value) {
        total += value;
        if (++elementCount == 1) onlyElement = e;
        else onlyElement.reset();
        if (value != 0) {
            if (++nonZeroCount == 1) onlyNonZero = e;
            else onlyNonZero.reset();
        }
    }
};

int giveEffect(const CommandContext& c, std::optional<int> seconds, int amplifier, bool particles) {
    const CommandSourceStack& source = c.getSource();
    auto entities = EntityArgument::getEntities(c, "targets");
    int effect = effectId(c, "effect");
    bool instant = isInstantaneous(effect);
    // computeDurationInTicks
    int duration = seconds ? (instant ? *seconds : (*seconds == -1 ? -1 : *seconds * 20)) : (instant ? 1 : 600);
    ResponseTracker tracker;
    for (const EntityRef& e : entities) {
        if (!e.isLiving()) continue;
        ServerPlayer& p = *e.player;
        ServerPlayer::EffectInstance inst;
        inst.amplifier = amplifier;
        inst.duration = duration;
        inst.visible = particles;
        inst.showIcon = true;
        // LivingEntity.addEffect: a new effect, or MobEffectInstance.update taking over a weaker/shorter one
        bool changed = true;
        auto it = p.effects.find(effect);
        if (it != p.effects.end() && (it->second.amplifier > amplifier ||
                                      (it->second.amplifier == amplifier && (it->second.duration == -1 || (duration != -1 && it->second.duration >= duration)))))
            changed = false;
        if (changed) p.addEffect(effect, inst);
        tracker.track(e, changed ? 1 : 0);
    }
    if (tracker.nonZeroCount == 0) throw error("commands.effect.give.failed");
    if (tracker.onlyNonZero)
        source.sendSuccess(Component::translatable("commands.effect.give.success.single", {effectName(effect), tracker.onlyNonZero->displayName(), num(duration / 20)}), true);
    else
        source.sendSuccess(Component::translatable("commands.effect.give.success.multiple", {effectName(effect), num(tracker.nonZeroCount), num(duration / 20)}), true);
    return tracker.total;
}

int clearEffects(const CommandSourceStack& source, const std::vector<EntityRef>& entities) {
    ResponseTracker tracker;
    for (const EntityRef& e : entities) {
        if (!e.isLiving()) continue;
        ServerPlayer& p = *e.player;
        bool any = !p.effects.empty();
        for (auto [effect, inst] : std::map(p.effects)) p.removeEffect(effect);
        tracker.track(e, any ? 1 : 0);
    }
    if (tracker.nonZeroCount == 0) throw error("commands.effect.clear.everything.failed");
    if (tracker.onlyNonZero)
        source.sendSuccess(Component::translatable("commands.effect.clear.everything.success.single", {tracker.onlyNonZero->displayName()}), true);
    else
        source.sendSuccess(Component::translatable("commands.effect.clear.everything.success.multiple", {num(tracker.nonZeroCount)}), true);
    return tracker.total;
}

int clearEffect(const CommandSourceStack& source, const std::vector<EntityRef>& entities, int effect) {
    ResponseTracker tracker;
    for (const EntityRef& e : entities) {
        if (!e.isLiving()) continue;
        ServerPlayer& p = *e.player;
        bool had = p.hasEffect(effect);
        if (had) p.removeEffect(effect);
        tracker.track(e, had ? 1 : 0);
    }
    if (tracker.nonZeroCount == 0) throw error("commands.effect.clear.specific.failed");
    if (tracker.onlyNonZero)
        source.sendSuccess(Component::translatable("commands.effect.clear.specific.success.single", {effectName(effect), tracker.onlyNonZero->displayName()}), true);
    else
        source.sendSuccess(Component::translatable("commands.effect.clear.specific.success.multiple", {effectName(effect), num(tracker.nonZeroCount)}), true);
    return tracker.total;
}

NodeBuilder effectCommand() {
    auto giveTail = [](std::optional<int> fixedSeconds) {
        // [<amplifier> [<hideParticles>]] after the seconds (or "infinite")
        return argument("amplifier", arg<IntegerArgument>(0, 255))
            .executes([fixedSeconds](const CommandContext& c) {
                return giveEffect(c, fixedSeconds ? fixedSeconds : std::optional<int>(c.get<int32_t>("seconds")), c.get<int32_t>("amplifier"), true);
            })
            .then(argument("hideParticles", arg<BoolArgument>()).executes([fixedSeconds](const CommandContext& c) {
                return giveEffect(c, fixedSeconds ? fixedSeconds : std::optional<int>(c.get<int32_t>("seconds")), c.get<int32_t>("amplifier"),
                                  !c.get<bool>("hideParticles"));
            }));
    };
    return literal("effect")
        .require(perm(kLevelGamemasters))
        .then(literal("clear")
                  .executes([](const CommandContext& c) { return clearEffects(c.getSource(), {c.getSource().getEntityOrException()}); })
                  .then(argument("targets", EntityArgument::entities())
                            .executes([](const CommandContext& c) { return clearEffects(c.getSource(), EntityArgument::getEntities(c, "targets")); })
                            .then(argument("effect", arg<ResourceArgument>("minecraft:mob_effect")).executes([](const CommandContext& c) {
                                return clearEffect(c.getSource(), EntityArgument::getEntities(c, "targets"), effectId(c, "effect"));
                            }))))
        .then(literal("give").then(argument("targets", EntityArgument::entities())
                                       .then(argument("effect", arg<ResourceArgument>("minecraft:mob_effect"))
                                                 .executes([](const CommandContext& c) { return giveEffect(c, std::nullopt, 0, true); })
                                                 .then(argument("seconds", arg<IntegerArgument>(1, 1000000))
                                                           .executes([](const CommandContext& c) { return giveEffect(c, c.get<int32_t>("seconds"), 0, true); })
                                                           .then(giveTail(std::nullopt)))
                                                 .then(literal("infinite")
                                                           .executes([](const CommandContext& c) { return giveEffect(c, -1, 0, true); })
                                                           .then(giveTail(-1))))));
}

// ---------------------------------------------------------------- chat commands

void sendEmote(const CommandContext& c) {
    MessageArgument::resolveChatMessage(c, "action", [&](const chat::PlayerChatMessage& message) {
        const CommandSourceStack& source = c.getSource();
        source.server->playerList().broadcastChatMessage(message, source.getPlayer(), source.bindChatType("minecraft:emote_command"));
    });
}

// MsgCommand.sendMessage
void sendPrivateMessage(const CommandSourceStack& source, const std::vector<ServerPlayer*>& players, const chat::PlayerChatMessage& message) {
    chat::ChatTypeBound incoming = source.bindChatType("minecraft:msg_command_incoming");
    for (ServerPlayer* player : players) {
        chat::ChatTypeBound outgoing = source.bindChatType("minecraft:msg_command_outgoing");
        outgoing.targetName = player->displayName();
        source.sendChatMessage(message, outgoing);
        if (player->game) player->game->sendChatMessage(message, incoming);
    }
}

// ---------------------------------------------------------------- fill / setblock

struct FillRegion {
    int minX, minY, minZ, maxX, maxY, maxZ;
};

enum class FillMode { Replace, Outline, Hollow, Destroy };

int fillBlocks(const CommandSourceStack& source, BlockPos a, BlockPos b, const BlockInput& target, FillMode mode,
               const std::function<bool(ServerLevel&, BlockPos)>& predicate, bool strict) {
    FillRegion r{std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z), std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
    int64_t area = int64_t{r.maxX - r.minX + 1} * (r.maxY - r.minY + 1) * (r.maxZ - r.minZ + 1);
    ServerLevel& level = *source.level;
    int limit = level.gameRules().getInt(GameRules::MaxBlockModifications);
    if (area > limit) throw error("commands.fill.toobig", {num(limit), num(area)});
    static const BlockInput kHollowCore{blocks::defaultState(*blocks::byName("minecraft:air")), {}, std::nullopt};
    std::vector<std::pair<BlockPos, BlockStateId>> updated;
    int count = 0;
    int flags = ServerLevel::kUpdateClients | (strict ? 816 : ServerLevel::kSkipBlockEntitySideEffects);
    // (BlockPos.betweenClosed: x fastest, then y, then z)
    for (int z = r.minZ; z <= r.maxZ; z++)
        for (int y = r.minY; y <= r.maxY; y++)
            for (int x = r.minX; x <= r.maxX; x++) {
                BlockPos pos{x, y, z};
                if (predicate && !predicate(level, pos)) continue;
                BlockStateId oldState = level.getBlockState(pos);
                bool affected = mode == FillMode::Destroy && level.destroyBlock(pos, true);
                bool border = x == r.minX || x == r.maxX || y == r.minY || y == r.maxY || z == r.minZ || z == r.maxZ;
                const BlockInput* block = &target;
                if (!border && mode == FillMode::Outline) block = nullptr;
                if (!border && mode == FillMode::Hollow) block = &kHollowCore;
                if (!block) {
                    if (affected) count++;
                } else if (!block->place(level, pos, flags)) {
                    if (affected) count++;
                } else {
                    if (!strict) updated.emplace_back(pos, oldState);
                    count++;
                }
            }
    for (const auto& [pos, oldState] : updated) level.updateNeighboursOnBlockSet(pos, oldState);
    if (count == 0) throw error("commands.fill.failed");
    source.sendSuccess(Component::translatable("commands.fill.success", {num(count)}), true);
    return count;
}

int setBlock(const CommandSourceStack& source, BlockPos pos, const BlockInput& block, bool destroy, bool keep, bool strict) {
    ServerLevel& level = *source.level;
    if (keep && !level.isEmptyBlock(pos)) throw error("commands.setblock.failed");
    bool placeNeeded = true;
    if (destroy) {
        level.destroyBlock(pos, true);
        placeNeeded = !blocks::isAir(block.state) || !blocks::isAir(level.getBlockState(pos));
    }
    BlockStateId oldState = level.getBlockState(pos);
    if (placeNeeded && !block.place(level, pos, ServerLevel::kUpdateClients | (strict ? 816 : ServerLevel::kSkipBlockEntitySideEffects)))
        throw error("commands.setblock.failed");
    if (!strict) level.updateNeighboursOnBlockSet(pos, oldState);
    source.sendSuccess(Component::translatable("commands.setblock.success", {num(pos.x), num(pos.y), num(pos.z)}), true);
    return 1;
}

NodeBuilder fillWithModes(NodeBuilder builder, bool withFilter) {
    auto run = [withFilter](FillMode mode, bool strict) {
        return [withFilter, mode, strict](const CommandContext& c) {
            std::function<bool(ServerLevel&, BlockPos)> filter;
            if (withFilter) {
                BlockPredicate p = c.get<BlockPredicate>("filter");
                filter = [p](ServerLevel& level, BlockPos pos) { return p.test(level, pos); };
            }
            return fillBlocks(c.getSource(), BlockPosArgument::getLoadedBlockPos(c, "from"), BlockPosArgument::getLoadedBlockPos(c, "to"),
                              c.get<BlockInput>("block"), mode, filter, strict);
        };
    };
    return std::move(builder)
        .executes(run(FillMode::Replace, false))
        .then(literal("outline").executes(run(FillMode::Outline, false)))
        .then(literal("hollow").executes(run(FillMode::Hollow, false)))
        .then(literal("destroy").executes(run(FillMode::Destroy, false)))
        .then(literal("strict").executes(run(FillMode::Replace, true)));
}

NodeBuilder fillCommand() {
    auto keep = [](const CommandContext& c) {
        return fillBlocks(c.getSource(), BlockPosArgument::getLoadedBlockPos(c, "from"), BlockPosArgument::getLoadedBlockPos(c, "to"), c.get<BlockInput>("block"),
                          FillMode::Replace, [](ServerLevel& level, BlockPos pos) { return level.isEmptyBlock(pos); }, false);
    };
    auto replace = [](const CommandContext& c) {
        return fillBlocks(c.getSource(), BlockPosArgument::getLoadedBlockPos(c, "from"), BlockPosArgument::getLoadedBlockPos(c, "to"), c.get<BlockInput>("block"),
                          FillMode::Replace, nullptr, false);
    };
    return literal("fill").require(perm(kLevelGamemasters))
        .then(argument("from", arg<BlockPosArgument>())
                  .then(argument("to", arg<BlockPosArgument>())
                            .then(fillWithModes(argument("block", arg<BlockStateArgument>()), false)
                                      .then(literal("replace").executes(replace).then(fillWithModes(argument("filter", arg<BlockPredicateArgument>()), true)))
                                      .then(literal("keep").executes(keep)))));
}

NodeBuilder setBlockCommand() {
    auto run = [](bool destroy, bool keep, bool strict) {
        return [=](const CommandContext& c) {
            return setBlock(c.getSource(), BlockPosArgument::getLoadedBlockPos(c, "pos"), c.get<BlockInput>("block"), destroy, keep, strict);
        };
    };
    return literal("setblock").require(perm(kLevelGamemasters))
        .then(argument("pos", arg<BlockPosArgument>())
                  .then(argument("block", arg<BlockStateArgument>())
                            .executes(run(false, false, false))
                            .then(literal("destroy").executes(run(true, false, false)))
                            .then(literal("keep").executes(run(false, true, false)))
                            .then(literal("replace").executes(run(false, false, false)))
                            .then(literal("strict").executes(run(false, false, true)))));
}

// ---------------------------------------------------------------- teleport

// Level.isInSpawnableBounds
bool inSpawnableBoundsCheck(BlockPos pos) {
    return !(pos.y < -20000000 || pos.y >= 20000000) && pos.x >= -30000000 && pos.z >= -30000000 && pos.x < 30000000 && pos.z < 30000000;
}

// Relative (ClientboundPlayerPositionPacket flags)
enum Relative : int { RelX = 1 << 0, RelY = 1 << 1, RelZ = 1 << 2, RelYRot = 1 << 3, RelXRot = 1 << 4, RelDeltaX = 1 << 5, RelDeltaY = 1 << 6, RelDeltaZ = 1 << 7 };

struct LookAt {
    std::optional<EntityRef> entity;
    Anchor entityAnchor = Anchor::Feet;
    double x = 0, y = 0, z = 0; // (a position)
};

// Entity.lookAt: face the point from the anchor.
void lookAtPoint(EntityRef victim, Anchor fromAnchor, double tx, double ty, double tz, float& yRot, float& xRot) {
    double fx = victim.x(), fy = victim.y() + (fromAnchor == Anchor::Eyes ? victim.eyeHeight() : 0.0), fz = victim.z();
    double xd = tx - fx, yd = ty - fy, zd = tz - fz;
    double sd = std::sqrt(xd * xd + zd * zd);
    xRot = mth::wrapDegrees(static_cast<float>(-(mth::atan2(yd, sd) * 57.2957763671875)));
    yRot = mth::wrapDegrees(static_cast<float>(mth::atan2(zd, xd) * 57.2957763671875) - 90.0f);
}

void performTeleport(const CommandSourceStack& source, EntityRef victim, double x, double y, double z, int relatives, float yRot, float xRot,
                     const std::optional<LookAt>& lookAt) {
    BlockPos blockPos{static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)), static_cast<int>(std::floor(z))};
    if (!inSpawnableBoundsCheck(blockPos)) throw error("commands.teleport.invalidPosition");
    double rx = relatives & RelX ? x - victim.x() : x;
    double ry = relatives & RelY ? y - victim.y() : y;
    double rz = relatives & RelZ ? z - victim.z() : z;
    float ryRot = mth::wrapDegrees(relatives & RelYRot ? yRot - victim.yRot() : yRot);
    float rxRot = mth::wrapDegrees(relatives & RelXRot ? xRot - victim.xRot() : xRot);
    // Entity.teleportTo (same level): PositionMoveRotation.calculateAbsolute
    double ax = relatives & RelX ? victim.x() + rx : rx, ay = relatives & RelY ? victim.y() + ry : ry, az = relatives & RelZ ? victim.z() + rz : rz;
    float ayRot = relatives & RelYRot ? victim.yRot() + ryRot : ryRot, axRot = relatives & RelXRot ? victim.xRot() + rxRot : rxRot;
    if (victim.player) {
        if (victim.player->game) victim.player->game->teleport(rx, ry, rz, ryRot, rxRot, relatives);
    } else {
        Entity& e = *victim.entity;
        e.x = ax, e.y = ay, e.z = az;
        e.yRot = ayRot, e.xRot = std::clamp(axRot, -90.0f, 90.0f);
        e.vy = 0; // setDeltaMovement(delta.multiply(1, 0, 1))
        e.onGround = true;
        e.needsSync = true;
    }
    if (lookAt) {
        double tx = lookAt->x, ty = lookAt->y, tz = lookAt->z;
        if (lookAt->entity) {
            tx = lookAt->entity->x(), ty = lookAt->entity->y() + (lookAt->entityAnchor == Anchor::Eyes ? lookAt->entity->eyeHeight() : 0.0), tz = lookAt->entity->z();
        }
        float ly, lx;
        lookAtPoint(victim, source.anchor, tx, ty, tz, ly, lx);
        if (victim.player) {
            victim.player->yRot = ly, victim.player->xRot = lx;
            if (victim.player->game) { // ClientboundPlayerLookAtPacket
                Packet p(packets::play_cb::PLAYER_LOOK_AT);
                p.writeVarInt(source.anchor == Anchor::Eyes ? 1 : 0);
                if (lookAt->entity) {
                    p.writeDouble(tx), p.writeDouble(ty), p.writeDouble(tz);
                    p.writeBool(true);
                    p.writeVarInt(lookAt->entity->id());
                    p.writeVarInt(lookAt->entityAnchor == Anchor::Eyes ? 1 : 0);
                } else {
                    p.writeDouble(tx), p.writeDouble(ty), p.writeDouble(tz);
                    p.writeBool(false);
                }
                victim.player->connection().send(p);
            }
        } else {
            victim.entity->yRot = ly, victim.entity->xRot = lx;
            victim.entity->needsSync = true;
        }
    }
}

std::string formatDouble(double v) { return std::format("{:.6f}", v); } // String.format("%f")

int teleportToPos(const CommandSourceStack& source, const std::vector<EntityRef>& entities, const Coordinates& destination,
                  const std::optional<Coordinates>& rotation, const std::optional<LookAt>& lookAt) {
    double x, y, z;
    destination.getPosition(source, x, y, z);
    float rotX = 0, rotY = 0;
    if (rotation) rotation->getRotation(source, rotX, rotY);
    ResponseTracker tracker;
    for (const EntityRef& e : entities) {
        // getRelatives (always the same dimension here)
        int relatives = (destination.isXRelative() ? RelDeltaX | RelX : 0) | (destination.isYRelative() ? RelDeltaY | RelY : 0) |
                        (destination.isZRelative() ? RelDeltaZ | RelZ : 0);
        relatives |= rotation ? (rotation->isYRelative() ? RelYRot : 0) | (rotation->isXRelative() ? RelXRot : 0) : RelYRot | RelXRot;
        if (rotation) performTeleport(source, e, x, y, z, relatives, rotY, rotX, lookAt);
        else performTeleport(source, e, x, y, z, relatives, e.yRot(), e.xRot(), lookAt);
        tracker.track(e, 1);
    }
    if (tracker.onlyNonZero)
        source.sendSuccess(Component::translatable("commands.teleport.success.location.single",
                                                  {tracker.onlyNonZero->displayName(), lit(formatDouble(x)), lit(formatDouble(y)), lit(formatDouble(z))}),
                           true);
    else
        source.sendSuccess(Component::translatable("commands.teleport.success.location.multiple",
                                                  {num(tracker.nonZeroCount), lit(formatDouble(x)), lit(formatDouble(y)), lit(formatDouble(z))}),
                           true);
    return tracker.total;
}

int teleportToEntity(const CommandSourceStack& source, const std::vector<EntityRef>& entities, EntityRef destination) {
    ResponseTracker tracker;
    for (const EntityRef& e : entities) {
        performTeleport(source, e, destination.x(), destination.y(), destination.z(), 0, destination.yRot(), destination.xRot(), std::nullopt);
        tracker.track(e, 1);
    }
    if (tracker.onlyNonZero)
        source.sendSuccess(Component::translatable("commands.teleport.success.entity.single", {tracker.onlyNonZero->displayName(), destination.displayName()}), true);
    else
        source.sendSuccess(Component::translatable("commands.teleport.success.entity.multiple", {num(tracker.nonZeroCount), destination.displayName()}), true);
    return tracker.total;
}

NodeBuilder teleportCommand() {
    auto targets = [](const CommandContext& c) { return EntityArgument::getEntities(c, "targets"); };
    return literal("teleport")
        .require(perm(kLevelGamemasters))
        .then(argument("location", arg<Vec3Argument>()).executes([](const CommandContext& c) {
            return teleportToPos(c.getSource(), {c.getSource().getEntityOrException()}, c.get<Coordinates>("location"), std::nullopt, std::nullopt);
        }))
        .then(argument("destination", EntityArgument::entity()).executes([](const CommandContext& c) {
            return teleportToEntity(c.getSource(), {c.getSource().getEntityOrException()}, EntityArgument::getEntity(c, "destination"));
        }))
        .then(argument("targets", EntityArgument::entities())
                  .then(argument("location", arg<Vec3Argument>())
                            .executes([targets](const CommandContext& c) {
                                return teleportToPos(c.getSource(), targets(c), c.get<Coordinates>("location"), std::nullopt, std::nullopt);
                            })
                            .then(argument("rotation", arg<RotationArgument>()).executes([targets](const CommandContext& c) {
                                return teleportToPos(c.getSource(), targets(c), c.get<Coordinates>("location"), c.get<Coordinates>("rotation"), std::nullopt);
                            }))
                            .then(literal("facing")
                                      .then(literal("entity").then(
                                          argument("facingEntity", EntityArgument::entity())
                                              .executes([targets](const CommandContext& c) {
                                                  LookAt look{EntityArgument::getEntity(c, "facingEntity"), Anchor::Feet};
                                                  return teleportToPos(c.getSource(), targets(c), c.get<Coordinates>("location"), std::nullopt, look);
                                              })
                                              .then(argument("facingAnchor", arg<EntityAnchorArgument>()).executes([targets](const CommandContext& c) {
                                                  LookAt look{EntityArgument::getEntity(c, "facingEntity"), c.get<Anchor>("facingAnchor")};
                                                  return teleportToPos(c.getSource(), targets(c), c.get<Coordinates>("location"), std::nullopt, look);
                                              }))))
                                      .then(argument("facingLocation", arg<Vec3Argument>()).executes([targets](const CommandContext& c) {
                                          LookAt look;
                                          c.get<Coordinates>("facingLocation").getPosition(c.getSource(), look.x, look.y, look.z);
                                          return teleportToPos(c.getSource(), targets(c), c.get<Coordinates>("location"), std::nullopt, look);
                                      }))))
                  .then(argument("destination", EntityArgument::entity()).executes([targets](const CommandContext& c) {
                      return teleportToEntity(c.getSource(), targets(c), EntityArgument::getEntity(c, "destination"));
                  })));
}

// ---------------------------------------------------------------- time

struct TimeMarker {
    std::string id;
    int ticks;
    std::optional<int> period;
    bool showInCommands;
};

// The time markers of a clock's timelines (ClockManager.registerTimeMarker).
std::vector<TimeMarker> timeMarkers(std::string_view clock) {
    std::vector<TimeMarker> out;
    static const json::Value all = json::parse(gen::kTimelinesJson);
    for (const auto& [name, t] : all.object()) {
        if (!t["clock"].isString() || t["clock"].string() != clock || !t.has("time_markers")) continue;
        std::optional<int> period;
        if (t["period_ticks"].isNumber()) period = static_cast<int>(t["period_ticks"].number());
        for (const auto& [id, m] : t["time_markers"].object()) {
            if (m.isNumber()) out.push_back({id, static_cast<int>(m.number()), period, false});
            else out.push_back({id, static_cast<int>(m["ticks"].number()), period, m["show_in_commands"].boolean(false)});
        }
    }
    return out;
}

// SharedSuggestionProvider.suggestResource
void suggestResource(const std::vector<std::string>& ids, SuggestionsBuilder& builder) {
    std::string contents = builder.getRemainingLowerCase();
    bool hasNamespace = contents.find(':') != std::string::npos;
    for (const auto& id : ids) {
        size_t colon = id.find(':');
        if (hasNamespace ? matchesSubStr(contents, id) : matchesSubStr(contents, id.substr(0, colon)) || matchesSubStr(contents, id.substr(colon + 1)))
            builder.suggest(id);
    }
}

using ClockGetter = std::function<std::string(const CommandContext&)>;

ServerClockManager::Clock& clockOf(const CommandSourceStack& source, const std::string& name) { return *source.server->clockManager().byName(name); }

int wrapTime(int64_t ticks) { return static_cast<int>(ticks % 2147483647LL); }

NodeBuilder addClockNodes(NodeBuilder node, ClockGetter clockGetter) {
    auto broadcast = [](const CommandSourceStack& s) { s.server->playerList().broadcastTime(true); };
    return std::move(node)
        .then(literal("set")
                  .then(argument("time", arg<TimeArgument>()).executes([=](const CommandContext& c) {
                      std::string name = clockGetter(c);
                      auto& clock = clockOf(c.getSource(), name);
                      int ticks = c.get<int32_t>("time");
                      if (clock.totalTicks == ticks) throw error("commands.time.set.already_at_time", {lit(name), num(ticks)});
                      clock.totalTicks = ticks;
                      clock.partialTick = 0;
                      broadcast(c.getSource());
                      c.getSource().sendSuccess(Component::translatable("commands.time.set.absolute", {lit(name), num(ticks)}), true);
                      return ticks;
                  }))
                  .then(argument("timemarker", arg<IdentifierArgument>())
                            .suggests([=](const CommandContext& c, SuggestionsBuilder& b) {
                                std::vector<std::string> ids;
                                for (const auto& m : timeMarkers(clockGetter(c)))
                                    if (m.showInCommands) ids.push_back(m.id);
                                suggestResource(ids, b);
                            })
                            .executes([=](const CommandContext& c) {
                                std::string name = clockGetter(c);
                                std::string markerId = c.get<std::string>("timemarker");
                                auto& clock = clockOf(c.getSource(), name);
                                auto markers = timeMarkers(name);
                                auto marker = std::find_if(markers.begin(), markers.end(), [&](const TimeMarker& m) { return m.id == markerId; });
                                if (marker == markers.end()) throw error("commands.time.no_time_marker_found", {lit(markerId), lit(name)});
                                // ClockTimeMarker.occursAt / resolveTimeToMoveTo
                                bool at = marker->period ? marker->ticks == clock.totalTicks % *marker->period : marker->ticks == clock.totalTicks;
                                if (at) throw error("commands.time.set.already_at_time_marker", {lit(name), lit(markerId)});
                                if (marker->period) {
                                    int64_t duration = marker->ticks - clock.totalTicks % *marker->period;
                                    clock.totalTicks += duration > 0 ? duration : *marker->period + duration;
                                } else {
                                    clock.totalTicks = marker->ticks;
                                }
                                clock.partialTick = 0;
                                broadcast(c.getSource());
                                c.getSource().sendSuccess(Component::translatable("commands.time.set.time_marker", {lit(name), lit(markerId)}), true);
                                return wrapTime(clock.totalTicks);
                            })))
        .then(literal("add").then(argument("time", arg<TimeArgument>(std::numeric_limits<int32_t>::min())).executes([=](const CommandContext& c) {
            std::string name = clockGetter(c);
            auto& clock = clockOf(c.getSource(), name);
            clock.totalTicks = std::max<int64_t>(clock.totalTicks + c.get<int32_t>("time"), 0);
            broadcast(c.getSource());
            c.getSource().sendSuccess(Component::translatable("commands.time.set.absolute", {lit(name), num(clock.totalTicks)}), true);
            return wrapTime(clock.totalTicks);
        })))
        .then(literal("pause").executes([=](const CommandContext& c) {
            std::string name = clockGetter(c);
            auto& clock = clockOf(c.getSource(), name);
            if (clock.paused) throw error("commands.time.pause.already_paused", {lit(name)});
            clock.paused = true;
            broadcast(c.getSource());
            c.getSource().sendSuccess(Component::translatable("commands.time.pause", {lit(name)}), true);
            return 1;
        }))
        .then(literal("resume").executes([=](const CommandContext& c) {
            std::string name = clockGetter(c);
            auto& clock = clockOf(c.getSource(), name);
            if (!clock.paused) throw error("commands.time.pause.already_running", {lit(name)});
            clock.paused = false;
            broadcast(c.getSource());
            c.getSource().sendSuccess(Component::translatable("commands.time.resume", {lit(name)}), true);
            return 1;
        }))
        .then(literal("rate").then(argument("rate", arg<FloatArgument>(1.0e-5f, 1000.0f)).executes([=](const CommandContext& c) {
            std::string name = clockGetter(c);
            auto& clock = clockOf(c.getSource(), name);
            float rate = c.get<float>("rate");
            if (clock.rate == rate) throw error("commands.time.rate.already_same", {lit(name), number(rate)});
            clock.rate = rate;
            broadcast(c.getSource());
            c.getSource().sendSuccess(Component::translatable("commands.time.rate", {lit(name), number(rate)}), true);
            return 1;
        })))
        .then(literal("query")
                  .then(literal("time").executes([=](const CommandContext& c) {
                      std::string name = clockGetter(c);
                      int64_t total = clockOf(c.getSource(), name).totalTicks;
                      c.getSource().sendSuccess(Component::translatable("commands.time.query.absolute", {lit(name), num(total)}), false);
                      return wrapTime(total);
                  }))
                  .then(argument("timeline", arg<ResourceArgument>("minecraft:timeline"))
                            .executes([=](const CommandContext& c) {
                                std::string name = clockGetter(c), timeline = c.get<std::string>("timeline");
                                static const json::Value all = json::parse(gen::kTimelinesJson);
                                const json::Value& t = all[timeline];
                                if (!t["clock"].isString() || t["clock"].string() != name) throw error("commands.time.wrong_timeline_for_clock", {lit(timeline), lit(name)});
                                int64_t total = clockOf(c.getSource(), name).totalTicks;
                                int64_t current = t["period_ticks"].isNumber() ? total % static_cast<int>(t["period_ticks"].number()) : total;
                                c.getSource().sendSuccess(Component::translatable("commands.time.query.timeline", {lit(timeline), num(current)}), false);
                                return wrapTime(current);
                            })
                            .then(literal("repetition").executes([=](const CommandContext& c) {
                                std::string name = clockGetter(c), timeline = c.get<std::string>("timeline");
                                static const json::Value all = json::parse(gen::kTimelinesJson);
                                const json::Value& t = all[timeline];
                                if (!t["clock"].isString() || t["clock"].string() != name) throw error("commands.time.wrong_timeline_for_clock", {lit(timeline), lit(name)});
                                int64_t total = clockOf(c.getSource(), name).totalTicks;
                                int64_t repetitions = t["period_ticks"].isNumber() ? static_cast<int>(total / static_cast<int>(t["period_ticks"].number())) : 0;
                                c.getSource().sendSuccess(Component::translatable("commands.time.query.timeline.repetitions", {lit(timeline), num(repetitions)}), false);
                                return wrapTime(repetitions);
                            }))));
}

NodeBuilder timeCommand() {
    // getDefaultClock: the dimension type's default clock (the overworld's is "minecraft:overworld")
    ClockGetter defaultClock = [](const CommandContext& c) -> std::string {
        (void)c;
        return "minecraft:overworld";
    };
    return addClockNodes(literal("time").require(perm(kLevelGamemasters)), defaultClock)
        .then(literal("query").then(literal("gametime").executes([](const CommandContext& c) {
            int64_t gameTime = c.getSource().server->clockManager().gameTime();
            c.getSource().sendSuccess(Component::translatable("commands.time.query.gametime", {num(gameTime)}), false);
            return wrapTime(gameTime);
        })))
        .then(literal("of").then(addClockNodes(argument("clock", arg<ResourceArgument>("minecraft:world_clock")),
                                               [](const CommandContext& c) { return c.get<std::string>("clock"); })));
}

// ---------------------------------------------------------------- gamerule

NodeBuilder gameRuleCommand() {
    NodeBuilder base = literal("gamerule").require(perm(kLevelGamemasters));
    for (size_t i = 0; i < GameRules::infos().size(); i++) {
        auto rule = static_cast<GameRules::Rule>(i);
        const auto& info = GameRules::info(rule);
        auto build = [&](std::string name) {
            std::shared_ptr<ArgumentType> valueType = info.isBool ? arg<BoolArgument>() : arg<IntegerArgument>(info.min, info.max);
            return literal(std::move(name))
                .executes([rule](const CommandContext& c) {
                    GameRules& rules = c.getSource().server->gameRules();
                    const auto& info = GameRules::info(rule);
                    c.getSource().sendSuccess(Component::translatable("commands.gamerule.query", {lit(std::string(info.id)), lit(rules.serialize(rule))}), false);
                    return rules.getInt(rule);
                })
                .then(argument("value", valueType).executes([rule](const CommandContext& c) {
                    GameRules& rules = c.getSource().server->gameRules();
                    const auto& info = GameRules::info(rule);
                    int value = info.isBool ? (c.get<bool>("value") ? 1 : 0) : c.get<int32_t>("value");
                    std::string serialized = info.isBool ? (value ? "true" : "false") : std::to_string(value);
                    if (rules.getInt(rule) == value) throw error("commands.gamerule.not_set", {lit(std::string(info.id)), lit(serialized)});
                    rules.set(rule, value);
                    c.getSource().server->onGameRuleChanged(rule);
                    c.getSource().sendSuccess(Component::translatable("commands.gamerule.set", {lit(std::string(info.id)), lit(serialized)}), true);
                    return value;
                }));
        };
        base = std::move(base).then(build(std::string(info.id))).then(build("minecraft:" + std::string(info.id)));
    }
    return base;
}

// ---------------------------------------------------------------- others

int setMode(const CommandSourceStack& source, const std::vector<ServerPlayer*>& players, GameType type) {
    int count = 0;
    for (ServerPlayer* player : players) {
        if (!player->game || !player->game->setGameMode(type)) continue;
        count++;
        // logGamemodeChange
        std::string names[] = {"survival", "creative", "adventure", "spectator"};
        Component mode = Component::translatable("gameMode." + names[static_cast<int>(type)]);
        if (source.entity.player == player) {
            source.sendSuccess(Component::translatable("commands.gamemode.success.self", {mode}), true);
        } else {
            if (source.level->gameRules().getBool(GameRules::SendCommandFeedback)) player->game->sendSystemMessage(Component::translatable("gameMode.changed", {mode}));
            source.sendSuccess(Component::translatable("commands.gamemode.success.other", {player->displayName(), mode}), true);
        }
    }
    return count;
}

int listPlayers(const CommandSourceStack& source, bool uuids) {
    auto& list = source.server->playerList();
    std::vector<EntityRef> players;
    for (const auto& p : list.players()) players.push_back(EntityRef::of(*p));
    Component names = Component::empty();
    for (size_t i = 0; i < players.size(); i++) { // ComponentUtils.formatList
        if (i) {
            Component sep = Component::literal(", ");
            sep.withColor("gray");
            names.append(sep);
        }
        const ServerPlayer& p = *players[i].player;
        names.append(uuids ? Component::translatable("commands.list.nameAndId", {lit(p.profile().name), lit(p.profile().id.toString())}) : p.displayName());
    }
    if (players.size() == 1) names = names.extra[0];
    source.sendSuccess(Component::translatable("commands.list.players", {num(static_cast<int64_t>(players.size())), num(source.server->properties().maxPlayers), names}), false);
    return static_cast<int>(players.size());
}

} // namespace

Commands::Commands(MinecraftServer& server) : server_(server) {
    auto& d = dispatcher_;
    // (in vanilla's registration order)
    d.registerCommand(effectCommand());
    d.registerCommand(literal("me").then(argument("action", arg<MessageArgument>()).executes([](const CommandContext& c) {
        sendEmote(c);
        return 1;
    })));
    d.registerCommand(fillCommand());
    d.registerCommand(literal("gamemode").require(perm(kLevelGamemasters))
                          .then(argument("gamemode", arg<GameModeArgument>())
                                    .executes([](const CommandContext& c) {
                                        return setMode(c.getSource(), {&c.getSource().getPlayerOrException()}, c.get<GameType>("gamemode"));
                                    })
                                    .then(argument("target", EntityArgument::players()).executes([](const CommandContext& c) {
                                        return setMode(c.getSource(), EntityArgument::getPlayers(c, "target"), c.get<GameType>("gamemode"));
                                    }))));
    d.registerCommand(gameRuleCommand());
    d.registerCommand(literal("help")
                          .executes([this](const CommandContext& c) {
                              auto usage = dispatcher_.getSmartUsage(dispatcher_.getRoot(), c.getSource());
                              for (const auto& [node, line] : usage) c.getSource().sendSuccess(lit("/" + line), false);
                              return static_cast<int>(usage.size());
                          })
                          .then(argument("command", arg<StringArgument>(StringArgument::Type::GreedyPhrase)).executes([this](const CommandContext& c) {
                              ParseResults command = dispatcher_.parse(StringReader(c.get<std::string>("command")), c.sourcePtr());
                              if (command.context.getNodes().empty()) throw error("commands.help.failed");
                              auto usage = dispatcher_.getSmartUsage(command.context.getNodes().back().node, c.getSource());
                              for (const auto& [node, line] : usage) c.getSource().sendSuccess(lit("/" + command.reader.getString() + " " + line), false);
                              return static_cast<int>(usage.size());
                          })));
    d.registerCommand(literal("kick").require(perm(kLevelAdmins))
                          .then(argument("targets", EntityArgument::players())
                                    .executes([](const CommandContext& c) {
                                        auto players = EntityArgument::getPlayers(c, "targets");
                                        Component reason = Component::translatable("multiplayer.disconnect.kicked");
                                        for (ServerPlayer* p : players) {
                                            p->game->disconnect(reason);
                                            c.getSource().sendSuccess(Component::translatable("commands.kick.success", {p->displayName(), reason}), true);
                                        }
                                        return static_cast<int>(players.size());
                                    })
                                    .then(argument("reason", arg<MessageArgument>()).executes([](const CommandContext& c) {
                                        auto players = EntityArgument::getPlayers(c, "targets");
                                        Component reason = MessageArgument::getMessage(c, "reason");
                                        for (ServerPlayer* p : players) {
                                            p->game->disconnect(reason);
                                            c.getSource().sendSuccess(Component::translatable("commands.kick.success", {p->displayName(), reason}), true);
                                        }
                                        return static_cast<int>(players.size());
                                    }))));
    d.registerCommand(literal("list")
                          .executes([](const CommandContext& c) { return listPlayers(c.getSource(), false); })
                          .then(literal("uuids").executes([](const CommandContext& c) { return listPlayers(c.getSource(), true); })));
    CommandNode* msg = d.registerCommand(literal("msg").then(argument("targets", EntityArgument::players())
                                                                 .then(argument("message", arg<MessageArgument>()).executes([](const CommandContext& c) {
                                                                     auto players = EntityArgument::getPlayers(c, "targets");
                                                                     if (!players.empty())
                                                                         MessageArgument::resolveChatMessage(c, "message", [&](const chat::PlayerChatMessage& m) {
                                                                             sendPrivateMessage(c.getSource(), players, m);
                                                                         });
                                                                     return static_cast<int>(players.size());
                                                                 }))));
    d.registerCommand(literal("tell").redirect(msg));
    d.registerCommand(literal("w").redirect(msg));
    d.registerCommand(literal("say").require(perm(kLevelGamemasters)).then(argument("message", arg<MessageArgument>()).executes([](const CommandContext& c) {
        MessageArgument::resolveChatMessage(c, "message", [&](const chat::PlayerChatMessage& message) {
            const CommandSourceStack& source = c.getSource();
            source.server->playerList().broadcastChatMessage(message, source.getPlayer(), source.bindChatType("minecraft:say_command"));
        });
        return 1;
    })));
    d.registerCommand(literal("seed").require(perm(kLevelGamemasters)).executes([](const CommandContext& c) {
        int64_t seed = c.getSource().level->seed();
        // ComponentUtils.copyOnClickText
        Component text = lit(std::to_string(seed));
        text.withColor("green");
        nbt::Compound click;
        click.put("action", std::string("copy_to_clipboard"));
        click.put("value", std::to_string(seed));
        nbt::Compound hover;
        hover.put("action", std::string("show_text"));
        hover.put("value", Component::translatable("chat.copy.click").toNbt());
        text.clickEvent = click;
        text.hoverEvent = hover;
        text.insertion = std::to_string(seed);
        c.getSource().sendSuccess(Component::translatable("commands.seed.success", {Component::translatable("chat.square_brackets", {text})}), false);
        return static_cast<int>(seed);
    }));
    d.registerCommand(setBlockCommand());
    CommandNode* teammsg = d.registerCommand(literal("teammsg").then(argument("message", arg<MessageArgument>()).executes([](const CommandContext& c) {
        c.getSource().getEntityOrException();
        throw error("commands.teammsg.failed.noteam"); // (no scoreboard teams yet: nobody is on one)
        return 0;
    })));
    d.registerCommand(literal("tm").redirect(teammsg));
    CommandNode* teleport = d.registerCommand(teleportCommand());
    d.registerCommand(literal("tp").require(perm(kLevelGamemasters)).redirect(teleport));
    d.registerCommand(timeCommand());
    // (dedicated server commands)
    d.registerCommand(literal("deop").require(perm(kLevelAdmins))
                          .then(argument("targets", arg<GameProfileArgument>())
                                    .suggests([](const CommandContext& c, SuggestionsBuilder& b) {
                                        brigadier::suggest(c.getSource().server->playerList().ops().names(), b);
                                    })
                                    .executes([](const CommandContext& c) {
                                        auto& list = c.getSource().server->playerList();
                                        int count = 0;
                                        for (const NameAndId& p : GameProfileArgument::getGameProfiles(c, "targets")) {
                                            if (!list.isOp(p.id)) continue;
                                            list.deop(p);
                                            count++;
                                            c.getSource().sendSuccess(Component::translatable("commands.deop.success", {lit(p.name)}), true);
                                        }
                                        if (count == 0) throw error("commands.deop.failed");
                                        return count;
                                    })));
    d.registerCommand(literal("op").require(perm(kLevelAdmins))
                          .then(argument("targets", arg<GameProfileArgument>())
                                    .suggests([](const CommandContext& c, SuggestionsBuilder& b) {
                                        auto& list = c.getSource().server->playerList();
                                        std::vector<std::string> names;
                                        for (const auto& p : list.players())
                                            if (!list.isOp(p->profile().id)) names.push_back(p->profile().name);
                                        brigadier::suggest(names, b);
                                    })
                                    .executes([](const CommandContext& c) {
                                        auto& list = c.getSource().server->playerList();
                                        int count = 0;
                                        for (const NameAndId& p : GameProfileArgument::getGameProfiles(c, "targets")) {
                                            if (list.isOp(p.id)) continue;
                                            list.op(p);
                                            count++;
                                            c.getSource().sendSuccess(Component::translatable("commands.op.success", {lit(p.name)}), true);
                                        }
                                        if (count == 0) throw error("commands.op.failed");
                                        return count;
                                    })));
    auto saveAll = [](const CommandSourceStack& source, bool flush) {
        source.sendSuccess(Component::translatable("commands.save.saving"), false);
        source.server->saveEverything(false);
        (void)flush;
        source.sendSuccess(Component::translatable("commands.save.success"), true);
        return 1;
    };
    d.registerCommand(literal("save-all")
                          .require(perm(kLevelOwners))
                          .executes([saveAll](const CommandContext& c) { return saveAll(c.getSource(), false); })
                          .then(literal("flush").executes([saveAll](const CommandContext& c) { return saveAll(c.getSource(), true); })));
    d.registerCommand(literal("stop").require(perm(kLevelOwners)).executes([](const CommandContext& c) {
        c.getSource().sendSuccess(Component::translatable("commands.stop.stopping"), true);
        c.getSource().server->halt();
        return 1;
    }));
}

ParseResults Commands::parse(const std::string& command, const CommandSourceStack& source) const {
    return dispatcher_.parse(StringReader(command), std::make_shared<const CommandSourceStack>(source));
}

void Commands::performPrefixedCommand(const CommandSourceStack& source, std::string command) {
    if (command.starts_with('/')) command.erase(0, 1);
    performCommand(parse(command, source), command);
}

namespace {
// Moves back `n` characters (UTF-16 units) from byte offset `at` in UTF-8 text.
size_t backChars(const std::string& s, size_t at, int n) {
    while (n > 0 && at > 0) {
        size_t start = at - 1;
        while (start > 0 && (static_cast<unsigned char>(s[start]) & 0xC0) == 0x80) start--;
        n -= static_cast<unsigned char>(s[start]) >= 0xF0 ? 2 : 1;
        at = start;
    }
    return at;
}

// Commands.getParseException
std::optional<CommandSyntaxException> getParseException(const ParseResults& parse) {
    if (!parse.reader.canRead()) return std::nullopt;
    if (parse.exceptions.size() == 1) return parse.exceptions.front().second;
    if (parse.context.getRange().isEmpty()) return errorAt(parse.reader, "command.unknown.command");
    return errorAt(parse.reader, "command.unknown.argument");
}
} // namespace

void Commands::performCommand(const ParseResults& parse, const std::string& command) {
    const CommandSourceStack& sender = parse.context.getSource();
    // finishParsing
    std::optional<CommandContext> chainTop;
    try {
        if (auto e = getParseException(parse)) throw *e;
        CommandContext context = parse.context.build(command);
        // ContextChain.tryFlatten: the last context in the chain must have a command
        const CommandContext* current = &context;
        while (current->getChild()) current = current->getChild().get();
        if (!current->getCommand()) throw errorAt(parse.reader, "command.unknown.command");
        chainTop = std::move(context);
    } catch (const CommandSyntaxException& e) {
        sender.sendFailure(e.message);
        if (e.input && e.cursor >= 0) {
            size_t cursor = std::min(e.input->size(), static_cast<size_t>(e.cursor));
            Component context = Component::empty();
            context.withColor("gray");
            nbt::Compound click;
            click.put("action", std::string("suggest_command"));
            click.put("command", "/" + command);
            context.clickEvent = click;
            size_t from = backChars(*e.input, cursor, 10);
            if (from > 0) context.append(lit("...")); // (cursor > 10)
            context.append(lit(e.input->substr(from, cursor - from)));
            if (cursor < e.input->size()) {
                Component remaining = lit(e.input->substr(cursor));
                remaining.withColor("red").withUnderlined();
                context.append(remaining);
            }
            Component here = Component::translatable("command.context.here");
            here.withColor("red").withItalic();
            context.append(here);
            sender.sendFailure(context);
        }
        return;
    }
    // Execution: the source passes down the chain (redirects have no modifiers here) to the
    // final context, whose command runs.
    std::shared_ptr<const CommandSourceStack> source = chainTop->sourcePtr();
    const CommandContext* current = &*chainTop;
    while (current->getChild()) current = current->getChild().get();
    try {
        current->getCommand()(current->copyFor(source));
    } catch (const CommandSyntaxException& e) {
        sender.sendFailure(e.message); // ExecutionCommandSource.handleError
    } catch (const std::exception& e) {
        Component failed = Component::translatable("command.failed");
        nbt::Compound hover;
        hover.put("action", std::string("show_text"));
        hover.put("value", lit(e.what()).toNbt());
        failed.hoverEvent = hover;
        sender.sendFailure(failed);
        log::error("'/{}' threw an exception: {}", command, e.what());
    }
}

std::vector<std::pair<std::string, std::string>> Commands::signableArguments(const ParseResults& parse) {
    // ArgumentVisitor.visitArguments(parse, ..., rejectRootRedirects = true)
    std::vector<std::pair<std::string, std::string>> out;
    const std::string& command = parse.reader.getString();
    auto visit = [&](const CommandContextBuilder& context) {
        for (const auto& node : context.getNodes()) {
            if (node.node->kind() != CommandNode::Kind::Argument || !node.node->type()->isSigned()) continue;
            for (const auto& [name, value] : context.getArguments())
                if (name == node.node->name()) out.emplace_back(name, value.range.get(command));
        }
    };
    const CommandContextBuilder* context = &parse.context;
    visit(*context);
    while (context->getChild() && context->getChild()->getRootNode() != parse.context.getRootNode()) {
        context = context->getChild().get();
        visit(*context);
    }
    return out;
}

void Commands::sendCommands(ServerPlayer& player) {
    if (!player.game) return;
    CommandSourceStack source = CommandSourceStack::forPlayer(server_, player);
    // fillUsableCommands: a copy of the tree with the nodes the player can use.
    struct Out {
        const CommandNode* src = nullptr;
        std::vector<Out*> children;
        Out* redirect = nullptr;
    };
    std::deque<Out> nodes;
    std::map<const CommandNode*, Out*> converted;
    Out* root = &nodes.emplace_back();
    root->src = dispatcher_.getRoot();
    converted[dispatcher_.getRoot()] = root;
    std::function<void(const CommandNode*, Out*)> fill = [&](const CommandNode* from, Out* to) {
        for (const CommandNode* child : from->children()) {
            if (!child->canUse(source)) continue;
            Out* node = &nodes.emplace_back();
            node->src = child;
            if (child->redirect()) {
                auto it = converted.find(child->redirect());
                node->redirect = it != converted.end() ? it->second : nullptr;
            }
            converted[child] = node;
            to->children.push_back(node);
            if (!child->children().empty()) fill(child, node);
        }
    };
    fill(dispatcher_.getRoot(), root);
    // ClientboundCommandsPacket.enumerateNodes: breadth first, redirects after children
    std::vector<Out*> order;
    std::map<Out*, int> ids;
    std::deque<Out*> queue{root};
    while (!queue.empty()) {
        Out* n = queue.front();
        queue.pop_front();
        if (ids.contains(n)) continue;
        ids[n] = static_cast<int>(order.size());
        order.push_back(n);
        for (Out* c : n->children) queue.push_back(c);
        if (n->redirect) queue.push_back(n->redirect);
    }
    Packet p(packets::play_cb::COMMANDS);
    p.writeVarInt(static_cast<int32_t>(order.size()));
    for (Out* n : order) {
        const CommandNode* src = n->src;
        int flags = src->kind() == CommandNode::Kind::Root ? 0 : src->kind() == CommandNode::Kind::Literal ? 1 : 2;
        if (src->command()) flags |= 4;
        if (n->redirect) flags |= 8;
        bool customSuggestions = src->kind() == CommandNode::Kind::Argument && src->customSuggestions();
        if (customSuggestions) flags |= 16;
        if (src->kind() != CommandNode::Kind::Root && !src->canUse(CommandSourceStack::noPermissions())) flags |= 32; // restricted
        p.writeByte(static_cast<uint8_t>(flags));
        p.writeVarInt(static_cast<int32_t>(n->children.size()));
        for (Out* c : n->children) p.writeVarInt(ids[c]);
        if (n->redirect) p.writeVarInt(ids[n->redirect]);
        if (src->kind() == CommandNode::Kind::Argument) {
            p.writeUtf(src->name());
            p.writeVarInt(registries::id("minecraft:command_argument_type", src->type()->parserId()).value_or(0));
            src->type()->writeProperties(p);
            if (customSuggestions) p.writeUtf("minecraft:ask_server"); // (SuggestionProviders.getName of an unregistered provider)
        } else if (src->kind() == CommandNode::Kind::Literal) {
            p.writeUtf(src->name());
        }
    }
    p.writeVarInt(0); // root
    player.connection().send(p);
}

} // namespace mc::commands
