#pragma once

#include <map>
#include <memory>
#include <string>

#include "network/chat/component.hpp"
#include "network/chat/signed_chat.hpp"
#include "util/uuid.hpp"
#include "world/phys/aabb.hpp"

namespace mc {
class Entity;
class MinecraftServer;
class ServerGamePacketListener;
class ServerLevel;
class ServerPlayer;
} // namespace mc

namespace mc::commands {

// PermissionLevel ids (Commands.LEVEL_*).
inline constexpr int kLevelAll = 0;
inline constexpr int kLevelModerators = 1;
inline constexpr int kLevelGamemasters = 2;
inline constexpr int kLevelAdmins = 3;
inline constexpr int kLevelOwners = 4;

// PermissionSet: a LevelBasedPermissionSet, or NO_PERMISSIONS (what the command tree's
// "restricted" flag is judged against).
struct PermissionSet {
    int level = kLevelAll; // -1: no permissions at all
    static PermissionSet none() { return {-1}; }
    static PermissionSet forLevel(int level) { return {level}; }
    // hasPermission(Permission.HasCommandLevel(required))
    bool hasLevel(int required) const { return level >= 0 && level >= required; }
    // hasPermission(Permissions.COMMANDS_ENTITY_SELECTORS)
    bool entitySelectors() const { return level >= kLevelGamemasters; }
};

// An entity as commands see it: a player, or one of a level's simulated entities.
struct EntityRef {
    ServerPlayer* player = nullptr;
    Entity* entity = nullptr;
    ServerLevel* level = nullptr;

    static EntityRef of(ServerPlayer& player);
    static EntityRef of(Entity& entity, ServerLevel& level) { return {nullptr, &entity, &level}; }
    explicit operator bool() const { return player || entity; }
    bool operator==(const EntityRef& o) const { return player == o.player && entity == o.entity; }

    int id() const;
    UUID uuid() const;
    std::string_view type() const; // minecraft:entity_type id
    double x() const;
    double y() const;
    double z() const;
    float yRot() const;
    float xRot() const;
    AABB boundingBox() const;
    double eyeHeight() const;
    bool isAlive() const;
    bool isLiving() const { return player != nullptr; } // (LivingEntity: only players so far)
    double distanceToSqr(double px, double py, double pz) const;
    Component name() const;        // Entity.getName
    std::string plainName() const; // getPlainTextName
    Component displayName() const; // Entity.getDisplayName (with the entity hover and insertion)
};

// EntityAnchorArgument.Anchor
enum class Anchor { Feet, Eyes };

// Where a command's output goes (CommandSource): a player, the server console, or nowhere
// (CommandSource.NULL, e.g. signs).
enum class Output { Null, Server, Player };

// net.minecraft.commands.CommandSourceStack
class CommandSourceStack {
public:
    MinecraftServer* server = nullptr;
    ServerLevel* level = nullptr;
    double x = 0, y = 0, z = 0;
    float rotX = 0, rotY = 0; // Vec2(xRot, yRot)
    PermissionSet permissions;
    EntityRef entity;
    Output output = Output::Null;
    ServerGamePacketListener* outputPlayer = nullptr; // with Output::Player
    std::string textName = "Server";
    Component displayNameOverride = Component::literal("Server"); // (NamesProvider for no entity)
    bool silent = false;
    Anchor anchor = Anchor::Feet;
    // CommandSigningContext: the signed message arguments by name (none: anonymous).
    std::shared_ptr<const std::map<std::string, chat::PlayerChatMessage>> signedArguments;

    // ServerPlayer.createCommandSourceStack / MinecraftServer.createCommandSourceStack
    static CommandSourceStack forPlayer(MinecraftServer& server, ServerPlayer& player);
    static CommandSourceStack forServer(MinecraftServer& server);
    // Commands.createCompilationContext(NO_PERMISSIONS): judges which nodes are restricted.
    static const CommandSourceStack& noPermissions();

    bool hasPermission(int level) const { return permissions.hasLevel(level); }
    Component getDisplayName() const;
    std::string getTextName() const;
    ServerPlayer* getPlayer() const { return entity.player; }
    bool isPlayer() const { return entity.player != nullptr; }
    ServerPlayer& getPlayerOrException() const;
    EntityRef getEntityOrException() const;
    // anchor.apply(this): the eye position for EYES when there's an entity.
    void anchorPosition(double& ax, double& ay, double& az) const;

    void sendSuccess(const Component& message, bool broadcast) const;
    void sendFailure(const Component& message) const;
    void sendSystemMessage(const Component& message) const;
    // sendChatMessage(OutgoingChatMessage, filtered, bound)
    void sendChatMessage(const chat::PlayerChatMessage& message, const chat::ChatTypeBound& chatType) const;
    // ChatType.bind(type, this)
    chat::ChatTypeBound bindChatType(std::string type) const { return {std::move(type), getDisplayName(), std::nullopt}; }

private:
    bool acceptsSuccess() const;
    bool acceptsFailure() const { return output != Output::Null; }
    bool shouldInformAdmins() const;
    void sourceSendSystemMessage(const Component& message) const; // CommandSource.sendSystemMessage
    void broadcastToAdmins(const Component& message) const;
};

} // namespace mc::commands
