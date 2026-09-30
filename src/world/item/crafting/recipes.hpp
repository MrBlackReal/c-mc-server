#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "world/item/item_stack.hpp"

namespace mc {
class ServerLevel;
}

// Port of the recipe system (RecipeManager, CraftingInput, the recipe types), interpreting the
// recipe JSON from the jar (data/recipes.json).
namespace mc::recipes {

// CraftingInput: a crafting grid trimmed to its non-empty rows and columns.
struct CraftingInput {
    int width = 0, height = 0;
    std::vector<ItemStack> items;
    int ingredientCount = 0; // non-empty items
    const ItemStack& at(int x, int y) const { return items[x + y * width]; }
};
// CraftingInput.Positioned: the trimmed input and where it starts in the grid.
struct PositionedInput {
    CraftingInput input;
    int left = 0, top = 0;
};
PositionedInput positioned(int width, int height, const std::vector<ItemStack>& grid);

// RecipeManager.getRecipeFor(RecipeType.CRAFTING, input): the first matching crafting recipe's id.
std::optional<std::string> findCrafting(const CraftingInput& input);
// Recipe.assemble and CraftingRecipe.getRemainingItems for recipe `id` (null: none matched,
// so every input item stays except for crafting remainders).
ItemStack assemble(const std::string& id, const CraftingInput& input);
std::vector<ItemStack> remainingItems(const std::string* id, const CraftingInput& input);

// AbstractCookingRecipe (smelting, blasting, smoking, campfire_cooking) for one input item.
struct Cooking {
    std::string id;
    ItemStack result;
    float experience = 0;
    int cookingTime = 200;
};
std::optional<Cooking> findCooking(std::string_view type, const ItemStack& input);
// A cooking recipe's experience by id (0 if unknown).
float cookingExperience(std::string_view id);

// StonecutterRecipe, in the order clients get them (ClientboundUpdateRecipesPacket); a stonecutter's
// buttons index the recipes that accept its input, in this order.
struct Stonecutting {
    std::string id;
    ItemStack result;
};
std::vector<Stonecutting> stonecuttingFor(const ItemStack& input);
// SelectableRecipe.SingleInputSet.acceptsInput: some stonecutter recipe takes this item.
bool acceptsStonecutting(const ItemStack& input);

// SmithingTransformRecipe / SmithingTrimRecipe: the first matching recipe, and its result.
std::optional<std::string> findSmithing(const ItemStack& templ, const ItemStack& base, const ItemStack& addition);
ItemStack assembleSmithing(const std::string& id, const ItemStack& base, const ItemStack& addition);

// RecipePropertySet.test for the smithing and brewing sets ("minecraft:smithing_base", ...).
bool inPropertySet(std::string_view set, const ItemStack& stack);

// BrewingRecipe: what brewing `input` with `reagent` makes (the first matching recipe).
std::optional<ItemStack> findBrewing(const ItemStack& input, const ItemStack& reagent);

// ClientboundUpdateRecipesPacket: the recipe property sets (furnace inputs, smithing and brewing
// ingredients) and the stonecutter recipes.
void writeUpdateRecipes(ByteWriter& out);

void bootstrap();

} // namespace mc::recipes
