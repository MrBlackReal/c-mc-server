// BlockBehaviour.useItemOn and useWithoutItem for blocks that need the player: flower pots,
// cakes and candles, composters, berry bushes, pumpkins, ... (the ones that only change the
// block live in block_behaviour::useWithoutItem).
#include <random>

#include "core/registries.hpp"
#include "network/chat/component.hpp"
#include "generated/behaviour.hpp"
#include "server/network/listeners.hpp"
#include "world/inventory/workstation_menus.hpp"
#include "world/entity/item_entity.hpp"
#include "world/item/item_logic.hpp"
#include "world/item/item_nbt.hpp"
#include "world/level/block/block_behaviour.hpp"
#include "world/level/block/block_state.hpp"
#include "world/level/block/entity/block_entity_container.hpp"
#include "world/level/block/entity/block_entity_ticker.hpp"
#include "world/level/storage/loot/loot_tables.hpp"

namespace mc {

namespace bs = blocks;

namespace {

int32_t soundId(std::string_view name) { return registries::id("minecraft:sound_event", name).value_or(-1); }
int32_t itemId(std::string_view name) { return registries::id("minecraft:item", name).value_or(-1); }

// The potted block holding `plant` (FlowerPotBlock.POTTED_BY_CONTENT), or -1.
int32_t pottedBlockFor(int32_t plant) {
    static const std::vector<int32_t> byPlant = [] {
        std::vector<int32_t> v(gen::kBlockBehaviour.size(), -1);
        for (size_t b = 0; b < gen::kBlockBehaviour.size(); b++) {
            int32_t p = gen::kBlockBehaviour[b].potted;
            if (p >= 0 && p != static_cast<int32_t>(gen::block::AIR)) v[p] = static_cast<int32_t>(b);
        }
        return v;
    }();
    return plant >= 0 && plant < static_cast<int32_t>(byPlant.size()) ? byPlant[plant] : -1;
}

int intValue(BlockStateId s, std::string_view property) { return std::stoi(std::string(bs::getValue(s, property).value_or("0"))); }

bool isSuccess(UseResult r) { return r == UseResult::Consume || r == UseResult::Success || r == UseResult::SuccessServer; }

// A block entity's "Items" slot (ContainerHelper form: {Slot, id, count, components}).
ItemStack slotItem(const nbt::Compound& be, int slot) {
    if (const nbt::List* items = be.getList("Items"))
        for (const auto& e : items->items)
            if (const nbt::Compound* c = e.asCompound(); c && c->getInt("Slot") == slot)
                if (auto stack = item_nbt::read(*c)) return *stack;
    return {};
}
void setSlotItem(nbt::Compound& be, int slot, const ItemStack& stack) {
    nbt::List items{nbt::TagType::Compound, {}};
    if (const nbt::List* old = be.getList("Items"))
        for (const auto& e : old->items)
            if (const nbt::Compound* c = e.asCompound(); c && c->getInt("Slot") != slot) items.items.push_back(e);
    if (!stack.isEmpty()) {
        nbt::Compound c = item_nbt::write(stack);
        c.entries.insert(c.entries.begin(), {"Slot", static_cast<int8_t>(slot)});
        items.items.push_back(std::move(c));
        std::sort(items.items.begin(), items.items.end(),
                  [](const nbt::Tag& a, const nbt::Tag& b) { return a.asCompound()->getInt("Slot") < b.asCompound()->getInt("Slot"); });
    }
    be.put("Items", std::move(items));
}

// SelectableSlotContainer.getHitSlot: which cell of a rows x columns grid on the block's front
// face was clicked, or -1.
int hitSlot(BlockPos pos, Direction face, Direction facing, double hx, double hy, double hz, int rows, int columns) {
    if (face != facing || !direction::isHorizontal(face)) return -1;
    BlockPos front = pos.relative(face);
    double rx = hx - front.x, ry = hy - front.y, rz = hz - front.z;
    float x = face == Direction::North ? static_cast<float>(1.0 - rx)
            : face == Direction::South ? static_cast<float>(rx)
            : face == Direction::West  ? static_cast<float>(rz)
                                       : static_cast<float>(1.0 - rz);
    auto section = [](float rel, int n) { return std::clamp(static_cast<int>(std::floor(rel * 16.0f / (16.0f / n))), 0, n - 1); };
    return section(x, columns) + section(1.0f - static_cast<float>(ry), rows) * columns;
}

Direction facingOf(BlockStateId s) { return direction::byName(bs::getValue(s, "facing").value_or("north")); }

} // namespace

nbt::Compound* ServerGamePacketListener::blockEntityAt(BlockPos pos, std::string_view type) {
    LevelChunk* chunk = player_.level().getChunkIfLoaded({pos.x >> 4, pos.z >> 4});
    auto* be = chunk ? chunk->blockEntityAt(pos.x, pos.y, pos.z) : nullptr;
    if (!be || bs::blockOf(player_.level().getBlockState(pos)) != be->blockAtLoad || be->tag.getString("id") != type) return nullptr;
    chunk->dirty = true; // (callers change it)
    return &be->tag;
}

// Player.addItem, else drop it (Prediction.PREDICTED: the client already shows it).
void ServerGamePacketListener::giveOrDrop(ItemStack stack) {
    if (!player_.inventory.add(stack) && !stack.isEmpty()) player_.drop(std::move(stack), false);
}

UseResult ServerGamePacketListener::useItemOnBlock(ItemStack& stack, int hand, BlockPos pos, BlockStateId state, Direction face, double hx,
                                                   double hitY, double hz) {
    ServerLevel& level = player_.level();
    uint32_t block = bs::blockOf(state);
    const auto& beh = bs::behaviour(block);
    std::string_view itemClass = stack.isEmpty() ? std::string_view{} : stack.info().itemClass;
    auto consume = [&] { // ItemStack.consume
        if (!player_.hasInfiniteMaterials()) {
            stack.shrink(1);
            stack.clearIfEmpty();
        }
    };

    if (bs::isInstance(block, "SignBlock")) {
        // Hanging signs: holding another hanging sign against the side (or bottom) places it.
        if (itemClass == "HangingSignItem" && signAt(pos)) {
            nbt::Compound* sign = signAt(pos);
            bool front = isFacingFrontText(pos, state);
            bool clickCommands = false; // SignBlockEntity.canExecuteClickCommands (waxed, with a run_command)
            if (sign->getBool("is_waxed"))
                if (const nbt::List* m = sign->getCompound(front ? "front_text" : "back_text")
                                             ? sign->getCompound(front ? "front_text" : "back_text")->getList("messages")
                                             : nullptr)
                    for (const auto& line : m->items) {
                        const nbt::Tag& c = line.asList() && !line.asList()->items.empty() ? line.asList()->items[0] : line;
                        const nbt::Compound* click = c.asCompound() ? c.asCompound()->getCompound("click_event") : nullptr;
                        clickCommands = clickCommands || (click && click->getString("action") == "run_command");
                    }
            if (!clickCommands) {
                if (bs::isInstance(block, "CeilingHangingSignBlock") && face == Direction::Down) return UseResult::Pass;
                if (bs::isInstance(block, "WallHangingSignBlock") &&
                    direction::axis(face) != direction::axis(direction::byName(bs::getValue(state, "facing").value_or("north"))))
                    return UseResult::Pass;
            }
        }
        return useItemOnSign(stack, pos, state);
    }

    if (bs::isInstance(block, "FlowerPotBlock")) {
        int32_t potted = stack.isEmpty() ? -1 : pottedBlockFor(stack.info().block);
        if (potted < 0) return UseResult::TryEmptyHand;
        if (beh.potted != static_cast<int32_t>(gen::block::AIR)) return UseResult::Consume; // already holds a plant
        level.setBlock(pos, bs::defaultState(static_cast<uint32_t>(potted)));
        consume();
        return UseResult::Success;
    }

    if (bs::isInstance(block, "CakeBlock")) {
        if (!stack.isEmpty() && items::hasTag(stack.item, "minecraft:candles") && intValue(state, "bites") == 0 && stack.info().block >= 0 &&
            bs::behaviour(static_cast<uint32_t>(stack.info().block)).candleCake >= 0) {
            uint32_t cake = static_cast<uint32_t>(bs::behaviour(static_cast<uint32_t>(stack.info().block)).candleCake);
            consume();
            static const int32_t kAddCandle = soundId("minecraft:block.cake.add_candle");
            level.playBlockSound(-1, pos, kAddCandle, 1.0f, 1.0f);
            level.setBlock(pos, bs::defaultState(cake));
            return UseResult::Success;
        }
        return UseResult::TryEmptyHand;
    }

    // AbstractCandleBlock.extinguish
    auto extinguish = [&] {
        level.setBlock(pos, bs::setBool(state, "lit", false), ServerLevel::kUpdateAll | 8);
        static const int32_t kExtinguish = soundId("minecraft:block.candle.extinguish");
        level.playBlockSound(-1, pos, kExtinguish, 1.0f, 1.0f);
    };
    if (bs::isInstance(block, "CandleCakeBlock")) {
        static const int32_t kFlintAndSteel = itemId("minecraft:flint_and_steel"), kFireCharge = itemId("minecraft:fire_charge");
        if (!stack.isEmpty() && (stack.item == kFlintAndSteel || stack.item == kFireCharge)) return UseResult::Pass;
        if (hitY - pos.y > 0.5 && stack.isEmpty() && bs::getBool(state, "lit")) { // candleHit
            extinguish();
            return UseResult::Success;
        }
        return UseResult::TryEmptyHand;
    }
    if (bs::isInstance(block, "CandleBlock")) {
        if (stack.isEmpty() && canModifyWorld(player_.gameMode) && bs::getBool(state, "lit")) {
            extinguish();
            return UseResult::Success;
        }
        return UseResult::TryEmptyHand;
    }

    if (bs::isInstance(block, "NoteBlock")) { // a head on top changes the instrument: place it
        if (!stack.isEmpty() && items::hasTag(stack.item, "minecraft:noteblock_top_instruments") && face == Direction::Up) return UseResult::Pass;
        return UseResult::TryEmptyHand;
    }

    if (bs::isInstance(block, "ComposterBlock")) {
        int fill = intValue(state, "level");
        auto patched = stack.isEmpty() ? std::nullopt : items::componentValue(stack, "minecraft:compostable");
        std::string layers;
        if (patched && patched->asCompound()) {
            if (const nbt::Tag* l = patched->asCompound()->get("layers"); l && l->asString()) layers = *l->asString();
        } else if (!stack.isEmpty()) {
            layers = std::string(stack.info().compostable);
        }
        if (fill < 8 && !layers.empty()) {
            if (fill < 7) {
                // ComposterBlock.addLayer: the compostable's layers provider decides how much it adds.
                loot::BlockContext ctx{level, level.random(), state, pos, &stack, &player_, nullptr};
                int add = loot::contextInt(layers, ctx);
                BlockStateId updated = state;
                if (add > 0) {
                    int newLevel = std::clamp(fill + add, 0, 7);
                    updated = bs::setValue(state, "level", std::to_string(newLevel));
                    level.setBlock(pos, updated);
                    if (newLevel == 7) level.scheduleTick(pos, block, 20);
                }
                level.levelEvent(1500, pos, updated != state ? 1 : 0);
                consume();
            }
            return UseResult::Success;
        }
        return UseResult::TryEmptyHand;
    }

    if (bs::isInstance(block, "PumpkinBlock")) {
        static const int32_t kShears = itemId("minecraft:shears");
        if (stack.isEmpty() || stack.item != kShears) return UseResult::TryEmptyHand;
        Direction dir = direction::axis(face) == Axis::Y ? direction::opposite(direction::fromYRot(player_.yRot)) : face;
        loot::BlockContext ctx{level, level.random(), state, pos, &stack, &player_, nullptr};
        std::uniform_real_distribution<double> u(0.0, 1.0);
        for (ItemStack& seeds : loot::getRandomItems("minecraft:carve/pumpkin", ctx)) {
            double x = pos.x + 0.5 + direction::stepX(dir) * 0.65, y = pos.y + 0.1, z = pos.z + 0.5 + direction::stepZ(dir) * 0.65;
            if (ItemEntity* e = ItemEntity::spawn(level, x, y, z, std::move(seeds), level.random())) {
                e->vx = 0.05 * direction::stepX(dir) + u(level.random()) * 0.02;
                e->vy = 0.05;
                e->vz = 0.05 * direction::stepZ(dir) + u(level.random()) * 0.02;
            }
        }
        static const int32_t kCarve = soundId("minecraft:block.pumpkin.carve");
        level.playBlockSound(-1, pos, kCarve, 1.0f, 1.0f);
        BlockStateId carved = bs::setValue(bs::defaultState(gen::block::CARVED_PUMPKIN), "facing", direction::name(dir));
        level.setBlock(pos, carved, ServerLevel::kUpdateAll | 8);
        hurtAndBreak(stack, 1, hand);
        return UseResult::Success;
    }

    if (bs::isInstance(block, "SweetBerryBushBlock")) {
        static const int32_t kBoneMeal = itemId("minecraft:bone_meal");
        if (intValue(state, "age") != 3 && !stack.isEmpty() && stack.item == kBoneMeal) return UseResult::Pass;
        return UseResult::TryEmptyHand;
    }

    if (bs::isInstance(block, "BeehiveBlock")) {
        static const int32_t kShears = itemId("minecraft:shears"), kBottle = itemId("minecraft:glass_bottle");
        bool emptied = false;
        if (intValue(state, "honey_level") >= 5 && !stack.isEmpty()) {
            if (stack.item == kShears) { // BeehiveBlock.dropHoneycomb
                loot::BlockContext ctx{level, level.random(), state, pos, &stack, &player_, blockEntityAt(pos, "minecraft:beehive")};
                for (ItemStack& s : loot::getRandomItems("minecraft:harvest/beehive", ctx)) level.popResource(pos, std::move(s));
                static const int32_t kShear = soundId("minecraft:block.beehive.shear");
                level.playSound(-1, player_.x, player_.y, player_.z, kShear, ServerLevel::kSoundSourceBlocks, 1.0f, 1.0f);
                hurtAndBreak(stack, 1, hand);
                emptied = true;
            } else if (stack.item == kBottle) {
                stack.shrink(1); // (in creative too)
                static const int32_t kFill = soundId("minecraft:item.bottle.fill");
                level.playSound(player_.id(), player_.x, player_.y, player_.z, kFill, ServerLevel::kSoundSourceBlocks, 1.0f, 1.0f);
                ItemStack honey(itemId("minecraft:honey_bottle"), 1);
                if (stack.isEmpty()) stack = honey;
                else giveOrDrop(honey);
                emptied = true;
            }
        }
        if (!emptied) return UseResult::TryEmptyHand;
        // Outside campfire smoke the bees inside get angry and leave (releaseBeesAndResetHoneyLevel).
        // TODO: bees (entity simulation); until then they stay in the hive.
        level.setBlock(pos, bs::setValue(state, "honey_level", "0"));
        return UseResult::Success;
    }

    if (bs::isInstance(block, "DecoratedPotBlock")) {
        nbt::Compound* pot = blockEntityAt(pos, "minecraft:decorated_pot");
        if (!pot) return UseResult::Pass;
        std::optional<ItemStack> inside;
        if (const nbt::Compound* item = pot->getCompound("item")) inside = item_nbt::read(*item);
        bool empty = !inside || inside->isEmpty();
        if (stack.isEmpty() || (!empty && (!ItemStack::isSameItemSameComponents(*inside, stack) || inside->count >= inside->maxStackSize())))
            return UseResult::TryEmptyHand;
        level.blockEvent(pos, block, 1, 0); // wobble(POSITIVE)
        ItemStack one = stack;
        one.count = 1;
        consume();
        if (empty) inside = one;
        else inside->count++;
        float pitchBend = static_cast<float>(inside->count) / static_cast<float>(inside->maxStackSize());
        pot->put("item", item_nbt::write(*inside));
        static const int32_t kInsert = soundId("minecraft:block.decorated_pot.insert");
        level.playBlockSound(-1, pos, kInsert, 1.0f, 0.7f + 0.5f * pitchBend);
        level.sendParticles("minecraft:dust_plume", pos.x + 0.5, pos.y + 1.2, pos.z + 0.5, 7, 0, 0, 0, 0);
        return UseResult::Success;
    }

    if (bs::isInstance(block, "ChiseledBookShelfBlock")) {
        nbt::Compound* shelf = blockEntityAt(pos, "minecraft:chiseled_bookshelf");
        if (!shelf) return UseResult::Pass;
        if (stack.isEmpty() || !items::hasTag(stack.item, "minecraft:bookshelf_books")) return UseResult::TryEmptyHand;
        int slot = hitSlot(pos, face, facingOf(state), hx, hitY, hz, 2, 3);
        if (slot < 0) return UseResult::Pass;
        if (bs::getBool(state, "slot_" + std::to_string(slot) + "_occupied")) return UseResult::TryEmptyHand;
        // ChiseledBookShelfBlock.addBook
        static const int32_t kEnchantedBook = itemId("minecraft:enchanted_book");
        static const int32_t kInsert = soundId("minecraft:block.chiseled_bookshelf.insert");
        static const int32_t kInsertEnchanted = soundId("minecraft:block.chiseled_bookshelf.insert.enchanted");
        int32_t sound = stack.item == kEnchantedBook ? kInsertEnchanted : kInsert;
        ItemStack one = stack;
        one.count = 1;
        consume();
        setSlotItem(*shelf, slot, one);
        shelf->put("last_interacted_slot", slot);
        level.setBlock(pos, bs::setBool(state, "slot_" + std::to_string(slot) + "_occupied", true)); // updateState
        level.playBlockSound(-1, pos, sound, 1.0f, 1.0f);
        return UseResult::Success;
    }

    if (bs::isInstance(block, "LecternBlock")) {
        if (bs::getBool(state, "has_book")) return UseResult::TryEmptyHand;
        if (!stack.isEmpty() && items::hasTag(stack.item, "minecraft:lectern_books")) { // LecternBlock.placeBook
            if (nbt::Compound* lectern = blockEntityAt(pos, "minecraft:lectern")) {
                ItemStack one = stack;
                one.count = 1;
                consume();
                lectern->put("Book", item_nbt::write(one));
                lectern->put("Page", 0);
                level.setBlock(pos, bs::setBool(bs::setBool(state, "powered", false), "has_book", true)); // resetBookState
                static const int32_t kPut = soundId("minecraft:item.book.put");
                level.playBlockSound(-1, pos, kPut, 1.0f, 1.0f);
            }
            return UseResult::Success;
        }
        return stack.isEmpty() && hand == 0 ? UseResult::Pass : UseResult::TryEmptyHand;
    }

    if (bs::isInstance(block, "RespawnAnchorBlock")) {
        static const int32_t kGlowstone = itemId("minecraft:glowstone");
        int charges = intValue(state, "charges");
        if (!stack.isEmpty() && stack.item == kGlowstone && charges < 4) { // RespawnAnchorBlock.charge
            level.setBlock(pos, bs::setValue(state, "charges", std::to_string(charges + 1)));
            static const int32_t kCharge = soundId("minecraft:block.respawn_anchor.charge");
            level.playBlockSound(-1, pos, kCharge, 1.0f, 1.0f);
            consume();
            return UseResult::Success;
        }
        const ItemStack& offhand = player_.inventory.equipment(EquipmentSlot::OffHand);
        if (hand == 0 && !offhand.isEmpty() && offhand.item == kGlowstone && charges < 4) return UseResult::Pass;
        return UseResult::TryEmptyHand;
    }

    if (bs::isInstance(block, "CopperGolemStatueBlock")) {
        bool axe = !stack.isEmpty() && items::hasTag(stack.item, "minecraft:axes");
        static const int32_t kHoneycomb = itemId("minecraft:honeycomb");
        bool weathering = bs::isInstance(block, "WeatheringCopperGolemStatueBlock");
        // TODO: an axe on an unaffected (unwaxed, unoxidized) statue turns it back into a copper golem (entities).
        if (axe || (weathering && !stack.isEmpty() && stack.item == kHoneycomb)) return UseResult::Pass;
        // CopperGolemStatueBlock.updatePose: the next pose, in CopperGolemStatueBlock.Pose order
        static constexpr std::string_view kPoses[] = {"standing", "sitting", "running", "star"};
        std::string_view pose = bs::getValue(state, "copper_golem_pose").value_or("standing");
        size_t i = 0;
        while (i < 4 && kPoses[i] != pose) i++;
        static const int32_t kBecomeStatue = soundId("minecraft:entity.copper_golem_become_statue");
        level.playBlockSound(-1, pos, kBecomeStatue, 1.0f, 1.0f);
        level.setBlock(pos, bs::setValue(state, "copper_golem_pose", kPoses[(i + 1) % 4]));
        return UseResult::Success;
    }

    if (bs::isInstance(block, "JukeboxBlock")) { // JukeboxPlayable.tryInsertIntoJukebox
        if (bs::getBool(state, "has_record") || block_entity::jukebox::songOf(stack).empty()) return UseResult::TryEmptyHand;
        if (nbt::Compound* jukebox = blockEntityAt(pos, "minecraft:jukebox")) {
            ItemStack disc = stack;
            disc.count = 1;
            consume();
            block_entity::jukebox::insert(level, pos, state, *jukebox, disc);
        }
        return UseResult::Success;
    }

    if (bs::isInstance(block, "AbstractCauldronBlock")) return useItemOnCauldron(stack, hand, pos, state);

    return UseResult::TryEmptyHand;
}

// CauldronInteractions: buckets, bottles and washing dyed items, banners and shulker boxes.
UseResult ServerGamePacketListener::useItemOnCauldron(ItemStack& stack, int hand, BlockPos pos, BlockStateId state) {
    if (stack.isEmpty()) return UseResult::TryEmptyHand;
    ServerLevel& level = player_.level();
    uint32_t block = bs::blockOf(state);
    std::string_view item = items::name(stack.item);
    auto sound = [&](std::string_view name) { level.playBlockSound(-1, pos, soundId(name), 1.0f, 1.0f); };
    // ItemUtils.createFilledResult: the used item becomes `result` (in creative the result is only
    // added, once, unless `limitCreativeStackSize` is false).
    auto fill = [&](ItemStack result, bool limitCreativeStackSize = true) {
        if (limitCreativeStackSize && player_.hasInfiniteMaterials()) {
            if (player_.inventory.findSlotMatchingItem(result) == -1) player_.inventory.add(result);
            return;
        }
        if (!player_.hasInfiniteMaterials()) stack.shrink(1);
        if (stack.isEmpty()) stack = std::move(result);
        else giveOrDrop(std::move(result));
    };
    // LayeredCauldronBlock.lowerFillLevel
    auto lower = [&] {
        int lvl = intValue(state, "level") - 1;
        level.setBlock(pos, lvl == 0 ? bs::defaultState(gen::block::CAULDRON) : bs::setValue(state, "level", std::to_string(lvl)));
    };
    auto waterPotion = [&](const ItemStack& s) {
        auto contents = items::componentValue(s, "minecraft:potion_contents");
        if (!contents) return false;
        const nbt::Tag* potion = contents->asCompound() ? contents->asCompound()->get("potion") : &*contents;
        return potion && potion->asString() && *potion->asString() == "minecraft:water";
    };
    bool underWater = bs::isWaterSource(level.getBlockState(pos.above())) || (bs::flags(level.getBlockState(pos.above())) & bs::kWater);

    // addDefaultInteractions: any cauldron can be filled from a bucket.
    auto emptyBucket = [&](BlockStateId newState, std::string_view snd) {
        fill(ItemStack(itemId("minecraft:bucket"), 1));
        level.setBlock(pos, newState);
        sound(snd);
        return UseResult::Success;
    };
    if (item == "minecraft:water_bucket")
        return emptyBucket(bs::setValue(bs::defaultState(gen::block::WATER_CAULDRON), "level", "3"), "minecraft:item.bucket.empty");
    if (item == "minecraft:lava_bucket")
        return underWater ? UseResult::Consume : emptyBucket(bs::defaultState(gen::block::LAVA_CAULDRON), "minecraft:item.bucket.empty_lava");
    if (item == "minecraft:powder_snow_bucket")
        return underWater ? UseResult::Consume
                          : emptyBucket(bs::setValue(bs::defaultState(gen::block::POWDER_SNOW_CAULDRON), "level", "3"),
                                        "minecraft:item.bucket.empty_powder_snow");

    auto fillBucket = [&](std::string_view result, bool canFill, std::string_view snd) {
        if (!canFill) return UseResult::TryEmptyHand;
        fill(ItemStack(itemId(result), 1));
        level.setBlock(pos, bs::defaultState(gen::block::CAULDRON));
        sound(snd);
        return UseResult::Success;
    };
    int fillLevel = bs::getValue(state, "level") ? intValue(state, "level") : 0;
    if (block == gen::block::CAULDRON) {
        if (item == "minecraft:potion" && waterPotion(stack)) {
            fill(ItemStack(itemId("minecraft:glass_bottle"), 1));
            level.setBlock(pos, bs::defaultState(gen::block::WATER_CAULDRON));
            sound("minecraft:item.bottle.empty");
            return UseResult::Success;
        }
    } else if (block == gen::block::WATER_CAULDRON) {
        if (item == "minecraft:bucket") return fillBucket("minecraft:water_bucket", fillLevel == 3, "minecraft:item.bucket.fill");
        if (item == "minecraft:glass_bottle") {
            ItemStack water(itemId("minecraft:potion"), 1);
            nbt::Compound contents;
            contents.put("potion", std::string("minecraft:water"));
            items::setComponentValue(water, "minecraft:potion_contents", contents);
            fill(std::move(water));
            lower();
            sound("minecraft:item.bottle.fill");
            return UseResult::Success;
        }
        if (item == "minecraft:potion") {
            if (fillLevel == 3 || !waterPotion(stack)) return UseResult::TryEmptyHand;
            fill(ItemStack(itemId("minecraft:glass_bottle"), 1));
            level.setBlock(pos, bs::setValue(state, "level", std::to_string(fillLevel + 1)));
            sound("minecraft:item.bottle.empty");
            return UseResult::Success;
        }
        if (items::hasTag(stack.item, "minecraft:cauldron_can_remove_dye")) { // dyedItemIteration
            if (!items::componentValue(stack, "minecraft:dyed_color")) return UseResult::TryEmptyHand;
            items::resetComponent(stack, "minecraft:dyed_color");
            lower();
            return UseResult::Success;
        }
        if (stack.info().itemClass == "BannerItem") { // bannerInteraction: wash off the last pattern
            auto patterns = items::componentValue(stack, "minecraft:banner_patterns");
            const nbt::List* layers = patterns ? patterns->asList() : nullptr;
            if (!layers || layers->items.empty()) return UseResult::TryEmptyHand;
            ItemStack cleaned = stack;
            cleaned.count = 1;
            nbt::List fewer = *layers;
            fewer.items.pop_back();
            if (fewer.items.empty()) items::resetComponent(cleaned, "minecraft:banner_patterns");
            else items::setComponentValue(cleaned, "minecraft:banner_patterns", fewer);
            fill(std::move(cleaned), false);
            lower();
            return UseResult::Success;
        }
        if (stack.info().block >= 0 && bs::isInstance(static_cast<uint32_t>(stack.info().block), "ShulkerBoxBlock") &&
            stack.info().block != static_cast<int32_t>(gen::block::SHULKER_BOX)) { // shulkerBoxInteraction: back to undyed
            ItemStack cleaned = stack; // ItemStack.transmuteCopy keeps the components
            cleaned.item = itemId("minecraft:shulker_box");
            cleaned.count = 1;
            fill(std::move(cleaned), false);
            lower();
            return UseResult::Success;
        }
    } else if (block == gen::block::LAVA_CAULDRON) {
        if (item == "minecraft:bucket") return fillBucket("minecraft:lava_bucket", true, "minecraft:item.bucket.fill_lava");
    } else if (block == gen::block::POWDER_SNOW_CAULDRON) {
        if (item == "minecraft:bucket") return fillBucket("minecraft:powder_snow_bucket", fillLevel == 3, "minecraft:item.bucket.fill_powder_snow");
    }
    (void)hand;
    return UseResult::TryEmptyHand;
}

// Block.getMenuProvider + ServerPlayer.openMenu for the blocks with a container menu. Returns
// false if the block has none (or it's blocked right now).
bool ServerGamePacketListener::openBlockMenu(BlockPos pos, BlockStateId state) {
    ServerLevel& level = player_.level();
    uint32_t block = bs::blockOf(state);
    auto menuId = [](std::string_view name) { return registries::id("minecraft:menu", name).value_or(0); };
    // The block entity's custom name, else the default title.
    auto customName = [&](BlockPos at) -> const nbt::Tag* {
        LevelChunk* chunk = level.getChunkIfLoaded({at.x >> 4, at.z >> 4});
        auto* be = chunk ? chunk->blockEntityAt(at.x, at.y, at.z) : nullptr;
        return be ? be->tag.get("CustomName") : nullptr;
    };
    auto title = [&](BlockPos at, std::string_view fallback) -> nbt::Tag {
        if (const nbt::Tag* name = customName(at)) return *name;
        return Component::translatable(std::string(fallback)).toNbt();
    };
    auto grid = [&](ContainerGridMenu::Kind kind, std::string_view menu, std::shared_ptr<Container> container, nbt::Tag name) {
        int id = player_.nextContainerId();
        player_.openMenu(std::make_unique<ContainerGridMenu>(kind, menuId(menu), id, player_, std::move(container)), menuId(menu), name);
        return true;
    };
    auto conductorAbove = [&](BlockPos at) { return (bs::flags(level.getBlockState(at.above())) & bs::kRedstoneConductor) != 0; };

    if (bs::isInstance(block, "ChestBlock")) {
        // ChestBlock.getMenuProvider / combine: a double chest shows both halves (the right one first).
        auto self = level.blockEntityContainer(pos);
        if (!self || conductorAbove(pos)) return false; // (cats sitting on it aren't simulated)
        std::string_view type = bs::getValue(state, "type").value_or("single");
        if (type != "single") {
            Direction facing = direction::byName(bs::getValue(state, "facing").value_or("north"));
            BlockPos other = pos.relative(type == "left" ? direction::clockWise(facing) : direction::counterClockWise(facing));
            auto otherContainer = level.blockEntityContainer(other);
            if (otherContainer && bs::blockOf(level.getBlockState(other)) == block) {
                if (conductorAbove(other)) return false;
                bool right = type == "right";
                auto first = right ? self : otherContainer, second = right ? otherContainer : self;
                BlockPos firstPos = right ? pos : other, secondPos = right ? other : pos;
                nbt::Tag name = customName(firstPos) ? *customName(firstPos) : title(secondPos, "container.chestDouble");
                return grid(ContainerGridMenu::Kind::Chest, "minecraft:generic_9x6", std::make_shared<CompoundContainer>(first, second), name);
            }
        }
        return grid(ContainerGridMenu::Kind::Chest, "minecraft:generic_9x3", self, title(pos, "container.chest"));
    }
    if (bs::isInstance(block, "BarrelBlock")) {
        auto c = level.blockEntityContainer(pos);
        return c && grid(ContainerGridMenu::Kind::Chest, "minecraft:generic_9x3", c, title(pos, "container.barrel"));
    }
    if (bs::isInstance(block, "ShulkerBoxBlock")) {
        auto c = level.blockEntityContainer(pos);
        if (!c) return false;
        if (c->openers == 0) { // ShulkerBoxBlock.canOpen: a closed lid needs half a block of room to open
            Direction facing = direction::byName(bs::getValue(state, "facing").value_or("up"));
            BlockPos front = pos.relative(facing);
            AABB lid{static_cast<double>(front.x), static_cast<double>(front.y), static_cast<double>(front.z), front.x + 1.0, front.y + 1.0,
                     front.z + 1.0};
            double sx = direction::stepX(facing), sy = direction::stepY(facing), sz = direction::stepZ(facing);
            if (sx > 0) lid.maxX = front.x + 0.5; else if (sx < 0) lid.minX = front.x + 0.5;
            if (sy > 0) lid.maxY = front.y + 0.5; else if (sy < 0) lid.minY = front.y + 0.5;
            if (sz > 0) lid.maxZ = front.z + 0.5; else if (sz < 0) lid.minZ = front.z + 0.5;
            lid = collision::deflate(lid, 1.0E-6);
            for (const auto& b : level.collisionBoxes(lid))
                if (b.intersects(lid)) return false;
        }
        return grid(ContainerGridMenu::Kind::ShulkerBox, "minecraft:shulker_box", c, title(pos, "container.shulkerBox"));
    }
    if (bs::isInstance(block, "EnderChestBlock")) {
        if (conductorAbove(pos) || player_.gameMode == GameType::Spectator) return false;
        auto lid = level.blockEntityContainer(pos); // (only the lid: the items are the player's)
        if (!lid) return false;
        return grid(ContainerGridMenu::Kind::Chest, "minecraft:generic_9x3", std::make_shared<EnderChestContainer>(player_.enderChest, lid),
                    Component::translatable("container.enderchest").toNbt());
    }
    if (bs::isInstance(block, "HopperBlock")) {
        auto c = level.blockEntityContainer(pos);
        return c && grid(ContainerGridMenu::Kind::Hopper, "minecraft:hopper", c, title(pos, "container.hopper"));
    }
    if (bs::isInstance(block, "DispenserBlock")) { // (and droppers)
        auto c = level.blockEntityContainer(pos);
        bool dropper = bs::isInstance(block, "DropperBlock");
        return c && grid(ContainerGridMenu::Kind::Dispenser, "minecraft:generic_3x3", c, title(pos, dropper ? "container.dropper" : "container.dispenser"));
    }
    // Workstations: the menu owns its input slots (ContainerLevelAccess menus).
    auto open = [&](std::unique_ptr<AbstractContainerMenu> menu, nbt::Tag name) {
        int type = menu->menuType;
        player_.openMenu(std::move(menu), type, std::move(name));
        return true;
    };
    auto translatable = [](const char* key) { return Component::translatable(key).toNbt(); };
    if (bs::isInstance(block, "FletchingTableBlock")) return false; // (a CraftingTableBlock without a menu)
    if (bs::isInstance(block, "SmithingTableBlock"))
        return open(std::make_unique<SmithingMenu>(player_.nextContainerId(), player_, pos), translatable("container.upgrade"));
    if (bs::isInstance(block, "StonecutterBlock"))
        return open(std::make_unique<StonecutterMenu>(player_.nextContainerId(), player_, pos), translatable("container.stonecutter"));
    if (bs::isInstance(block, "GrindstoneBlock"))
        return open(std::make_unique<GrindstoneMenu>(player_.nextContainerId(), player_, pos), translatable("container.grindstone_title"));
    if (bs::isInstance(block, "AnvilBlock"))
        return open(std::make_unique<AnvilMenu>(player_.nextContainerId(), player_, pos), translatable("container.repair"));
    if (bs::isInstance(block, "LoomBlock"))
        return open(std::make_unique<LoomMenu>(player_.nextContainerId(), player_, pos), translatable("container.loom"));
    if (bs::isInstance(block, "EnchantingTableBlock"))
        return open(std::make_unique<EnchantmentMenu>(player_.nextContainerId(), player_, pos), title(pos, "container.enchant"));
    if (bs::isInstance(block, "CraftingTableBlock")) {
        int id = player_.nextContainerId();
        player_.openMenu(std::make_unique<CraftingMenu>(id, player_, pos), menuId("minecraft:crafting"), Component::translatable("container.crafting").toNbt());
        return true;
    }
    if (bs::isInstance(block, "AbstractFurnaceBlock")) {
        auto c = level.blockEntityContainer(pos);
        if (!c) return false;
        std::string_view kind = c->type == "minecraft:smoker" ? "smoker" : c->type == "minecraft:blast_furnace" ? "blast_furnace" : "furnace";
        int type = menuId("minecraft:" + std::string(kind));
        int id = player_.nextContainerId();
        player_.openMenu(std::make_unique<FurnaceMenu>(type, id, player_, c), type, title(pos, "container." + std::string(kind)));
        return true;
    }
    if (bs::isInstance(block, "BrewingStandBlock")) {
        auto c = level.blockEntityContainer(pos);
        return c && open(std::make_unique<BrewingStandMenu>(player_.nextContainerId(), player_, c), title(pos, "container.brewing"));
    }
    if (bs::isInstance(block, "CrafterBlock")) {
        auto c = level.blockEntityContainer(pos);
        return c && open(std::make_unique<CrafterMenu>(player_.nextContainerId(), player_, c), title(pos, "container.crafter"));
    }
    if (bs::isInstance(block, "BeaconBlock")) {
        if (!blockEntityAt(pos, "minecraft:beacon")) return false;
        return open(std::make_unique<BeaconMenu>(player_.nextContainerId(), player_, pos), title(pos, "container.beacon"));
    }
    if (bs::isInstance(block, "LecternBlock")) {
        if (!bs::getBool(state, "has_book") || !blockEntityAt(pos, "minecraft:lectern")) return false;
        return open(std::make_unique<LecternMenu>(player_.nextContainerId(), player_, pos), translatable("container.lectern"));
    }
    // TODO: cartography tables (they need maps).
    return false;
}

UseResult ServerGamePacketListener::useBlockWithoutItem(BlockPos pos, BlockStateId state, Direction face, double hx, double hy, double hz) {
    ServerLevel& level = player_.level();
    uint32_t block = bs::blockOf(state);
    const auto& beh = bs::behaviour(block);
    std::uniform_real_distribution<float> uf(0.0f, 1.0f);

    if (bs::isInstance(block, "SignBlock")) return useSignWithoutItem(pos, state);
    if (bs::isInstance(block, "BedBlock")) return useBed(pos, state);

    if (bs::isInstance(block, "FletchingTableBlock")) return UseResult::Pass;
    static constexpr std::string_view kMenuBlocks[] = {"ChestBlock",         "BarrelBlock",      "ShulkerBoxBlock", "EnderChestBlock",
                                                       "HopperBlock",        "DispenserBlock",   "CraftingTableBlock", "AbstractFurnaceBlock",
                                                       "StonecutterBlock",   "GrindstoneBlock",  "AnvilBlock",       "LoomBlock",
                                                       "EnchantingTableBlock", "BrewingStandBlock", "CrafterBlock",    "BeaconBlock"};
    for (auto cls : kMenuBlocks)
        if (bs::isInstance(block, cls)) {
            openBlockMenu(pos, state);
            return UseResult::Success; // (vanilla returns SUCCESS even when the chest is blocked)
        }

    if (bs::isInstance(block, "FlowerPotBlock")) {
        if (beh.potted == static_cast<int32_t>(gen::block::AIR)) return UseResult::Consume;
        giveOrDrop(ItemStack(bs::behaviour(static_cast<uint32_t>(beh.potted)).item, 1));
        level.setBlock(pos, bs::defaultState(gen::block::FLOWER_POT));
        return UseResult::Success;
    }

    // CakeBlock.eat: Player.canEat(false) holds for invulnerable (creative) players or a food
    // level below 20. We don't model hunger yet, so survival players are always full.
    // TODO: FoodData (hunger, saturation, exhaustion) - then survival players can eat too.
    auto eat = [&](BlockStateId cake) {
        if (player_.gameMode != GameType::Creative) return UseResult::Pass;
        int bites = intValue(cake, "bites");
        if (bites < 6) level.setBlock(pos, bs::setValue(cake, "bites", std::to_string(bites + 1)));
        else level.setBlock(pos, bs::defaultState(gen::block::AIR));
        return UseResult::Success;
    };
    if (bs::isInstance(block, "CakeBlock")) return eat(state);
    if (bs::isInstance(block, "CandleCakeBlock")) {
        UseResult r = eat(bs::defaultState(gen::block::CAKE));
        if (isSuccess(r)) level.dropResources(state, pos, nullptr, nullptr, nullptr); // the candle
        return r;
    }

    if (bs::isInstance(block, "ComposterBlock")) {
        if (intValue(state, "level") != 8) return UseResult::Pass;
        // ComposterBlock.extractProduce
        double x = pos.x + 0.5 + (uf(level.random()) * 2.0f - 1.0f) * 0.7f, z = pos.z + 0.5 + (uf(level.random()) * 2.0f - 1.0f) * 0.7f;
        if (ItemEntity* e = ItemEntity::spawn(level, x, pos.y + 1.01, z, ItemStack(itemId("minecraft:bone_meal"), 1), level.random()))
            e->pickupDelay = ItemEntity::kDefaultPickupDelay;
        level.setBlock(pos, bs::setValue(state, "level", "0"));
        static const int32_t kEmpty = soundId("minecraft:block.composter.empty");
        level.playBlockSound(-1, pos, kEmpty, 1.0f, 1.0f);
        return UseResult::Success;
    }

    // Block.dropFromBlockInteractLootTable + popResource
    auto harvest = [&](std::string_view table) {
        loot::BlockContext ctx{level, level.random(), state, pos, nullptr, &player_, nullptr};
        for (ItemStack& s : loot::getRandomItems(table, ctx)) level.popResource(pos, std::move(s));
    };
    if (bs::isInstance(block, "SweetBerryBushBlock")) {
        if (intValue(state, "age") <= 1) return UseResult::Pass;
        harvest("minecraft:harvest/sweet_berry_bush");
        static const int32_t kPick = soundId("minecraft:block.sweet_berry_bush.pick_berries");
        level.playBlockSound(-1, pos, kPick, 1.0f, 0.8f + uf(level.random()) * 0.4f);
        level.setBlock(pos, bs::setValue(state, "age", "1"), ServerLevel::kUpdateClients);
        return UseResult::Success;
    }
    if (bs::isInstance(block, "CaveVinesBlock") || bs::isInstance(block, "CaveVinesPlantBlock")) { // CaveVines.use
        if (!bs::getBool(state, "berries")) return UseResult::Pass;
        harvest("minecraft:harvest/cave_vine");
        static const int32_t kPick = soundId("minecraft:block.cave_vines.pick_berries");
        level.playBlockSound(-1, pos, kPick, 1.0f, 0.8f + uf(level.random()) * 0.4f);
        level.setBlock(pos, bs::setBool(state, "berries", false), ServerLevel::kUpdateClients);
        return UseResult::Success;
    }

    if (bs::isInstance(block, "ChiseledBookShelfBlock")) {
        nbt::Compound* shelf = blockEntityAt(pos, "minecraft:chiseled_bookshelf");
        if (!shelf) return UseResult::Pass;
        int slot = hitSlot(pos, face, facingOf(state), hx, hy, hz, 2, 3);
        if (slot < 0) return UseResult::Pass;
        if (!bs::getBool(state, "slot_" + std::to_string(slot) + "_occupied")) return UseResult::Consume;
        // ChiseledBookShelfBlock.removeBook
        ItemStack book = slotItem(*shelf, slot);
        setSlotItem(*shelf, slot, {});
        if (!book.isEmpty()) {
            shelf->put("last_interacted_slot", slot);
            level.setBlock(pos, bs::setBool(state, "slot_" + std::to_string(slot) + "_occupied", false));
        }
        static const int32_t kEnchantedBook = itemId("minecraft:enchanted_book");
        static const int32_t kPickup = soundId("minecraft:block.chiseled_bookshelf.pickup");
        static const int32_t kPickupEnchanted = soundId("minecraft:block.chiseled_bookshelf.pickup.enchanted");
        level.playBlockSound(-1, pos, !book.isEmpty() && book.item == kEnchantedBook ? kPickupEnchanted : kPickup, 1.0f, 1.0f);
        giveOrDrop(std::move(book));
        return UseResult::Success;
    }

    if (bs::isInstance(block, "DecoratedPotBlock")) {
        if (!blockEntityAt(pos, "minecraft:decorated_pot")) return UseResult::Pass;
        static const int32_t kFail = soundId("minecraft:block.decorated_pot.insert_fail");
        level.playBlockSound(-1, pos, kFail, 1.0f, 1.0f);
        level.blockEvent(pos, block, 1, 1); // wobble(NEGATIVE)
        return UseResult::Success;
    }

    if (bs::isInstance(block, "JukeboxBlock")) {
        nbt::Compound* jukebox = blockEntityAt(pos, "minecraft:jukebox");
        if (!bs::getBool(state, "has_record") || !jukebox) return UseResult::Pass;
        block_entity::jukebox::popOut(level, pos, *jukebox);
        return UseResult::Success;
    }

    if (bs::isInstance(block, "LecternBlock")) {
        if (!bs::getBool(state, "has_book")) return UseResult::Consume;
        openBlockMenu(pos, state);
        return UseResult::Success;
    }

    if (bs::isInstance(block, "RespawnAnchorBlock")) {
        // TODO: outside the Nether a charged anchor explodes (explosions), inside it sets the spawn.
        return UseResult::Pass;
    }

    if (block_behaviour::useWithoutItem(level, pos, state, player_.yRot, player_.id(), canModifyWorld(player_.gameMode))) return UseResult::Success;
    return UseResult::Pass;
}

} // namespace mc
