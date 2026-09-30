#pragma once

#include <optional>
#include <string>
#include <vector>

#include "nbt/tag.hpp"

namespace mc {

// Subset of net.minecraft.network.chat.Component: literal text or a translatable key, with
// optional color and children. Serialises to NBT (play/configuration) or JSON (login/status).
struct Component {
    std::string text;
    std::optional<std::string> translate;
    std::vector<Component> with;
    std::optional<std::string> fallback; // TranslatableContents.fallback
    std::optional<std::string> color;
    std::optional<bool> bold, italic, underlined, strikethrough, obfuscated;
    std::vector<Component> extra;
    // Style events (NBT form only): click_event, hover_event as their codec output, insertion.
    std::optional<nbt::Compound> clickEvent, hoverEvent;
    std::optional<std::string> insertion;

    static Component literal(std::string s) {
        Component c;
        c.text = std::move(s);
        return c;
    }
    static Component empty() { return {}; }
    static Component translatable(std::string key, std::vector<Component> args = {}) {
        Component c;
        c.translate = std::move(key);
        c.with = std::move(args);
        return c;
    }
    Component& withColor(std::string c) {
        color = std::move(c);
        return *this;
    }
    // ChatFormatting styles: RED, GRAY, ITALIC, UNDERLINE, ... (withStyle(ChatFormatting...)).
    Component& withItalic(bool v = true) {
        italic = v;
        return *this;
    }
    Component& withUnderlined(bool v = true) {
        underlined = v;
        return *this;
    }
    Component& withBold(bool v = true) {
        bold = v;
        return *this;
    }
    Component& append(Component c) {
        extra.push_back(std::move(c));
        return *this;
    }

    nbt::Tag toNbt() const;
    // ComponentSerialization.CODEC (NBT form) for the parts this struct models: text or a
    // translation (with arguments and fallback), styles, events, insertion and children.
    static Component fromNbt(const nbt::Tag& tag);
    std::string toJson() const;
    // Component.getString: the plain text, with translations resolved from en_us (Language).
    std::string getString() const;
};

std::string jsonEscape(std::string_view s);

} // namespace mc
