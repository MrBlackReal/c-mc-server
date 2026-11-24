// Signs: SignBlock.useItemOn / useWithoutItem / setPlacedBy, SignBlockEntity's text editing and
// ServerGamePacketListenerImpl.handleSignUpdate. The sign's text lives in its block entity NBT
// ({front_text, back_text, is_waxed}); lines are text components in their saved form.
#include <cmath>
#include <numbers>

#include "core/registries.hpp"
#include "generated/packet_ids.hpp"
#include "server/commands/commands.hpp"
#include "server/minecraft_server.hpp"
#include "server/network/listeners.hpp"
#include "util/log.hpp"
#include "world/item/item_logic.hpp"
#include "world/level/block/block_state.hpp"
#include "world/level/block/entity/block_entity_components.hpp"

namespace mc {

namespace cb = packets::play_cb;
namespace bs = blocks;

namespace {

// A text component's own style and contents: the first element of a list is the component.
const nbt::Tag& component(const nbt::Tag& t) {
    if (const nbt::List* l = t.asList(); l && !l->items.empty()) return l->items[0];
    return t;
}

// Component.getContents() instanceof PlainTextContents
bool isPlainText(const nbt::Tag& line) {
    const nbt::Tag& c = component(line);
    if (c.asString()) return true;
    const nbt::Compound* comp = c.asCompound();
    if (!comp) return false;
    if (const nbt::Tag* type = comp->get("type")) return type->asString() && *type->asString() == "text";
    return comp->contains("text");
}

// Component.getString().isEmpty() (non-literal contents count as text).
bool isEmptyText(const nbt::Tag& t) {
    if (const std::string* s = t.asString()) return s->empty();
    if (const nbt::List* l = t.asList()) {
        for (const auto& e : l->items)
            if (!isEmptyText(e)) return false;
        return true;
    }
    const nbt::Compound* comp = t.asCompound();
    if (!comp) return true;
    if (!isPlainText(t) || !comp->getString("text").empty()) return false;
    if (const nbt::List* extra = comp->getList("extra"))
        for (const auto& e : extra->items)
            if (!isEmptyText(e)) return false;
    return true;
}

// Component.literal(text).setStyle(original's style), saved (a plain string when unstyled).
nbt::Tag withStyleOf(const nbt::Tag& original, std::string text) {
    nbt::Compound out;
    if (const nbt::Compound* comp = component(original).asCompound())
        for (const char* key : {"color", "shadow_color", "bold", "italic", "underlined", "strikethrough", "obfuscated", "click_event",
                                "hover_event", "insertion", "font"})
            if (const nbt::Tag* v = comp->get(key)) out.put(key, *v);
    if (out.empty()) return text;
    out.entries.insert(out.entries.begin(), {"text", std::move(text)});
    return out;
}

const nbt::List* messages(const nbt::Compound& signText) { return signText.getList("messages"); }

// SignText.hasEditableText / hasMessage (without text filtering).
bool hasEditableText(const nbt::Compound& signText) {
    if (const nbt::List* m = messages(signText))
        for (const auto& line : m->items)
            if (!isPlainText(line)) return false;
    return true;
}
bool hasMessage(const nbt::Compound& signText) {
    if (const nbt::List* m = messages(signText))
        for (const auto& line : m->items)
            if (!isEmptyText(line)) return true;
    return false;
}

nbt::Compound& textSlot(nbt::Compound& be, bool front) {
    const char* key = front ? "front_text" : "back_text";
    if (!be.getCompound(key)) be.put(key, block_entity::emptySignText());
    return *be.getCompound(key);
}

int32_t soundId(std::string_view name) { return registries::id("minecraft:sound_event", name).value_or(-1); }

} // namespace

nbt::Compound* ServerGamePacketListener::signAt(BlockPos pos) {
    LevelChunk* chunk = player_.level().getChunkIfLoaded({pos.x >> 4, pos.z >> 4});
    if (!chunk) return nullptr;
    auto* be = chunk->blockEntityAt(pos.x, pos.y, pos.z);
    if (!be || bs::blockOf(player_.level().getBlockState(pos)) != be->blockAtLoad) return nullptr;
    std::string id = be->tag.getString("id");
    return id == "minecraft:sign" || id == "minecraft:hanging_sign" ? &be->tag : nullptr;
}

// SignBlockEntity.getSlotPlayerIsFacing
bool ServerGamePacketListener::isFacingFrontText(BlockPos pos, BlockStateId state) {
    uint32_t block = bs::blockOf(state);
    double cx = 0.5, cz = 0.5; // SignBlock.getSignHitboxCenterPosition
    float signYRot;
    if (bs::isInstance(block, "WallSignBlock") || bs::isInstance(block, "WallHangingSignBlock")) {
        Direction facing = direction::byName(bs::getValue(state, "facing").value_or("north"));
        signYRot = direction::toYRot(facing);
        if (bs::isInstance(block, "WallSignBlock")) {
            // The centre of SHAPES.get(facing): a 2/16 thick board against the wall behind it.
            cx = 0.5 - direction::stepX(facing) * 0.4375;
            cz = 0.5 - direction::stepZ(facing) * 0.4375;
        }
    } else {
        signYRot = static_cast<float>(std::stoi(std::string(bs::getValue(state, "rotation").value_or("0")))) * 22.5f; // RotationSegment.convertToDegrees
    }
    double xd = player_.x - (pos.x + cx), zd = player_.z - (pos.z + cz);
    float playerYRot = static_cast<float>(std::atan2(zd, xd) * 57.2957763671875) - 90.0f;
    float diff = std::fmod(playerYRot - signYRot, 360.0f); // Mth.degreesDifferenceAbs
    if (diff >= 180.0f) diff -= 360.0f;
    if (diff < -180.0f) diff += 360.0f;
    return std::abs(diff) <= 90.0f;
}

// SignBlock.openTextEdit + ServerPlayer.openTextEdit
void ServerGamePacketListener::openTextEdit(BlockPos pos, bool front) {
    player_.level().setSignEditor(pos, player_.id());
    sendBlockUpdate(pos);
    Packet p(cb::OPEN_SIGN_EDITOR);
    p.writeBlockPos(pos.x, pos.y, pos.z);
    p.writeBool(front);
    conn_.send(p);
}

// SignBlock.setPlacedBy: placing a sign opens the editor for its front.
void ServerGamePacketListener::signPlaced(BlockPos pos) {
    nbt::Compound* sign = signAt(pos);
    if (sign && !sign->getBool("is_waxed") && hasEditableText(textSlot(*sign, true))) openTextEdit(pos, true);
}

// SignBlockEntity.executeClickCommandsIfPresent
bool ServerGamePacketListener::executeSignClickCommands(BlockPos pos, bool front) {
    nbt::Compound* sign = signAt(pos);
    if (!sign) return false;
    bool allowOp = sign->getBool("allow_op_features");
    std::vector<std::string> commandsToRun;
    bool any = false;
    if (const nbt::List* m = messages(textSlot(*sign, front))) {
        for (const auto& line : m->items) {
            const nbt::Compound* comp = component(line).asCompound();
            const nbt::Compound* click = comp ? comp->getCompound("click_event") : nullptr;
            if (!click) continue;
            std::string action = click->getString("action");
            if (action == "run_command") {
                any = true;
                if (allowOp) commandsToRun.push_back(click->getString("command"));
            } else if (action == "show_dialog" || action == "custom") {
                any = true; // (dialogs and custom actions aren't supported yet)
            }
        }
    }
    // SignBlockEntity.createCommandSourceStack: no output, at the sign's centre, game master level,
    // named after the player.
    commands::CommandSourceStack source = commands::CommandSourceStack::forPlayer(server_, player_);
    source.x = pos.x + 0.5, source.y = pos.y + 0.5, source.z = pos.z + 0.5;
    source.rotX = source.rotY = 0;
    source.permissions = commands::PermissionSet::forLevel(commands::kLevelGamemasters);
    source.output = commands::Output::Null;
    source.outputPlayer = nullptr;
    for (const std::string& c : commandsToRun) server_.commands().performPrefixedCommand(source, c);
    if (!allowOp && any) sendOverlay(Component::translatable("sign.click_actions_disabled").withColor("red"));
    return any;
}

// SignBlock.useWithoutItem
UseResult ServerGamePacketListener::useSignWithoutItem(BlockPos pos, BlockStateId state) {
    nbt::Compound* sign = signAt(pos);
    if (!sign) return UseResult::Pass;
    bool front = isFacingFrontText(pos, state);
    bool executed = executeSignClickCommands(pos, front);
    sign = signAt(pos); // (a command may have changed the world)
    if (!sign) return UseResult::SuccessServer;
    ServerLevel& level = player_.level();
    if (sign->getBool("is_waxed")) {
        static const int32_t kSign = soundId("minecraft:block.sign.waxed_interact_fail");
        static const int32_t kHanging = soundId("minecraft:block.hanging_sign.waxed_interact_fail");
        level.playBlockSound(-1, pos, sign->getString("id") == "minecraft:hanging_sign" ? kHanging : kSign, 1.0f, 1.0f);
        return UseResult::SuccessServer;
    }
    if (executed) return UseResult::SuccessServer;
    auto editor = level.signEditor(pos);
    bool otherPlayerEditing = editor && *editor != player_.id();
    if (!otherPlayerEditing && canModifyWorld(player_.gameMode) && hasEditableText(textSlot(*sign, front))) {
        openTextEdit(pos, front);
        return UseResult::SuccessServer;
    }
    return UseResult::Pass;
}

// SignBlock.useItemOn: dyes, (glow) ink sacs and honeycomb (SignApplicator).
UseResult ServerGamePacketListener::useItemOnSign(ItemStack& stack, BlockPos pos, BlockStateId state) {
    nbt::Compound* sign = signAt(pos);
    if (!sign) return UseResult::Pass;
    std::string_view cls = stack.isEmpty() ? std::string_view{} : stack.info().itemClass;
    bool applicator = cls == "DyeItem" || cls == "GlowInkSacItem" || cls == "InkSacItem" || cls == "HoneycombItem";
    auto editor = player_.level().signEditor(pos);
    bool otherPlayerEditing = editor && *editor != player_.id();
    if (!applicator || !canModifyWorld(player_.gameMode) || sign->getBool("is_waxed") || otherPlayerEditing) return UseResult::TryEmptyHand;

    bool front = isFacingFrontText(pos, state);
    nbt::Compound& text = textSlot(*sign, front);
    if (cls != "HoneycombItem" && !hasMessage(text)) return UseResult::TryEmptyHand; // SignApplicator.canApplyToSign
    ServerLevel& level = player_.level();
    bool changed = false;
    if (cls == "DyeItem") {
        auto patched = items::componentValue(stack, "minecraft:dye");
        std::string dye = patched && patched->asString() ? *patched->asString() : std::string(stack.info().dye);
        if (!dye.empty() && text.getString("color", "black") != dye) {
            text.put("color", dye);
            static const int32_t kDye = soundId("minecraft:item.dye.use");
            level.playBlockSound(-1, pos, kDye, 1.0f, 1.0f);
            changed = true;
        }
    } else if (cls == "GlowInkSacItem" || cls == "InkSacItem") {
        bool glow = cls == "GlowInkSacItem";
        if (text.getBool("has_glowing_text") != glow) {
            text.put("has_glowing_text", nbt::byteTag(glow));
            static const int32_t kGlow = soundId("minecraft:item.glow_ink_sac.use"), kInk = soundId("minecraft:item.ink_sac.use");
            level.playBlockSound(-1, pos, glow ? kGlow : kInk, 1.0f, 1.0f);
            changed = true;
        }
    } else { // HoneycombItem: SignBlockEntity.setWaxed
        sign->put("is_waxed", nbt::byteTag(true));
        level.levelEvent(3003, pos, 0);
        static const int32_t kWax = soundId("minecraft:item.honeycomb.wax_on");
        level.playBlockSound(-1, pos, kWax, 1.0f, 1.0f);
        changed = true;
    }
    if (!changed) return UseResult::TryEmptyHand;
    level.getChunk({pos.x >> 4, pos.z >> 4}).dirty = true;
    level.sendBlockUpdated(pos);
    executeSignClickCommands(pos, front);
    if (!player_.hasInfiniteMaterials()) { // ItemStack.consume
        stack.shrink(1);
        stack.clearIfEmpty();
    }
    return UseResult::Success;
}

// ServerGamePacketListenerImpl.handleSignUpdate + SignBlockEntity.updateSignText
void ServerGamePacketListener::handleSignUpdate(ByteReader& in) {
    BlockPos pos = BlockPos::fromLong(in.readLong());
    std::string lines[4];
    for (auto& line : lines) line = in.readUtf(384);
    bool front = in.readVarInt() == 1; // SignTextSlot: BACK 0, FRONT 1
    for (auto& line : lines) { // ChatFormatting.stripFormatting: "(?i)§[0-9A-FK-OR]"
        std::string out;
        for (size_t i = 0; i < line.size(); i++) {
            if (line.compare(i, 2, "§") == 0 && i + 2 < line.size()) {
                char c = static_cast<char>(std::tolower(static_cast<unsigned char>(line[i + 2])));
                if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'k' && c <= 'o') || c == 'r') {
                    i += 2;
                    continue;
                }
            }
            out += line[i];
        }
        line = std::move(out);
    }

    ServerLevel& level = player_.level();
    nbt::Compound* sign = signAt(pos);
    if (!sign) return;
    auto editor = level.signEditor(pos);
    if (sign->getBool("is_waxed") || editor != player_.id()) {
        log::warn("Player {} just tried to change non-editable sign", player_.profile().name);
        return;
    }
    // updateMessages without text filtering: the filtered lines equal the raw ones.
    nbt::Compound& text = textSlot(*sign, front);
    const nbt::List* old = messages(text);
    nbt::List updated{nbt::TagType::End, {}};
    for (int i = 0; i < 4; i++) {
        const nbt::Tag* original = old && i < static_cast<int>(old->items.size()) ? &old->items[i] : nullptr;
        updated.items.push_back(original ? withStyleOf(*original, lines[i]) : nbt::Tag(lines[i]));
    }
    text.put("messages", std::move(updated));
    text.remove("filtered_messages");
    level.setSignEditor(pos, std::nullopt);
    level.getChunk({pos.x >> 4, pos.z >> 4}).dirty = true;
    level.sendBlockUpdated(pos);
}

} // namespace mc
