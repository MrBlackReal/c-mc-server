#include "server/minecraft_server.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <random>
#include <cerrno>
#include <chrono>
#include <cstring>

#include "core/registries.hpp"
#include "generated/packet_ids.hpp"
#include "network/connection.hpp"
#include "server/commands/commands.hpp"
#include "server/network/listeners.hpp"
#include "util/log.hpp"

namespace mc {
namespace {
using Clock = std::chrono::steady_clock;
constexpr auto kTickDuration = std::chrono::milliseconds(50);

// epoll user data tags for non-connection fds.
constexpr uint64_t kListenTag = ~0ull;
constexpr uint64_t kConsoleTag = ~0ull - 1;

void setNonBlocking(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK); }
} // namespace

MinecraftServer::MinecraftServer(ServerProperties properties) : properties_(std::move(properties)), playerList_(*this) {
    defaultGameType_ = gameTypeByName(properties_.gamemode, GameType::Survival);
}

namespace {
// WorldOptions.parseSeed: a number, else String.hashCode(), else random.
int64_t parseSeed(const std::string& s) {
    if (s.empty()) {
        std::random_device rd;
        return static_cast<int64_t>(static_cast<uint64_t>(rd()) << 32 | rd());
    }
    int64_t v;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec == std::errc() && p == s.data() + s.size()) return v;
    int32_t h = 0;
    for (unsigned char c : s) h = static_cast<int32_t>(31u * static_cast<uint32_t>(h) + c); // ASCII == UTF-16 units
    return h;
}
} // namespace

bool MinecraftServer::loadLevel() {
    std::string error;
    storage_ = LevelStorage::open(properties_.levelName, error);
    if (!storage_) {
        log::error("Failed to open world \"{}\": {}", properties_.levelName, error);
        return false;
    }
    LevelStorage::WorldGen gen;
    std::vector<LevelStorage::Clock> clocks;
    bool created = !storage_->exists();
    if (created) {
        log::info("No existing world data, creating new world");
        if (properties_.levelType != "minecraft:flat" && properties_.levelType != "flat")
            log::warn("level-type={} isn't supported yet (only superflat generation exists); creating a superflat world",
                      properties_.levelType);
        gen = LevelStorage::defaultFlat(parseSeed(properties_.levelSeed));
    } else if (auto err = storage_->load(levelData_, gen, clocks, gameRules_)) {
        log::error("Can't load world \"{}\": {}", properties_.levelName, *err);
        return false;
    }

    bool flat = gen.overworldGenerator == "minecraft:flat" && !gen.layers.empty();
    std::vector<FlatLevelSource::Layer> layers;
    for (const auto& l : gen.layers) layers.push_back({l.state, l.height});
    uint32_t biome = static_cast<uint32_t>(registries::id("minecraft:worldgen/biome", gen.biome).value_or(0));
    FlatLevelSource source = flat ? FlatLevelSource(layers, biome) : FlatLevelSource::classicFlat();
    if (!flat) {
        log::warn("The world uses the {} generator, which isn't ported yet. New chunks will be superflat and are only saved "
                  "if changed, so vanilla can still generate the real terrain there later.",
                  gen.overworldGenerator.empty() ? "unknown" : gen.overworldGenerator);
    }
    // RegionFileVersion.configure: how new chunk data is compressed (reading takes any of them).
    RegionFile::Compression compression = RegionFile::Deflate;
    if (properties_.regionFileCompression == "lz4") compression = RegionFile::Lz4;
    else if (properties_.regionFileCompression == "none") compression = RegionFile::None;
    else if (properties_.regionFileCompression != "deflate")
        log::warn("Invalid value for region-file-compression: {}, using deflate", properties_.regionFileCompression);
    auto regions = std::make_unique<RegionFileStorage>(storage_->dimensionPath("minecraft:overworld") / "region",
                                                       properties_.syncChunkWrites, compression);
    auto entities = std::make_unique<RegionFileStorage>(storage_->dimensionPath("minecraft:overworld") / "entities",
                                                        properties_.syncChunkWrites, compression);
    overworld_ = std::make_unique<ServerLevel>("minecraft:overworld", "minecraft:overworld", DimensionInfo{-64, 384}, std::move(source),
                                               std::move(regions), std::move(entities), flat, gen.seed, [this] { return nextEntityId(); });
    if (created) {
        const auto& s = overworld_->spawn();
        levelData_.spawnX = s.x, levelData_.spawnY = s.y, levelData_.spawnZ = s.z;
        storage_->saveWorldGen(gen);
    } else {
        overworld_->setSpawn({levelData_.spawnX, levelData_.spawnY, levelData_.spawnZ, levelData_.spawnYaw, levelData_.spawnPitch});
        clocks_.setGameTime(levelData_.gameTime);
        for (const auto& c : clocks) {
            if (auto* clock = clocks_.byName(c.name)) *clock = {c.totalTicks, c.partialTick, c.rate, c.paused};
        }
    }
    overworld_->setGameTime(clocks_.gameTime());
    overworld_->setGameRules(&gameRules_);
    overworld_->players = [this] {
        std::vector<ServerPlayer*> out;
        for (const auto& p : playerList_.players()) out.push_back(p.get());
        return out;
    };
    overworld_->onLightWork = [this] { playerList_.broadcastLightChanges(); };
    overworld_->onEntityAdded = [this](Entity& e) { playerList_.trackNewEntity(e); };
    overworld_->skyDarken = [this] { // Level.updateSkyBrightness (no weather yet)
        auto* clock = clocks_.byName("minecraft:overworld");
        return clock ? static_cast<int>(15.0f - 15.0f * ServerClockManager::skyLightMultiplier(clock->totalTicks)) : 0;
    };
    overworld_->simulationDistance = properties_.simulationDistance;
    overworld_->dayTime = [this] {
        auto* clock = clocks_.byName("minecraft:overworld");
        return clock ? clock->totalTicks : int64_t{0};
    };
    overworld_->playerChunkDistance = [this](ChunkPos pos) {
        int best = std::numeric_limits<int>::max();
        for (const auto& p : playerList_.players()) {
            ChunkPos c = p->chunkPosition();
            best = std::min(best, std::max(std::abs(c.x - pos.x), std::abs(c.z - pos.z)));
        }
        return best;
    };
    overworld_->anyPlayerCloseEnoughTo = [this](BlockPos pos, int distance) {
        for (const auto& p : playerList_.players()) {
            if (p->gameMode == GameType::Spectator) continue;
            double dx = p->x - pos.x, dy = p->y - pos.y, dz = p->z - pos.z;
            if (std::sqrt(dx * dx + dy * dy + dz * dz) < distance) return true;
        }
        return false;
    };
    // Like DedicatedServer, server.properties decides difficulty and hardcore.
    levelData_.difficulty = properties_.difficulty;
    { // Difficulty ids: peaceful 0, easy 1, normal 2, hard 3
        const std::string& d = levelData_.difficulty;
        overworld_->difficulty = d == "peaceful" ? 0 : d == "easy" ? 1 : d == "hard" ? 3 : 2;
    }
    levelData_.hardcore = properties_.hardcore;
    saveEverything(false);
    return true;
}

void MinecraftServer::saveEverything(bool logIt) {
    if (!storage_ || !overworld_) return;
    if (logIt) log::info("Saving the game (this may take a moment!)");
    playerList_.saveAll();
    int chunks = overworld_->saveChunks(clocks_.gameTime());
    levelData_.gameTime = clocks_.gameTime();
    storage_->saveLevel(levelData_);
    std::vector<LevelStorage::Clock> clocks;
    for (int i = 0; i < clocks_.count(); i++) {
        const auto& c = clocks_.clock(i);
        clocks.push_back({std::string(clocks_.name(i)), c.totalTicks, c.partialTick, c.rate, c.paused});
    }
    storage_->saveClocks(clocks);
    storage_->saveGameRules(gameRules_);
    if (logIt) log::info("Saved the game ({} chunks)", chunks);
}

MinecraftServer::~MinecraftServer() {
    connections_.clear();
    if (listenFd_ >= 0) ::close(listenFd_);
    if (epollFd_ >= 0) ::close(epollFd_);
}

bool MinecraftServer::bind() {
    listenFd_ = ::socket(AF_INET6, SOCK_STREAM, 0);
    bool v6 = listenFd_ >= 0;
    if (!v6) listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1, zero = 0;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    sockaddr_storage addr{};
    socklen_t len;
    const std::string& ip = properties_.serverIp;
    if (v6) {
        setsockopt(listenFd_, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
        auto* a = reinterpret_cast<sockaddr_in6*>(&addr);
        a->sin6_family = AF_INET6;
        a->sin6_port = htons(static_cast<uint16_t>(properties_.serverPort));
        a->sin6_addr = in6addr_any;
        if (!ip.empty()) {
            in_addr v4;
            if (inet_pton(AF_INET, ip.c_str(), &v4) == 1) {
                // IPv4-mapped address on a dual-stack socket.
                a->sin6_addr = {};
                a->sin6_addr.s6_addr[10] = a->sin6_addr.s6_addr[11] = 0xff;
                std::memcpy(&a->sin6_addr.s6_addr[12], &v4, 4);
            } else if (inet_pton(AF_INET6, ip.c_str(), &a->sin6_addr) != 1) {
                log::error("Invalid server-ip: {}", ip);
                return false;
            }
        }
        len = sizeof(sockaddr_in6);
    } else {
        auto* a = reinterpret_cast<sockaddr_in*>(&addr);
        a->sin_family = AF_INET;
        a->sin_port = htons(static_cast<uint16_t>(properties_.serverPort));
        a->sin_addr.s_addr = INADDR_ANY;
        if (!ip.empty() && inet_pton(AF_INET, ip.c_str(), &a->sin_addr) != 1) {
            log::error("Invalid server-ip: {}", ip);
            return false;
        }
        len = sizeof(sockaddr_in);
    }
    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), len) != 0 || ::listen(listenFd_, 128) != 0) {
        log::error("**** FAILED TO BIND TO PORT!");
        log::error("The exception was: {}", std::strerror(errno));
        log::error("Perhaps a server is already running on that port?");
        return false;
    }
    setNonBlocking(listenFd_);
    return true;
}

int MinecraftServer::run() {
    auto started = Clock::now();
    log::info("Starting minecraft server version {}", kGameVersion);
    log::info("Default game type: {}", properties_.gamemode);
    log::info("Generating keypair");
    keyPair_ = crypt::KeyPair::generate(); // MinecraftServer.initializeKeyPair
    if (usesAuthentication()) sessionService_->refreshKeysInBackground(); // (the services keys, for chat)
    if (!usesAuthentication()) {
        log::warn("**** SERVER IS RUNNING IN OFFLINE/INSECURE MODE!");
        log::warn("The server will make no attempt to authenticate usernames. Beware.");
        log::warn("While this makes the game possible to play without internet access, it also opens up the ability for hackers to "
                  "connect with any username they choose.");
        log::warn("To change this, set \"online-mode\" to \"true\" in the server.properties file.");
    }
    log::info("Starting Minecraft server on {}:{}", properties_.serverIp.empty() ? "*" : properties_.serverIp, properties_.serverPort);
    if (!bind()) return 1;
    log::info("Preparing level \"{}\"", properties_.levelName);
    if (!loadLevel()) return 1;

    epollFd_ = epoll_create1(EPOLL_CLOEXEC);
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.u64 = kListenTag;
    epoll_ctl(epollFd_, EPOLL_CTL_ADD, listenFd_, &ev);
    setNonBlocking(STDIN_FILENO);
    ev.data.u64 = kConsoleTag;
    bool console = epoll_ctl(epollFd_, EPOLL_CTL_ADD, STDIN_FILENO, &ev) == 0;

    const auto& spawn = overworld_->spawn();
    for (int x = -2; x <= 2; x++)
        for (int z = -2; z <= 2; z++) overworld_->getChunk({(spawn.x >> 4) + x, (spawn.z >> 4) + z});
    auto elapsed = std::chrono::duration<double>(Clock::now() - started).count();
    log::info("Done ({:.3f}s)! For help, type \"help\"", elapsed);

    auto nextTick = Clock::now();
    epoll_event events[256];
    while (running_) {
        auto now = Clock::now();
        int timeout = static_cast<int>(std::max<int64_t>(
            0, std::chrono::duration_cast<std::chrono::milliseconds>(nextTick - now).count()));
        int n = epoll_wait(epollFd_, events, 256, timeout);
        if (n < 0 && errno != EINTR) {
            log::error("epoll_wait failed: {}", std::strerror(errno));
            break;
        }
        for (int i = 0; i < n; i++) {
            uint64_t tag = events[i].data.u64;
            if (tag == kListenTag) {
                acceptConnections();
            } else if (tag == kConsoleTag) {
                if (console) readConsole();
            } else {
                auto* c = reinterpret_cast<Connection*>(tag);
                if (events[i].events & (EPOLLIN | EPOLLHUP | EPOLLERR)) c->onReadable();
                if (events[i].events & EPOLLOUT) c->flush();
            }
        }
        if (Clock::now() >= nextTick) {
            tick();
            nextTick += kTickDuration;
            if (Clock::now() - nextTick > std::chrono::seconds(2)) {
                log::warn("Can't keep up! Is the server overloaded?");
                nextTick = Clock::now();
            }
        }
        // Flush what this iteration produced and reap closed connections.
        for (auto& c : connections_) {
            if (!c->isClosed()) c->flush();
            if (!c->isClosed()) updateInterest(*c);
        }
        std::erase_if(connections_, [&](const std::unique_ptr<Connection>& c) {
            if (!c->isClosed()) return false;
            epoll_ctl(epollFd_, EPOLL_CTL_DEL, c->fd(), nullptr);
            return true;
        });
    }

    log::info("Stopping server");
    for (auto& c : connections_) {
        c->disconnect(Component::translatable("multiplayer.disconnect.server_shutdown")); // saves the player
        c->flush();
    }
    connections_.clear();
    log::info("Saving worlds");
    saveEverything(false);
    return 0;
}

void MinecraftServer::updateInterest(Connection& c) {
    epoll_event ev{};
    ev.events = EPOLLIN | (c.wantsWrite() ? EPOLLOUT : 0u);
    ev.data.u64 = reinterpret_cast<uint64_t>(&c);
    epoll_ctl(epollFd_, EPOLL_CTL_MOD, c.fd(), &ev);
}

void MinecraftServer::acceptConnections() {
    while (true) {
        sockaddr_storage addr{};
        socklen_t len = sizeof addr;
        int fd = ::accept4(listenFd_, reinterpret_cast<sockaddr*>(&addr), &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) break;
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        char host[INET6_ADDRSTRLEN] = "?";
        int port = 0;
        if (addr.ss_family == AF_INET6) {
            auto* a = reinterpret_cast<sockaddr_in6*>(&addr);
            inet_ntop(AF_INET6, &a->sin6_addr, host, sizeof host);
            port = ntohs(a->sin6_port);
        } else if (addr.ss_family == AF_INET) {
            auto* a = reinterpret_cast<sockaddr_in*>(&addr);
            inet_ntop(AF_INET, &a->sin_addr, host, sizeof host);
            port = ntohs(a->sin_port);
        }
        std::string remote = std::string(host).starts_with("::ffff:") ? std::string(host + 7) : std::string(host);
        auto conn = std::make_unique<Connection>(fd, std::format("{}:{}", remote, port));
        conn->setListener(std::make_unique<ServerHandshakePacketListener>(*this, *conn));
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.u64 = reinterpret_cast<uint64_t>(conn.get());
        epoll_ctl(epollFd_, EPOLL_CTL_ADD, fd, &ev);
        connections_.push_back(std::move(conn));
    }
}

void MinecraftServer::readConsole() {
    char buf[1024];
    ssize_t n;
    while ((n = ::read(STDIN_FILENO, buf, sizeof buf)) > 0) consoleBuffer_.append(buf, static_cast<size_t>(n));
    if (n == 0) {
        epoll_ctl(epollFd_, EPOLL_CTL_DEL, STDIN_FILENO, nullptr); // stdin closed (e.g. running detached)
    }
    size_t pos;
    while ((pos = consoleBuffer_.find('\n')) != std::string::npos) {
        std::string line = consoleBuffer_.substr(0, pos);
        consoleBuffer_.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) handleConsoleCommand(line);
    }
}

void MinecraftServer::handleConsoleCommand(const std::string& line) {
    std::string cmd = line[0] == '/' ? line.substr(1) : line;
    if (cmd == "stop") {
        halt();
        return;
    }
    if (cmd == "save-all" || cmd == "save-all flush") {
        saveEverything(true);
        return;
    }
    commands::execute({*this, nullptr}, cmd);
}

void MinecraftServer::onGameRuleChanged(GameRules::Rule rule) {
    bool value = gameRules_.getBool(rule);
    if (rule == GameRules::ReducedDebugInfo) {
        for (const auto& p : playerList_.players()) { // ClientboundEntityEventPacket
            Packet ev(packets::play_cb::ENTITY_EVENT);
            ev.writeInt(p->id());
            ev.writeByte(value ? 22 : 23);
            p->connection().send(ev);
        }
    } else if (rule == GameRules::LimitedCrafting || rule == GameRules::ImmediateRespawn) {
        Packet ev(packets::play_cb::GAME_EVENT);
        ev.writeByte(rule == GameRules::LimitedCrafting ? 12 : 11);
        ev.writeFloat(value ? 1.0f : 0.0f);
        playerList_.broadcastAll(ev);
    } else if (rule == GameRules::AdvanceTime) {
        playerList_.broadcastTime(true);
    }
}

void MinecraftServer::tick() {
    tickCount_++;
    clocks_.tick(advanceTime());
    overworld_->setGameTime(clocks_.gameTime());
    overworld_->tickBlocks();
    overworld_->tickChunks();
    overworld_->tickEntities();
    overworld_->tickBlockEntities();
    overworld_->runLightUpdates(); // (ThreadedLevelLightEngine.tryScheduleUpdate, at the end of the level tick)
    // Vanilla ticks levels (which broadcast block changes) before connections (keep-alive, block
    // change acks); keep that order so acks follow the updates they confirm.
    playerList_.tick();
    for (auto& c : connections_) c->tick();
    if (tickCount_ % 6000 == 0) saveEverything(false); // autosave, like MinecraftServer.autoSave
    if (tickCount_ % 100 == 0) {
        // Unload chunks no player is tracking (ChunkMap), saving the ones that changed.
        int margin = properties_.viewDistance + 2;
        overworld_->unloadChunks(
            [&](ChunkPos pos) {
                for (const auto& p : playerList_.players()) {
                    ChunkPos c = p->chunkPosition();
                    if (std::abs(c.x - pos.x) <= margin && std::abs(c.z - pos.z) <= margin) return true;
                }
                return false;
            },
            clocks_.gameTime());
    }
}

} // namespace mc
