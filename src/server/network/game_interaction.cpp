// Block breaking/placing and inventory handling for ServerGamePacketListener.
// Ports of ServerGamePacketListenerImpl (handlePlayerAction, handleUseItemOn, handleSetCreativeModeSlot,
// handleSetCarriedItem, handlePickItemFromBlock, handleContainerClick) and ServerPlayerGameMode.
#include <cmath>

#include "core/registries.hpp"
#include "generated/behaviour.hpp"
#include "generated/packet_ids.hpp"
#include "server/minecraft_server.hpp"
#include "server/network/listeners.hpp"
#include "util/log.hpp"
#include "world/entity/ai/attributes/attributes.hpp"
#include "world/item/adventure_mode_predicate.hpp"
#include "world/item/enchantment/enchantment_helper.hpp"
#include "world/item/item_logic.hpp"
#include "world/level/block/entity/block_entity_components.hpp"
#include "world/item/item_nbt.hpp"
#include "world/level/block/block_behaviour.hpp"
#include "world/level/storage/loot/loot_tables.hpp"

namespace mc {

namespace cb = packets::play_cb;
namespace bs = blocks;

namespace {
enum PlayerAction {
    StartDestroyBlock = 0,
    ChangeDestroyDirection = 1,
    AbortDestroyBlock = 2,
    StopDestroyBlock = 3,
    DropAllItems = 4,
    DropItem = 5,
    ReleaseUseItem = 6,
    SwapItemWithOffhand = 7,
    Stab = 8,
};
} // namespace

// Player.blockActionRestricted: spectators can't touch blocks; adventure players only with an item
// whose can_break allows it.
bool ServerGamePacketListener::blockActionRestricted(BlockPos pos) {
    if (player_.gameMode == GameType::Spectator) return true;
    if (player_.gameMode != GameType::Adventure) return false;
    const ItemStack& held = player_.inventory.selectedItem();
    return held.isEmpty() || !adventure::test(held, "minecraft:can_break", player_.level(), pos);
}

void ServerGamePacketListener::sendBlockUpdate(BlockPos pos) {
    Packet p(cb::BLOCK_UPDATE);
    p.writeBlockPos(pos.x, pos.y, pos.z);
    p.writeVarInt(static_cast<int32_t>(player_.level().getBlockState(pos)));
    conn_.send(p);
}

void ServerGamePacketListener::swing(int hand, bool includeSelf) {
    // ClientboundSwingAnimationPacket with SwingAnimation.DEFAULT (whack, 6 ticks).
    Packet p(cb::SWING_ANIMATION);
    p.writeVarInt(player_.id());
    p.writeVarInt(hand);
    p.writeVarInt(1);
    p.writeVarInt(6);
    server_.playerList().broadcastAll(p, includeSelf ? nullptr : &player_);
}

// ---------------------------------------------------------------- breaking

void ServerGamePacketListener::handlePlayerAction(ByteReader& in) {
    int action = in.readVarInt();
    BlockPos pos = BlockPos::fromLong(in.readLong());
    int face = in.readUnsignedByte();
    int sequence = in.readVarInt();
    switch (action) {
    case StartDestroyBlock:
    case ChangeDestroyDirection:
    case AbortDestroyBlock:
    case StopDestroyBlock:
        if (face <= 5) player_.destroyDirection = static_cast<Direction>(face);
        handleBlockBreakAction(action, pos, sequence);
        ackBlockChangesUpTo(sequence);
        break;
    case SwapItemWithOffhand:
        if (player_.gameMode != GameType::Spectator)
            std::swap(player_.inventory.selectedItem(), player_.inventory.equipment(EquipmentSlot::OffHand));
        break;
    case DropItem:
    case DropAllItems: {
        // Inventory.removeFromSelected, then Player.drop(stack, false, true).
        if (player_.gameMode == GameType::Spectator) break;
        ItemStack& held = player_.inventory.selectedItem();
        if (held.isEmpty()) break;
        player_.drop(held.split(action == DropAllItems ? held.count : 1), true);
        break;
    }
    default: break; // RELEASE_USE_ITEM, STAB: need item use / combat
    }
}

float ServerGamePacketListener::destroyProgress(BlockStateId state) {
    // BlockBehaviour.getDestroyProgress + Player.getDestroySpeed
    float hardness = bs::behaviour(bs::blockOf(state)).hardness;
    if (hardness < 0) return 0.0f;
    const ItemStack& tool = player_.inventory.selectedItem();
    bool correct = !(bs::flags(state) & bs::kRequiresCorrectTool) || items::isCorrectForDrops(tool, state);
    static const int kEfficiency = *attributes::byName("minecraft:mining_efficiency");
    static const int kBreakSpeed = *attributes::byName("minecraft:block_break_speed");
    static const int kSubmerged = *attributes::byName("minecraft:submerged_mining_speed");
    static const int kHaste = registries::id("minecraft:mob_effect", "minecraft:haste").value_or(-1);
    static const int kConduit = registries::id("minecraft:mob_effect", "minecraft:conduit_power").value_or(-1);
    static const int kFatigue = registries::id("minecraft:mob_effect", "minecraft:mining_fatigue").value_or(-1);
    float speed = items::miningSpeed(tool, state);
    if (speed > 1.0f) speed += static_cast<float>(player_.attributeValue(kEfficiency));
    if (player_.hasEffect(kHaste) || player_.hasEffect(kConduit)) { // MobEffectUtil.getDigSpeedAmplification
        int amp = std::max(player_.hasEffect(kHaste) ? player_.effects[kHaste].amplifier : 0,
                           player_.hasEffect(kConduit) ? player_.effects[kConduit].amplifier : 0);
        speed *= 1.0f + static_cast<float>(amp + 1) * 0.2f;
    }
    if (player_.hasEffect(kFatigue)) speed *= static_cast<float>(std::pow(0.3, player_.effects[kFatigue].amplifier + 1));
    speed *= static_cast<float>(player_.attributeValue(kBreakSpeed));
    // isEyeInFluid(WATER)
    BlockStateId eye = player_.level().getBlockState(static_cast<int>(std::floor(player_.x)), static_cast<int>(std::floor(player_.eyeY())),
                                                     static_cast<int>(std::floor(player_.z)));
    if (bs::flags(eye) & bs::kWater) speed *= static_cast<float>(player_.attributeValue(kSubmerged));
    if (!player_.onGround) speed /= 5.0f;
    if (hardness == 0.0f) return 1.0e9f;
    return speed / hardness / (correct ? 30.0f : 100.0f);
}

void ServerGamePacketListener::sendDestroyProgress(BlockPos pos, int stage) {
    // ServerLevel.destroyBlockProgress: everyone within 32 blocks except the breaker.
    Packet p(cb::BLOCK_DESTRUCTION);
    p.writeVarInt(player_.id());
    p.writeBlockPos(pos.x, pos.y, pos.z);
    p.writeByte(static_cast<uint8_t>(stage));
    for (const auto& other : server_.playerList().players()) {
        if (other.get() == &player_) continue;
        double dx = pos.x - other->x, dy = pos.y - other->y, dz = pos.z - other->z;
        if (dx * dx + dy * dy + dz * dz < 1024.0) other->connection().send(p);
    }
}

float ServerGamePacketListener::incrementDestroyProgress(BlockStateId state, BlockPos pos, int ticksSpentDestroying) {
    float progress = destroyProgress(state) * static_cast<float>(ticksSpentDestroying + 1);
    // (int)(progress * 10): not clamped (stages past 10 are sent too); Java saturates huge values.
    int stage = static_cast<int>(std::clamp(progress * 10.0f, -2.0e9f, 2.0e9f));
    if (stage != player_.lastSentDestroyStage) {
        sendDestroyProgress(pos, stage);
        player_.lastSentDestroyStage = stage;
    }
    return progress;
}

void ServerGamePacketListener::tickDestroy() {
    // ServerPlayerGameMode.tick
    player_.gameTicks++;
    ServerLevel& level = player_.level();
    if (player_.hasDelayedDestroy) {
        BlockStateId state = level.getBlockState(player_.delayedDestroyPos);
        if (bs::isAir(state)) {
            player_.hasDelayedDestroy = false;
        } else if (incrementDestroyProgress(state, player_.delayedDestroyPos, static_cast<int>(player_.delayedTickStart)) >= 1.0f) {
            // Vanilla passes the tick the digging *started* as the ticks spent (not the elapsed
            // time), so once a player has been online a while a delayed destroy finishes at once.
            player_.hasDelayedDestroy = false;
            destroyBlock(player_.delayedDestroyPos);
        }
    } else if (player_.isDestroyingBlock) {
        BlockStateId state = level.getBlockState(player_.destroyPos);
        if (bs::isAir(state)) {
            sendDestroyProgress(player_.destroyPos, -1);
            player_.lastSentDestroyStage = -1;
            player_.isDestroyingBlock = false;
        } else {
            int ticks = static_cast<int>(player_.gameTicks - player_.destroyStartTick);
            incrementDestroyProgress(state, player_.destroyPos, ticks);
            // LevelEvent.PARTICLES_(AND_SOUND_)DESTROY_PROGRESS, for everyone nearby.
            Packet ev(cb::LEVEL_EVENT);
            ev.writeInt(ticks % 4 == 0 ? 2020 : 2019);
            ev.writeBlockPos(player_.destroyPos.x, player_.destroyPos.y, player_.destroyPos.z);
            ev.writeInt(static_cast<int>(player_.destroyDirection));
            ev.writeBool(false);
            for (const auto& other : server_.playerList().players()) {
                double dx = player_.destroyPos.x - other->x, dz = player_.destroyPos.z - other->z;
                if (dx * dx + dz * dz < 64.0 * 64.0) other->connection().send(ev);
            }
        }
    }
}

void ServerGamePacketListener::handleBlockBreakAction(int action, BlockPos pos, int sequence) {
    // ServerPlayerGameMode.handleBlockBreakAction
    ServerLevel& level = player_.level();
    if (!player_.isWithinBlockInteractionRange(pos, 1.0)) {
        if (player_.isDestroyingBlock && action == AbortDestroyBlock) abortDestroy(pos);
        return;
    }
    if (pos.y > level.maxY()) {
        sendBlockUpdate(pos);
        return;
    }
    if (action == StartDestroyBlock) {
        if (blockActionRestricted(pos)) {
            sendBlockUpdate(pos);
            return;
        }
        if (player_.hasInfiniteMaterials()) { // abilities.instabuild
            if (!destroyBlock(pos)) sendBlockUpdate(pos);
            return;
        }
        player_.destroyStartTick = player_.gameTicks;
        BlockStateId state = level.getBlockState(pos);
        if (!bs::isAir(state)) block_behaviour::attack(level, pos, state); // BlockState.attack (note blocks)
        float progress = bs::isAir(state) ? 1.0f : destroyProgress(state);
        if (!bs::isAir(state) && progress >= 1.0f) { // insta-mine
            if (!destroyBlock(pos)) sendBlockUpdate(pos);
            return;
        }
        if (player_.isDestroyingBlock) sendBlockUpdate(player_.destroyPos); // client insta-mined, server disagreed
        player_.isDestroyingBlock = true;
        player_.destroyPos = pos;
        int stage = static_cast<int>(progress * 10.0f);
        sendDestroyProgress(pos, stage);
        player_.lastSentDestroyStage = stage;
    } else if (action == StopDestroyBlock) {
        if (pos == player_.destroyPos) {
            int ticks = static_cast<int>(player_.gameTicks - player_.destroyStartTick);
            BlockStateId state = level.getBlockState(pos);
            if (!bs::isAir(state)) {
                float progress = destroyProgress(state) * static_cast<float>(ticks + 1);
                if (progress >= 0.7f) { // allows for a little lag
                    player_.isDestroyingBlock = false;
                    sendDestroyProgress(pos, -1);
                    if (!destroyBlock(pos)) sendBlockUpdate(pos);
                    return;
                }
                if (!player_.hasDelayedDestroy) { // finish it once the server catches up
                    player_.isDestroyingBlock = false;
                    player_.hasDelayedDestroy = true;
                    player_.delayedDestroyPos = pos;
                    player_.delayedTickStart = player_.destroyStartTick;
                }
            }
        }
    } else if (action == AbortDestroyBlock) {
        abortDestroy(pos);
    }
    (void)sequence;
}

void ServerGamePacketListener::abortDestroy(BlockPos pos) {
    player_.isDestroyingBlock = false;
    if (!(player_.destroyPos == pos)) sendDestroyProgress(player_.destroyPos, -1);
    sendDestroyProgress(pos, -1);
}

void ServerGamePacketListener::hurtTool(ItemStack& tool, BlockStateId state) {
    // ItemStack.mineBlock -> Tool.mineBlock -> hurtAndBreak
    if (tool.isEmpty() || tool.info().toolDefaultSpeed < 0) return;
    if (bs::behaviour(bs::blockOf(state)).hardness == 0.0f) return;
    hurtAndBreak(tool, items::toolDamagePerBlock(tool), 0);
}

void ServerGamePacketListener::hurtAndBreak(ItemStack& stack, int amount, int hand) {
    // ItemStack.hurtAndBreak: not in creative, not for unbreakable items; enchantments'
    // item_damage effects (Unbreaking) reduce the damage.
    if (stack.isEmpty() || player_.hasInfiniteMaterials() || amount <= 0) return;
    int max = items::maxDamage(stack);
    if (max <= 0 || items::findComponent(stack, "minecraft:unbreakable")) return;
    int applied = enchantments::processDurabilityChange(player_.level(), stack, amount);
    if (applied <= 0) return;
    int damage = items::damage(stack) + applied;
    if (damage >= max) {
        stack = {};
        Packet ev(cb::ENTITY_EVENT); // EntityEvent.MAINHAND_BREAK / OFFHAND_BREAK: break particles and sound
        ev.writeInt(player_.id());
        ev.writeByte(hand == 0 ? 47 : 48);
        server_.playerList().broadcastAll(ev);
    } else {
        items::setDamage(stack, damage);
    }
}

bool ServerGamePacketListener::destroyBlock(BlockPos pos) {
    // ServerPlayerGameMode.destroyBlock
    ServerLevel& level = player_.level();
    BlockStateId state = level.getBlockState(pos);
    if (bs::isAir(state) || blockActionRestricted(pos)) return false;
    ItemStack& held = player_.inventory.selectedItem();
    bool creative = player_.hasInfiniteMaterials();
    if (creative && !held.isEmpty() && held.info().noCreativeDestroy) return false; // swords etc.
    uint32_t block = bs::blockOf(state);
    if (bs::behaviour(block).hardness < 0.0f && !creative) return false; // bedrock, barriers

    std::optional<nbt::Compound> blockEntity;
    if (auto* be = level.getChunk({pos.x >> 4, pos.z >> 4}).blockEntityAt(pos.x, pos.y, pos.z)) blockEntity = be->tag;
    bool canHarvest = !(bs::flags(state) & bs::kRequiresCorrectTool) || items::isCorrectForDrops(held, state);

    // Block.playerWillDestroy: break particles for everyone but the breaker...
    level.levelEvent(2001, pos, static_cast<int>(state), player_.id());
    // ...and for doors/tall plants broken from the top without drops, remove the bottom half
    // quietly so the neighbour update doesn't drop it (DoublePlantBlock.preventDropFromBottomPart).
    bool door = bs::isInstance(block, "DoorBlock"), plant = bs::isInstance(block, "DoublePlantBlock");
    if ((door && (creative || !canHarvest)) || (plant && creative)) {
        BlockPos below = pos.below();
        BlockStateId bottom = level.getBlockState(below);
        if (bs::getValue(state, "half") == "upper" && bs::blockOf(bottom) == block && bs::getValue(bottom, "half") == "lower") {
            BlockStateId repl = bs::getBool(bottom, "waterlogged") ? bs::defaultState(gen::block::WATER) : bs::defaultState(gen::block::AIR);
            level.setBlock(below, repl, ServerLevel::kUpdateAll | ServerLevel::kSuppressDrops);
            level.levelEvent(2001, below, static_cast<int>(bottom), player_.id());
        }
    }

    // DecoratedPotBlock.playerWillDestroy: tools that break pots crack them (drops sherds, not the pot),
    // unless an enchantment prevents shattering.
    if (bs::isInstance(block, "DecoratedPotBlock") && items::hasTag(held.item, "minecraft:breaks_decorated_pots")) {
        bool prevented = false;
        for (auto ench : loot::registryTagEntries("minecraft:enchantment", "minecraft:prevents_decorated_pot_shattering"))
            prevented = prevented || items::enchantmentLevel(held, ench) > 0;
        if (!prevented) state = bs::setBool(state, "cracked", true);
    }

    bool shulker = bs::isInstance(block, "ShulkerBoxBlock");
    ItemStack toolBefore = held;
    // Level.removeBlock keeps the fluid of waterlogged blocks.
    BlockStateId replacement = bs::getBool(state, "waterlogged") ? bs::defaultState(gen::block::WATER) : bs::defaultState(gen::block::AIR);
    level.setBlock(pos, replacement); // (containers spill their contents, in creative too: preRemoveSideEffects)
    if (!creative) {
        hurtTool(held, state);
        if (canHarvest) level.dropResources(state, pos, blockEntity ? &*blockEntity : nullptr, &toolBefore, &player_);
    } else if (shulker && blockEntity && blockEntity->getList("Items") && !blockEntity->getList("Items")->items.empty()) {
        // ShulkerBoxBlock.playerWillDestroy: in creative a non-empty box still drops, contents kept.
        level.dropResources(state, pos, &*blockEntity, nullptr, &player_);
    }
    return true;
}

// ---------------------------------------------------------------- placing and using

void ServerGamePacketListener::handleUseItemOn(ByteReader& in) {
    int hand = in.readVarInt();
    BlockPos pos = BlockPos::fromLong(in.readLong());
    int faceId = in.readVarInt();
    float cx = in.readFloat(), cy = in.readFloat(), cz = in.readFloat();
    in.readBool(); // inside
    in.readBool(); // world border hit
    int sequence = in.readVarInt();
    ackBlockChangesUpTo(sequence);
    if (faceId < 0 || faceId > 5 || (hand != 0 && hand != 1)) return;
    Direction face = static_cast<Direction>(faceId);
    double hx = pos.x + cx, hy = pos.y + cy, hz = pos.z + cz;

    ServerLevel& level = player_.level();
    if (!player_.isWithinBlockInteractionRange(pos, 1.0)) return;
    if (std::abs(hx - (pos.x + 0.5)) >= 1.0000001 || std::abs(hy - (pos.y + 0.5)) >= 1.0000001 ||
        std::abs(hz - (pos.z + 0.5)) >= 1.0000001) {
        log::warn("Rejecting UseItemOnPacket from {}: location too far away from hit block", player_.profile().name);
        return;
    }
    if (pos.y >= level.minY() && pos.y <= level.maxY() && awaitingTeleport_ == 0) {
        UseResult result = useItemOn(hand, pos, face, hx, hy, hz);
        if (result == UseResult::Success) swing(hand, false); // predicted by the client: others see the swing
        else if (result == UseResult::SuccessServer) swing(hand, true);
    }
    // Always resync the two blocks the client may have predicted.
    sendBlockUpdate(pos);
    sendBlockUpdate(pos.relative(face));
}

// ServerPlayerGameMode.useItemOn
UseResult ServerGamePacketListener::useItemOn(int hand, BlockPos hitPos, Direction face, double hx, double hy, double hz) {
    if (player_.gameMode == GameType::Spectator) { // spectators may look into containers (not change them)
        // (containers with an unrolled loot table would refuse: BaseContainerBlockEntity.canOpen)
        return openBlockMenu(hitPos, player_.level().getBlockState(hitPos)) ? UseResult::Consume : UseResult::Pass;
    }
    ServerLevel& level = player_.level();
    BlockStateId state = level.getBlockState(hitPos);
    ItemStack& stack = itemInHand(hand);
    bool holdingSomething = !player_.inventory.selectedItem().isEmpty() || !player_.inventory.equipment(EquipmentSlot::OffHand).isEmpty();
    bool suppressUsingBlock = player_.sneaking && holdingSomething;
    if (!suppressUsingBlock) {
        UseResult itemUse = useItemOnBlock(stack, hand, hitPos, state, face, hx, hy, hz);
        if (itemUse == UseResult::Consume || itemUse == UseResult::Success || itemUse == UseResult::SuccessServer) return itemUse;
        if (itemUse == UseResult::TryEmptyHand && hand == 0) {
            UseResult use = useBlockWithoutItem(hitPos, state, face, hx, hy, hz);
            if (use == UseResult::Consume || use == UseResult::Success || use == UseResult::SuccessServer) return use;
        }
    }
    if (stack.isEmpty()) return UseResult::Pass;
    // ItemStack.useOn: without mayBuild, only on blocks the item's can_place_on names.
    if (!canModifyWorld(player_.gameMode) && !adventure::test(stack, "minecraft:can_place_on", level, hitPos)) return UseResult::Pass;
    // In creative the stack keeps its count, whatever the use did to it.
    int count = stack.count;
    int32_t item = stack.item;
    UseResult result = useItemStackOn(stack, hand, hitPos, face, hx, hy, hz);
    if (player_.hasInfiniteMaterials() && stack.item == item) stack.count = count;
    return result;
}

// Port of BlockItem.place.
bool ServerGamePacketListener::placeBlock(ItemStack& stack, BlockPos hitPos, Direction face, double hx, double hy, double hz) {
    ServerLevel& level = player_.level();
    PlaceContext ctx{level, hitPos, face, hx, hy, hz, player_.yRot, player_.xRot, player_.sneaking, stack, player_.gameMode == GameType::Creative,
                     false, hitPos};
    ctx.replaceClicked = block_behaviour::canBeReplaced(level.getBlockState(hitPos), ctx);
    ctx.pos = ctx.replaceClicked ? hitPos : hitPos.relative(face);
    if (ctx.pos.y < level.minY() || ctx.pos.y > level.maxY()) return false;
    if (!ctx.replaceClicked && !block_behaviour::canBeReplaced(level.getBlockState(ctx.pos), ctx)) return false;
    if (!block_behaviour::updatePlacementContext(ctx)) return false;
    if (ctx.pos.y < level.minY() || ctx.pos.y > level.maxY()) return false;

    const gen::ItemInfo& info = stack.info();
    std::optional<BlockStateId> placement;
    if (info.wallBlock >= 0) {
        // StandingAndWallBlockItem.getPlacementState: pick the standing or wall variant by look direction.
        Direction attach = static_cast<Direction>(info.attachment);
        auto wall = block_behaviour::getStateForPlacement(static_cast<uint32_t>(info.wallBlock), ctx);
        for (Direction d : ctx.nearestLookingDirections()) {
            if (d == direction::opposite(attach)) continue;
            auto candidate = d == attach ? block_behaviour::getStateForPlacement(static_cast<uint32_t>(info.block), ctx) : wall;
            if (candidate && block_behaviour::canSurvive(*candidate, level, ctx.pos)) {
                placement = candidate;
                break;
            }
        }
    } else {
        placement = block_behaviour::getStateForPlacement(static_cast<uint32_t>(info.block), ctx);
        if (placement && !block_behaviour::canSurvive(*placement, level, ctx.pos)) placement.reset();
    }
    if (!placement) return false;

    // Level.isUnobstructed: the new block's collision shape mustn't overlap a player.
    for (const auto& p : server_.playerList().players()) {
        if (p->gameMode == GameType::Spectator) continue;
        AABB box = p->boundingBox();
        for (const auto& b : gen::kShapes[gen::kStateShape[*placement]]) {
            AABB shape{ctx.pos.x + b.minX, ctx.pos.y + b.minY, ctx.pos.z + b.minZ, ctx.pos.x + b.maxX, ctx.pos.y + b.maxY, ctx.pos.z + b.maxZ};
            if (shape.intersects(box)) return false;
        }
    }

    if (!level.setBlock(ctx.pos, *placement)) return false;
    BlockStateId placed = level.getBlockState(ctx.pos);
    createBlockEntityFromItem(ctx.pos, placed, stack);
    block_behaviour::setPlacedBy(level, ctx.pos, placed);
    if (bs::isInstance(bs::blockOf(placed), "SignBlock")) signPlaced(ctx.pos);

    // Place sound for everyone else (the placing client plays it locally).
    const auto& beh = bs::behaviour(bs::blockOf(placed));
    level.playBlockSound(player_.id(), ctx.pos, beh.placeSound, (beh.soundVolume + 1.0f) / 2.0f, beh.soundPitch * 0.8f);

    if (!player_.hasInfiniteMaterials()) {
        stack.shrink(1);
        stack.clearIfEmpty();
    }
    return true;
}

void ServerGamePacketListener::createBlockEntityFromItem(BlockPos pos, BlockStateId placed, const ItemStack& stack) {
    // BlockItem.updateCustomBlockEntityTag + updateBlockEntityComponents, on the block entity the
    // chunk created with the block.
    LevelChunk& chunk = player_.level().getChunk({pos.x >> 4, pos.z >> 4});
    auto* be = chunk.blockEntityAt(pos.x, pos.y, pos.z);
    if (!be || be->blockAtLoad != bs::blockOf(placed)) return;
    // Player.canUseGameMasterBlocks: creative and op; there are no ops yet, so creative decides.
    block_entity::applyItem(be->tag, stack, player_.gameMode == GameType::Creative);
    chunk.dirty = true;
}

// ---------------------------------------------------------------- inventory

void ServerGamePacketListener::handleSetCreativeModeSlot(ByteReader& in) {
    int slot = in.readShort();
    ItemStack stack = ItemStack::readUntrusted(in);
    if (!player_.hasInfiniteMaterials()) return;
    bool validData = stack.isEmpty() || stack.count <= stack.maxStackSize();
    if (slot >= 1 && slot <= 45 && validData) {
        player_.inventoryMenu->slot(slot).setByPlayer(stack);
        player_.inventoryMenu->setRemoteSlot(slot, stack);
        player_.inventoryMenu->broadcastChanges();
    } else if (slot < 0 && validData && !stack.isEmpty()) {
        // Throwing an item out of the creative menu, rate limited like vanilla.
        if (player_.dropSpam < 200) {
            player_.dropSpam += 20;
            player_.drop(std::move(stack), true);
        } else {
            log::warn("Player {} was dropping items too fast in creative mode, ignoring.", player_.profile().name);
        }
    }
}

void ServerGamePacketListener::handleSetCarriedItem(ByteReader& in) {
    int slot = in.readShort();
    if (slot >= 0 && slot < Inventory::kHotbarSize) {
        player_.inventory.setSelected(slot);
        player_.isDestroyingBlock = false;
    } else {
        log::warn("{} tried to set an invalid carried item", player_.profile().name);
    }
}

void ServerGamePacketListener::handlePickItemFromBlock(ByteReader& in) {
    BlockPos pos = BlockPos::fromLong(in.readLong());
    in.readBool(); // includeData (block entity data; none yet)
    if (!player_.isWithinBlockInteractionRange(pos, 1.0)) return;
    BlockStateId state = player_.level().getBlockState(pos);
    int32_t item = bs::behaviour(bs::blockOf(state)).item;
    if (item <= 0) return;
    ItemStack stack(item, 1);
    Inventory& inv = player_.inventory;
    int existing = inv.findSlotMatchingItem(stack);
    if (existing != -1) {
        if (existing < Inventory::kHotbarSize) inv.setSelected(existing);
        else inv.pickSlot(existing);
    } else if (player_.hasInfiniteMaterials()) {
        inv.addAndPickItem(stack);
    }
    Packet p(cb::SET_HELD_SLOT);
    p.writeVarInt(inv.selected());
    conn_.send(p);
}

void ServerGamePacketListener::handleContainerClick(ByteReader& in) {
    int containerId = in.readVarInt();
    int stateId = in.readVarInt();
    int slot = in.readShort();
    int button = in.readByte();
    int type = in.readVarInt();
    int changed = in.readVarInt();
    if (changed < 0 || changed > 128) throw DecoderException("too many changed slots");
    std::vector<std::pair<int, HashedStack>> changedSlots;
    for (int i = 0; i < changed; i++) {
        int s = in.readShort();
        changedSlots.emplace_back(s, HashedStack::read(in));
    }
    HashedStack carried = HashedStack::read(in);

    // ServerGamePacketListenerImpl.handleContainerClick
    AbstractContainerMenu& menu = *player_.containerMenu;
    if (menu.containerId != containerId) return;
    if (player_.gameMode == GameType::Spectator) {
        menu.sendAllDataToRemote();
        return;
    }
    if (!menu.stillValid(player_)) return;
    if (!menu.isValidSlotIndex(slot) || type < 0 || type > 6) return;
    bool fullResyncNeeded = stateId != menu.stateId();
    menu.suppressRemoteUpdates();
    menu.clicked(slot, button, static_cast<AbstractContainerMenu::ClickType>(type), player_);
    // The client's predicted result becomes the remote state; broadcastChanges then resends only
    // the slots whose real contents don't match it (compared by component hashes).
    for (auto& [s, stack] : changedSlots) menu.setRemoteSlotUnsafe(s, std::move(stack));
    menu.setRemoteCarried(std::move(carried));
    menu.resumeRemoteUpdates();
    if (fullResyncNeeded) menu.broadcastFullState();
    else menu.broadcastChanges();
}

} // namespace mc
