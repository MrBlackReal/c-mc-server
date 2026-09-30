#pragma once

#include <array>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

#include "network/chat/signed_chat.hpp"
#include "server/level/server_level.hpp"
#include "util/uuid.hpp"
#include "world/entity/ai/attributes/attributes.hpp"
#include "world/entity/player/inventory.hpp"
#include "world/inventory/menus.hpp"

namespace mc {

class Connection;
class ServerGamePacketListener;

// com.mojang.authlib.properties.Property: the session server's signed profile data (textures).
struct ProfileProperty {
    std::string name, value;
    std::optional<std::string> signature;
};

struct GameProfile {
    UUID id;
    std::string name;
    std::vector<ProfileProperty> properties; // (online mode: the skin)
};

// ByteBufCodecs.GAME_PROFILE_PROPERTIES: the profile's properties (name, value, optional signature).
void writeProfileProperties(ByteWriter& out, const GameProfile& profile);

enum class GameType : int { Survival = 0, Creative = 1, Adventure = 2, Spectator = 3 };
GameType gameTypeByName(std::string_view name, GameType fallback);

struct ClientInformation {
    std::string language = "en_us";
    int viewDistance = 2;
    int chatVisibility = 0;
    bool chatColors = true;
    int modelCustomisation = 0;
    int mainHand = 1;
    bool textFiltering = false;
    bool allowsListing = false;
    int particleStatus = 0;
};

// Port of PlayerChunkSender: rate-limited, client-acknowledged chunk batches.
class PlayerChunkSender {
public:
    void markPending(ChunkPos pos) { pending_.insert(pos.pack()); }
    bool isPending(ChunkPos pos) const { return pending_.contains(pos.pack()); }
    // Returns true if the chunk was still pending (never sent), false if the client has it.
    bool dropPending(ChunkPos pos) { return pending_.erase(pos.pack()) > 0; }
    void sendNextChunks(class ServerPlayer& player);
    void onBatchReceived(float desiredChunksPerTick);

private:
    std::unordered_set<int64_t> pending_;
    float desiredChunksPerTick_ = 9.0f;
    float batchQuota_ = 0.0f;
    int unacknowledgedBatches_ = 0;
    int maxUnacknowledgedBatches_ = 1;
};

class ServerPlayer {
public:
    ServerPlayer(int entityId, GameProfile profile, ServerLevel& level, Connection& connection, ClientInformation info);
    ~ServerPlayer();
    bool disconnected = false; // ServerPlayer.hasDisconnected: items that would go back to the inventory drop

    int id() const { return entityId_; }
    const GameProfile& profile() const { return profile_; }
    ServerLevel& level() { return *level_; }
    Connection& connection() { return *connection_; }
    // Player.getDisplayName: the name, with a click that suggests /tell, a hover showing the
    // player and the name as insertion (no teams yet).
    Component displayName() const;
    // ServerPlayer.chatSession: the validated chat session (its key signs the player's chat).
    std::optional<chat::RemoteChatSession> chatSession;

    double x = 0, y = 0, z = 0;
    float yRot = 0, xRot = 0;
    bool onGround = false;
    GameType gameMode = GameType::Survival;
    ClientInformation clientInfo;
    int latencyMs = 0;

    Inventory inventory;
    // The inventory menu (container 0) and the menu currently shown (Player.containerMenu).
    std::unique_ptr<InventoryMenu> inventoryMenu;
    AbstractContainerMenu* containerMenu = nullptr;
    // Player.enderChestInventory ("EnderItems").
    SimpleContainer enderChest{27};
    // ServerPlayer.openMenu: closes what's open, shows `menu` (created for nextContainerId()).
    void openMenu(std::unique_ptr<AbstractContainerMenu> menu, int menuType, const nbt::Tag& title); // title: a text component
    int nextContainerId() { return containerCounter_ = containerCounter_ % 100 + 1; }
    // ServerPlayer.closeContainer (tells the client) / doCloseContainer (the client closed it).
    void closeContainer();
    void doCloseContainer();
    nbt::Compound savedTag; // players/data/<uuid>.dat as loaded, for fields we pass through
    bool sneaking = false;
    bool sprinting = false; // ServerboundPlayerCommandPacket START/STOP_SPRINTING
    bool flying = false;    // ServerboundPlayerAbilitiesPacket (only while the game mode allows it)

    // ServerPlayerGameMode block breaking state (ticks count ServerPlayerGameMode.gameTicks, from join).
    uint64_t gameTicks = 0;
    bool isDestroyingBlock = false;
    BlockPos destroyPos;
    Direction destroyDirection = Direction::Down;
    uint64_t destroyStartTick = 0;
    bool hasDelayedDestroy = false;
    BlockPos delayedDestroyPos;
    uint64_t delayedTickStart = 0;
    int lastSentDestroyStage = -1;
    int ackBlockChangesUpTo = -1;
    int dropSpam = 0; // ServerGamePacketListenerImpl.dropSpamThrottler

    ServerGamePacketListener* game = nullptr; // set by the listener

    // Sleeping (LivingEntity.getSleepingPos, Player.sleepCounter) and the spawn point
    // (ServerPlayer.RespawnConfig, saved as "respawn").
    std::optional<BlockPos> sleepingPos;
    int sleepCounter = 0;
    struct RespawnConfig {
        std::string dimension;
        BlockPos pos;
        float yaw = 0, pitch = 0;
        bool forced = false;
        bool samePosition(const RespawnConfig& o) const { return dimension == o.dimension && pos == o.pos; }
    };
    std::optional<RespawnConfig> respawn;

    // Experience (Player.experienceLevel, experienceProgress, totalExperience, ...).
    int experienceLevel = 0;
    float experienceProgress = 0;
    int totalExperience = 0;
    int enchantmentSeed = 0;
    int score = 0;
    int takeXpDelay = 0;
    int64_t lastLevelUpTime = 0;
    int lastSentExp = -99999999;
    int xpNeededForNextLevel() const;
    void giveExperiencePoints(int amount);
    void giveExperienceLevels(int amount);
    // Player.tick's takeXpDelay countdown and ServerPlayer.doTick's ClientboundSetExperiencePacket.
    void tickExperience();

    // Active mob effects (LivingEntity.activeEffects), by minecraft:mob_effect id.
    struct EffectInstance {
        int amplifier = 0;
        int duration = 0; // ticks; -1 = infinite
        bool ambient = false, visible = true, showIcon = true;
    };
    std::map<int, EffectInstance> effects;
    // LivingEntity.addEffect / removeEffect: also tells the client.
    void addEffect(int effect, EffectInstance e);
    void removeEffect(int effect);
    bool hasEffect(int effect) const { return effects.contains(effect); }
    // LivingEntity.tickEffects: counts down and removes what ran out.
    void tickEffects();

    // Attribute modifiers from equipment (item and enchantment modifiers) and effects, and the
    // resulting value (LivingEntity.getAttributeValue).
    std::vector<attributes::Modifier> attributeModifiers(int attribute);
    double attributeValue(int attribute);
    // Sends changed client-syncable attributes (ClientboundUpdateAttributesPacket) to the player;
    // `all` sends every syncable attribute (on join).
    void syncAttributes(bool all);

    // Player.drop / createItemStackToDrop: throws `stack` from eye height in the look direction.
    ItemEntity* drop(ItemStack stack, bool thrownFromHand);

    std::unordered_set<int> seenEntities; // entities this client has been sent (ChunkMap.TrackedEntity)

    // Last equipment sent to other players (ServerEntity.sendDirtyEntityData), by EquipmentSlot.
    std::array<ItemStack, 6> sentEquipment{};
    // Syncable attribute values last sent to the client (base + modifiers), by attribute id.
    std::map<int, std::vector<attributes::Modifier>> sentAttributes;

    bool hasInfiniteMaterials() const { return gameMode == GameType::Creative; }
    AABB boundingBox() const { return {x - 0.3, y, z - 0.3, x + 0.3, y + (sneaking ? 1.5 : 1.8), z + 0.3}; }
    double eyeY() const { return y + (sneaking ? 1.27 : 1.62); }
    double blockInteractionRange() const { return gameMode == GameType::Creative ? 5.0 : 4.5; }
    // Player.isWithinBlockInteractionRange
    bool isWithinBlockInteractionRange(BlockPos pos, double buffer) const;

    ChunkPos chunkPosition() const { return ChunkPos::ofBlock(x, z); }
    // True once the client has been sent this chunk (so it needs block updates for it).
    bool hasChunk(ChunkPos pos) const { return tracked_.contains(pos.pack()) && !chunkSender_.isPending(pos); }
    int effectiveViewDistance(int serverViewDistance) const;

    // ChunkMap.updateChunkTracking: diff the tracked area against the player's current view.
    void updateChunkTracking(int serverViewDistance);
    PlayerChunkSender& chunkSender() { return chunkSender_; }

    // Movement broadcast bookkeeping (ServerEntity).
    bool consumeMoved();

private:
    int entityId_;
    GameProfile profile_;
    ServerLevel* level_;
    Connection* connection_;
    PlayerChunkSender chunkSender_;
    std::unordered_set<int64_t> tracked_;
    bool trackingStarted_ = false;
    ChunkPos lastCenter_{};
    int lastViewDistance_ = 0;
    double sentX_ = 0, sentY_ = 0, sentZ_ = 0;
    float sentYRot_ = 0, sentXRot_ = 0;
    std::unique_ptr<AbstractContainerMenu> openMenu_; // containerMenu, unless that's the inventory
    int containerCounter_ = 0;
};

} // namespace mc
