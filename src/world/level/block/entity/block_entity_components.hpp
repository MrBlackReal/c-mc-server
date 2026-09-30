#pragma once

#include "core/direction.hpp"
#include "nbt/tag.hpp"
#include "world/item/item_stack.hpp"

// Block entities are kept as their saved NBT. This ports the
// part of BlockEntity that moves data between a block entity and its item, working on the NBT:
// collectImplicitComponents (breaking: loot tables' copy_components) and applyImplicitComponents
// (placing: BlockItem.updateBlockEntityComponents / updateCustomBlockEntityTag).
namespace mc::block_entity {

// BlockEntity.collectComponents: {component name: saved value} for the block entity `be` (whose
// "id" says its type), including the leftover components it stored under "components".
nbt::Compound collectComponents(const nbt::Compound& be);

// Applies `stack`'s block_entity_data (unless the type is op-only and `allowOpData` is false) and
// its components to `be`, like placing the item does. Unused components go to "components".
void applyItem(nbt::Compound& be, const ItemStack& stack, bool allowOpData);

// BlockEntity.getUpdateTag from the saved NBT: what clients get about a block entity (signs'
// text, banner patterns, ...). Empty for most types - containers never reveal their contents.
nbt::Compound getUpdateTag(const nbt::Compound& be);
// Whether the type overrides getUpdatePacket, i.e. clients get ClientboundBlockEntityDataPacket
// when it changes.
bool sendsUpdates(std::string_view type);

// A fresh block entity for `block` at `pos` (LevelChunk.setBlockState -> EntityBlock.newBlockEntity),
// as vanilla saves it: the type's defaults plus id and position.
nbt::Compound createDefault(uint32_t block, BlockPos pos);
// SignText.EMPTY as saved.
nbt::Compound emptySignText();

} // namespace mc::block_entity
