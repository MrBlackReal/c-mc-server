// ItemStack.useOn for items that aren't blocks: data-driven block transformers (axes strip and
// scrape, shovels make paths, hoes till), honeycomb, flint and steel and fire charges.
#include <random>

#include "core/registries.hpp"
#include "generated/behaviour.hpp"
#include "server/network/listeners.hpp"
#include "util/json.hpp"
#include "util/log.hpp"
#include "world/entity/item_entity.hpp"
#include "world/item/item_logic.hpp"
#include "world/level/block/block_behaviour.hpp"
#include "world/level/block/block_state.hpp"
#include "world/level/levelgen/block_predicate.hpp"
#include "world/level/storage/loot/loot_tables.hpp"
#include "world/level/material/fluids.hpp"
#include "util/mth.hpp"

namespace mc {

namespace bs = blocks;

namespace {

int32_t soundId(std::string_view name) { return registries::id("minecraft:sound_event", name).value_or(-1); }

// data/block_transformers.json: minecraft:block_transformer entries by id.
const json::Value& transformers() {
    static const json::Value v = [] {
        try {
            return json::parse(gen::kBlockTransformersJson);
        } catch (const std::exception& e) {
            log::error("Failed to parse block transformers: {}", e.what());
            return json::Value{};
        }
    }();
    return v;
}

// HoneycombItem.WAXABLES: unwaxed -> waxed copper, the inverse of the axe's wax_off transforms.
const std::unordered_map<uint32_t, uint32_t>& waxables() {
    static const std::unordered_map<uint32_t, uint32_t> map = [] {
        std::unordered_map<uint32_t, uint32_t> m;
        for (const auto& t : transformers()["minecraft:axe"].array()) {
            if (t["particle"].string() != "wax_off") continue;
            for (const auto& rule : t["block_state_provider"]["rules"].array()) {
                auto waxed = bs::byName(rule["if_true"]["blocks"].string());
                auto plain = bs::byName(rule["then"]["source"]["id"].string());
                if (waxed && plain) m[*plain] = *waxed;
            }
        }
        return m;
    }();
    return map;
}

// ChestBlock.getConnectedBlockPos for a double chest half.
BlockPos connectedChest(BlockPos pos, BlockStateId state) {
    Direction facing = direction::byName(bs::getValue(state, "facing").value_or("north"));
    Direction d = bs::getValue(state, "type") == "left" ? direction::clockWise(facing) : direction::counterClockWise(facing);
    return pos.relative(d);
}

// TransformParticle level events
int particleEvent(std::string_view particle) {
    return particle == "scrape" ? 3005 : particle == "wax_on" ? 3003 : particle == "wax_off" ? 3004 : 0;
}

bool isDoubleCopperChest(BlockStateId state) {
    return bs::isInstance(bs::blockOf(state), "CopperChestBlock") && bs::getValue(state, "type").value_or("single") != "single";
}

} // namespace

UseResult ServerGamePacketListener::useItemStackOn(ItemStack& stack, int hand, BlockPos pos, Direction face, double hx, double hy, double hz) {
    std::string transformer;
    if (auto patched = items::componentValue(stack, "minecraft:block_transformer"); patched && patched->asString()) transformer = *patched->asString();
    else transformer = std::string(stack.info().blockTransformer);
    if (!transformer.empty()) return transformBlock(transformer, stack, hand, pos, face);

    std::string_view itemClass = stack.info().itemClass;
    ServerLevel& level = player_.level();
    BlockStateId state = level.getBlockState(pos);

    if (itemClass == "HoneycombItem") { // HoneycombItem.useOn
        auto it = waxables().find(bs::blockOf(state));
        if (it == waxables().end()) return UseResult::Pass;
        BlockStateId waxed = block_behaviour::withPropertiesOf(it->second, state);
        stack.shrink(1);
        level.setBlock(pos, waxed, ServerLevel::kUpdateAll | 8);
        level.levelEvent(3003, pos, 0, player_.id());
        static const int32_t kWaxOn = soundId("minecraft:item.honeycomb.wax_on");
        level.playBlockSound(player_.id(), pos, kWaxOn, 1.0f, 1.0f);
        if (bs::isInstance(bs::blockOf(state), "ChestBlock") && bs::getValue(state, "type").value_or("single") != "single") {
            level.levelEvent(3003, connectedChest(pos, state), 0, player_.id());
            level.playBlockSound(player_.id(), pos, kWaxOn, 1.0f, 1.0f);
        }
        return UseResult::Success;
    }

    if (itemClass == "FlintAndSteelItem" || itemClass == "FireChargeItem") {
        bool charge = itemClass == "FireChargeItem";
        std::uniform_real_distribution<float> u(0.0f, 1.0f);
        auto sound = [&](BlockPos at) {
            static const int32_t kFlint = soundId("minecraft:item.flintandsteel.use"), kCharge = soundId("minecraft:item.firecharge.use");
            if (charge) level.playBlockSound(-1, at, kCharge, 1.0f, (u(level.random()) - u(level.random())) * 0.2f + 1.0f);
            else level.playBlockSound(player_.id(), at, kFlint, 1.0f, u(level.random()) * 0.4f + 0.8f);
        };
        auto used = [&] {
            if (charge) stack.shrink(1);
            else hurtAndBreak(stack, 1, hand);
            return UseResult::Success;
        };
        // CampfireBlock / CandleBlock / CandleCakeBlock.canLight
        bool lit = bs::getBool(state, "lit"), waterlogged = bs::getBool(state, "waterlogged");
        bool hasLit = bs::hasProperty(state, "lit"), hasWaterlogged = bs::hasProperty(state, "waterlogged");
        bool canLight = (bs::stateHasTag(state, "minecraft:campfires") && hasLit && hasWaterlogged && !lit && !waterlogged) ||
                        (bs::stateHasTag(state, "minecraft:candles") && hasLit && hasWaterlogged && !lit && !waterlogged) ||
                        (bs::stateHasTag(state, "minecraft:candle_cakes") && hasLit && !lit);
        if (canLight) {
            sound(pos);
            level.setBlock(pos, bs::setBool(state, "lit", true), ServerLevel::kUpdateAll | 8);
            return used();
        }
        BlockPos firePos = pos.relative(face);
        if (!block_behaviour::fireCanBePlacedAt(level, firePos)) return UseResult::Pass; // (FAIL)
        sound(firePos);
        level.setBlock(firePos, block_behaviour::fireStateFor(level, firePos), ServerLevel::kUpdateAll | 8);
        return used();
    }

    if (itemClass == "BoneMealItem") { // BoneMealItem.useOn
        if (!block_behaviour::growCrop(level, pos)) return UseResult::Pass; // TODO: growWaterPlant (seagrass, corals)
        stack.shrink(1);
        level.levelEvent(1505, pos, 15); // (growth particles)
        return UseResult::SuccessServer;
    }

    // TODO: the other items with a use on blocks (buckets, spawn eggs, ...).
    if (stack.info().block < 0) return UseResult::Pass;
    return placeBlock(stack, pos, face, hx, hy, hz) ? UseResult::Success : UseResult::Pass;
}

// BlockTransformer.transformBlock
UseResult ServerGamePacketListener::transformBlock(const std::string& id, ItemStack& stack, int hand, BlockPos pos, Direction face) {
    // playerHasBlockingItemUseIntent: a shield in the other hand takes over (unless sneaking).
    const ItemStack& offhand = player_.inventory.equipment(EquipmentSlot::OffHand);
    static const int32_t kShield = registries::id("minecraft:item", "minecraft:shield").value_or(-1);
    if (hand == 0 && !offhand.isEmpty() && (offhand.item == kShield || items::componentValue(offhand, "minecraft:blocks_attacks")) &&
        !player_.sneaking)
        return UseResult::Pass;

    ServerLevel& level = player_.level();
    for (const auto& t : transformers()[id].array()) {
        bool disallowed = false;
        for (const auto& f : t["disallowed_faces"].array()) disallowed = disallowed || direction::byName(f.string()) == face;
        if (disallowed) continue;
        auto newState = levelgen::optionalState(t["block_state_provider"], level, pos, level.random());
        if (!newState) continue;
        BlockStateId updated = t["update_from_neighbors"].boolean(true) ? block_behaviour::updateFromNeighbourShapes(*newState, level, pos) : *newState;
        BlockStateId old = level.getBlockState(pos);
        if (t.has("loot")) {
            loot::BlockContext ctx{level, level.random(), old, pos, &stack, &player_, nullptr};
            bool fromFace = t["drop_strategy"].string() == "clicked_face";
            for (ItemStack& drop : loot::getRandomItems(t["loot"].string(), ctx)) {
                if (!fromFace) {
                    level.popResource(pos, std::move(drop));
                    continue;
                }
                // Block.popResourceFromFace
                std::uniform_real_distribution<double> u(0.0, 1.0);
                auto between = [&](double lo, double hi) { return lo + u(level.random()) * (hi - lo); };
                int sx = direction::stepX(face), sy = direction::stepY(face), sz = direction::stepZ(face);
                double hw = ItemEntity::kWidth / 2, hh = ItemEntity::kHeight / 2;
                double x = pos.x + 0.5 + (sx == 0 ? between(-0.25, 0.25) : sx * (0.5 + hw));
                double y = pos.y + 0.5 + (sy == 0 ? between(-0.25, 0.25) : sy * (0.5 + hh)) - hh;
                double z = pos.z + 0.5 + (sz == 0 ? between(-0.25, 0.25) : sz * (0.5 + hw));
                double vx = sx == 0 ? between(-0.1, 0.1) : sx * 0.1, vy = sy == 0 ? between(0.0, 0.1) : sy * 0.1 + 0.1,
                       vz = sz == 0 ? between(-0.1, 0.1) : sz * 0.1;
                if (ItemEntity* e = ItemEntity::spawn(level, x, y, z, std::move(drop), level.random())) {
                    e->vx = vx, e->vy = vy, e->vz = vz;
                    e->pickupDelay = ItemEntity::kDefaultPickupDelay;
                }
            }
        }
        if (stack.maxStackSize() > 1) { // ItemStack.consume
            if (t["consume_on_use"].boolean(true) && !player_.hasInfiniteMaterials()) stack.shrink(1);
        } else {
            hurtAndBreak(stack, static_cast<int>(t["item_damage_per_use"].number(0)), hand);
        }
        level.setBlock(pos, updated, ServerLevel::kUpdateAll | 8);
        level.playBlockSound(player_.id(), pos, soundId(t["sound"].string()), 1.0f, 1.0f);
        if (int ev = particleEvent(t["particle"].string())) {
            level.levelEvent(ev, pos, 0, player_.id());
            if (t["transform_type"].string() == "copper_chest" && isDoubleCopperChest(old))
                level.levelEvent(ev, connectedChest(pos, old), 0, player_.id());
        }
        return UseResult::Success;
    }
    return UseResult::Pass;
}

} // namespace mc

namespace mc {

void ServerGamePacketListener::handleUseItem(ByteReader& in) {
    // ServerboundUseItemPacket -> ServerGamePacketListenerImpl.handleUseItem
    int hand = in.readVarInt();
    int sequence = in.readVarInt();
    float yRot = in.readFloat(), xRot = in.readFloat();
    if (!hasClientLoaded()) return;
    ackBlockChangesUpTo(sequence);
    ItemStack& stack = itemInHand(hand);
    if (stack.isEmpty()) return;
    yRot = mth::wrapDegrees(yRot), xRot = mth::wrapDegrees(xRot);
    if (xRot != player_.xRot || yRot != player_.yRot) player_.yRot = yRot, player_.xRot = std::clamp(xRot, -90.0f, 90.0f); // absSnapRotationTo
    // ServerPlayerGameMode.useItem
    if (player_.gameMode == GameType::Spectator) return;
    std::string_view itemClass = stack.info().itemClass;
    if (itemClass != "BucketItem" && itemClass != "MobBucketItem") return; // TODO: food, bows, potions, spawn eggs, ...
    ItemStack transformed;
    UseResult result = useBucket(stack, hand, transformed);
    if (result != UseResult::Success && result != UseResult::SuccessServer) return;
    // (InteractionResult.heldItemTransformedTo; the menu is resent only if the held stack changed)
    if (!ItemStack::matches(transformed, stack)) {
        stack = transformed;
        player_.inventoryMenu->sendAllDataToRemote();
    }
    swing(hand, result == UseResult::SuccessServer); // (SUCCESS: the user's client swung already)
}

UseResult ServerGamePacketListener::useBucket(ItemStack& stack, int hand, ItemStack& transformedTo) {
    // BucketItem.use
    (void)hand;
    ServerLevel& level = player_.level();
    std::string_view id = items::name(stack.item);
    uint8_t content = id == "minecraft:bucket" ? fluids::kEmpty : id == "minecraft:lava_bucket" ? fluids::kLava : fluids::kWater;
    // Item.getPlayerPOVHitResult: from the eyes along the view vector, as far as the player reaches.
    constexpr float kDeg = 3.14159265358979323846f / 180.0f;
    float f = player_.xRot * kDeg, g = -player_.yRot * kDeg;
    float h = mth::cos(g), i = mth::sin(g), j = mth::cos(f), k = mth::sin(f);
    double range = player_.blockInteractionRange();
    double ex = player_.x, ey = player_.eyeY(), ez = player_.z;
    auto hit = level.clip(ex, ey, ez, ex + i * j * range, ey - k * range, ez + h * j * range, ServerLevel::ClipBlock::Outline,
                          content == fluids::kEmpty ? ServerLevel::ClipFluid::SourceOnly : ServerLevel::ClipFluid::None);
    if (!hit) return UseResult::Pass;
    BlockPos pos = hit->pos;
    BlockPos placePos = pos.relative(hit->direction);
    // Level.mayInteract (no spawn protection yet) / Player.mayUseItemAt. TODO: adventure mode's can_place_on
    if (!canModifyWorld(player_.gameMode)) return UseResult::Pass;
    auto filledResult = [&](ItemStack filled) { // ItemUtils.createFilledResult
        if (player_.hasInfiniteMaterials()) {
            bool has = false;
            for (int s = 0; s <= Inventory::kOffhandSlot; s++)
                if (!player_.inventory.slot(s).isEmpty() && ItemStack::isSameItemSameComponents(player_.inventory.slot(s), filled)) has = true;
            if (!has) player_.inventory.add(filled);
            return stack;
        }
        ItemStack rest = stack;
        rest.shrink(1);
        if (rest.isEmpty()) return filled;
        giveOrDrop(std::move(filled));
        return rest;
    };
    if (content == fluids::kEmpty) {
        BlockStateId state = level.getBlockState(pos);
        if (!fluids::info::isBucketPickup(state)) return UseResult::Pass; // (FAIL)
        std::string_view sound;
        auto taken = fluids::pickupBlock(level, pos, state, &sound);
        if (!taken) return UseResult::Pass;
        if (!sound.empty()) // Player.playSound: at the player, for everyone else
            level.playSound(player_.id(), player_.x, player_.y, player_.z, registries::id("minecraft:sound_event", sound).value_or(-1),
                            ServerLevel::kSoundSourcePlayers, 1.0f, 1.0f);
        transformedTo = filledResult(ItemStack(*items::byName(*taken), 1));
        return UseResult::Success;
    }
    BlockStateId clicked = level.getBlockState(pos);
    BlockPos target = fluids::info::isLiquidBlockContainer(clicked) && content == fluids::kWater ? pos : placePos;
    if (!emptyBucketContents(content, target, hit)) return UseResult::Pass; // (FAIL)
    // TODO: MobBucketItem.checkExtraContent (the fish comes out, once there are mobs)
    // BucketItem.getEmptySuccessItem, through ItemUtils.createFilledResult
    transformedTo = filledResult(player_.hasInfiniteMaterials() ? stack : ItemStack(*items::byName("minecraft:bucket"), 1));
    return UseResult::Success;
}

bool ServerGamePacketListener::emptyBucketContents(uint8_t content, BlockPos pos, const std::optional<ServerLevel::BlockHit>& hit) {
    // BucketItem.emptyContents
    ServerLevel& level = player_.level();
    BlockStateId state = level.getBlockState(pos);
    bool canBeReplaced = fluids::info::canBeReplacedBy(state, content);
    bool placeLiquid = canBeReplaced || (fluids::info::isLiquidBlockContainer(state) && fluids::info::canPlaceLiquid(state, content));
    // (sneaking: into the block next to it unless already there)
    bool canPlaceFluidInsideBlock = bs::isAir(state) || (placeLiquid && (!player_.sneaking || !hit));
    auto playEmptySound = [&] {
        level.playBlockSound(player_.id(), pos, soundId(fluids::isLava(content) ? "minecraft:item.bucket.empty_lava" : "minecraft:item.bucket.empty"), 1.0f, 1.0f);
    };
    if (!canPlaceFluidInsideBlock) return hit && emptyBucketContents(content, hit->pos.relative(hit->direction), std::nullopt);
    if (level.waterEvaporates() && fluids::isWater(content)) {
        std::uniform_real_distribution<float> u(0.0f, 1.0f);
        auto& r = level.random();
        level.playSound(player_.id(), pos.x + 0.5, pos.y + 0.5, pos.z + 0.5, soundId("minecraft:block.fire.extinguish"), ServerLevel::kSoundSourceBlocks, 0.5f,
                        2.6f + (u(r) - u(r)) * 0.8f);
        level.sendParticles("minecraft:large_smoke", pos.x, pos.y, pos.z, 8, 1.0f, 1.0f, 1.0f, 0.0f); // TODO: RandomizationType.ALTERNATIVE
        return true;
    }
    if (fluids::info::isLiquidBlockContainer(state) && content == fluids::kWater) {
        fluids::placeLiquid(level, pos, state, fluids::source(content));
        playEmptySound();
        return true;
    }
    if (canBeReplaced && !bs::isInstance(bs::blockOf(state), "LiquidBlock")) level.destroyBlock(pos, true);
    if (!level.setBlock(pos, fluids::legacyBlock(fluids::source(content)), ServerLevel::kUpdateAll | 8) && !fluids::stateOf(state).isSource()) return false;
    playEmptySound();
    return true;
}

} // namespace mc
