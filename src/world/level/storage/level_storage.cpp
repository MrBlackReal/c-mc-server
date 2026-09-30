#include "world/level/storage/level_storage.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstring>

#include "server/minecraft_server.hpp"
#include "util/log.hpp"
#include "world/level/chunk/storage/chunk_serializer.hpp"

namespace mc {
namespace fs = std::filesystem;

namespace {
constexpr int kNbtVersion = 19133; // level.dat "version" (NBT format version)

int64_t nowMillis() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

nbt::Compound wrapSavedData(nbt::Compound data) {
    nbt::Compound root;
    root.put("data", std::move(data));
    root.put("DataVersion", kDataVersion);
    return root;
}

nbt::Compound versionTag() {
    nbt::Compound v;
    v.put("Id", kDataVersion);
    v.put("Name", std::string(kGameVersion));
    v.put("Series", std::string("main"));
    v.put("Snapshot", nbt::byteTag(false));
    return v;
}

nbt::Compound noiseDimension(const char* type, const char* settings, nbt::Compound biomeSource) {
    nbt::Compound gen;
    gen.put("type", std::string("minecraft:noise"));
    gen.put("settings", std::string(settings));
    gen.put("biome_source", std::move(biomeSource));
    nbt::Compound dim;
    dim.put("type", std::string(type));
    dim.put("generator", std::move(gen));
    return dim;
}
} // namespace

std::unique_ptr<LevelStorage> LevelStorage::open(const fs::path& dir, std::string& error) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    auto storage = std::unique_ptr<LevelStorage>(new LevelStorage(dir));
    // DirectoryLock: write "☃" and take an exclusive lock (Java's FileChannel.tryLock is a POSIX lock).
    fs::path lockPath = dir / "session.lock";
    int fd = ::open(lockPath.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        error = "can't open " + lockPath.string() + ": " + std::strerror(errno);
        return nullptr;
    }
    struct flock fl{};
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    if (fcntl(fd, F_SETLK, &fl) != 0) {
        ::close(fd);
        error = lockPath.string() + ": already locked (is another server using this world?)";
        return nullptr;
    }
    const char snowman[] = "\xE2\x98\x83";
    if (::pwrite(fd, snowman, 3, 0) != 3) log::warn("Failed to write {}", lockPath.string());
    storage->lockFd_ = fd;
    return storage;
}

LevelStorage::~LevelStorage() {
    if (lockFd_ >= 0) ::close(lockFd_);
}

bool LevelStorage::exists() const {
    std::error_code ec;
    return fs::exists(dir_ / "level.dat", ec);
}

fs::path LevelStorage::dimensionPath(std::string_view dimension) const {
    auto colon = dimension.find(':');
    std::string ns = colon == std::string_view::npos ? "minecraft" : std::string(dimension.substr(0, colon));
    std::string path = colon == std::string_view::npos ? std::string(dimension) : std::string(dimension.substr(colon + 1));
    return dir_ / "dimensions" / ns / path;
}

std::optional<std::string> LevelStorage::load(LevelData& level, WorldGen& gen, std::vector<Clock>& clocks, GameRules& rules) {
    auto root = nbt::readCompressedFile(dir_ / "level.dat");
    if (!root) {
        // LevelStorageSource falls back to level.dat_old.
        root = nbt::readCompressedFile(dir_ / "level.dat_old");
        if (!root) return "level.dat is missing or damaged";
        log::warn("level.dat is damaged, using level.dat_old");
    }
    const nbt::Compound* data = root->getCompound("Data");
    if (!data) return "level.dat has no Data";
    int dataVersion = data->getInt("DataVersion", -1);
    if (dataVersion > kDataVersion)
        return "the world was saved by a newer version (data version " + std::to_string(dataVersion) + ")";
    if (dataVersion < kDataVersion) {
        return "the world was saved by an older version (data version " + std::to_string(dataVersion) +
               "). Upgrade it once with the vanilla 26.3 server: java -jar server.jar --forceUpgrade";
    }
    level.raw = *data;
    level.gameTime = data->getLong("Time");
    if (const nbt::Compound* spawn = data->getCompound("spawn")) {
        if (const auto* pos = spawn->getIntArray("pos"); pos && pos->size() == 3)
            level.spawnX = (*pos)[0], level.spawnY = (*pos)[1], level.spawnZ = (*pos)[2];
        level.spawnYaw = static_cast<float>(spawn->getDouble("yaw"));
        level.spawnPitch = static_cast<float>(spawn->getDouble("pitch"));
    }
    if (const nbt::Compound* diff = data->getCompound("difficulty_settings")) {
        level.difficulty = diff->getString("difficulty", "easy");
        level.hardcore = diff->getBool("hardcore");
    }

    auto wg = nbt::readCompressedFile(dataFile("world_gen_settings"));
    const nbt::Compound* wgData = wg ? wg->getCompound("data") : nullptr;
    if (!wgData) return "data/minecraft/world_gen_settings.dat is missing or damaged";
    gen.raw = *wgData;
    gen.seed = wgData->getLong("seed");
    gen.overworldGenerator = "";
    if (const nbt::Compound* dims = wgData->getCompound("dimensions")) {
        if (const nbt::Compound* ow = dims->getCompound("minecraft:overworld")) {
            if (const nbt::Compound* g = ow->getCompound("generator")) {
                gen.overworldGenerator = g->getString("type");
                if (gen.overworldGenerator == "minecraft:flat") {
                    if (const nbt::Compound* settings = g->getCompound("settings")) {
                        gen.biome = settings->getString("biome", "minecraft:plains");
                        if (const nbt::List* layers = settings->getList("layers")) {
                            for (const auto& l : layers->items) {
                                const nbt::Compound* lc = l.asCompound();
                                if (!lc) continue;
                                auto block = blocks::byName(lc->getString("block"));
                                if (!block) return "unknown block in flat layers: " + lc->getString("block");
                                gen.layers.push_back({blocks::defaultState(*block), lc->getInt("height", 1)});
                            }
                        }
                    }
                }
            }
        }
    }

    clocks.clear();
    if (auto c = nbt::readCompressedFile(dataFile("world_clocks"))) {
        if (const nbt::Compound* cd = c->getCompound("data")) {
            for (const auto& [name, v] : cd->entries) {
                const nbt::Compound* st = v.asCompound();
                if (!st) continue;
                clocks.push_back({name, st->getLong("total_ticks"), static_cast<float>(st->getDouble("partial_tick")),
                                  static_cast<float>(st->getDouble("rate", 1.0)), st->getBool("paused")});
            }
        }
    }
    if (auto saved = nbt::readCompressedFile(dataFile("game_rules")))
        if (const nbt::Compound* rd = saved->getCompound("data")) rules.load(*rd);
    return std::nullopt;
}

LevelStorage::WorldGen LevelStorage::defaultFlat(int64_t seed) {
    using namespace gen::block;
    WorldGen g;
    g.seed = seed;
    g.layers = {{blocks::defaultState(BEDROCK), 1}, {blocks::defaultState(DIRT), 2}, {blocks::defaultState(GRASS_BLOCK), 1}};
    // The same world_gen_settings vanilla writes for level-type=minecraft:flat.
    nbt::List layers;
    for (const auto& [block, height] : {std::pair{"minecraft:bedrock", 1}, {"minecraft:dirt", 2}, {"minecraft:grass_block", 1}}) {
        nbt::Compound l;
        l.put("block", std::string(block));
        l.put("height", height);
        layers.items.push_back(std::move(l));
    }
    nbt::Compound settings;
    settings.put("features", nbt::byteTag(false));
    settings.put("biome", std::string("minecraft:plains"));
    settings.put("layers", std::move(layers));
    settings.put("structure_overrides", nbt::List{nbt::TagType::String, {std::string("minecraft:strongholds"), std::string("minecraft:villages")}});
    settings.put("lakes", nbt::byteTag(false));
    nbt::Compound owGen;
    owGen.put("settings", std::move(settings));
    owGen.put("type", std::string("minecraft:flat"));
    nbt::Compound overworld;
    overworld.put("generator", std::move(owGen));
    overworld.put("type", std::string("minecraft:overworld"));

    nbt::Compound netherSource;
    netherSource.put("preset", std::string("minecraft:nether"));
    netherSource.put("type", std::string("minecraft:multi_noise"));
    nbt::Compound endSource;
    endSource.put("type", std::string("minecraft:the_end"));
    nbt::Compound dims;
    dims.put("minecraft:overworld", std::move(overworld));
    dims.put("minecraft:the_nether", noiseDimension("minecraft:the_nether", "minecraft:nether", std::move(netherSource)));
    dims.put("minecraft:the_end", noiseDimension("minecraft:the_end", "minecraft:end", std::move(endSource)));

    g.raw.put("bonus_chest", nbt::byteTag(false));
    g.raw.put("seed", seed);
    g.raw.put("generate_structures", nbt::byteTag(true));
    g.raw.put("dimensions", std::move(dims));
    return g;
}

void LevelStorage::saveLevel(const LevelData& level) {
    nbt::Compound data = level.raw;
    if (data.empty()) {
        // New world: the fields PrimaryLevelData writes.
        data.put("LevelName", dir_.filename().string());
        data.put("initialized", nbt::byteTag(true));
        data.put("allowCommands", nbt::byteTag(false));
        data.put("GameType", 0);
        nbt::Compound packs;
        packs.put("Enabled", nbt::List{nbt::TagType::String, {std::string("vanilla")}});
        packs.put("Disabled", nbt::List{nbt::TagType::String,
                                        {std::string("minecart_improvements"), std::string("redstone_experiments"), std::string("trade_rebalance")}});
        data.put("DataPacks", std::move(packs));
        data.put("version_history", nbt::List{nbt::TagType::Int, {kDataVersion}});
    }
    data.put("DataVersion", kDataVersion);
    data.put("version", kNbtVersion);
    data.put("Version", versionTag());
    data.put("Time", level.gameTime);
    data.put("LastPlayed", nowMillis());
    nbt::Compound spawn;
    spawn.put("pos", std::vector<int32_t>{level.spawnX, level.spawnY, level.spawnZ});
    spawn.put("yaw", level.spawnYaw);
    spawn.put("pitch", level.spawnPitch);
    spawn.put("dimension", std::string("minecraft:overworld"));
    data.put("spawn", std::move(spawn));
    nbt::Compound diff;
    if (const nbt::Compound* old = data.getCompound("difficulty_settings")) diff = *old;
    diff.put("difficulty", level.difficulty);
    diff.put("hardcore", nbt::byteTag(level.hardcore));
    if (!diff.contains("locked")) diff.put("locked", nbt::byteTag(false));
    data.put("difficulty_settings", std::move(diff));
    // Like vanilla on a modded server: record our brand and mark the world as touched by it.
    nbt::List brands;
    bool haveBrand = false;
    if (const nbt::List* old = data.getList("ServerBrands")) {
        brands = *old;
        for (const auto& b : old->items)
            if (b.asString() && *b.asString() == kServerBrand) haveBrand = true;
    }
    if (!haveBrand) brands.items.push_back(std::string(kServerBrand));
    data.put("ServerBrands", std::move(brands));
    data.put("WasModded", nbt::byteTag(true));

    nbt::Compound root;
    root.put("Data", std::move(data));
    nbt::writeCompressedFile(dir_ / "level.dat", root);
}

void LevelStorage::saveWorldGen(const WorldGen& gen) { nbt::writeCompressedFile(dataFile("world_gen_settings"), wrapSavedData(gen.raw)); }

void LevelStorage::saveClocks(const std::vector<Clock>& clocks) {
    nbt::Compound data;
    for (const auto& c : clocks) {
        nbt::Compound st;
        st.put("total_ticks", c.totalTicks);
        if (c.partialTick != 0.0f) st.put("partial_tick", c.partialTick);
        if (c.rate != 1.0f) st.put("rate", c.rate);
        if (c.paused) st.put("paused", nbt::byteTag(true));
        data.put(c.name, std::move(st));
    }
    nbt::writeCompressedFile(dataFile("world_clocks"), wrapSavedData(std::move(data)));
}

void LevelStorage::saveGameRules(const GameRules& rules) { nbt::writeCompressedFile(dataFile("game_rules"), wrapSavedData(rules.save())); }

std::optional<nbt::Compound> LevelStorage::loadPlayer(const UUID& id) {
    fs::path p = dir_ / "players" / "data" / (id.toString() + ".dat");
    std::error_code ec;
    if (!fs::exists(p, ec)) return std::nullopt;
    auto tag = nbt::readCompressedFile(p);
    if (!tag) tag = nbt::readCompressedFile(p.string() + "_old");
    return tag;
}

bool LevelStorage::savePlayer(const UUID& id, const nbt::Compound& tag) {
    return nbt::writeCompressedFile(dir_ / "players" / "data" / (id.toString() + ".dat"), tag);
}

} // namespace mc
