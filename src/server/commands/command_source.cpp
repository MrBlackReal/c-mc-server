#include "server/commands/command_source.hpp"

#include <cmath>

#include "server/commands/brigadier.hpp"
#include "server/level/server_level.hpp"
#include "server/level/server_player.hpp"
#include "server/minecraft_server.hpp"
#include "server/network/listeners.hpp"
#include "util/log.hpp"
#include "world/entity/entity.hpp"
#include "world/entity/item_entity.hpp"
#include "world/item/item_logic.hpp"

namespace mc::commands {

// ---------------------------------------------------------------- EntityRef

EntityRef EntityRef::of(ServerPlayer& player) { return {&player, nullptr, &player.level()}; }

int EntityRef::id() const { return player ? player->id() : entity->id(); }
UUID EntityRef::uuid() const { return player ? player->profile().id : entity->uuid(); }
std::string_view EntityRef::type() const { return player ? std::string_view("minecraft:player") : entity->type(); }
double EntityRef::x() const { return player ? player->x : entity->x; }
double EntityRef::y() const { return player ? player->y : entity->y; }
double EntityRef::z() const { return player ? player->z : entity->z; }
float EntityRef::yRot() const { return player ? player->yRot : entity->yRot; }
float EntityRef::xRot() const { return player ? player->xRot : entity->xRot; }
AABB EntityRef::boundingBox() const { return player ? player->boundingBox() : entity->boundingBox(); }
// Entity.getEyeHeight: players' pose; others EntityDimensions' default (0.85 of the height).
double EntityRef::eyeHeight() const { return player ? player->eyeY() - player->y : entity->height() * 0.85; }
bool EntityRef::isAlive() const { return player ? !player->disconnected : !entity->removed; }

double EntityRef::distanceToSqr(double px, double py, double pz) const {
    double dx = x() - px, dy = y() - py, dz = z() - pz;
    return dx * dx + dy * dy + dz * dz;
}

Component EntityRef::name() const {
    if (player) return Component::literal(player->profile().name);
    if (auto* item = dynamic_cast<ItemEntity*>(entity)) { // ItemEntity.getName: the stack's name
        if (auto custom = items::componentValue(item->item, "minecraft:custom_name")) return Component::fromNbt(*custom);
        if (auto name = items::effectiveComponent(item->item, "minecraft:item_name")) return Component::fromNbt(*name);
    }
    // EntityType.getDescription: "entity.<namespace>.<path>"
    std::string_view id = entity->type();
    size_t colon = id.find(':');
    return Component::translatable("entity." + std::string(id.substr(0, colon)) + "." + std::string(id.substr(colon + 1)));
}

std::string EntityRef::plainName() const { return name().getString(); }

Component EntityRef::displayName() const {
    if (player) return player->displayName();
    // (no teams) name with the entity tooltip and the UUID as insertion
    Component c = name();
    nbt::Compound hover;
    hover.put("action", std::string("show_entity"));
    hover.put("id", std::string(entity->type()));
    UUID u = entity->uuid();
    hover.put("uuid", std::vector<int32_t>{static_cast<int32_t>(u.most >> 32), static_cast<int32_t>(u.most), static_cast<int32_t>(u.least >> 32),
                                           static_cast<int32_t>(u.least)});
    hover.put("name", name().toNbt());
    c.hoverEvent = std::move(hover);
    c.insertion = u.toString();
    return c;
}

// ---------------------------------------------------------------- CommandSourceStack

CommandSourceStack CommandSourceStack::forPlayer(MinecraftServer& server, ServerPlayer& player) {
    CommandSourceStack s;
    s.server = &server;
    s.level = &player.level();
    s.x = player.x, s.y = player.y, s.z = player.z;
    s.rotX = player.xRot, s.rotY = player.yRot;
    s.permissions = PermissionSet::forLevel(server.getProfilePermissions(player.profile().id));
    s.entity = EntityRef::of(player);
    s.output = Output::Player;
    s.outputPlayer = player.game;
    return s;
}

CommandSourceStack CommandSourceStack::forServer(MinecraftServer& server) {
    CommandSourceStack s;
    s.server = &server;
    s.level = &server.overworld(); // findRespawnDimension
    const auto& spawn = server.overworld().spawn();
    s.x = spawn.x, s.y = spawn.y, s.z = spawn.z; // Vec3.atLowerCornerOf(respawn pos)
    s.permissions = PermissionSet::forLevel(kLevelOwners);
    s.output = Output::Server;
    return s;
}

const CommandSourceStack& CommandSourceStack::noPermissions() {
    static const CommandSourceStack s = [] {
        CommandSourceStack c;
        c.permissions = PermissionSet::none();
        c.textName = "";
        c.displayNameOverride = Component::literal("");
        return c;
    }();
    return s;
}

Component CommandSourceStack::getDisplayName() const { return entity ? entity.displayName() : displayNameOverride; }
std::string CommandSourceStack::getTextName() const { return entity ? entity.plainName() : textName; }

ServerPlayer& CommandSourceStack::getPlayerOrException() const {
    if (!entity.player) throw brigadier::error("permissions.requires.player");
    return *entity.player;
}

EntityRef CommandSourceStack::getEntityOrException() const {
    if (!entity) throw brigadier::error("permissions.requires.entity");
    return entity;
}

void CommandSourceStack::anchorPosition(double& ax, double& ay, double& az) const {
    ax = x, ay = y, az = z;
    if (anchor == Anchor::Eyes && entity) ay += entity.eyeHeight();
}

bool CommandSourceStack::acceptsSuccess() const {
    switch (output) {
    case Output::Null: return false;
    case Output::Server: return true;
    case Output::Player: return outputPlayer && outputPlayer->player().level().gameRules().getBool(GameRules::SendCommandFeedback);
    }
    return false;
}

bool CommandSourceStack::shouldInformAdmins() const {
    switch (output) {
    case Output::Null: return false;
    case Output::Server: return server->properties().broadcastConsoleToOps;
    case Output::Player: return true;
    }
    return false;
}

void CommandSourceStack::sourceSendSystemMessage(const Component& message) const {
    switch (output) {
    case Output::Null: break;
    case Output::Server: server->sendSystemMessage(message); break;
    case Output::Player:
        if (outputPlayer) outputPlayer->sendSystemMessage(message);
        break;
    }
}

void CommandSourceStack::sendSuccess(const Component& message, bool broadcast) const {
    bool shouldSendSystemMessage = acceptsSuccess() && !silent;
    bool shouldBroadcast = broadcast && shouldInformAdmins() && !silent;
    if (shouldSendSystemMessage) sourceSendSystemMessage(message);
    if (shouldBroadcast) broadcastToAdmins(message);
}

void CommandSourceStack::broadcastToAdmins(const Component& message) const {
    Component broadcast = Component::translatable("chat.type.admin", {getDisplayName(), message});
    broadcast.withColor("gray").withItalic();
    GameRules& rules = level->gameRules();
    if (rules.getBool(GameRules::SendCommandFeedback)) {
        for (const auto& p : server->playerList().players()) {
            bool isSource = output == Output::Player && outputPlayer == p->game;
            if (!isSource && server->playerList().isOp(p->profile().id) && p->game) p->game->sendSystemMessage(broadcast);
        }
    }
    if (output != Output::Server && rules.getBool(GameRules::LogAdminCommands)) server->sendSystemMessage(broadcast);
}

void CommandSourceStack::sendFailure(const Component& message) const {
    if (acceptsFailure() && !silent) {
        Component red = Component::empty();
        red.append(message);
        red.withColor("red");
        sourceSendSystemMessage(red);
    }
}

void CommandSourceStack::sendSystemMessage(const Component& message) const {
    if (silent) return;
    if (ServerPlayer* p = getPlayer()) {
        if (p->game) p->game->sendSystemMessage(message);
    } else {
        sourceSendSystemMessage(message);
    }
}

void CommandSourceStack::sendChatMessage(const chat::PlayerChatMessage& message, const chat::ChatTypeBound& chatType) const {
    if (silent) return;
    if (ServerPlayer* p = getPlayer()) {
        if (p->game) p->game->sendChatMessage(message, chatType);
    } else {
        sourceSendSystemMessage(chatType.decorate(message.decoratedContent()));
    }
}

} // namespace mc::commands
