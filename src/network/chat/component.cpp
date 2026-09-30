#include "network/chat/component.hpp"

#include <format>

#include "generated/behaviour.hpp"
#include "util/json.hpp"

namespace mc {

std::string jsonEscape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) out += std::format("\\u{:04x}", c);
            else out += static_cast<char>(c);
        }
    }
    out += '"';
    return out;
}

nbt::Tag Component::toNbt() const {
    bool styled = color || bold || italic || underlined || strikethrough || obfuscated;
    bool plain = !translate && !styled && extra.empty() && !clickEvent && !hoverEvent && !insertion;
    if (plain) return nbt::Tag(text); // ComponentSerialization encodes bare text as a string tag
    nbt::Compound c;
    if (translate) {
        c.put("translate", *translate);
        if (!with.empty()) {
            nbt::List args{nbt::TagType::Compound, {}};
            for (const auto& w : with) {
                nbt::Tag t = w.toNbt();
                if (t.type() == nbt::TagType::String) t = nbt::Compound{}.put("text", t);
                args.items.push_back(std::move(t));
            }
            c.put("with", std::move(args));
        }
        if (fallback) c.put("fallback", *fallback);
    } else {
        c.put("text", text);
    }
    if (color) c.put("color", *color);
    if (bold) c.put("bold", static_cast<int8_t>(*bold));
    if (italic) c.put("italic", static_cast<int8_t>(*italic));
    if (underlined) c.put("underlined", static_cast<int8_t>(*underlined));
    if (strikethrough) c.put("strikethrough", static_cast<int8_t>(*strikethrough));
    if (obfuscated) c.put("obfuscated", static_cast<int8_t>(*obfuscated));
    if (clickEvent) c.put("click_event", *clickEvent);
    if (hoverEvent) c.put("hover_event", *hoverEvent);
    if (insertion) c.put("insertion", *insertion);
    if (!extra.empty()) {
        nbt::List list{nbt::TagType::Compound, {}};
        for (const auto& e : extra) {
            nbt::Tag t = e.toNbt();
            if (t.type() == nbt::TagType::String) t = nbt::Compound{}.put("text", t);
            list.items.push_back(std::move(t));
        }
        c.put("extra", std::move(list));
    }
    return c;
}

Component Component::fromNbt(const nbt::Tag& tag) {
    if (const std::string* s = tag.asString()) return literal(*s);
    if (tag.isNumeric()) return literal(std::to_string(tag.asLong())); // (a primitive translation argument)
    if (const nbt::List* list = tag.asList()) { // a list is its first element with the rest appended
        if (list->items.empty()) return literal("");
        Component c = fromNbt(list->items[0]);
        for (size_t i = 1; i < list->items.size(); i++) c.append(fromNbt(list->items[i]));
        return c;
    }
    const nbt::Compound* c = tag.asCompound();
    if (!c) return literal("");
    Component out;
    if (const nbt::Tag* t = c->get("translate")) {
        out.translate = t->asString() ? *t->asString() : "";
        if (const nbt::Tag* w = c->get("with"))
            if (const nbt::List* l = w->asList())
                for (const auto& arg : l->items) out.with.push_back(fromNbt(arg));
        if (const nbt::Tag* f = c->get("fallback"); f && f->asString()) out.fallback = *f->asString();
    } else {
        out.text = c->getString("text", c->getString(""));
    }
    if (const nbt::Tag* t = c->get("color"); t && t->asString()) out.color = *t->asString();
    auto flag = [&](std::string_view key, std::optional<bool>& field) {
        if (c->get(key)) field = c->getBool(key);
    };
    flag("bold", out.bold);
    flag("italic", out.italic);
    flag("underlined", out.underlined);
    flag("strikethrough", out.strikethrough);
    flag("obfuscated", out.obfuscated);
    if (const nbt::Tag* t = c->get("click_event"); t && t->asCompound()) out.clickEvent = *t->asCompound();
    if (const nbt::Tag* t = c->get("hover_event"); t && t->asCompound()) out.hoverEvent = *t->asCompound();
    if (const nbt::Tag* t = c->get("insertion"); t && t->asString()) out.insertion = *t->asString();
    if (const nbt::Tag* e = c->get("extra"))
        if (const nbt::List* l = e->asList())
            for (const auto& x : l->items) out.extra.push_back(fromNbt(x));
    return out;
}

std::string Component::toJson() const {
    std::string out = "{";
    if (translate) {
        out += "\"translate\":" + jsonEscape(*translate);
        if (!with.empty()) {
            out += ",\"with\":[";
            for (size_t i = 0; i < with.size(); i++) out += (i ? "," : "") + with[i].toJson();
            out += "]";
        }
        if (fallback) out += ",\"fallback\":" + jsonEscape(*fallback);
    } else {
        out += "\"text\":" + jsonEscape(text);
    }
    if (color) out += ",\"color\":" + jsonEscape(*color);
    auto flag = [&](const char* name, const std::optional<bool>& v) {
        if (v) out += std::format(",\"{}\":{}", name, *v ? "true" : "false");
    };
    flag("bold", bold);
    flag("italic", italic);
    flag("underlined", underlined);
    flag("strikethrough", strikethrough);
    flag("obfuscated", obfuscated);
    if (!extra.empty()) {
        out += ",\"extra\":[";
        for (size_t i = 0; i < extra.size(); i++) out += (i ? "," : "") + extra[i].toJson();
        out += "]";
    }
    return out + "}";
}

namespace {
// Language.getInstance(): the en_us translations bundled with the server.
const json::Value& language() {
    static const json::Value lang = json::parse(gen::kLanguageJson);
    return lang;
}

// TranslatableContents.decomposeTemplate: "%s" takes the next argument, "%1$s" a given one, "%%"
// is a percent sign. A malformed pattern shows the key itself.
std::optional<std::string> formatTranslation(std::string_view pattern, const std::vector<Component>& args) {
    std::string out;
    size_t next = 0;
    for (size_t i = 0; i < pattern.size(); i++) {
        if (pattern[i] != '%') {
            out += pattern[i];
            continue;
        }
        size_t j = i + 1;
        size_t digits = j;
        while (digits < pattern.size() && pattern[digits] >= '0' && pattern[digits] <= '9') digits++;
        std::optional<size_t> index;
        if (digits > j && digits < pattern.size() && pattern[digits] == '$') {
            index = std::stoul(std::string(pattern.substr(j, digits - j))) - 1;
            j = digits + 1;
        }
        if (j >= pattern.size()) { // "%" at the end (the regex's |$ alternative)
            if (index) return std::nullopt;
            out += '%';
            break;
        }
        char type = pattern[j];
        if (type == '%' && !index) {
            out += '%';
        } else if (type == 's') {
            size_t arg = index ? *index : next++;
            if (arg < args.size()) out += args[arg].getString();
            else return std::nullopt; // (TranslatableFormatException: index out of bounds)
        } else {
            return std::nullopt; // unsupported format
        }
        i = j;
    }
    return out;
}
} // namespace

std::string Component::getString() const {
    std::string out;
    if (!translate) {
        out = text;
    } else {
        const json::Value& lang = language();
        std::string_view pattern = lang.has(*translate) ? std::string_view(lang[*translate].string())
                                   : fallback                ? std::string_view(*fallback)
                                                             : std::string_view(*translate);
        auto formatted = formatTranslation(pattern, with);
        out = formatted ? *formatted : *translate;
    }
    for (const auto& e : extra) out += e.getString();
    return out;
}

} // namespace mc
