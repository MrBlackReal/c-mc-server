#include "server/commands/arguments.hpp"

#include <algorithm>
#include <cmath>
#include <random>

#include "core/registries.hpp"
#include "generated/blocks.hpp"
#include "nbt/snbt.hpp"
#include "server/level/server_level.hpp"
#include "server/level/server_player.hpp"
#include "server/minecraft_server.hpp"
#include "world/entity/entity.hpp"
#include "world/level/block/block_behaviour.hpp"
#include "util/mth.hpp"

namespace mc::commands {

using brigadier::CommandSyntaxException;
using brigadier::error;
using brigadier::errorAt;
using brigadier::number;

namespace {
Component lit(std::string s) { return Component::literal(std::move(s)); }

// Level.isInWorldBoundsHorizontal / isInSpawnableBounds / isInWorldBounds
bool inWorldBoundsHorizontal(BlockPos pos) { return pos.x >= -30000000 && pos.z >= -30000000 && pos.x < 30000000 && pos.z < 30000000; }
bool inSpawnableBounds(BlockPos pos) { return !(pos.y < -20000000 || pos.y >= 20000000) && inWorldBoundsHorizontal(pos); }

int floorToInt(double v) { return static_cast<int>(std::floor(v)); }

// NbtUtils.compareNbt(expected, actual, partialListMatch)
bool compareNbt(const nbt::Tag* expected, const nbt::Tag* actual, bool partialListMatch) {
    if (!expected) return true;
    if (!actual) return false;
    if (expected->type() != actual->type()) return false;
    if (const nbt::Compound* e = expected->asCompound()) {
        const nbt::Compound* a = actual->asCompound();
        for (const auto& [k, v] : e->entries)
            if (!compareNbt(&v, a->get(k), partialListMatch)) return false;
        return true;
    }
    if (const nbt::List* e = expected->asList(); e && partialListMatch) {
        const nbt::List* a = actual->asList();
        if (e->items.empty()) return a->items.empty();
        for (const auto& ev : e->items) {
            bool found = std::any_of(a->items.begin(), a->items.end(), [&](const nbt::Tag& av) { return compareNbt(&ev, &av, partialListMatch); });
            if (!found) return false;
        }
        return true;
    }
    return *expected == *actual;
}

// TagParser.parseCompoundAsArgument
nbt::Compound readCompoundArgument(StringReader& reader) {
    std::string_view rest = std::string_view(reader.getString()).substr(reader.getCursor());
    size_t consumed = 0, errorPos = 0;
    std::string message;
    auto tag = nbt::parseSnbtPrefix(rest, consumed, &message, &errorPos);
    if (!tag) {
        reader.setCursor(reader.getCursor() + static_cast<int>(errorPos));
        CommandSyntaxException e = errorAt(reader, "snbt.parser.invalid");
        e.message = Component::literal(message); // (the packrat grammar's own messages aren't ported)
        throw e;
    }
    reader.setCursor(reader.getCursor() + static_cast<int>(consumed));
    const nbt::Compound* c = tag->asCompound();
    if (!c) throw errorAt(reader, "argument.nbt.expected.compound");
    return *c;
}

const gen::BlockProperty* findProperty(uint32_t block, std::string_view name) {
    for (const auto& p : gen::kBlocks[block].properties)
        if (p.name == name) return &p;
    return nullptr;
}

bool validValue(const gen::BlockProperty& p, std::string_view value) {
    return std::find(p.values.begin(), p.values.end(), value) != p.values.end();
}

std::vector<std::string> gameModeNames() { return {"survival", "creative", "adventure", "spectator"}; }
} // namespace

// ---------------------------------------------------------------- brigadier types

void BoolArgument::listSuggestions(const CommandContext&, SuggestionsBuilder& builder) const {
    if (std::string_view("true").starts_with(builder.getRemainingLowerCase())) builder.suggest("true");
    if (std::string_view("false").starts_with(builder.getRemainingLowerCase())) builder.suggest("false");
}

std::any IntegerArgument::parse(StringReader& reader, const CommandSourceStack&) const {
    int start = reader.getCursor();
    int32_t result = reader.readInt();
    if (result < min_) {
        reader.setCursor(start);
        throw errorAt(reader, "argument.integer.low", {number(int64_t{min_}), number(int64_t{result})});
    }
    if (result > max_) {
        reader.setCursor(start);
        throw errorAt(reader, "argument.integer.big", {number(int64_t{max_}), number(int64_t{result})});
    }
    return result;
}

void IntegerArgument::writeProperties(ByteWriter& out) const {
    bool hasMin = min_ != std::numeric_limits<int32_t>::min(), hasMax = max_ != std::numeric_limits<int32_t>::max();
    out.writeByte((hasMin ? 1 : 0) | (hasMax ? 2 : 0));
    if (hasMin) out.writeInt(min_);
    if (hasMax) out.writeInt(max_);
}

std::any FloatArgument::parse(StringReader& reader, const CommandSourceStack&) const {
    int start = reader.getCursor();
    float result = reader.readFloat();
    if (result < min_) {
        reader.setCursor(start);
        throw errorAt(reader, "argument.float.low", {number(min_), number(result)});
    }
    if (result > max_) {
        reader.setCursor(start);
        throw errorAt(reader, "argument.float.big", {number(max_), number(result)});
    }
    return result;
}

void FloatArgument::writeProperties(ByteWriter& out) const {
    bool hasMin = min_ != -std::numeric_limits<float>::max(), hasMax = max_ != std::numeric_limits<float>::max();
    out.writeByte((hasMin ? 1 : 0) | (hasMax ? 2 : 0));
    if (hasMin) out.writeFloat(min_);
    if (hasMax) out.writeFloat(max_);
}

std::any StringArgument::parse(StringReader& reader, const CommandSourceStack&) const {
    if (type_ == Type::GreedyPhrase) {
        std::string text = reader.getRemaining();
        reader.setCursor(reader.getTotalLength());
        return text;
    }
    return type_ == Type::SingleWord ? reader.readUnquotedString() : reader.readString();
}

void StringArgument::writeProperties(ByteWriter& out) const { out.writeVarInt(static_cast<int>(type_)); }

// ---------------------------------------------------------------- simple minecraft types

std::any GameModeArgument::parse(StringReader& reader, const CommandSourceStack&) const {
    std::string name = reader.readUnquotedString();
    GameType type = gameTypeByName(name, static_cast<GameType>(-1));
    if (static_cast<int>(type) < 0) throw errorAt(reader, "argument.gamemode.invalid", {lit(name)});
    return type;
}

void GameModeArgument::listSuggestions(const CommandContext&, SuggestionsBuilder& builder) const { brigadier::suggest(gameModeNames(), builder); }

std::any EntityAnchorArgument::parse(StringReader& reader, const CommandSourceStack&) const {
    int start = reader.getCursor();
    std::string name = reader.readUnquotedString();
    if (name == "feet") return Anchor::Feet;
    if (name == "eyes") return Anchor::Eyes;
    reader.setCursor(start);
    throw errorAt(reader, "argument.anchor.invalid", {lit(name)});
}

void EntityAnchorArgument::listSuggestions(const CommandContext&, SuggestionsBuilder& builder) const { brigadier::suggest({"feet", "eyes"}, builder); }

std::string readIdentifier(StringReader& reader) {
    int start = reader.getCursor();
    // Identifier.isAllowedInIdentifier
    auto allowed = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '_' || c == ':' || c == '/' || c == '.' || c == '-'; };
    while (reader.canRead() && allowed(reader.peek())) reader.skip();
    std::string raw = reader.getString().substr(start, reader.getCursor() - start);
    size_t colon = raw.find(':');
    std::string ns = colon == std::string::npos ? "minecraft" : raw.substr(0, colon);
    std::string path = colon == std::string::npos ? raw : raw.substr(colon + 1);
    bool valid = ns != ".." && path.find(':') == std::string::npos && ns.find('/') == std::string::npos;
    if (colon != std::string::npos && ns.empty()) ns = "minecraft"; // (":path" is minecraft:path)
    if (!valid) {
        reader.setCursor(start);
        throw errorAt(reader, "argument.id.invalid");
    }
    return ns + ":" + path;
}

std::any IdentifierArgument::parse(StringReader& reader, const CommandSourceStack&) const { return readIdentifier(reader); }

std::any ResourceArgument::parse(StringReader& reader, const CommandSourceStack&) const {
    std::string id = readIdentifier(reader);
    if (!registries::id(registry_, id)) throw errorAt(reader, "argument.resource.not_found", {lit(id), lit(registry_)});
    return id;
}

void ResourceArgument::listSuggestions(const CommandContext&, SuggestionsBuilder& builder) const {
    std::vector<std::string> names;
    for (int i = 0, n = registries::size(registry_); i < n; i++) names.emplace_back(registries::name(registry_, i));
    brigadier::suggest(names, builder);
}

void ResourceArgument::writeProperties(ByteWriter& out) const { out.writeUtf(registry_); }

std::any TimeArgument::parse(StringReader& reader, const CommandSourceStack&) const {
    float value = reader.readFloat();
    std::string unit = reader.readUnquotedString();
    int factor = unit.empty() || unit == "t" ? 1 : unit == "s" ? 20 : unit == "d" ? 24000 : 0;
    if (factor == 0) throw errorAt(reader, "argument.time.invalid_unit");
    int32_t ticks = static_cast<int32_t>(std::floor(value * static_cast<float>(factor) + 0.5f)); // Math.round(float)
    if (ticks < minimum_) throw errorAt(reader, "argument.time.tick_count_too_low", {number(int64_t{minimum_}), number(int64_t{ticks})});
    return ticks;
}

void TimeArgument::writeProperties(ByteWriter& out) const { out.writeInt(minimum_); }

// ---------------------------------------------------------------- coordinates

namespace {
WorldCoordinate parseWorldDouble(StringReader& reader, bool center) {
    if (reader.canRead() && reader.peek() == '^') throw errorAt(reader, "argument.pos.mixed");
    if (!reader.canRead()) throw errorAt(reader, "argument.pos.missing.double");
    bool relative = reader.peek() == '~';
    if (relative) reader.skip();
    int start = reader.getCursor();
    double value = reader.canRead() && reader.peek() != ' ' ? reader.readDouble() : 0.0;
    std::string number = reader.getString().substr(start, reader.getCursor() - start);
    if (relative && number.empty()) return {true, 0.0};
    if (number.find('.') == std::string::npos && !relative && center) value += 0.5;
    return {relative, value};
}

WorldCoordinate parseWorldInt(StringReader& reader) {
    if (reader.canRead() && reader.peek() == '^') throw errorAt(reader, "argument.pos.mixed");
    if (!reader.canRead()) throw errorAt(reader, "argument.pos.missing.int");
    bool relative = reader.peek() == '~';
    if (relative) reader.skip();
    double value = 0.0;
    if (reader.canRead() && reader.peek() != ' ') value = relative ? reader.readDouble() : reader.readInt();
    return {relative, value};
}

double readLocal(StringReader& reader, int start) {
    if (!reader.canRead()) throw errorAt(reader, "argument.pos.missing.double");
    if (reader.peek() != '^') {
        reader.setCursor(start);
        throw errorAt(reader, "argument.pos.mixed");
    }
    reader.skip();
    return reader.canRead() && reader.peek() != ' ' ? reader.readDouble() : 0.0;
}

// LocalCoordinates.parse
Coordinates parseLocal(StringReader& reader) {
    int start = reader.getCursor();
    Coordinates c;
    c.local = true;
    c.x.value = readLocal(reader, start);
    if (!reader.canRead() || reader.peek() != ' ') {
        reader.setCursor(start);
        throw errorAt(reader, "argument.pos3d.incomplete");
    }
    reader.skip();
    c.y.value = readLocal(reader, start);
    if (!reader.canRead() || reader.peek() != ' ') {
        reader.setCursor(start);
        throw errorAt(reader, "argument.pos3d.incomplete");
    }
    reader.skip();
    c.z.value = readLocal(reader, start);
    return c;
}

template <class F>
Coordinates parseWorld(StringReader& reader, F&& coordinate) {
    int start = reader.getCursor();
    Coordinates c;
    c.x = coordinate(reader, 0);
    if (!reader.canRead() || reader.peek() != ' ') {
        reader.setCursor(start);
        throw errorAt(reader, "argument.pos3d.incomplete");
    }
    reader.skip();
    c.y = coordinate(reader, 1);
    if (!reader.canRead() || reader.peek() != ' ') {
        reader.setCursor(start);
        throw errorAt(reader, "argument.pos3d.incomplete");
    }
    reader.skip();
    c.z = coordinate(reader, 2);
    return c;
}
} // namespace

void Coordinates::getPosition(const CommandSourceStack& source, double& ox, double& oy, double& oz) const {
    if (!local) {
        ox = x.get(source.x), oy = y.get(source.y), oz = z.get(source.z);
        return;
    }
    // LocalCoordinates.apply, from the anchor, facing the source's rotation
    double sx, sy, sz;
    source.anchorPosition(sx, sy, sz);
    float yCos = mth::cos((source.rotY + 90.0f) * 0.017453292f);
    float ySin = mth::sin((source.rotY + 90.0f) * 0.017453292f);
    float xCos = mth::cos(-source.rotX * 0.017453292f);
    float xSin = mth::sin(-source.rotX * 0.017453292f);
    float xCosUp = mth::cos((-source.rotX + 90.0f) * 0.017453292f);
    float xSinUp = mth::sin((-source.rotX + 90.0f) * 0.017453292f);
    double fx = yCos * xCos, fy = xSin, fz = ySin * xCos;          // forwards
    double ux = yCos * xCosUp, uy = xSinUp, uz = ySin * xCosUp;    // up
    double lx = -(fy * uz - fz * uy), ly = -(fz * ux - fx * uz), lz = -(fx * uy - fy * ux); // forwards x up, negated
    double left = x.value, upV = y.value, forwards = z.value;
    ox = sx + (fx * forwards + ux * upV + lx * left);
    oy = sy + (fy * forwards + uy * upV + ly * left);
    oz = sz + (fz * forwards + uz * upV + lz * left);
}

void Coordinates::getRotation(const CommandSourceStack& source, float& xRot, float& yRot) const {
    if (local) {
        xRot = yRot = 0;
        return;
    }
    xRot = static_cast<float>(x.get(source.rotX));
    yRot = static_cast<float>(y.get(source.rotY));
}

BlockPos Coordinates::getBlockPos(const CommandSourceStack& source) const {
    double px, py, pz;
    getPosition(source, px, py, pz);
    return {floorToInt(px), floorToInt(py), floorToInt(pz)}; // BlockPos.containing
}

std::any Vec3Argument::parse(StringReader& reader, const CommandSourceStack&) const {
    if (reader.canRead() && reader.peek() == '^') return parseLocal(reader);
    return parseWorld(reader, [&](StringReader& r, int axis) { return parseWorldDouble(r, axis != 1 && centerCorrect_); });
}

std::any BlockPosArgument::parse(StringReader& reader, const CommandSourceStack&) const {
    if (reader.canRead() && reader.peek() == '^') return parseLocal(reader);
    return parseWorld(reader, [](StringReader& r, int) { return parseWorldInt(r); });
}

BlockPos BlockPosArgument::getLoadedBlockPos(const CommandContext& context, std::string_view name) {
    const CommandSourceStack& source = context.getSource();
    BlockPos pos = context.get<Coordinates>(name).getBlockPos(source);
    ServerLevel& level = *source.level;
    if (!level.getChunkIfLoaded({pos.x >> 4, pos.z >> 4})) throw error("argument.pos.unloaded");
    if (!(pos.y >= level.minY() && pos.y <= level.maxY() && inWorldBoundsHorizontal(pos))) throw error("argument.pos.outofworld");
    return pos;
}

std::any RotationArgument::parse(StringReader& reader, const CommandSourceStack&) const {
    int start = reader.getCursor();
    if (!reader.canRead()) throw errorAt(reader, "argument.rotation.incomplete");
    WorldCoordinate yaw = parseWorldDouble(reader, false);
    if (!reader.canRead() || reader.peek() != ' ') {
        reader.setCursor(start);
        throw errorAt(reader, "argument.rotation.incomplete");
    }
    reader.skip();
    WorldCoordinate pitch = parseWorldDouble(reader, false);
    Coordinates c; // WorldCoordinates(x = pitch, y = yaw, z = ~)
    c.x = pitch;
    c.y = yaw;
    c.z = {true, 0.0};
    return c;
}

// ---------------------------------------------------------------- entity selectors

namespace {
// MinMaxBounds.Bounds.fromReader over doubles, ints or floats.
template <class T, class Convert>
std::pair<std::optional<T>, std::optional<T>> readBounds(StringReader& reader, Convert convert, const char* invalidKey) {
    if (!reader.canRead()) throw errorAt(reader, "argument.range.empty");
    int start = reader.getCursor();
    auto isAllowedInputChar = [&] {
        char c = reader.peek();
        if ((c >= '0' && c <= '9') || c == '-') return true;
        return c == '.' && !(reader.canRead(2) && reader.peek(1) == '.');
    };
    auto readNumber = [&]() -> std::optional<T> {
        int s = reader.getCursor();
        while (reader.canRead() && isAllowedInputChar()) reader.skip();
        std::string number = reader.getString().substr(s, reader.getCursor() - s);
        if (number.empty()) return std::nullopt;
        auto v = convert(number);
        if (!v) throw errorAt(reader, invalidKey, {lit(number)});
        return v;
    };
    try {
        std::optional<T> min = readNumber(), max;
        if (reader.canRead(2) && reader.peek() == '.' && reader.peek(1) == '.') {
            reader.skip();
            reader.skip();
            max = readNumber();
        } else {
            max = min;
        }
        if (!min && !max) throw errorAt(reader, "argument.range.empty");
        return {min, max};
    } catch (CommandSyntaxException& e) {
        reader.setCursor(start);
        e.cursor = start;
        throw;
    }
}

DoubleBounds readDoubleBounds(StringReader& reader) {
    int start = reader.getCursor();
    auto [min, max] = readBounds<double>(reader, brigadier::parseJavaDouble, "parsing.double.invalid");
    if (min && max && *min > *max) {
        reader.setCursor(start);
        throw errorAt(reader, "argument.range.swapped");
    }
    return {min, max};
}

IntBounds readIntBounds(StringReader& reader) {
    int start = reader.getCursor();
    auto [min, max] = readBounds<int32_t>(reader, brigadier::parseJavaInt, "parsing.int.invalid");
    if (min && max && *min > *max) {
        reader.setCursor(start);
        throw errorAt(reader, "argument.range.swapped");
    }
    return {min, max};
}

FloatBounds readFloatBounds(StringReader& reader) {
    auto [min, max] = readBounds<float>(reader, brigadier::parseJavaFloat, "parsing.float.invalid");
    return {min, max};
}

// InvertableSetOptionState
struct InvertableSetOptionState {
    enum { None, Single, Multiple } state = None;
    std::vector<std::string> tags;
    bool canParsePositiveElement() const { return state == None; }
    bool canParseNegativeElement() const { return state != Single; }
    bool canParseAnyTag() const { return state != Single; }
    bool canParseTag(const std::string& tag) const { return canParseAnyTag() && std::find(tags.begin(), tags.end(), tag) == tags.end(); }
    bool canParseAny() const { return state != Single; }
    bool canParseElement(bool inverted) const { return inverted ? canParseNegativeElement() : canParsePositiveElement(); }
    void markParsedElement(bool inverted) { state = inverted ? Multiple : Single; }
    void markParsedTag(const std::string& tag) {
        state = Multiple;
        tags.push_back(tag);
    }
};

class EntitySelectorParser {
public:
    EntitySelectorParser(StringReader& reader, bool allowSelectors) : reader_(reader), allowSelectors_(allowSelectors) {}

    EntitySelector parse() {
        if (reader_.canRead() && reader_.peek() == '@') {
            if (!allowSelectors_) throw errorAt(reader_, "argument.entity.selector.not_allowed");
            reader_.skip();
            parseSelector();
        } else {
            parseNameOrUUID();
        }
        finalizePredicates();
        return getSelector();
    }

private:
    [[noreturn]] void rollbackAndThrow(int start, std::string key, std::vector<Component> args = {}) {
        reader_.setCursor(start);
        throw errorAt(reader_, std::move(key), std::move(args));
    }

    void parseSelector() {
        s_.usesSelector = true;
        if (!reader_.canRead()) throw errorAt(reader_, "argument.entity.selector.missing");
        int start = reader_.getCursor();
        char type = reader_.read();
        bool alive;
        switch (type) {
        case 'a': s_.maxResults = INT32_MAX, s_.includesEntities = false, s_.order = EntitySelector::Order::Arbitrary, s_.type = "minecraft:player", alive = false; break;
        case 'e': s_.maxResults = INT32_MAX, s_.includesEntities = true, s_.order = EntitySelector::Order::Arbitrary, alive = true; break;
        case 'n': s_.maxResults = 1, s_.includesEntities = true, s_.order = EntitySelector::Order::Nearest, alive = true; break;
        case 'p': s_.maxResults = 1, s_.includesEntities = false, s_.order = EntitySelector::Order::Nearest, s_.type = "minecraft:player", alive = false; break;
        case 'r': s_.maxResults = 1, s_.includesEntities = false, s_.order = EntitySelector::Order::Random, s_.type = "minecraft:player", alive = false; break;
        case 's': s_.maxResults = 1, s_.includesEntities = true, s_.currentEntity = true, alive = false; break;
        default:
            reader_.setCursor(start);
            throw errorAt(reader_, "argument.entity.selector.unknown", {lit("@" + std::string(1, type))});
        }
        if (alive) s_.predicates.push_back([](const EntityRef& e) { return e.isAlive(); });
        if (reader_.canRead() && reader_.peek() == '[') {
            reader_.skip();
            parseOptions();
        }
    }

    void parseNameOrUUID() {
        int start = reader_.getCursor();
        std::string name = reader_.readString();
        if (auto uuid = UUID::fromJavaString(name)) {
            s_.entityUUID = *uuid;
            s_.includesEntities = true;
        } else {
            if (name.empty() || name.size() > 16) { // (length in UTF-16 units; names are ASCII)
                reader_.setCursor(start);
                throw errorAt(reader_, "argument.entity.invalid");
            }
            s_.includesEntities = false;
            s_.playerName = name;
        }
        s_.maxResults = 1;
    }

    bool shouldInvertValue() {
        reader_.skipWhitespace();
        if (reader_.canRead() && reader_.peek() == '!') {
            reader_.skip();
            reader_.skipWhitespace();
            return true;
        }
        return false;
    }

    bool isTag() {
        reader_.skipWhitespace();
        if (reader_.canRead() && reader_.peek() == '#') {
            reader_.skip();
            reader_.skipWhitespace();
            return true;
        }
        return false;
    }

    void parseOptions() {
        reader_.skipWhitespace();
        while (reader_.canRead() && reader_.peek() != ']') {
            reader_.skipWhitespace();
            int start = reader_.getCursor();
            std::string key = reader_.readString();
            checkOption(key, start); // EntitySelectorOptions.get
            reader_.skipWhitespace();
            if (!reader_.canRead() || reader_.peek() != '=') {
                reader_.setCursor(start);
                throw errorAt(reader_, "argument.entity.options.valueless", {lit(key)});
            }
            reader_.skip();
            reader_.skipWhitespace();
            handleOption(key);
            reader_.skipWhitespace();
            if (reader_.canRead()) {
                if (reader_.peek() != ',') {
                    if (reader_.peek() != ']') throw errorAt(reader_, "argument.entity.options.unterminated");
                    break;
                }
                reader_.skip();
            }
        }
        if (!reader_.canRead()) throw errorAt(reader_, "argument.entity.options.unterminated");
        reader_.skip();
    }

    void checkOption(const std::string& key, int start) {
        bool canUse;
        if (key == "name") canUse = nameOption_.canParseAny();
        else if (key == "distance") canUse = !distance_;
        else if (key == "level") canUse = !level_;
        else if (key == "x") canUse = !s_.px;
        else if (key == "y") canUse = !s_.py;
        else if (key == "z") canUse = !s_.pz;
        else if (key == "dx") canUse = !dx_;
        else if (key == "dy") canUse = !dy_;
        else if (key == "dz") canUse = !dz_;
        else if (key == "x_rotation") canUse = !rotX_;
        else if (key == "y_rotation") canUse = !rotY_;
        else if (key == "limit") canUse = !s_.currentEntity && !limited_;
        else if (key == "sort") canUse = !s_.currentEntity && !sorted_;
        else if (key == "gamemode") canUse = gamemodeOption_.canParseAny();
        else if (key == "team") canUse = teamOption_.canParseAny();
        else if (key == "type") canUse = typeOption_.canParseAny();
        else if (key == "tag" || key == "nbt" || key == "predicate") canUse = true;
        else if (key == "scores") canUse = !scores_;
        else if (key == "advancements") canUse = !advancements_;
        else rollbackAndThrow(start, "argument.entity.options.unknown", {lit(key)});
        if (!canUse) rollbackAndThrow(start, "argument.entity.options.inapplicable", {lit(key)});
    }

    void handleOption(const std::string& key) {
        int start = reader_.getCursor();
        if (key == "name") {
            bool inverted = shouldInvertValue();
            std::string name = reader_.readString();
            if (!nameOption_.canParseElement(inverted)) rollbackAndThrow(start, "argument.entity.options.inapplicable", {lit("name")});
            nameOption_.markParsedElement(inverted);
            s_.predicates.push_back([name, inverted](const EntityRef& e) { return (e.plainName() == name) != inverted; });
        } else if (key == "distance") {
            DoubleBounds value = readDoubleBounds(reader_);
            if ((value.min && *value.min < 0.0) || (value.max && *value.max < 0.0)) rollbackAndThrow(start, "argument.entity.options.distance.negative");
            distance_ = value;
            s_.worldLimited = true;
        } else if (key == "level") {
            IntBounds value = readIntBounds(reader_);
            if ((value.min && *value.min < 0) || (value.max && *value.max < 0)) rollbackAndThrow(start, "argument.entity.options.level.negative");
            level_ = value;
            s_.includesEntities = false;
        } else if (key == "x" || key == "y" || key == "z" || key == "dx" || key == "dy" || key == "dz") {
            s_.worldLimited = true;
            double v = reader_.readDouble();
            (key == "x" ? s_.px : key == "y" ? s_.py : key == "z" ? s_.pz : key == "dx" ? dx_ : key == "dy" ? dy_ : dz_) = v;
        } else if (key == "x_rotation") {
            rotX_ = readFloatBounds(reader_);
        } else if (key == "y_rotation") {
            rotY_ = readFloatBounds(reader_);
        } else if (key == "limit") {
            int count = reader_.readInt();
            if (count < 1) rollbackAndThrow(start, "argument.entity.options.limit.toosmall");
            s_.maxResults = count;
            limited_ = true;
        } else if (key == "sort") {
            std::string name = reader_.readUnquotedString();
            if (name == "nearest") s_.order = EntitySelector::Order::Nearest;
            else if (name == "furthest") s_.order = EntitySelector::Order::Furthest;
            else if (name == "random") s_.order = EntitySelector::Order::Random;
            else if (name == "arbitrary") s_.order = EntitySelector::Order::Arbitrary;
            else rollbackAndThrow(start, "argument.entity.options.sort.irreversible", {lit(name)});
            sorted_ = true;
        } else if (key == "gamemode") {
            bool inverted = shouldInvertValue();
            if (!gamemodeOption_.canParseElement(inverted)) rollbackAndThrow(start, "argument.entity.options.inapplicable", {lit("gamemode")});
            std::string name = reader_.readUnquotedString();
            GameType expected = gameTypeByName(name, static_cast<GameType>(-1));
            if (static_cast<int>(expected) < 0) rollbackAndThrow(start, "argument.entity.options.mode.invalid", {lit(name)});
            s_.includesEntities = false;
            s_.predicates.push_back([expected, inverted](const EntityRef& e) { return e.player && ((e.player->gameMode == expected) ^ inverted); });
            gamemodeOption_.markParsedElement(inverted);
        } else if (key == "team") {
            bool inverted = shouldInvertValue();
            std::string expected = reader_.readUnquotedString();
            if (!teamOption_.canParseElement(inverted)) rollbackAndThrow(start, "argument.entity.options.inapplicable", {lit("team")});
            // (no scoreboard teams yet: everyone's team name is "")
            s_.predicates.push_back([expected, inverted](const EntityRef&) { return (std::string() == expected) != inverted; });
            teamOption_.markParsedElement(inverted);
        } else if (key == "type") {
            bool inverted = shouldInvertValue();
            if (isTag()) {
                if (!typeOption_.canParseAnyTag()) rollbackAndThrow(start, "argument.entity.options.inapplicable", {lit("type")});
                std::string id = readIdentifier(reader_);
                if (!typeOption_.canParseTag(id)) rollbackAndThrow(start, "argument.entity.options.inapplicable", {lit("type")});
                s_.predicates.push_back([id, inverted](const EntityRef& e) { return registries::hasTag("minecraft:entity_type", e.type(), id) != inverted; });
                typeOption_.markParsedTag(id);
            } else {
                if (!typeOption_.canParseElement(inverted)) rollbackAndThrow(start, "argument.entity.options.inapplicable", {lit("type")});
                std::string id = readIdentifier(reader_);
                if (!registries::id("minecraft:entity_type", id)) rollbackAndThrow(start, "argument.entity.options.type.invalid", {lit(id)});
                if (id == "minecraft:player" && !inverted) s_.includesEntities = false;
                s_.predicates.push_back([id, inverted](const EntityRef& e) { return (e.type() == id) != inverted; });
                if (!inverted) s_.type = id;
                typeOption_.markParsedElement(inverted);
            }
        } else if (key == "tag") {
            bool inverted = shouldInvertValue();
            std::string tag = reader_.readUnquotedString();
            // (entity tags aren't kept yet: every entity has none)
            s_.predicates.push_back([tag, inverted](const EntityRef&) { return tag.empty() ? !inverted : inverted; });
        } else if (key == "nbt") {
            bool inverted = shouldInvertValue();
            nbt::Compound tag = readCompoundArgument(reader_);
            s_.predicates.push_back([tag, inverted](const EntityRef& e) {
                // (non-player entities: their saved form; players' saved data isn't built here yet)
                nbt::Tag actual = e.entity ? nbt::Tag(e.entity->save()) : nbt::Tag(nbt::Compound{});
                nbt::Tag expected(tag);
                return compareNbt(&expected, &actual, true) != inverted;
            });
        } else if (key == "scores") {
            reader_.expect('{');
            reader_.skipWhitespace();
            bool any = false;
            while (reader_.canRead() && reader_.peek() != '}') {
                reader_.skipWhitespace();
                reader_.readUnquotedString();
                reader_.skipWhitespace();
                reader_.expect('=');
                reader_.skipWhitespace();
                readIntBounds(reader_);
                any = true;
                reader_.skipWhitespace();
                if (reader_.canRead() && reader_.peek() == ',') reader_.skip();
            }
            reader_.expect('}');
            // (no scoreboard objectives exist, so any score requirement fails)
            if (any) s_.predicates.push_back([](const EntityRef&) { return false; });
            scores_ = true;
        } else if (key == "advancements") {
            reader_.expect('{');
            reader_.skipWhitespace();
            bool any = false;
            while (reader_.canRead() && reader_.peek() != '}') {
                reader_.skipWhitespace();
                readIdentifier(reader_);
                reader_.skipWhitespace();
                reader_.expect('=');
                reader_.skipWhitespace();
                if (reader_.canRead() && reader_.peek() == '{') {
                    reader_.skipWhitespace();
                    reader_.expect('{');
                    reader_.skipWhitespace();
                    while (reader_.canRead() && reader_.peek() != '}') {
                        reader_.skipWhitespace();
                        reader_.readUnquotedString();
                        reader_.skipWhitespace();
                        reader_.expect('=');
                        reader_.skipWhitespace();
                        reader_.readBoolean();
                        reader_.skipWhitespace();
                        if (reader_.canRead() && reader_.peek() == ',') reader_.skip();
                    }
                    reader_.skipWhitespace();
                    reader_.expect('}');
                    reader_.skipWhitespace();
                } else {
                    reader_.readBoolean();
                }
                any = true;
                reader_.skipWhitespace();
                if (reader_.canRead() && reader_.peek() == ',') reader_.skip();
            }
            reader_.expect('}');
            if (any) {
                // (advancements aren't loaded: none is found, so the requirement fails)
                s_.predicates.push_back([](const EntityRef&) { return false; });
                s_.includesEntities = false;
            }
            advancements_ = true;
        } else if (key == "predicate") {
            bool inverted = shouldInvertValue();
            readIdentifier(reader_);
            // (predicate files aren't loaded: a missing predicate never matches)
            s_.predicates.push_back([inverted](const EntityRef&) {
                (void)inverted;
                return false;
            });
        }
    }

    void finalizePredicates() {
        auto rotation = [](const FloatBounds& range, bool xAxis) {
            float min = mth::wrapDegrees(range.min.value_or(0.0f));
            float max = mth::wrapDegrees(range.max.value_or(359.0f));
            return [min, max, xAxis](const EntityRef& e) {
                float r = mth::wrapDegrees(xAxis ? e.xRot() : e.yRot());
                return min > max ? r >= min || r <= max : r >= min && r <= max;
            };
        };
        if (rotX_) s_.predicates.push_back(rotation(*rotX_, true));
        if (rotY_) s_.predicates.push_back(rotation(*rotY_, false));
        if (level_) {
            IntBounds level = *level_;
            s_.predicates.push_back([level](const EntityRef& e) { return e.player && level.matches(e.player->experienceLevel); });
        }
    }

    EntitySelector getSelector() {
        if (!dx_ && !dy_ && !dz_) {
            if (distance_ && distance_->max) {
                double r = *distance_->max;
                s_.aabb = AABB{-r, -r, -r, r + 1.0, r + 1.0, r + 1.0};
            }
        } else {
            double x = dx_.value_or(0.0), y = dy_.value_or(0.0), z = dz_.value_or(0.0);
            s_.aabb = AABB{x < 0 ? x : 0.0, y < 0 ? y : 0.0, z < 0 ? z : 0.0, (x < 0 ? 0.0 : x) + 1.0, (y < 0 ? 0.0 : y) + 1.0, (z < 0 ? 0.0 : z) + 1.0};
        }
        s_.range = distance_;
        return std::move(s_);
    }

    StringReader& reader_;
    bool allowSelectors_;
    EntitySelector s_;
    std::optional<DoubleBounds> distance_;
    std::optional<IntBounds> level_;
    std::optional<double> dx_, dy_, dz_;
    std::optional<FloatBounds> rotX_, rotY_;
    InvertableSetOptionState nameOption_, gamemodeOption_, teamOption_, typeOption_;
    bool limited_ = false, sorted_ = false, scores_ = false, advancements_ = false;
};
} // namespace

EntitySelector parseEntitySelector(StringReader& reader, bool allowSelectors) { return EntitySelectorParser(reader, allowSelectors).parse(); }

void EntitySelector::checkPermissions(const CommandSourceStack& source) const {
    if (usesSelector && !source.permissions.entitySelectors()) throw error("argument.entity.selector.not_allowed");
}

std::function<bool(const EntityRef&)> EntitySelector::getPredicate(double x, double y, double z, const std::optional<AABB>& box) const {
    return [this, x, y, z, box](const EntityRef& e) {
        for (const auto& p : predicates)
            if (!p(e)) return false;
        if (box) {
            AABB b = e.boundingBox();
            if (!(box->minX < b.maxX && box->maxX > b.minX && box->minY < b.maxY && box->maxY > b.minY && box->minZ < b.maxZ && box->maxZ > b.minZ)) return false;
        }
        if (range && !range->matchesSqr(e.distanceToSqr(x, y, z))) return false;
        return true;
    };
}

namespace {
EntityRef refOf(const EntityRef& e) { return e; }
EntityRef refOf(ServerPlayer* p) { return EntityRef::of(*p); }
} // namespace

template <class T>
void EntitySelector::sortAndLimit(double x, double y, double z, std::vector<T>& result) const {
    auto distance = [&](const T& e) { return refOf(e).distanceToSqr(x, y, z); };
    if (result.size() > 1) {
        switch (order) {
        case Order::Arbitrary: break;
        case Order::Nearest: std::stable_sort(result.begin(), result.end(), [&](const T& a, const T& b) { return distance(a) < distance(b); }); break;
        case Order::Furthest: std::stable_sort(result.begin(), result.end(), [&](const T& a, const T& b) { return distance(a) > distance(b); }); break;
        case Order::Random: {
            static std::mt19937 random{std::random_device{}()};
            std::shuffle(result.begin(), result.end(), random); // Collections.shuffle
            break;
        }
        }
    }
    if (static_cast<int>(result.size()) > maxResults) result.resize(maxResults);
}

namespace {
// Players and entities of the level in the order they were added (entity ids count up).
std::vector<EntityRef> levelEntities(MinecraftServer& server, ServerLevel& level) {
    std::vector<EntityRef> all;
    for (const auto& p : server.playerList().players())
        if (&p->level() == &level) all.push_back(EntityRef::of(*p));
    for (const auto& e : level.entities())
        if (!e->removed) all.push_back(EntityRef::of(*e, level));
    std::stable_sort(all.begin(), all.end(), [](const EntityRef& a, const EntityRef& b) { return a.id() < b.id(); });
    return all;
}
} // namespace

std::vector<EntityRef> EntitySelector::findEntities(const CommandSourceStack& source) const {
    checkPermissions(source);
    if (!includesEntities) {
        std::vector<EntityRef> out;
        for (ServerPlayer* p : findPlayers(source)) out.push_back(EntityRef::of(*p));
        return out;
    }
    if (playerName) {
        ServerPlayer* p = source.server->playerList().getPlayerByName(*playerName);
        return p ? std::vector<EntityRef>{EntityRef::of(*p)} : std::vector<EntityRef>{};
    }
    if (entityUUID) {
        if (ServerPlayer* p = source.server->playerList().getPlayer(*entityUUID)) return {EntityRef::of(*p)};
        ServerLevel& level = source.server->overworld(); // (getAllLevels: only the overworld exists)
        for (const auto& e : level.entities())
            if (e->uuid() == *entityUUID && !e->removed) return {EntityRef::of(*e, level)};
        return {};
    }
    double x = px.value_or(source.x), y = py.value_or(source.y), z = pz.value_or(source.z);
    std::optional<AABB> box;
    if (aabb) box = AABB{aabb->minX + x, aabb->minY + y, aabb->minZ + z, aabb->maxX + x, aabb->maxY + y, aabb->maxZ + z};
    auto predicate = getPredicate(x, y, z, box);
    if (currentEntity) return source.entity && predicate(source.entity) ? std::vector<EntityRef>{source.entity} : std::vector<EntityRef>{};
    std::vector<EntityRef> result;
    int limit = getResultLimit();
    ServerLevel& level = worldLimited ? *source.level : source.server->overworld();
    for (const EntityRef& e : levelEntities(*source.server, level)) {
        if (static_cast<int>(result.size()) >= limit) break;
        if (type && e.type() != *type) continue; // (the EntityTypeTest)
        if (predicate(e)) result.push_back(e);
    }
    sortAndLimit(x, y, z, result);
    return result;
}

EntityRef EntitySelector::findSingleEntity(const CommandSourceStack& source) const {
    checkPermissions(source);
    auto entities = findEntities(source);
    if (entities.empty()) throw error("argument.entity.notfound.entity");
    if (entities.size() > 1) throw error("argument.entity.toomany");
    return entities[0];
}

std::vector<ServerPlayer*> EntitySelector::findPlayers(const CommandSourceStack& source) const {
    checkPermissions(source);
    auto& list = source.server->playerList();
    if (playerName) {
        ServerPlayer* p = list.getPlayerByName(*playerName);
        return p ? std::vector<ServerPlayer*>{p} : std::vector<ServerPlayer*>{};
    }
    if (entityUUID) {
        ServerPlayer* p = list.getPlayer(*entityUUID);
        return p ? std::vector<ServerPlayer*>{p} : std::vector<ServerPlayer*>{};
    }
    double x = px.value_or(source.x), y = py.value_or(source.y), z = pz.value_or(source.z);
    std::optional<AABB> box;
    if (aabb) box = AABB{aabb->minX + x, aabb->minY + y, aabb->minZ + z, aabb->maxX + x, aabb->maxY + y, aabb->maxZ + z};
    auto predicate = getPredicate(x, y, z, box);
    if (currentEntity) {
        if (source.entity.player && predicate(source.entity)) return {source.entity.player};
        return {};
    }
    int limit = getResultLimit();
    std::vector<ServerPlayer*> result;
    for (const auto& p : list.players()) {
        if (worldLimited && &p->level() != source.level) continue;
        if (predicate(EntityRef::of(*p))) {
            result.push_back(p.get());
            if (static_cast<int>(result.size()) >= limit) return result; // (unsorted, like vanilla)
        }
    }
    sortAndLimit(x, y, z, result);
    return result;
}

ServerPlayer* EntitySelector::findSinglePlayer(const CommandSourceStack& source) const {
    checkPermissions(source);
    auto players = findPlayers(source);
    if (players.size() != 1) throw error("argument.entity.notfound.player");
    return players[0];
}

Component EntitySelector::joinNames(const std::vector<EntityRef>& entities) {
    // ComponentUtils.formatList(entities, getDisplayName): joined with ", " in gray
    if (entities.empty()) return Component::empty();
    if (entities.size() == 1) return entities[0].displayName();
    Component out = Component::empty();
    for (size_t i = 0; i < entities.size(); i++) {
        if (i) {
            Component sep = Component::literal(", ");
            sep.withColor("gray");
            out.append(sep);
        }
        out.append(entities[i].displayName());
    }
    return out;
}

std::any EntityArgument::parse(StringReader& reader, const CommandSourceStack& source) const {
    EntitySelector selector = parseEntitySelector(reader, source.permissions.entitySelectors());
    if (selector.maxResults > 1 && single_) {
        reader.setCursor(0);
        throw errorAt(reader, playersOnly_ ? "argument.player.toomany" : "argument.entity.toomany");
    }
    if (selector.includesEntities && playersOnly_ && !selector.currentEntity) {
        reader.setCursor(0);
        throw errorAt(reader, "argument.player.entities");
    }
    return selector;
}

void EntityArgument::listSuggestions(const CommandContext& context, SuggestionsBuilder& builder) const {
    std::vector<std::string> names;
    for (const auto& p : context.getSource().server->playerList().players()) names.push_back(p->profile().name);
    brigadier::suggest(names, builder);
}

void EntityArgument::writeProperties(ByteWriter& out) const { out.writeByte((single_ ? 1 : 0) | (playersOnly_ ? 2 : 0)); }

EntityRef EntityArgument::getEntity(const CommandContext& context, std::string_view name) {
    return context.get<EntitySelector>(name).findSingleEntity(context.getSource());
}

std::vector<EntityRef> EntityArgument::getEntities(const CommandContext& context, std::string_view name) {
    auto result = getOptionalEntities(context, name);
    if (result.empty()) throw error("argument.entity.notfound.entity");
    return result;
}

std::vector<EntityRef> EntityArgument::getOptionalEntities(const CommandContext& context, std::string_view name) {
    return context.get<EntitySelector>(name).findEntities(context.getSource());
}

ServerPlayer* EntityArgument::getPlayer(const CommandContext& context, std::string_view name) {
    return context.get<EntitySelector>(name).findSinglePlayer(context.getSource());
}

std::vector<ServerPlayer*> EntityArgument::getPlayers(const CommandContext& context, std::string_view name) {
    auto players = context.get<EntitySelector>(name).findPlayers(context.getSource());
    if (players.empty()) throw error("argument.entity.notfound.player");
    return players;
}

std::any GameProfileArgument::parse(StringReader& reader, const CommandSourceStack& source) const {
    Result result;
    if (reader.canRead() && reader.peek() == '@') {
        EntitySelector selector = parseEntitySelector(reader, source.permissions.entitySelectors());
        if (selector.includesEntities) throw errorAt(reader, "argument.player.entities");
        result.selector = std::move(selector);
        return result;
    }
    int start = reader.getCursor();
    while (reader.canRead() && reader.peek() != ' ') reader.skip();
    result.name = reader.getString().substr(start, reader.getCursor() - start);
    return result;
}

std::vector<NameAndId> GameProfileArgument::getGameProfiles(const CommandContext& context, std::string_view name) {
    const Result& r = context.get<Result>(name);
    const CommandSourceStack& source = context.getSource();
    if (r.selector) {
        auto players = r.selector->findPlayers(source);
        if (players.empty()) throw error("argument.entity.notfound.player");
        std::vector<NameAndId> out;
        for (ServerPlayer* p : players) out.push_back({p->profile().id, p->profile().name});
        return out;
    }
    auto profile = source.server->nameToIdCache().get(r.name);
    if (!profile) throw error("argument.player.unknown");
    return {*profile};
}

// ---------------------------------------------------------------- messages

std::any MessageArgument::parse(StringReader& reader, const CommandSourceStack& source) const {
    // Message.parseText(reader, allowSelectors)
    if (reader.getRemainingLength() > 256)
        throw error("argument.message.too_long", {number(int64_t{reader.getRemainingLength()}), number(int64_t{256})});
    Message message;
    message.text = reader.getRemaining();
    if (!source.permissions.entitySelectors()) {
        reader.setCursor(reader.getTotalLength());
        return message;
    }
    int offset = reader.getCursor();
    while (reader.canRead()) {
        if (reader.peek() != '@') {
            reader.skip();
            continue;
        }
        int start = reader.getCursor();
        try {
            EntitySelector selector = EntitySelectorParser(reader, true).parse();
            message.parts.push_back({start - offset, reader.getCursor() - offset, std::move(selector)});
        } catch (const CommandSyntaxException& e) {
            if (e.type != "argument.entity.selector.missing" && e.type != "argument.entity.selector.unknown") throw;
            reader.setCursor(start + 1);
        }
    }
    return message;
}

Component MessageArgument::Message::toComponent(const CommandSourceStack& source, bool allowSelectors) const {
    if (parts.empty() || !allowSelectors) return Component::literal(text);
    Component result = Component::literal(text.substr(0, parts[0].start));
    int readTo = parts[0].start;
    for (const Part& part : parts) {
        Component names = EntitySelector::joinNames(part.selector.findEntities(source));
        if (readTo < part.start) result.append(Component::literal(text.substr(readTo, part.start - readTo)));
        result.append(names);
        readTo = part.end;
    }
    if (readTo < static_cast<int>(text.size())) result.append(Component::literal(text.substr(readTo)));
    return result;
}

Component MessageArgument::getMessage(const CommandContext& context, std::string_view name) {
    const CommandSourceStack& source = context.getSource();
    return context.get<Message>(name).toComponent(source, source.permissions.entitySelectors());
}

void MessageArgument::resolveChatMessage(const CommandContext& context, std::string_view name, const std::function<void(const chat::PlayerChatMessage&)>& task) {
    const Message& message = context.get<Message>(name);
    const CommandSourceStack& sender = context.getSource();
    bool selectors = sender.permissions.entitySelectors();
    Component formatted = message.toComponent(sender, selectors);
    // withUnsignedContent: only when it differs from the plain text (a selector was resolved)
    bool differs = selectors && !message.parts.empty();
    const chat::PlayerChatMessage* signedArgument = nullptr;
    if (sender.signedArguments)
        if (auto it = sender.signedArguments->find(std::string(name)); it != sender.signedArguments->end()) signedArgument = &it->second;
    chat::PlayerChatMessage resolved = signedArgument ? *signedArgument : chat::PlayerChatMessage::system(message.text);
    resolved.unsignedContent = differs ? std::optional<Component>(formatted) : std::nullopt;
    // (ChatDecorator.PLAIN; no text filter, so the signed message passes through unfiltered)
    task(resolved);
}

// ---------------------------------------------------------------- blocks

namespace {
// BlockStateParser (forTesting: tags allowed; allowNbt)
struct ParsedBlock {
    std::optional<uint32_t> block;
    BlockStateId state = 0;
    std::vector<std::pair<std::string, std::string>> properties; // (the Property map, in parse order)
    std::optional<std::string> tag;
    std::vector<std::pair<std::string, std::string>> vagueProperties;
    std::optional<nbt::Compound> nbt;
};

ParsedBlock parseBlock(StringReader& reader, bool forTesting, bool allowNbt) {
    int cursor = reader.getCursor();
    ParsedBlock r;
    std::string id = "minecraft:";
    try {
        if (reader.canRead() && reader.peek() == '#') {
            if (!forTesting) throw errorAt(reader, "argument.block.tag.disallowed");
            int start = reader.getCursor();
            reader.expect('#');
            std::string tag = readIdentifier(reader);
            if (!registries::tagExists("minecraft:block", tag)) {
                reader.setCursor(start);
                throw errorAt(reader, "arguments.block.tag.unknown", {lit(tag)});
            }
            r.tag = tag;
            if (reader.canRead() && reader.peek() == '[') {
                // readVagueProperties
                reader.skip();
                int valueStart = -1;
                reader.skipWhitespace();
                bool closed = false;
                while (reader.canRead() && reader.peek() != ']') {
                    reader.skipWhitespace();
                    int keyStart = reader.getCursor();
                    std::string key = reader.readString();
                    bool dup = std::any_of(r.vagueProperties.begin(), r.vagueProperties.end(), [&](const auto& p) { return p.first == key; });
                    if (dup) {
                        reader.setCursor(keyStart);
                        throw errorAt(reader, "argument.block.property.duplicate", {lit(key), lit(id)});
                    }
                    reader.skipWhitespace();
                    if (!reader.canRead() || reader.peek() != '=') {
                        reader.setCursor(keyStart);
                        throw errorAt(reader, "argument.block.property.novalue", {lit(key), lit(id)});
                    }
                    reader.skip();
                    reader.skipWhitespace();
                    valueStart = reader.getCursor();
                    std::string value = reader.readString();
                    r.vagueProperties.emplace_back(key, value);
                    reader.skipWhitespace();
                    if (reader.canRead()) {
                        valueStart = -1;
                        if (reader.peek() != ',') {
                            if (reader.peek() != ']') throw errorAt(reader, "argument.block.property.unclosed");
                            break;
                        }
                        reader.skip();
                    }
                }
                if (reader.canRead()) {
                    reader.skip();
                    closed = true;
                }
                if (!closed) {
                    if (valueStart >= 0) reader.setCursor(valueStart);
                    throw errorAt(reader, "argument.block.property.unclosed");
                }
            }
        } else {
            int start = reader.getCursor();
            id = readIdentifier(reader);
            r.block = blocks::byName(id);
            if (!r.block) {
                reader.setCursor(start);
                throw errorAt(reader, "argument.block.id.invalid", {lit(id)});
            }
            r.state = blocks::defaultState(*r.block);
            if (reader.canRead() && reader.peek() == '[') {
                // readProperties
                reader.skip();
                reader.skipWhitespace();
                bool closed = false;
                while (reader.canRead() && reader.peek() != ']') {
                    reader.skipWhitespace();
                    int keyStart = reader.getCursor();
                    std::string key = reader.readString();
                    const gen::BlockProperty* property = findProperty(*r.block, key);
                    if (!property) {
                        reader.setCursor(keyStart);
                        throw errorAt(reader, "argument.block.property.unknown", {lit(id), lit(key)});
                    }
                    if (std::any_of(r.properties.begin(), r.properties.end(), [&](const auto& p) { return p.first == key; })) {
                        reader.setCursor(keyStart);
                        throw errorAt(reader, "argument.block.property.duplicate", {lit(key), lit(id)});
                    }
                    reader.skipWhitespace();
                    if (!reader.canRead() || reader.peek() != '=') throw errorAt(reader, "argument.block.property.novalue", {lit(key), lit(id)});
                    reader.skip();
                    reader.skipWhitespace();
                    int valueStart = reader.getCursor();
                    std::string value = reader.readString();
                    if (!validValue(*property, value)) {
                        reader.setCursor(valueStart);
                        throw errorAt(reader, "argument.block.property.invalid", {lit(id), lit(value), lit(key)});
                    }
                    r.state = blocks::setValue(r.state, key, value);
                    r.properties.emplace_back(key, value);
                    reader.skipWhitespace();
                    if (reader.canRead()) {
                        if (reader.peek() != ',') {
                            if (reader.peek() != ']') throw errorAt(reader, "argument.block.property.unclosed");
                            break;
                        }
                        reader.skip();
                    }
                }
                if (reader.canRead()) {
                    reader.skip();
                    closed = true;
                }
                if (!closed) throw errorAt(reader, "argument.block.property.unclosed");
            }
        }
        if (allowNbt && reader.canRead() && reader.peek() == '{') r.nbt = readCompoundArgument(reader);
    } catch (const CommandSyntaxException&) {
        reader.setCursor(cursor);
        throw;
    }
    return r;
}
} // namespace

std::any BlockStateArgument::parse(StringReader& reader, const CommandSourceStack&) const {
    ParsedBlock b = parseBlock(reader, false, true);
    return BlockInput{b.state, b.properties, b.nbt};
}

std::any BlockPredicateArgument::parse(StringReader& reader, const CommandSourceStack&) const {
    ParsedBlock b = parseBlock(reader, true, true);
    BlockPredicate p;
    if (b.tag) {
        p.tag = b.tag;
        p.properties = b.vagueProperties;
    } else {
        p.block = b.block;
        p.properties = b.properties;
        for (auto& [k, v] : p.properties) v = std::string(*blocks::getValue(b.state, k));
    }
    p.nbt = b.nbt;
    return p;
}

bool BlockInput::place(ServerLevel& level, BlockPos pos, int flags) const {
    BlockStateId placed = (flags & ServerLevel::kUpdateKnownShape) ? this->state : block_behaviour::updateFromNeighbourShapes(this->state, level, pos);
    if (blocks::isAir(placed)) placed = this->state;
    if (placed != this->state) // overwriteWithDefinedProperties
        for (const auto& [k, v] : definedProperties) placed = blocks::setValue(placed, k, v);
    bool affected = level.setBlock(pos, placed, flags);
    if (tag) {
        LevelChunk& chunk = level.getChunk({pos.x >> 4, pos.z >> 4});
        if (auto* be = chunk.blockEntityAt(pos.x, pos.y, pos.z)) {
            // loadWithComponents: the given fields over the saved ones
            nbt::Compound before = be->tag;
            for (const auto& [k, v] : tag->entries) {
                if (k == "id" || k == "x" || k == "y" || k == "z") continue;
                be->tag.put(k, v);
            }
            if (!(nbt::Tag(be->tag) == nbt::Tag(before))) {
                affected = true;
                chunk.dirty = true;
                level.blockEntityChanged(pos);
            }
        }
    }
    return affected;
}

bool BlockPredicate::test(ServerLevel& level, BlockPos pos) const {
    BlockStateId state = level.getBlockState(pos);
    uint32_t b = blocks::blockOf(state);
    if (block ? b != *block : !blocks::hasTag(b, *tag)) return false;
    for (const auto& [k, v] : properties) {
        auto value = blocks::getValue(state, k);
        if (!value) return false;
        if (tag) { // (a vague property that isn't one of the block's values fails)
            const gen::BlockProperty* property = findProperty(b, k);
            if (!property || !validValue(*property, v)) return false;
        }
        if (*value != v) return false;
    }
    if (!nbt) return true;
    LevelChunk& chunk = level.getChunk({pos.x >> 4, pos.z >> 4});
    auto* be = chunk.blockEntityAt(pos.x, pos.y, pos.z);
    if (!be) return false;
    nbt::Tag expected(*nbt), actual(be->tag); // (saveWithFullMetadata: the stored tag has id and position)
    return compareNbt(&expected, &actual, true);
}

} // namespace mc::commands
