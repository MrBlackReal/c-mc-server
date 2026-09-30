#pragma once

// The argument types the commands use (brigadier's own and net.minecraft.commands.arguments),
// with their parsed values and the ArgumentTypeInfo network properties.

#include <functional>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "core/direction.hpp"
#include "nbt/tag.hpp"
#include "server/commands/brigadier.hpp"
#include "server/commands/command_source.hpp"
#include "server/players/user_lists.hpp"
#include "world/level/block/block_state.hpp"

namespace mc {
enum class GameType : int;
}

namespace mc::commands {

using brigadier::ArgumentType;
using brigadier::CommandContext;
using brigadier::StringReader;
using brigadier::SuggestionsBuilder;

// ---------------------------------------------------------------- brigadier types

class BoolArgument : public ArgumentType {
public:
    std::any parse(StringReader& reader, const CommandSourceStack&) const override { return reader.readBoolean(); }
    void listSuggestions(const CommandContext&, SuggestionsBuilder& builder) const override;
    std::string_view parserId() const override { return "brigadier:bool"; }
};

class IntegerArgument : public ArgumentType {
public:
    explicit IntegerArgument(int32_t min = std::numeric_limits<int32_t>::min(), int32_t max = std::numeric_limits<int32_t>::max()) : min_(min), max_(max) {}
    std::any parse(StringReader& reader, const CommandSourceStack&) const override;
    std::string_view parserId() const override { return "brigadier:integer"; }
    void writeProperties(ByteWriter& out) const override;

private:
    int32_t min_, max_;
};

class FloatArgument : public ArgumentType {
public:
    explicit FloatArgument(float min = -std::numeric_limits<float>::max(), float max = std::numeric_limits<float>::max()) : min_(min), max_(max) {}
    std::any parse(StringReader& reader, const CommandSourceStack&) const override;
    std::string_view parserId() const override { return "brigadier:float"; }
    void writeProperties(ByteWriter& out) const override;

private:
    float min_, max_;
};

// StringArgumentType: word, quotable phrase or the greedy rest of the input.
class StringArgument : public ArgumentType {
public:
    enum class Type { SingleWord = 0, QuotablePhrase = 1, GreedyPhrase = 2 };
    explicit StringArgument(Type type) : type_(type) {}
    std::any parse(StringReader& reader, const CommandSourceStack&) const override;
    std::string_view parserId() const override { return "brigadier:string"; }
    void writeProperties(ByteWriter& out) const override;

private:
    Type type_;
};

// ---------------------------------------------------------------- simple minecraft types

class GameModeArgument : public ArgumentType {
public:
    std::any parse(StringReader& reader, const CommandSourceStack&) const override;
    void listSuggestions(const CommandContext&, SuggestionsBuilder& builder) const override;
    std::string_view parserId() const override { return "minecraft:gamemode"; }
};

class EntityAnchorArgument : public ArgumentType {
public:
    std::any parse(StringReader& reader, const CommandSourceStack&) const override;
    void listSuggestions(const CommandContext&, SuggestionsBuilder& builder) const override;
    std::string_view parserId() const override { return "minecraft:entity_anchor"; }
};

// IdentifierArgument (minecraft:resource_location): a namespaced id.
class IdentifierArgument : public ArgumentType {
public:
    std::any parse(StringReader& reader, const CommandSourceStack&) const override;
    std::string_view parserId() const override { return "minecraft:resource_location"; }
};
// Identifier.read: the id with "minecraft:" filled in.
std::string readIdentifier(StringReader& reader);

// ResourceArgument: an entry of a registry, as its full id.
class ResourceArgument : public ArgumentType {
public:
    explicit ResourceArgument(std::string registry) : registry_(std::move(registry)) {}
    std::any parse(StringReader& reader, const CommandSourceStack&) const override;
    void listSuggestions(const CommandContext&, SuggestionsBuilder& builder) const override;
    std::string_view parserId() const override { return "minecraft:resource"; }
    void writeProperties(ByteWriter& out) const override;

private:
    std::string registry_;
};

// TimeArgument: ticks, with d/s/t units.
class TimeArgument : public ArgumentType {
public:
    explicit TimeArgument(int32_t minimum = 0) : minimum_(minimum) {}
    std::any parse(StringReader& reader, const CommandSourceStack&) const override;
    std::string_view parserId() const override { return "minecraft:time"; }
    void writeProperties(ByteWriter& out) const override;

private:
    int32_t minimum_;
};

// ---------------------------------------------------------------- coordinates

struct WorldCoordinate {
    bool relative = false;
    double value = 0;
    double get(double original) const { return relative ? value + original : value; }
};

// Coordinates: world coordinates (absolute or ~relative) or local ^left ^up ^forwards ones.
struct Coordinates {
    bool local = false;
    WorldCoordinate x, y, z; // (local: left, up, forwards as values)
    void getPosition(const CommandSourceStack& source, double& ox, double& oy, double& oz) const;
    // getRotation: Vec2(xRot, yRot)
    void getRotation(const CommandSourceStack& source, float& xRot, float& yRot) const;
    BlockPos getBlockPos(const CommandSourceStack& source) const;
    bool isXRelative() const { return local || x.relative; }
    bool isYRelative() const { return local || y.relative; }
    bool isZRelative() const { return local || z.relative; }
};

class Vec3Argument : public ArgumentType {
public:
    explicit Vec3Argument(bool centerCorrect = true) : centerCorrect_(centerCorrect) {}
    std::any parse(StringReader& reader, const CommandSourceStack&) const override;
    std::string_view parserId() const override { return "minecraft:vec3"; }

private:
    bool centerCorrect_;
};

class BlockPosArgument : public ArgumentType {
public:
    std::any parse(StringReader& reader, const CommandSourceStack&) const override;
    std::string_view parserId() const override { return "minecraft:block_pos"; }
    // getLoadedBlockPos: in a loaded chunk and inside the world's height.
    static BlockPos getLoadedBlockPos(const CommandContext& context, std::string_view name);
};

class RotationArgument : public ArgumentType {
public:
    std::any parse(StringReader& reader, const CommandSourceStack&) const override;
    std::string_view parserId() const override { return "minecraft:rotation"; }
};

// ---------------------------------------------------------------- entity selectors

struct DoubleBounds {
    std::optional<double> min, max;
    bool matchesSqr(double valueSqr) const {
        if (min && *min * *min > valueSqr) return false;
        return !max || !(*max * *max < valueSqr);
    }
};
struct IntBounds {
    std::optional<int32_t> min, max;
    bool matches(int32_t v) const { return !(min && *min > v) && (!max || *max >= v); }
};
struct FloatBounds {
    std::optional<float> min, max;
};

class EntitySelector {
public:
    enum class Order { Arbitrary, Nearest, Furthest, Random };

    int maxResults = 0;
    bool includesEntities = false;
    bool worldLimited = false;
    std::vector<std::function<bool(const EntityRef&)>> predicates;
    std::optional<DoubleBounds> range;
    std::optional<double> px, py, pz; // x=, y=, z= (else the source position)
    std::optional<AABB> aabb;         // relative to the position
    Order order = Order::Arbitrary;
    bool currentEntity = false;
    std::optional<std::string> playerName;
    std::optional<UUID> entityUUID;
    std::optional<std::string> type; // limitToType
    bool usesSelector = false;

    EntityRef findSingleEntity(const CommandSourceStack& source) const;
    std::vector<EntityRef> findEntities(const CommandSourceStack& source) const;
    ServerPlayer* findSinglePlayer(const CommandSourceStack& source) const;
    std::vector<ServerPlayer*> findPlayers(const CommandSourceStack& source) const;
    // EntitySelector.joinNames: the display names as a list ("a, b and c" style).
    static Component joinNames(const std::vector<EntityRef>& entities);

private:
    void checkPermissions(const CommandSourceStack& source) const;
    std::function<bool(const EntityRef&)> getPredicate(double x, double y, double z, const std::optional<AABB>& box) const;
    template <class T>
    void sortAndLimit(double x, double y, double z, std::vector<T>& result) const;
    int getResultLimit() const { return order == Order::Arbitrary ? maxResults : std::numeric_limits<int>::max(); }
};

// EntitySelectorParser.parse: "@a[...]", a player name or an entity UUID.
EntitySelector parseEntitySelector(StringReader& reader, bool allowSelectors);

class EntityArgument : public ArgumentType {
public:
    EntityArgument(bool single, bool playersOnly) : single_(single), playersOnly_(playersOnly) {}
    static std::shared_ptr<EntityArgument> entity() { return std::make_shared<EntityArgument>(true, false); }
    static std::shared_ptr<EntityArgument> entities() { return std::make_shared<EntityArgument>(false, false); }
    static std::shared_ptr<EntityArgument> player() { return std::make_shared<EntityArgument>(true, true); }
    static std::shared_ptr<EntityArgument> players() { return std::make_shared<EntityArgument>(false, true); }

    std::any parse(StringReader& reader, const CommandSourceStack& source) const override;
    void listSuggestions(const CommandContext& context, SuggestionsBuilder& builder) const override;
    std::string_view parserId() const override { return "minecraft:entity"; }
    void writeProperties(ByteWriter& out) const override;

    static EntityRef getEntity(const CommandContext& context, std::string_view name);
    static std::vector<EntityRef> getEntities(const CommandContext& context, std::string_view name);
    static std::vector<EntityRef> getOptionalEntities(const CommandContext& context, std::string_view name);
    static ServerPlayer* getPlayer(const CommandContext& context, std::string_view name);
    static std::vector<ServerPlayer*> getPlayers(const CommandContext& context, std::string_view name);

private:
    bool single_, playersOnly_;
};

// GameProfileArgument: players by selector or by name (looked up in the user cache).
class GameProfileArgument : public ArgumentType {
public:
    struct Result {
        std::optional<EntitySelector> selector;
        std::string name;
    };
    std::any parse(StringReader& reader, const CommandSourceStack& source) const override;
    std::string_view parserId() const override { return "minecraft:game_profile"; }
    static std::vector<NameAndId> getGameProfiles(const CommandContext& context, std::string_view name);
};

// MessageArgument: the rest of the input, with @-selectors resolved to names (signed as chat).
class MessageArgument : public ArgumentType {
public:
    struct Part {
        int start, end;
        EntitySelector selector;
    };
    struct Message {
        std::string text;
        std::vector<Part> parts;
        Component toComponent(const CommandSourceStack& source, bool allowSelectors) const;
    };
    std::any parse(StringReader& reader, const CommandSourceStack& source) const override;
    std::string_view parserId() const override { return "minecraft:message"; }
    bool isSigned() const override { return true; }

    static Component getMessage(const CommandContext& context, std::string_view name);
    // resolveChatMessage: the signed argument (if the command came signed) or a disguised one.
    static void resolveChatMessage(const CommandContext& context, std::string_view name, const std::function<void(const chat::PlayerChatMessage&)>& task);
};

// ---------------------------------------------------------------- blocks

// BlockInput: a state, the properties the input named, and block entity data.
struct BlockInput {
    BlockStateId state = 0;
    std::vector<std::pair<std::string, std::string>> definedProperties;
    std::optional<nbt::Compound> tag;
    // BlockInput.place: shaped by the neighbours (unless flag 16), the named properties kept.
    bool place(ServerLevel& level, BlockPos pos, int flags) const;
};

// BlockPredicateArgument.Result: a block (or #tag) with properties and NBT to match.
struct BlockPredicate {
    std::optional<uint32_t> block;
    std::optional<std::string> tag;
    std::vector<std::pair<std::string, std::string>> properties;
    std::optional<nbt::Compound> nbt;
    bool test(ServerLevel& level, BlockPos pos) const;
};

class BlockStateArgument : public ArgumentType {
public:
    std::any parse(StringReader& reader, const CommandSourceStack&) const override;
    std::string_view parserId() const override { return "minecraft:block_state"; }
};

class BlockPredicateArgument : public ArgumentType {
public:
    std::any parse(StringReader& reader, const CommandSourceStack&) const override;
    std::string_view parserId() const override { return "minecraft:block_predicate"; }
};

} // namespace mc::commands
