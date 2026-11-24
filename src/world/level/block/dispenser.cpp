// Dispensers and droppers: DispenserBlock / DropperBlock and the DispenseItemBehavior registry
// (net.minecraft.core.dispenser), for the items whose behaviour doesn't need entities we don't have.
#include "world/level/block/dispenser.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

#include "core/registries.hpp"
#include "generated/behaviour.hpp"
#include "server/level/server_level.hpp"
#include "server/level/server_player.hpp"
#include "util/json.hpp"
#include "util/log.hpp"
#include "world/entity/item_entity.hpp"
#include "world/item/enchantment/enchantment_helper.hpp"
#include "world/item/item_logic.hpp"
#include "world/level/block/block_behaviour.hpp"
#include "world/level/block/entity/block_entity_components.hpp"
#include "world/level/block/entity/block_entity_container.hpp"
#include "world/level/block/entity/hopper.hpp"
#include "world/level/material/fluids.hpp"
#include "world/level/redstone/redstone.hpp"
#include "world/level/storage/loot/loot_tables.hpp"

namespace mc::block_behaviour::dispenser {

namespace bs = blocks;
using namespace gen::block;

namespace {

int32_t itemId(std::string_view name) { return items::byName(name).value_or(-1); }
int32_t soundId(std::string_view name) { return registries::id("minecraft:sound_event", name).value_or(-1); }
Direction facing(BlockStateId s) { return direction::byName(bs::getValue(s, "facing").value_or("north")); }

struct Source { // BlockSource
    ServerLevel& level;
    BlockPos pos;
    BlockStateId state;
    BlockEntityContainer& container;
    Direction dir() const { return facing(state); }
    BlockPos target() const { return pos.relative(dir()); }
};

// DispenserBlock.getDispensePosition: 0.7 of a block out of the front face's centre.
void dispensePosition(const Source& s, double& x, double& y, double& z) {
    Direction d = s.dir();
    x = s.pos.x + 0.5 + 0.7 * direction::stepX(d);
    y = s.pos.y + 0.5 + 0.7 * direction::stepY(d);
    z = s.pos.z + 0.5 + 0.7 * direction::stepZ(d);
}

// DefaultDispenseItemBehavior.spawnItem
void spawnItem(ServerLevel& level, ItemStack stack, int accuracy, Direction dir, double x, double y, double z) {
    y -= direction::axis(dir) == Axis::Y ? 0.125 : 0.15625;
    auto& random = level.levelRandom();
    ItemEntity* e = ItemEntity::spawn(level, x, y, z, std::move(stack), level.random());
    if (!e) return;
    double pow = random.nextDouble() * 0.1 + 0.2;
    auto triangle = [&](double mean, double spread) { return mean + spread * (random.nextDouble() - random.nextDouble()); };
    e->vx = triangle(direction::stepX(dir) * pow, 0.0172275 * accuracy);
    e->vy = triangle(0.2, 0.0172275 * accuracy);
    e->vz = triangle(direction::stepZ(dir) * pow, 0.0172275 * accuracy);
}

void playDefaultSound(const Source& s) { s.level.levelEvent(1000, s.pos, 0); }
void playDefaultAnimation(const Source& s) { s.level.levelEvent(2000, s.pos, static_cast<int>(s.dir())); }

// DefaultDispenseItemBehavior.execute: one item flies out.
ItemStack defaultExecute(const Source& s, ItemStack dispensed) {
    double x, y, z;
    dispensePosition(s, x, y, z);
    ItemStack one = dispensed.split(1);
    spawnItem(s.level, std::move(one), 6, s.dir(), x, y, z);
    return dispensed;
}
// DefaultDispenseItemBehavior.dispense: execute, then the click and the smoke.
ItemStack defaultDispense(const Source& s, ItemStack dispensed) {
    ItemStack result = defaultExecute(s, std::move(dispensed));
    playDefaultSound(s);
    playDefaultAnimation(s);
    return result;
}

// DispenserBlockEntity.insertItem
ItemStack insertItem(BlockEntityContainer& c, ItemStack stack) {
    int max = c.maxStackSize(stack);
    for (int i = 0; i < c.size(); i++) {
        ItemStack& target = c.item(i);
        if (!target.isEmpty() && !ItemStack::isSameItemSameComponents(stack, target)) continue;
        int transfer = std::min(stack.count, max - target.count);
        if (transfer > 0) {
            if (target.isEmpty()) {
                c.setItem(i, stack.split(transfer));
            } else {
                stack.shrink(transfer);
                target.count += transfer;
                c.setChanged();
            }
        }
        if (stack.isEmpty()) break;
    }
    return stack;
}

// DefaultDispenseItemBehavior.consumeWithRemainder
ItemStack consumeWithRemainder(const Source& s, ItemStack dispensed, ItemStack remainder) {
    dispensed.shrink(1);
    if (dispensed.isEmpty()) return remainder;
    ItemStack left = insertItem(s.container, std::move(remainder)); // addToInventoryOrDispense
    if (!left.isEmpty()) {
        double x, y, z;
        dispensePosition(s, x, y, z);
        spawnItem(s.level, std::move(left), 6, s.dir(), x, y, z);
        playDefaultSound(s);
        playDefaultAnimation(s);
    }
    return dispensed;
}

// ItemStack.hurtAndBreak(amount, level, null, ...): with no player, the tool just breaks.
void hurtAndBreak(ServerLevel& level, ItemStack& stack, int amount) {
    int max = items::maxDamage(stack);
    if (stack.isEmpty() || max <= 0 || items::findComponent(stack, "minecraft:unbreakable")) return;
    int applied = enchantments::processDurabilityChange(level, stack, amount);
    if (applied <= 0) return;
    int damage = items::damage(stack) + applied;
    items::setDamage(stack, damage);
    if (damage >= max) stack.shrink(1);
}

// HoneycombItem.WAXABLES: unwaxed -> waxed, the inverse of the axe's wax_off transforms.
const std::unordered_map<uint32_t, uint32_t>& waxables() {
    static const std::unordered_map<uint32_t, uint32_t> map = [] {
        std::unordered_map<uint32_t, uint32_t> m;
        try {
            json::Value v = json::parse(gen::kBlockTransformersJson);
            for (const auto& t : v["minecraft:axe"].array()) {
                if (t["particle"].string() != "wax_off") continue;
                for (const auto& rule : t["block_state_provider"]["rules"].array()) {
                    auto waxed = bs::byName(rule["if_true"]["blocks"].string());
                    auto plain = bs::byName(rule["then"]["source"]["id"].string());
                    if (waxed && plain) m[*plain] = *waxed;
                }
            }
        } catch (const std::exception& e) {
            log::error("Failed to parse block transformers: {}", e.what());
        }
        return m;
    }();
    return map;
}

// BucketItem.emptyContents with no user and no hit result.
bool emptyContents(ServerLevel& level, uint8_t content, BlockPos pos, int32_t emptySound, int soundSource) {
    BlockStateId state = level.getBlockState(pos);
    bool mayReplace = fluids::info::canBeReplacedBy(state, content);
    bool placeLiquid = mayReplace || (fluids::info::isLiquidBlockContainer(state) && fluids::info::canPlaceLiquid(state, content));
    if (!bs::isAir(state) && !placeLiquid) return false;
    auto playEmpty = [&] { level.playSound(-1, pos.x + 0.5, pos.y + 0.5, pos.z + 0.5, emptySound, soundSource, 1.0f, 1.0f); };
    if (level.waterEvaporates() && fluids::isWater(content)) {
        auto& r = level.levelRandom();
        level.playSound(-1, pos.x + 0.5, pos.y + 0.5, pos.z + 0.5, soundId("minecraft:block.fire.extinguish"), ServerLevel::kSoundSourceBlocks, 0.5f,
                        2.6f + (r.nextFloat() - r.nextFloat()) * 0.8f);
        level.sendParticles("minecraft:large_smoke", pos.x, pos.y, pos.z, 8, 1.0f, 1.0f, 1.0f, 0.0f);
        return true;
    }
    if (fluids::info::isLiquidBlockContainer(state) && content == fluids::kWater) {
        fluids::placeLiquid(level, pos, state, fluids::source(content));
        playEmpty();
        return true;
    }
    if (mayReplace && !bs::isInstance(bs::blockOf(state), "LiquidBlock")) level.destroyBlock(pos, true);
    if (!level.setBlock(pos, fluids::legacyBlock(fluids::source(content)), ServerLevel::kUpdateAll | 8) && !fluids::stateOf(state).isSource()) return false;
    playEmpty();
    return true;
}

// EquipmentDispenseItemBehavior.dispenseEquipment: onto a player standing in front.
bool dispenseEquipment(const Source& s, ItemStack& dispensed) {
    int8_t slot = dispensed.info().equipSlot;
    if (slot < static_cast<int8_t>(EquipmentSlot::Feet)) return false; // (only armour slots: players can't use BODY)
    BlockPos t = s.target();
    AABB box{static_cast<double>(t.x), static_cast<double>(t.y), static_cast<double>(t.z), t.x + 1.0, t.y + 1.0, t.z + 1.0};
    for (ServerPlayer* p : s.level.players()) {
        if (p->gameMode == GameType::Spectator || p->disconnected || !p->boundingBox().intersects(box)) continue;
        ItemStack& worn = p->inventory.equipment(static_cast<EquipmentSlot>(slot));
        if (!worn.isEmpty()) continue; // canEquipWithDispenser
        worn = dispensed.split(1);
        return true;
    }
    return false;
}

enum class Kind { Default, Equipment, Unported };

// The result of one OptionalDispenseItemBehavior: what's left, and whether it worked.
struct Optional {
    ItemStack stack;
    bool success;
};

// OptionalDispenseItemBehavior.dispense: the click (or the failure click) and the smoke.
ItemStack optionalDispense(const Source& s, Optional r) {
    s.level.levelEvent(r.success ? 1000 : 1001, s.pos, 0);
    playDefaultAnimation(s);
    return r.stack;
}

bool isBeehiveWithHoney(BlockStateId st) {
    return bs::stateHasTag(st, "minecraft:beehives") && bs::isInstance(bs::blockOf(st), "BeehiveBlock") && bs::hasProperty(st, "honey_level") &&
           std::stoi(std::string(bs::getValue(st, "honey_level").value_or("0"))) >= 5;
}

// The registered behaviour for `stack`, run; empty if the item has none (DISPENSER_REGISTRY).
std::optional<ItemStack> registered(const Source& s, ItemStack d) {
    ServerLevel& level = s.level;
    BlockPos target = s.target();
    BlockStateId ts = level.getBlockState(target);
    std::string_view id = items::name(d.item);

    static const std::unordered_map<std::string_view, std::string_view> kFishSounds = {
        {"minecraft:cod_bucket", "minecraft:item.bucket.empty_fish"},         {"minecraft:salmon_bucket", "minecraft:item.bucket.empty_fish"},
        {"minecraft:pufferfish_bucket", "minecraft:item.bucket.empty_fish"},  {"minecraft:tropical_fish_bucket", "minecraft:item.bucket.empty_fish"},
        {"minecraft:axolotl_bucket", "minecraft:item.bucket.empty_axolotl"}, {"minecraft:tadpole_bucket", "minecraft:item.bucket.empty_tadpole"}};
    if (id == "minecraft:water_bucket" || id == "minecraft:lava_bucket" || kFishSounds.contains(id)) { // filledBucketBehavior
        bool lava = id == "minecraft:lava_bucket";
        uint8_t content = lava ? fluids::kLava : fluids::kWater;
        int32_t sound = kFishSounds.contains(id) ? soundId(kFishSounds.at(id)) : soundId(lava ? "minecraft:item.bucket.empty_lava" : "minecraft:item.bucket.empty");
        int source = kFishSounds.contains(id) ? 6 : ServerLevel::kSoundSourceBlocks; // (MobBucketItem: NEUTRAL)
        if (emptyContents(level, content, target, sound, source)) { // (no fish yet: MobBucketItem.checkExtraContent)
            ItemStack r = consumeWithRemainder(s, std::move(d), ItemStack(itemId("minecraft:bucket"), 1));
            playDefaultSound(s);
            playDefaultAnimation(s);
            return r;
        }
        return defaultDispense(s, std::move(d));
    }
    if (id == "minecraft:powder_snow_bucket") { // SolidBucketItem.emptyContents
        if (bs::isAir(ts)) {
            level.setBlock(target, bs::defaultState(POWDER_SNOW));
            level.playBlockSound(-1, target, soundId("minecraft:item.bucket.empty_powder_snow"), 1.0f, 1.0f);
            ItemStack r = consumeWithRemainder(s, std::move(d), ItemStack(itemId("minecraft:bucket"), 1));
            playDefaultSound(s);
            playDefaultAnimation(s);
            return r;
        }
        return defaultDispense(s, std::move(d));
    }
    if (id == "minecraft:bucket") {
        if (fluids::info::isBucketPickup(ts))
            if (auto taken = fluids::pickupBlock(level, target, ts, nullptr)) {
                ItemStack r = consumeWithRemainder(s, std::move(d), ItemStack(itemId(*taken), 1));
                playDefaultSound(s);
                playDefaultAnimation(s);
                return r;
            }
        return defaultDispense(s, std::move(d));
    }
    if (id == "minecraft:bone_meal") { // (TODO: BoneMealItem.growWaterPlant)
        bool ok = block_behaviour::growCrop(level, target);
        if (ok) {
            d.shrink(1);
            level.levelEvent(1505, target, 15);
        }
        return optionalDispense(s, {std::move(d), ok});
    }
    if (id == "minecraft:flint_and_steel") { // FlintAndSteelDispenseItemBehavior
        bool ok = true;
        uint32_t tb = bs::blockOf(ts);
        auto canLight = [&] {
            bool lit = bs::getBool(ts, "lit"), wet = bs::getBool(ts, "waterlogged");
            if (bs::stateHasTag(ts, "minecraft:campfires") && bs::hasProperty(ts, "waterlogged") && bs::hasProperty(ts, "lit")) return !lit && !wet;
            if (bs::stateHasTag(ts, "minecraft:candles") && bs::hasProperty(ts, "lit") && bs::hasProperty(ts, "waterlogged")) return !lit && !wet;
            if (bs::stateHasTag(ts, "minecraft:candle_cakes") && bs::hasProperty(ts, "lit")) return !lit;
            return false;
        };
        if (block_behaviour::fireCanBePlacedAt(level, target)) {
            level.setBlock(target, block_behaviour::fireStateFor(level, target));
        } else if (canLight()) {
            level.setBlock(target, bs::setBool(ts, "lit", true));
        } else if (bs::isInstance(tb, "TntBlock")) {
            ok = false; // TODO: TntBlock.prime (primed TNT is an entity)
        }
        if (ok) hurtAndBreak(level, d, 1);
        return optionalDispense(s, {std::move(d), ok});
    }
    if (id == "minecraft:shears") { // ShearsDispenseItemBehavior: a full beehive (no sheep yet)
        bool ok = false;
        if (isBeehiveWithHoney(ts)) {
            level.playBlockSound(-1, target, soundId("minecraft:block.beehive.shear"), 1.0f, 1.0f);
            LevelChunk* chunk = level.getChunkIfLoaded({target.x >> 4, target.z >> 4});
            auto* be = chunk ? chunk->blockEntityAt(target.x, target.y, target.z) : nullptr;
            loot::BlockContext ctx{level, level.random(), ts, target, &d, nullptr, be ? &be->tag : nullptr};
            for (ItemStack& comb : loot::getRandomItems("minecraft:harvest/beehive", ctx)) level.popResource(target, std::move(comb));
            level.setBlock(target, bs::setValue(ts, "honey_level", "0")); // releaseBeesAndResetHoneyLevel
            ok = true;
        }
        if (ok) hurtAndBreak(level, d, 1);
        return optionalDispense(s, {std::move(d), ok});
    }
    if (id == "minecraft:glass_bottle") {
        auto take = [&](ItemStack filled) { return optionalDispense(s, {consumeWithRemainder(s, std::move(d), std::move(filled)), true}); };
        if (isBeehiveWithHoney(ts)) {
            level.setBlock(target, bs::setValue(ts, "honey_level", "0"));
            return take(ItemStack(itemId("minecraft:honey_bottle"), 1));
        }
        if (fluids::isWater(fluids::stateOf(ts).type)) {
            ItemStack water(itemId("minecraft:potion"), 1);
            nbt::Compound contents;
            contents.put("potion", "minecraft:water");
            items::setComponentValue(water, "minecraft:potion_contents", contents);
            return take(std::move(water));
        }
        return optionalDispense(s, {defaultExecute(s, std::move(d)), false});
    }
    if (id == "minecraft:glowstone") {
        if (bs::is(ts, RESPAWN_ANCHOR)) {
            int charges = std::stoi(std::string(bs::getValue(ts, "charges").value_or("0")));
            if (charges == 4) return optionalDispense(s, {std::move(d), false});
            level.setBlock(target, bs::setValue(ts, "charges", std::to_string(charges + 1))); // RespawnAnchorBlock.charge
            level.playSound(-1, target.x + 0.5, target.y + 0.5, target.z + 0.5, soundId("minecraft:block.respawn_anchor.charge"),
                            ServerLevel::kSoundSourceBlocks, 1.0f, 1.0f);
            d.shrink(1);
            return optionalDispense(s, {std::move(d), true});
        }
        return optionalDispense(s, {defaultExecute(s, std::move(d)), true});
    }
    if (id == "minecraft:honeycomb") {
        if (auto it = waxables().find(bs::blockOf(ts)); it != waxables().end()) {
            level.setBlock(target, block_behaviour::withPropertiesOf(it->second, ts));
            level.levelEvent(3003, target, 0);
            level.playBlockSound(-1, target, soundId("minecraft:item.honeycomb.wax_on"), 1.0f, 1.0f);
            d.shrink(1);
            return optionalDispense(s, {std::move(d), true});
        }
        return optionalDispense(s, {defaultExecute(s, std::move(d)), true});
    }
    if (id == "minecraft:potion") { // a water bottle turns dirt into mud
        auto contents = items::effectiveComponent(d, "minecraft:potion_contents");
        const nbt::Tag* potion = contents && contents->asCompound() ? contents->asCompound()->get("potion") : nullptr;
        bool water = (potion && potion->asString() && *potion->asString() == "minecraft:water") ||
                     (contents && contents->asString() && *contents->asString() == "minecraft:water");
        if (!water || !bs::stateHasTag(ts, "minecraft:convertible_to_mud")) return defaultDispense(s, std::move(d));
        auto& r = level.levelRandom();
        for (int i = 0; i < 5; i++) level.sendParticles("minecraft:splash", s.pos.x + r.nextDouble(), s.pos.y + 1, s.pos.z + r.nextDouble(), 1, 0, 0, 0, 1);
        level.playBlockSound(-1, s.pos, soundId("minecraft:item.bottle.empty"), 1.0f, 1.0f);
        level.setBlock(target, bs::defaultState(MUD));
        ItemStack r2 = consumeWithRemainder(s, std::move(d), ItemStack(itemId("minecraft:glass_bottle"), 1));
        playDefaultSound(s);
        playDefaultAnimation(s);
        return r2;
    }
    if (id == "minecraft:carved_pumpkin" || id == "minecraft:wither_skeleton_skull") {
        // (no golems or withers yet: canSpawnGolem / canSpawnMob never; the head goes onto a player)
        bool ok = dispenseEquipment(s, d);
        return optionalDispense(s, {std::move(d), ok});
    }
    if (bs::isInstance(static_cast<uint32_t>(std::max(0, d.info().block)), "ShulkerBoxBlock") && d.info().block >= 0) {
        // ShulkerBoxDispenseBehavior: BlockItem.place with a DirectionalPlaceContext
        Direction dir = s.dir();
        Direction clicked = bs::isAir(level.getBlockState(target.below())) ? dir : Direction::Up;
        bool ok = false;
        if (bs::canBeReplaced(ts) && target.y >= level.minY() && target.y <= level.maxY()) {
            uint32_t block = static_cast<uint32_t>(d.info().block);
            BlockStateId placed = bs::setValue(bs::defaultState(block), "facing", direction::name(clicked));
            if (level.setBlock(target, placed, ServerLevel::kUpdateAll | 8)) {
                if (LevelChunk* chunk = level.getChunkIfLoaded({target.x >> 4, target.z >> 4}))
                    if (auto* be = chunk->blockEntityAt(target.x, target.y, target.z)) block_entity::applyItem(be->tag, d, false);
                level.blockEntitySetChanged(target);
                const auto& beh = bs::behaviour(block);
                level.playBlockSound(-1, target, beh.placeSound, (beh.soundVolume + 1.0f) / 2.0f, beh.soundPitch * 0.8f);
                d.shrink(1);
                ok = true;
            }
        }
        return optionalDispense(s, {std::move(d), ok});
    }
    if (id == "minecraft:brush") return optionalDispense(s, {std::move(d), false}); // (no armadillos)
    return std::nullopt;
}

bool unported(std::string_view id) {
    // Behaviours that spawn entities we don't simulate yet: projectiles, boats, minecarts, armor
    // stands, primed TNT and spawn eggs. They stay in the dispenser.
    static constexpr std::string_view kItems[] = {
        "minecraft:arrow", "minecraft:tipped_arrow", "minecraft:spectral_arrow", "minecraft:egg", "minecraft:blue_egg", "minecraft:brown_egg",
        "minecraft:snowball", "minecraft:experience_bottle", "minecraft:splash_potion", "minecraft:lingering_potion", "minecraft:firework_rocket",
        "minecraft:fire_charge", "minecraft:wind_charge", "minecraft:armor_stand", "minecraft:tnt", "minecraft:minecart", "minecraft:chest_minecart",
        "minecraft:furnace_minecart", "minecraft:tnt_minecart", "minecraft:hopper_minecart", "minecraft:command_block_minecart"};
    for (auto k : kItems)
        if (k == id) return true;
    return id.ends_with("_boat") || id.ends_with("_raft") || id.ends_with("_spawn_egg");
}

void dispenseFrom(ServerLevel& level, BlockPos pos, BlockStateId state, bool dropper) {
    auto c = level.blockEntityContainer(pos);
    if (!c) {
        log::warn("Ignoring dispensing attempt for {} without matching block entity at {}, {}, {}", dropper ? "Dropper" : "Dispenser", pos.x, pos.y, pos.z);
        return;
    }
    // DispenserBlockEntity.getRandomSlot
    int slot = -1, odds = 1;
    for (int i = 0; i < c->size(); i++)
        if (!c->item(i).isEmpty() && level.levelRandom().nextInt(odds++) == 0) slot = i;
    if (slot < 0) {
        level.levelEvent(1001, pos, 0);
        return;
    }
    Source s{level, pos, state, *c};
    ItemStack stack = c->item(slot);
    if (dropper) { // DropperBlock.dispenseFrom
        Direction dir = facing(level.getBlockState(pos));
        auto into = hopper::getContainerAt(level, pos.relative(dir));
        ItemStack remaining;
        if (!into) {
            remaining = defaultDispense(s, stack);
        } else {
            ItemStack one = stack;
            one.count = 1;
            remaining = stack;
            if (hopper::addItem(level, c.get(), *into, one, direction::opposite(dir)).isEmpty()) remaining.shrink(1);
        }
        c->setItem(slot, std::move(remaining));
        return;
    }
    std::string_view id = items::name(stack.item);
    if (unported(id)) return; // TODO: the entity-spawning behaviours
    std::optional<ItemStack> result = registered(s, stack);
    if (!result && stack.info().equipSlot >= static_cast<int8_t>(EquipmentSlot::Feet)) { // EquipmentDispenseItemBehavior
        if (!dispenseEquipment(s, stack)) stack = defaultExecute(s, std::move(stack));
        playDefaultSound(s);
        playDefaultAnimation(s);
        result = std::move(stack);
    }
    if (!result) result = defaultDispense(s, stack);
    c->setItem(slot, std::move(*result));
}

} // namespace

void neighborChanged(ServerLevel& level, BlockPos pos, BlockStateId state) {
    bool shouldTrigger = redstone::hasNeighborSignal(level, pos) || redstone::hasNeighborSignal(level, pos.above());
    bool triggered = bs::getBool(state, "triggered");
    if (shouldTrigger && !triggered) {
        level.scheduleTick(pos, bs::blockOf(state), 4);
        level.setBlock(pos, bs::setBool(state, "triggered", true), ServerLevel::kUpdateClients);
    } else if (!shouldTrigger && triggered) {
        level.setBlock(pos, bs::setBool(state, "triggered", false), ServerLevel::kUpdateClients);
    }
}

void tick(ServerLevel& level, BlockPos pos, BlockStateId state) { dispenseFrom(level, pos, state, bs::is(state, DROPPER)); }

} // namespace mc::block_behaviour::dispenser
