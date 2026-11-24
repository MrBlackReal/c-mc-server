#include "world/level/block/entity/hopper.hpp"

#include <algorithm>
#include <string>

#include "core/registries.hpp"
#include "server/level/server_level.hpp"
#include "world/entity/item_entity.hpp"
#include "world/item/item_logic.hpp"
#include "world/level/block/entity/block_entity_container.hpp"
#include "world/level/storage/loot/loot_tables.hpp"

namespace mc::hopper {

namespace bs = blocks;
using namespace gen::block;

namespace {

bool isA(uint32_t block, std::string_view cls) { return bs::isInstance(block, cls); }

// ---------------------------------------------------------------- composters (WorldlyContainerHolder)

// The compostable component's layers provider (ItemStack.get(COMPOSTABLE)), if any.
std::string compostLayers(const ItemStack& stack) {
    if (stack.isEmpty()) return {};
    if (auto patched = items::componentValue(stack, "minecraft:compostable"); patched && patched->asCompound())
        if (const nbt::Tag* l = patched->asCompound()->get("layers"); l && l->asString()) return *l->asString();
    return std::string(stack.info().compostable);
}

// ComposterBlock.addLayer
BlockStateId composterAddLayer(ServerLevel& level, BlockPos pos, BlockStateId state, const std::string& layers) {
    loot::BlockContext ctx{level, level.random(), state, pos, nullptr, nullptr, nullptr};
    int add = loot::contextInt(layers, ctx);
    if (add <= 0) return state;
    int fill = std::stoi(std::string(bs::getValue(state, "level").value_or("0")));
    int newLevel = std::clamp(fill + add, 0, 7);
    BlockStateId n = bs::setValue(state, "level", std::to_string(newLevel));
    level.setBlock(pos, n);
    if (newLevel == 7) level.scheduleTick(pos, bs::blockOf(state), 20);
    return n;
}

// ComposterBlock.InputContainer: one compostable item at a time, from above.
class ComposterInput : public SimpleContainer {
public:
    ComposterInput(ServerLevel& level, BlockPos pos, BlockStateId state) : SimpleContainer(1), level_(level), pos_(pos), state_(state) {}
    int maxStackSize() const override { return 1; }
    bool isWorldly() const override { return true; }
    std::vector<int> slotsForFace(Direction face) override { return face == Direction::Up ? std::vector<int>{0} : std::vector<int>{}; }
    bool canPlaceItemThroughFace(int, const ItemStack& stack, std::optional<Direction> face) override {
        return !changed_ && face == Direction::Up && !compostLayers(stack).empty();
    }
    bool canTakeItemThroughFace(int, const ItemStack&, Direction) override { return false; }
    void setChanged() override {
        ItemStack contents = items_[0];
        std::string layers = compostLayers(contents);
        if (contents.isEmpty() || layers.empty()) return;
        changed_ = true;
        BlockStateId n = composterAddLayer(level_, pos_, state_, layers);
        level_.levelEvent(1500, pos_, n != state_ ? 1 : 0);
        items_[0] = {}; // removeItemNoUpdate
    }

private:
    ServerLevel& level_;
    BlockPos pos_;
    BlockStateId state_;
    bool changed_ = false;
};

// ComposterBlock.OutputContainer: the bone meal of a full composter, from below.
class ComposterOutput : public SimpleContainer {
public:
    ComposterOutput(ServerLevel& level, BlockPos pos, BlockStateId state) : SimpleContainer(1), level_(level), pos_(pos), state_(state) {
        items_[0] = ItemStack(items::byName("minecraft:bone_meal").value_or(0), 1);
    }
    int maxStackSize() const override { return 1; }
    bool isWorldly() const override { return true; }
    std::vector<int> slotsForFace(Direction face) override { return face == Direction::Down ? std::vector<int>{0} : std::vector<int>{}; }
    bool canPlaceItemThroughFace(int, const ItemStack&, std::optional<Direction>) override { return false; }
    bool canTakeItemThroughFace(int, const ItemStack& stack, Direction face) override {
        static const int32_t kBoneMeal = items::byName("minecraft:bone_meal").value_or(-1);
        return !changed_ && face == Direction::Down && stack.item == kBoneMeal;
    }
    void setChanged() override { // ComposterBlock.empty
        level_.setBlock(pos_, bs::setValue(state_, "level", "0"));
        static const int32_t kEmpty = registries::id("minecraft:sound_event", "minecraft:block.composter.empty").value_or(-1);
        level_.playBlockSound(-1, pos_, kEmpty, 1.0f, 1.0f);
        changed_ = true;
    }

private:
    ServerLevel& level_;
    BlockPos pos_;
    BlockStateId state_;
    bool changed_ = false;
};

// ComposterBlock.EmptyContainer
class ComposterFull : public SimpleContainer {
public:
    ComposterFull() : SimpleContainer(0) {}
    bool isWorldly() const override { return true; }
    std::vector<int> slotsForFace(Direction) override { return {}; }
    bool canPlaceItemThroughFace(int, const ItemStack&, std::optional<Direction>) override { return false; }
    bool canTakeItemThroughFace(int, const ItemStack&, Direction) override { return false; }
};

// ---------------------------------------------------------------- the hopper's own state

nbt::Compound* hopperTag(ServerLevel& level, BlockPos pos) {
    LevelChunk* chunk = level.getChunkIfLoaded({pos.x >> 4, pos.z >> 4});
    auto* be = chunk ? chunk->blockEntityAt(pos.x, pos.y, pos.z) : nullptr;
    return be && be->tag.getString("id") == "minecraft:hopper" ? &be->tag : nullptr;
}
int cooldown(ServerLevel& level, BlockPos pos) {
    nbt::Compound* t = hopperTag(level, pos);
    return t ? t->getInt("TransferCooldown", -1) : -1;
}
void setCooldown(ServerLevel& level, BlockPos pos, int time) {
    if (nbt::Compound* t = hopperTag(level, pos)) t->put("TransferCooldown", time);
}

// The hopper a container is, if it's one (for the cooldown handover in tryMoveInItem).
const BlockEntityContainer* asHopper(const Container* c) {
    auto* be = dynamic_cast<const BlockEntityContainer*>(c);
    return be && be->type == "minecraft:hopper" ? be : nullptr;
}

bool isFullContainer(Container& c, Direction face) {
    for (int slot : c.isWorldly() ? c.slotsForFace(face) : c.Container::slotsForFace(face)) {
        const ItemStack& s = c.item(slot);
        if (s.isEmpty() || s.count < s.maxStackSize()) return false;
    }
    return true;
}

bool inventoryFull(Container& hopper) {
    for (int i = 0; i < hopper.size(); i++) {
        const ItemStack& s = hopper.item(i);
        if (s.isEmpty() || s.count != s.maxStackSize()) return false;
    }
    return true;
}

bool canMergeItems(const ItemStack& a, const ItemStack& b) { return a.count <= a.maxStackSize() && ItemStack::isSameItemSameComponents(a, b); }

ItemStack tryMoveInItem(ServerLevel& level, Container* from, Container& to, ItemStack stack, int slot, std::optional<Direction> face) {
    ItemStack& current = to.item(slot);
    if (!to.canPlaceItem(slot, stack) || (to.isWorldly() && !to.canPlaceItemThroughFace(slot, stack, face))) return stack;
    bool success = false;
    bool wasEmpty = to.isEmpty();
    if (current.isEmpty()) {
        to.setItem(slot, stack);
        stack = {};
        success = true;
    } else if (canMergeItems(current, stack)) {
        int space = stack.maxStackSize() - current.count;
        int count = std::min(stack.count, space);
        stack.shrink(count);
        current.count += count;
        success = count > 0;
    }
    if (success) {
        if (const BlockEntityContainer* toHopper = asHopper(&to); wasEmpty && toHopper && cooldown(level, toHopper->pos) <= 8) {
            int skip = 0;
            if (const BlockEntityContainer* fromHopper = asHopper(from)) {
                int64_t toTicked = level.hopperTickedGameTime[toHopper->pos.asLong()], fromTicked = level.hopperTickedGameTime[fromHopper->pos.asLong()];
                if (toTicked >= fromTicked) skip = 1;
            }
            setCooldown(level, toHopper->pos, 8 - skip);
        }
        to.setChanged();
    }
    return stack;
}

bool addItemEntity(ServerLevel& level, Container& hopper, ItemEntity& entity) { // addItem(container, entity)
    ItemStack result = addItem(level, nullptr, hopper, entity.item, std::nullopt);
    if (result.isEmpty()) {
        entity.item = {};
        entity.removed = true; // discard
        return true;
    }
    entity.item = result;
    entity.dataChanged = true;
    return false;
}

bool ejectItems(ServerLevel& level, BlockPos pos, BlockStateId state, Container& self) {
    Direction facing = direction::byName(bs::getValue(state, "facing").value_or("down"));
    auto target = getContainerAt(level, pos.relative(facing));
    if (!target) return false;
    Direction face = direction::opposite(facing);
    if (isFullContainer(*target, face)) return false;
    for (int slot = 0; slot < self.size(); slot++) {
        if (self.item(slot).isEmpty()) continue;
        ItemStack original = self.item(slot);
        ItemStack one = self.item(slot).split(1); // HopperBlockEntity.removeItem (no setChanged)
        ItemStack result = addItem(level, &self, *target, one, face);
        if (result.isEmpty()) {
            self.item(slot).clearIfEmpty();
            target->setChanged();
            return true;
        }
        self.item(slot) = original;
    }
    return false;
}

bool tryTakeInItemFromSlot(ServerLevel& level, Container& hopper, Container& from, int slot, Direction face) {
    ItemStack stack = from.item(slot);
    if (stack.isEmpty() || !from.canTakeItem(hopper, slot, stack) || (from.isWorldly() && !from.canTakeItemThroughFace(slot, stack, face))) return false;
    ItemStack original = stack;
    ItemStack result = addItem(level, &from, hopper, from.removeItem(slot, 1), std::nullopt);
    if (result.isEmpty()) {
        from.setChanged();
        return true;
    }
    from.item(slot) = original;
    from.setChanged();
    return false;
}

std::vector<ItemEntity*> itemsAtAndAbove(ServerLevel& level, BlockPos pos) { // getItemsAtAndAbove (Hopper.SUCK_AABB)
    AABB suck{static_cast<double>(pos.x), pos.y + 11.0 / 16.0, static_cast<double>(pos.z), pos.x + 1.0, pos.y + 2.0, pos.z + 1.0};
    std::vector<ItemEntity*> out;
    for (ItemEntity* e : level.itemsIn(suck))
        if (!e->item.isEmpty()) out.push_back(e);
    return out;
}

bool suckInItems(ServerLevel& level, BlockPos pos, Container& hopper) {
    BlockPos above = pos.above();
    BlockStateId aboveState = level.getBlockState(above);
    if (auto source = getContainerAt(level, above)) {
        for (int slot : source->isWorldly() ? source->slotsForFace(Direction::Down) : source->Container::slotsForFace(Direction::Down))
            if (tryTakeInItemFromSlot(level, hopper, *source, slot, Direction::Down)) return true;
        return false;
    }
    bool blocked = bs::isCollisionFullBlock(aboveState) && !bs::stateHasTag(aboveState, "minecraft:does_not_block_hoppers");
    if (blocked) return false;
    for (ItemEntity* e : itemsAtAndAbove(level, pos))
        if (addItemEntity(level, hopper, *e)) return true;
    return false;
}

template <class Action>
bool tryMoveItems(ServerLevel& level, BlockPos pos, BlockStateId state, Container& hopper, Action action) {
    if (cooldown(level, pos) > 0 || !bs::getBool(state, "enabled")) return false;
    bool changed = false;
    if (!hopper.isEmpty()) changed = ejectItems(level, pos, state, hopper);
    if (!inventoryFull(hopper)) changed |= action();
    if (!changed) return false;
    setCooldown(level, pos, 8);
    hopper.setChanged(); // (writes the items back; BlockEntity.setChanged)
    return true;
}

} // namespace

std::shared_ptr<Container> getContainerAt(ServerLevel& level, BlockPos pos) {
    BlockStateId state = level.getBlockState(pos);
    uint32_t block = bs::blockOf(state);
    if (isA(block, "ComposterBlock")) { // ComposterBlock.getContainer
        int fill = std::stoi(std::string(bs::getValue(state, "level").value_or("0")));
        if (fill == 8) return std::make_shared<ComposterOutput>(level, pos, state);
        if (fill < 7) return std::make_shared<ComposterInput>(level, pos, state);
        return std::make_shared<ComposterFull>();
    }
    auto self = level.blockEntityContainer(pos);
    if (!self || self->size() == 0) return nullptr; // (ender chests aren't containers)
    if (isA(block, "ChestBlock")) { // ChestBlock.getContainer(..., ignoreBlocked = true): both halves
        std::string_view type = bs::getValue(state, "type").value_or("single");
        if (type != "single") {
            Direction facing = direction::byName(bs::getValue(state, "facing").value_or("north"));
            BlockPos otherPos = pos.relative(type == "left" ? direction::clockWise(facing) : direction::counterClockWise(facing));
            BlockStateId os = level.getBlockState(otherPos);
            std::string_view otherType = bs::getValue(os, "type").value_or("single");
            if (bs::blockOf(os) == block && otherType != "single" && otherType != type && bs::getValue(os, "facing") == bs::getValue(state, "facing"))
                if (auto other = level.blockEntityContainer(otherPos)) {
                    bool right = type == "right"; // DoubleBlockCombiner: the first half comes first
                    return std::make_shared<CompoundContainer>(right ? self : other, right ? other : self);
                }
        }
    }
    return self;
}

ItemStack addItem(ServerLevel& level, Container* from, Container& to, ItemStack stack, std::optional<Direction> face) {
    if (to.isWorldly() && face) {
        for (int slot : to.slotsForFace(*face)) {
            if (stack.isEmpty()) break;
            stack = tryMoveInItem(level, from, to, stack, slot, face);
        }
    } else {
        for (int i = 0; i < to.size() && !stack.isEmpty(); i++) stack = tryMoveInItem(level, from, to, stack, i, face);
    }
    return stack;
}

void pushItemsTick(ServerLevel& level, BlockPos pos, BlockStateId state) {
    auto hopper = level.blockEntityContainer(pos);
    if (!hopper) return;
    setCooldown(level, pos, cooldown(level, pos) - 1);
    level.hopperTickedGameTime[pos.asLong()] = level.gameTime();
    if (cooldown(level, pos) > 0) return;
    setCooldown(level, pos, 0);
    tryMoveItems(level, pos, state, *hopper, [&] { return suckInItems(level, pos, *hopper); });
}

void entityInside(ServerLevel& level, BlockPos pos, BlockStateId state, ItemEntity& entity) {
    if (entity.item.isEmpty() || entity.removed) return;
    AABB box = entity.boundingBox().move(-pos.x, -pos.y, -pos.z);
    AABB suck{0.0, 11.0 / 16.0, 0.0, 1.0, 2.0, 1.0};
    if (!box.intersects(suck)) return;
    auto hopper = level.blockEntityContainer(pos);
    if (!hopper) return;
    tryMoveItems(level, pos, state, *hopper, [&] { return addItemEntity(level, *hopper, entity); });
}

} // namespace mc::hopper
