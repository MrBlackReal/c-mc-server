#include "world/level/block/entity/block_entity_container.hpp"

#include <cmath>
#include <random>

#include "core/registries.hpp"
#include "server/level/server_level.hpp"
#include "server/level/server_player.hpp"
#include "world/item/crafting/recipes.hpp"
#include "world/item/item_nbt.hpp"

namespace mc {

namespace bs = blocks;

BlockEntityContainer::BlockEntityContainer(ServerLevel& level, BlockPos pos, std::string type, int size)
    : SimpleContainer(size), pos(pos), type(std::move(type)), level_(level) {
    LevelChunk& chunk = level.getChunk({pos.x >> 4, pos.z >> 4});
    auto* be = chunk.blockEntityAt(pos.x, pos.y, pos.z);
    if (!be) return;
    if (const nbt::List* list = be->tag.getList("Items")) {
        for (const auto& e : list->items) {
            const nbt::Compound* c = e.asCompound();
            if (!c) continue;
            int slot = c->getInt("Slot") & 0xFF;
            if (slot >= size) continue;
            if (auto stack = item_nbt::read(*c)) items_[slot] = std::move(*stack);
        }
    }
}

int BlockEntityContainer::sizeFor(std::string_view type) {
    if (type == "minecraft:chest" || type == "minecraft:trapped_chest" || type == "minecraft:barrel" || type == "minecraft:shulker_box") return 27;
    if (type == "minecraft:dispenser" || type == "minecraft:dropper" || type == "minecraft:crafter") return 9;
    if (type == "minecraft:hopper" || type == "minecraft:brewing_stand") return 5;
    if (type == "minecraft:furnace" || type == "minecraft:smoker" || type == "minecraft:blast_furnace") return 3;
    return 0;
}

nbt::Compound* BlockEntityContainer::tag() {
    if (!valid) return nullptr;
    LevelChunk* chunk = level_.getChunkIfLoaded({pos.x >> 4, pos.z >> 4});
    auto* be = chunk ? chunk->blockEntityAt(pos.x, pos.y, pos.z) : nullptr;
    return be && be->tag.getString("id") == type ? &be->tag : nullptr;
}

void BlockEntityContainer::setItem(int slot, ItemStack stack) {
    bool same = !stack.isEmpty() && ItemStack::isSameItemSameComponents(items_[slot], stack);
    if (!stack.isEmpty() && stack.count > maxStackSize(stack)) stack.count = maxStackSize(stack); // limitSize
    items_[slot] = std::move(stack);
    if (isFurnace(type) && slot == 0 && !same)
        if (nbt::Compound* be = tag()) {
            // getTotalCookTime: the recipe's time, sped up by the fuel's multiplier (200 without a recipe)
            int total = 200;
            if (auto recipe = recipes::findCooking(cookingType(type), items_[0])) {
                float speed = static_cast<float>(be->getDouble("speed_multiplier", 1.0));
                total = speed > 0.0f ? static_cast<int>(std::ceil(recipe->cookingTime / speed)) : recipe->cookingTime;
            }
            be->put("cooking_total_time", total);
            be->put("cooking_time_spent", 0);
        }
    setChanged();
}

void BlockEntityContainer::setChanged() {
    if (!valid || items_.empty()) return;
    LevelChunk& chunk = level_.getChunk({pos.x >> 4, pos.z >> 4});
    auto* be = chunk.blockEntityAt(pos.x, pos.y, pos.z);
    if (!be) return;
    // ContainerHelper.saveAllItems (shulker boxes leave out an empty list)
    nbt::List list{nbt::TagType::Compound, {}};
    for (size_t i = 0; i < items_.size(); i++) {
        if (items_[i].isEmpty()) continue;
        nbt::Compound c = item_nbt::write(items_[i]);
        c.entries.insert(c.entries.begin(), {"Slot", static_cast<int8_t>(i)});
        list.items.push_back(std::move(c));
    }
    if (list.items.empty() && type == "minecraft:shulker_box") be->tag.remove("Items");
    else be->tag.put("Items", std::move(list));
    chunk.dirty = true;
}

bool BlockEntityContainer::stillValid(ServerPlayer& player) {
    if (!valid) return false;
    LevelChunk* chunk = level_.getChunkIfLoaded({pos.x >> 4, pos.z >> 4});
    auto* be = chunk ? chunk->blockEntityAt(pos.x, pos.y, pos.z) : nullptr;
    return be && be->tag.getString("id") == type && player.isWithinBlockInteractionRange(pos, 4.0);
}

void BlockEntityContainer::startOpen(ServerPlayer& player) {
    if (!valid || player.gameMode == GameType::Spectator) return;
    openers++;
    openerCountChanged(true);
}

void BlockEntityContainer::stopOpen(ServerPlayer& player) {
    if (!valid || player.gameMode == GameType::Spectator || openers <= 0) return;
    openers--;
    openerCountChanged(false);
}

void BlockEntityContainer::openerCountChanged(bool opened) {
    BlockStateId state = level_.getBlockState(pos);
    uint32_t block = bs::blockOf(state);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    float pitch = u(level_.random()) * 0.1f + 0.9f;
    auto sound = [&](std::string_view name, double x, double y, double z) {
        level_.playSound(-1, x, y, z, registries::id("minecraft:sound_event", name).value_or(-1), ServerLevel::kSoundSourceBlocks, 0.5f, pitch);
    };
    bool first = opened && openers == 1, last = !opened && openers == 0;
    if (type == "minecraft:chest" || type == "minecraft:trapped_chest") {
        // ChestBlockEntity: lid animation (block event 1 with the viewer count), sound on the first and last.
        if (first || last) {
            std::string_view chestType = bs::getValue(state, "type").value_or("single");
            if (chestType != "left") { // the right half sounds for both, from between them
                double x = pos.x + 0.5, z = pos.z + 0.5;
                if (chestType == "right") {
                    Direction facing = direction::byName(bs::getValue(state, "facing").value_or("north"));
                    Direction d = direction::counterClockWise(facing); // ChestBlock.getConnectedDirection for RIGHT
                    x += direction::stepX(d) * 0.5, z += direction::stepZ(d) * 0.5;
                }
                const auto& beh = bs::behaviour(block);
                int32_t id = first ? beh.openSound : beh.closeSound;
                level_.playSound(-1, x, pos.y + 0.5, z, id, ServerLevel::kSoundSourceBlocks, 0.5f, pitch);
            }
        }
        level_.blockEvent(pos, block, 1, openers);
    } else if (type == "minecraft:ender_chest") {
        if (first) sound("minecraft:block.ender_chest.open", pos.x + 0.5, pos.y + 0.5, pos.z + 0.5);
        if (last) sound("minecraft:block.ender_chest.close", pos.x + 0.5, pos.y + 0.5, pos.z + 0.5);
        level_.blockEvent(pos, block, 1, openers);
    } else if (type == "minecraft:shulker_box") {
        level_.blockEvent(pos, block, 1, openers);
        if (first) sound("minecraft:block.shulker_box.open", pos.x + 0.5, pos.y + 0.5, pos.z + 0.5);
        if (last) sound("minecraft:block.shulker_box.close", pos.x + 0.5, pos.y + 0.5, pos.z + 0.5);
    } else if (type == "minecraft:barrel" && (first || last)) {
        // BarrelBlockEntity: the lid opens (the "open" property), the sound comes from its face.
        Direction facing = direction::byName(bs::getValue(state, "facing").value_or("north"));
        double x = pos.x + 0.5 + direction::stepX(facing) / 2.0, y = pos.y + 0.5 + direction::stepY(facing) / 2.0,
               z = pos.z + 0.5 + direction::stepZ(facing) / 2.0;
        sound(first ? "minecraft:block.barrel.open" : "minecraft:block.barrel.close", x, y, z);
        if (bs::isInstance(block, "BarrelBlock")) level_.setBlock(pos, bs::setBool(state, "open", first));
    }
}

} // namespace mc
