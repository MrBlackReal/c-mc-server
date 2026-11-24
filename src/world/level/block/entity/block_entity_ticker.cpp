#include "world/level/block/entity/block_entity_ticker.hpp"
#include "world/level/block/redstone_blocks.hpp"

#include <cmath>
#include <random>
#include <unordered_map>

#include "core/registries.hpp"
#include "generated/behaviour.hpp"
#include "server/level/server_level.hpp"
#include "server/level/server_player.hpp"
#include "util/json.hpp"
#include "util/log.hpp"
#include "world/entity/item_entity.hpp"
#include "world/item/item_logic.hpp"
#include "world/entity/experience_orb.hpp"
#include "world/item/crafting/recipes.hpp"
#include "world/item/item_nbt.hpp"
#include "world/level/block/entity/block_entity_container.hpp"
#include "world/level/storage/loot/loot_tables.hpp"

namespace mc::block_entity {

namespace {

namespace bs = blocks;

// data/jukebox_songs.json: minecraft:jukebox_song definitions by id.
const json::Value& songs() {
    static const json::Value v = [] {
        try {
            return json::parse(gen::kJukeboxSongsJson);
        } catch (const std::exception& e) {
            log::error("Failed to parse jukebox songs: {}", e.what());
            return json::Value{};
        }
    }();
    return v;
}

// JukeboxSong.lengthInTicks
int64_t lengthInTicks(const std::string& song) {
    return static_cast<int64_t>(std::ceil(static_cast<float>(songs()[song]["length_in_seconds"].number()) * 20.0f));
}

std::string recordSong(const nbt::Compound& be) {
    const nbt::Compound* record = be.getCompound("RecordItem");
    auto stack = record ? item_nbt::read(*record) : std::nullopt;
    return stack ? jukebox::songOf(*stack) : std::string();
}

// JukeboxBlockEntity.onSongChanged: redstone around it hears about it (and comparators via setChanged).
void onSongChanged(ServerLevel& level, BlockPos pos) {
    level.updateNeighborsAt(pos, bs::blockOf(level.getBlockState(pos)));
    level.blockEntitySetChanged(pos);
}

// JukeboxSongPlayer.stop
void stopSong(ServerLevel& level, BlockPos pos, nbt::Compound& be) {
    if (!be.contains("ticks_since_song_started")) return;
    be.remove("ticks_since_song_started");
    level.levelEvent(1011, pos, 0);
    onSongChanged(level, pos);
}

// JukeboxSongPlayer.tick
bool tickJukebox(ServerLevel& level, BlockPos pos, nbt::Compound& be) {
    if (!be.contains("ticks_since_song_started")) return false;
    std::string song = recordSong(be);
    int64_t ticks = be.getLong("ticks_since_song_started");
    if (song.empty() || ticks >= lengthInTicks(song) + 20) { // JukeboxSong.hasFinished (with 20 ticks of padding)
        stopSong(level, pos, be);
        return true;
    }
    if (ticks % 20 == 0) { // spawnMusicParticles
        float color = static_cast<float>(std::uniform_int_distribution<int>(0, 3)(level.random())) / 24.0f;
        level.sendParticles("minecraft:note", pos.x + 0.5, pos.y + 1.2000000476837158, pos.z + 0.5, 0, color, 0, 0, 1);
    }
    be.put("ticks_since_song_started", ticks + 1);
    return true;
}

// A fuel's minecraft:cooking_fuel provider (burn_time or speed_multiplier), patched or default.
std::string fuelProvider(const ItemStack& fuel, std::string_view field) {
    if (auto patched = items::componentValue(fuel, "minecraft:cooking_fuel"); patched && patched->asCompound()) {
        const nbt::Tag* v = patched->asCompound()->get(field);
        if (v && v->asString()) return *v->asString();
        if (v && v->isNumeric()) return std::to_string(v->asDouble());
    }
    return std::string(field == "burn_time" ? fuel.info().fuelBurnTime : fuel.info().fuelSpeed);
}

// AbstractFurnaceBlockEntity.serverTick
bool tickFurnace(ServerLevel& level, BlockPos pos, BlockStateId state, nbt::Compound& be) {
    auto container = level.blockEntityContainer(pos);
    if (!container || container->size() < 3) return false;
    std::string type = be.getString("id");
    std::string_view cookingType = BlockEntityContainer::cookingType(type);
    int litRemaining = be.getInt("lit_time_remaining"), litTotal = be.getInt("lit_total_time");
    int timer = be.getInt("cooking_time_spent"), total = be.getInt("cooking_total_time");
    float speed = static_cast<float>(be.getDouble("speed_multiplier", 1.0));
    bool changed = false, itemsChanged = false;
    bool wasLit = litRemaining > 0, isLit = false;
    if (wasLit) isLit = --litRemaining > 0;

    ItemStack& ingredient = container->item(0);
    ItemStack& fuel = container->item(1);
    ItemStack& output = container->item(2);
    loot::BlockContext ctx{level, level.random(), state, pos, nullptr, nullptr, &be};
    auto totalCookTime = [&](const recipes::Cooking& r) {
        return speed > 0.0f ? static_cast<int>(std::ceil(r.cookingTime / speed)) : r.cookingTime;
    };
    if (isLit || (!fuel.isEmpty() && !ingredient.isEmpty())) {
        if (!ingredient.isEmpty()) {
            auto recipe = recipes::findCooking(cookingType, ingredient);
            if (recipe) {
                const ItemStack& result = recipe->result;
                // canBurn: the output slot takes it
                bool canBurn = !result.isEmpty() &&
                               (output.isEmpty() || (ItemStack::isSameItemSameComponents(output, result) &&
                                                     output.count + result.count <= std::min(container->maxStackSize(), result.maxStackSize())));
                if (canBurn) {
                    if (!isLit) {
                        int newLit = fuel.isEmpty() ? 0 : loot::contextInt(fuelProvider(fuel, "burn_time"), ctx);
                        float newSpeed = fuel.isEmpty() || fuelProvider(fuel, "speed_multiplier").empty()
                                             ? 1.0f : loot::contextFloat(fuelProvider(fuel, "speed_multiplier"), ctx);
                        litRemaining = litTotal = newLit;
                        speed = newSpeed;
                        if (total > 0 && timer < total) {
                            float ratio = static_cast<float>(timer) / total;
                            total = totalCookTime(*recipe);
                            timer = static_cast<int>(std::ceil(ratio * total));
                        }
                        if (newLit > 0) { // consumeFuel
                            int32_t remainder = fuel.info().craftRemainder;
                            fuel.shrink(1);
                            if (remainder > 0) {
                                if (fuel.isEmpty()) fuel = ItemStack(remainder, 1);
                                else level.popResource(pos, ItemStack(remainder, 1)); // (Containers.dropItemStack)
                            }
                            fuel.clearIfEmpty();
                            isLit = changed = itemsChanged = true;
                        }
                    }
                    if (isLit) {
                        if (++timer >= total) {
                            timer = 0;
                            total = totalCookTime(*recipe);
                            // burn
                            if (output.isEmpty()) output = result;
                            else output.grow(result.count);
                            static const int32_t kWetSponge = items::byName("minecraft:wet_sponge").value_or(-1);
                            static const int32_t kBucket = items::byName("minecraft:bucket").value_or(-1);
                            if (ingredient.item == kWetSponge && !fuel.isEmpty() && fuel.item == kBucket)
                                fuel = ItemStack(items::byName("minecraft:water_bucket").value_or(0), 1);
                            ingredient.shrink(1);
                            ingredient.clearIfEmpty();
                            // setRecipeUsed
                            nbt::Compound used = be.getCompound("RecipesUsed") ? *be.getCompound("RecipesUsed") : nbt::Compound{};
                            used.put(recipe->id, used.getInt(recipe->id) + 1);
                            be.put("RecipesUsed", std::move(used));
                            changed = itemsChanged = true;
                        }
                    } else {
                        timer = 0;
                    }
                } else {
                    timer = 0;
                }
            }
        } else {
            timer = 0;
        }
    } else if (timer > 0) {
        timer = std::clamp(timer - 2, 0, total);
    }
    be.put("lit_time_remaining", litRemaining);
    be.put("lit_total_time", litTotal);
    be.put("cooking_time_spent", timer);
    be.put("cooking_total_time", total);
    be.put("speed_multiplier", speed);
    if (wasLit != isLit) level.setBlock(pos, bs::setBool(state, "lit", isLit));
    if (itemsChanged) container->setChanged();
    (void)changed;
    return true;
}

// BrewingStandBlockEntity.ingredient: the reagent a brew started with (transient; after a reload
// it's whatever is in the ingredient slot, as in loadAdditional).
std::unordered_map<const ServerLevel*, std::unordered_map<int64_t, int32_t>> g_brewIngredients;

// BrewingStandBlockEntity.serverTick
bool tickBrewingStand(ServerLevel& level, BlockPos pos, BlockStateId state, nbt::Compound& be) {
    auto container = level.blockEntityContainer(pos);
    if (!container || container->size() < 5) return false;
    int fuel = be.getInt("Fuel"), brewTime = be.getInt("BrewTime");
    int totalFuel = be.getInt("total_fuel", 20), totalBrewTime = be.getInt("total_brew_time", 400);
    float speed = static_cast<float>(be.getDouble("speed_multiplier", 1.0));
    bool changed = false;
    loot::BlockContext ctx{level, level.random(), state, pos, nullptr, nullptr, &be};
    ItemStack& fuelStack = container->item(4);
    auto brewingFuel = fuelStack.isEmpty() ? std::nullopt : items::effectiveComponent(fuelStack, "minecraft:brewing_fuel");
    if (fuel <= 0 && brewingFuel && brewingFuel->asCompound()) {
        auto provider = [&](const char* key) -> std::string {
            const nbt::Tag* v = brewingFuel->asCompound()->get(key);
            if (v && v->asString()) return *v->asString();
            if (v && v->isNumeric()) return std::to_string(v->asDouble());
            return {};
        };
        std::string uses = provider("uses"), speedProvider = provider("speed_multiplier");
        fuel = uses.empty() ? 0 : loot::contextInt(uses, ctx);
        totalFuel = fuel;
        speed = speedProvider.empty() ? 1.0f : loot::contextFloat(speedProvider, ctx);
        int32_t remainder = fuelStack.info().craftRemainder;
        fuelStack.shrink(1);
        if (remainder > 0) {
            if (fuelStack.isEmpty()) fuelStack = ItemStack(remainder, 1);
            else level.popResource(pos, ItemStack(remainder, 1)); // (Containers.dropItemStack)
        }
        fuelStack.clearIfEmpty();
        container->setChanged();
        changed = true;
    }
    // isBrewable
    ItemStack& ingredient = container->item(3);
    auto brewable = [&] {
        if (ingredient.isEmpty() || !recipes::inPropertySet("minecraft:brewing_reagent", ingredient)) return false;
        for (int i = 0; i < 3; i++)
            if (!container->item(i).isEmpty() && recipes::findBrewing(container->item(i), ingredient)) return true;
        return false;
    };
    bool canBrew = brewable();
    auto& started = g_brewIngredients[&level];
    int64_t key = pos.asLong();
    if (brewTime > 0) {
        brewTime--;
        int32_t startedWith = started.contains(key) ? started[key] : ingredient.item;
        if (brewTime == 0 && canBrew) {
            // doBrew
            for (int i = 0; i < 3; i++)
                if (auto out = recipes::findBrewing(container->item(i), ingredient)) container->item(i) = *out;
            int32_t remainder = ingredient.info().craftRemainder;
            ingredient.shrink(1);
            if (remainder > 0) {
                if (ingredient.isEmpty()) ingredient = ItemStack(remainder, 1);
                else level.popResource(pos, ItemStack(remainder, 1));
            }
            ingredient.clearIfEmpty();
            container->setChanged();
            level.levelEvent(1035, pos, 0);
        } else if (!canBrew || ingredient.isEmpty() || ingredient.item != startedWith) {
            brewTime = 0;
        }
        if (brewTime == 0) started.erase(key);
        changed = true;
    } else if (canBrew && fuel > 0) {
        float multiplier = speed > 0.0f ? speed : 1.0f;
        fuel--;
        brewTime = static_cast<int>(std::ceil(400.0f / multiplier));
        totalBrewTime = brewTime;
        started[key] = ingredient.item;
        changed = true;
    }
    be.put("BrewTime", brewTime);
    be.put("total_brew_time", totalBrewTime);
    be.put("Fuel", fuel);
    be.put("total_fuel", totalFuel);
    be.put("speed_multiplier", speed);
    // the bottles on the stand
    BlockStateId shown = state;
    for (int i = 0; i < 3; i++) {
        std::string prop = "has_bottle_" + std::to_string(i);
        if (bs::hasProperty(shown, prop)) shown = bs::setBool(shown, prop, !container->item(i).isEmpty());
    }
    if (shown != state) level.setBlock(pos, shown, ServerLevel::kUpdateClients);
    return changed;
}

// BeaconBlockEntity's transient state: how far up the beam has been checked, the beam sections
// found so far and last time (only whether there are any matters on the server).
struct BeaconState {
    int lastCheckY = INT32_MIN;
    int checkingSections = 0;
    bool beam = false;
};
std::unordered_map<const ServerLevel*, std::unordered_map<int64_t, BeaconState>> g_beacons;

bool isBeaconBeamBlock(BlockStateId state) {
    uint32_t b = bs::blockOf(state);
    return bs::isInstance(b, "BeaconBlock") || bs::isInstance(b, "StainedGlassBlock") || bs::isInstance(b, "StainedGlassPaneBlock");
}

// BeaconBlockEntity.updateBase: how many complete pyramid layers are below.
int beaconLevels(ServerLevel& level, int x, int y, int z) {
    int levels = 0;
    for (int step = 1; step <= 4; levels = step++) {
        int ly = y - step;
        if (ly < level.minY()) break;
        for (int lx = x - step; lx <= x + step; lx++)
            for (int lz = z - step; lz <= z + step; lz++)
                if (!bs::stateHasTag(level.getBlockState(lx, ly, lz), "minecraft:beacon_base_blocks")) return levels;
    }
    return levels;
}

// BeaconBlockEntity.tick
bool tickBeacon(ServerLevel& level, BlockPos pos, nbt::Compound& be) {
    BeaconState& st = g_beacons[&level][pos.asLong()];
    int x = pos.x, y = pos.y, z = pos.z;
    int checkY;
    if (st.lastCheckY < y) {
        checkY = y;
        st.checkingSections = 0;
        st.lastCheckY = y - 1;
    } else {
        checkY = st.lastCheckY + 1;
    }
    LevelChunk& chunk = level.getChunk({x >> 4, z >> 4});
    int lastSetBlock = chunk.height(HeightmapType::WorldSurface, x & 15, z & 15);
    for (int i = 0; i < 10 && checkY <= lastSetBlock; i++) {
        BlockStateId state = level.getBlockState(x, checkY, z);
        if (isBeaconBeamBlock(state)) {
            if (st.checkingSections <= 1) st.checkingSections++;
            // (further colours start or extend sections: there is at least one either way)
        } else if (st.checkingSections == 0 || (bs::lightDampening(state) >= 15 && bs::blockOf(state) != gen::block::BEDROCK)) {
            st.checkingSections = 0;
            st.lastCheckY = lastSetBlock;
            break;
        }
        checkY++;
        st.lastCheckY++;
    }
    int levels = be.getInt("Levels");
    int previousLevels = levels;
    bool changed = false;
    if (level.gameTime() % 80 == 0) {
        if (st.beam) levels = beaconLevels(level, x, y, z);
        if (levels > 0 && st.beam) {
            // applyEffects
            auto effectId = [&](const char* key) -> std::optional<int> {
                if (!be.get(key)) return std::nullopt;
                return registries::id("minecraft:mob_effect", be.getString(key));
            };
            std::optional<int> primary = effectId("primary_effect"), secondary = effectId("secondary_effect");
            if (primary) {
                double range = levels * 10 + 10;
                int amplifier = levels >= 4 && primary == secondary ? 1 : 0;
                int duration = (9 + levels * 2) * 20;
                AABB box{x - range, y - range, z - range, x + 1 + range, y + 1 + range + level.info().height, z + 1 + range};
                for (ServerPlayer* p : level.players()) {
                    if (!p->boundingBox().intersects(box)) continue;
                    p->addEffect(*primary, {amplifier, duration, true, true, true});
                    if (levels >= 4 && secondary && primary != secondary) p->addEffect(*secondary, {0, duration, true, true, true});
                }
            }
            level.playBlockSound(-1, pos, registries::id("minecraft:sound_event", "minecraft:block.beacon.ambient").value_or(-1), 1.0f, 1.0f);
        }
    }
    if (st.lastCheckY >= lastSetBlock) {
        st.lastCheckY = level.minY() - 1;
        bool wasActive = previousLevels > 0;
        st.beam = st.checkingSections > 0;
        bool isActive = levels > 0;
        const char* sound = !wasActive && isActive ? "minecraft:block.beacon.activate" : wasActive && !isActive ? "minecraft:block.beacon.deactivate" : nullptr;
        if (sound) level.playBlockSound(-1, pos, registries::id("minecraft:sound_event", sound).value_or(-1), 1.0f, 1.0f);
    }
    if (levels != previousLevels) {
        be.put("Levels", levels);
        changed = true;
    }
    return changed;
}

} // namespace

void forgetBeacon(ServerLevel& level, BlockPos pos) { g_beacons[&level].erase(pos.asLong()); }

bool hasTicker(std::string_view type) {
    return type == "minecraft:jukebox" || type == "minecraft:brewing_stand" || type == "minecraft:beacon" || BlockEntityContainer::isFurnace(type) ||
           type == "minecraft:hopper" || type == "minecraft:daylight_detector";
}

bool tick(ServerLevel& level, BlockPos pos, BlockStateId state, nbt::Compound& be) {
    std::string type = be.getString("id");
    if (type == "minecraft:jukebox") return tickJukebox(level, pos, be);
    if (BlockEntityContainer::isFurnace(type)) return tickFurnace(level, pos, state, be);
    if (type == "minecraft:brewing_stand") return tickBrewingStand(level, pos, state, be);
    if (type == "minecraft:beacon") return tickBeacon(level, pos, be);
    if (type == "minecraft:daylight_detector") { // DaylightDetectorBlock.tickEntity
        if (level.gameTime() % 20 == 0) block_behaviour::redstone_blocks::daylightUpdate(level, pos, state);
        return false;
    }
    return false;
}

void awardFurnaceExperience(ServerLevel& level, nbt::Compound& be, double x, double y, double z) {
    // AbstractFurnaceBlockEntity.getRecipesToAwardAndPopExperience / createExperience
    const nbt::Compound* used = be.getCompound("RecipesUsed");
    if (!used) return;
    for (const auto& [id, count] : used->entries) {
        float experience = recipes::cookingExperience(id);
        float amount = static_cast<float>(count.asLong()) * experience;
        int reward = static_cast<int>(std::floor(amount));
        float fraction = amount - std::floor(amount);
        if (fraction != 0.0f && std::uniform_real_distribution<float>(0.0f, 1.0f)(level.random()) < fraction) reward++;
        ExperienceOrb::award(level, x, y, z, reward);
    }
    be.put("RecipesUsed", nbt::Compound{});
}

namespace jukebox {

std::string songOf(const ItemStack& stack) {
    if (stack.isEmpty()) return {};
    if (auto patched = items::componentValue(stack, "minecraft:jukebox_playable"); patched && patched->asString()) return *patched->asString();
    return std::string(stack.info().jukeboxSong);
}

void insert(ServerLevel& level, BlockPos pos, BlockStateId state, nbt::Compound& be, const ItemStack& disc) {
    be.put("RecordItem", item_nbt::write(disc));
    level.setBlock(pos, bs::setBool(state, "has_record", true), ServerLevel::kUpdateClients); // notifyItemChangedInJukebox
    std::string song = songOf(disc);
    if (song.empty()) return;
    be.put("ticks_since_song_started", int64_t{0}); // JukeboxSongPlayer.play
    level.levelEvent(1010, pos, registries::id("minecraft:jukebox_song", song).value_or(0));
    onSongChanged(level, pos);
}

int comparatorOutput(const ItemStack& stack) {
    std::string song = songOf(stack);
    if (song.empty()) return 0;
    const json::Value& def = songs()[song];
    return def.isObject() ? static_cast<int>(def["comparator_output"].number()) : 0;
}

void popOut(ServerLevel& level, BlockPos pos, nbt::Compound& be) {
    const nbt::Compound* record = be.getCompound("RecordItem");
    auto disc = record ? item_nbt::read(*record) : std::nullopt;
    if (!disc || disc->isEmpty()) return;
    be.remove("RecordItem"); // removeTheItem -> setTheItem(EMPTY)
    BlockStateId state = level.getBlockState(pos);
    if (bs::isInstance(bs::blockOf(state), "JukeboxBlock")) level.setBlock(pos, bs::setBool(state, "has_record", false), ServerLevel::kUpdateClients);
    stopSong(level, pos, be);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    double x = pos.x + 0.5 + (u(level.random()) * 2.0f - 1.0f) * 0.7f, z = pos.z + 0.5 + (u(level.random()) * 2.0f - 1.0f) * 0.7f;
    if (ItemEntity* e = ItemEntity::spawn(level, x, pos.y + 1.01, z, std::move(*disc), level.random())) e->pickupDelay = ItemEntity::kDefaultPickupDelay;
    onSongChanged(level, pos);
}

} // namespace jukebox

} // namespace mc::block_entity
