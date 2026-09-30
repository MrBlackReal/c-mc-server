#include "world/item/item_stack.hpp"

#include <algorithm>

#include "core/component/data_components.hpp"

namespace mc {
namespace {
constexpr int kMaxComponents = 256;
constexpr size_t kMaxComponentBytes = 2 * 1024 * 1024;
} // namespace

bool ItemStack::sameDisk(const ItemStack& a, const ItemStack& b) {
    if (a.diskComponents.empty() && b.diskComponents.empty()) return true;
    ByteWriter wa, wb;
    nbt::writeNetwork(wa, nbt::Tag(a.diskComponents));
    nbt::writeNetwork(wb, nbt::Tag(b.diskComponents));
    return wa.data() == wb.data();
}

ItemStack ItemStack::split(int n) {
    int take = std::min(n, count);
    ItemStack out = copyWithCount(take);
    shrink(take);
    clearIfEmpty();
    return out;
}

void ItemStack::write(ByteWriter& out) const {
    if (isEmpty()) {
        out.writeVarInt(0);
        return;
    }
    out.writeVarInt(count);
    out.writeVarInt(item);
    int added = 0, removed = 0;
    for (const auto& c : components) (c.removed ? removed : added)++;
    out.writeVarInt(added);
    out.writeVarInt(removed);
    for (const auto& c : components) {
        if (c.removed) continue;
        out.writeVarInt(c.type);
        out.writeBytes(c.value);
    }
    for (const auto& c : components)
        if (c.removed) out.writeVarInt(c.type);
}

ItemStack ItemStack::readUntrusted(ByteReader& in) {
    ItemStack s;
    s.count = in.readVarInt();
    if (s.count <= 0) return {};
    s.item = in.readVarInt();
    if (s.item < 0 || static_cast<size_t>(s.item) >= gen::kItems.size()) throw DecoderException("unknown item id");
    int added = in.readVarInt();
    int removed = in.readVarInt();
    if (added < 0 || removed < 0 || added + removed > kMaxComponents) throw DecoderException("too many item components");
    for (int i = 0; i < added; i++) {
        Component c;
        c.type = in.readVarInt();
        int32_t len = in.readVarInt();
        if (len < 0 || static_cast<size_t>(len) > kMaxComponentBytes) throw DecoderException("item component too large");
        auto bytes = in.readBytes(static_cast<size_t>(len));
        c.value.assign(bytes.begin(), bytes.end());
        s.components.push_back(std::move(c));
    }
    for (int i = 0; i < removed; i++) s.components.push_back({in.readVarInt(), true, {}});
    return s;
}

HashedStack HashedStack::read(ByteReader& in) {
    HashedStack h;
    if (!in.readBool()) return h;
    h.empty = false;
    h.item = in.readVarInt();
    h.count = in.readVarInt();
    int added = in.readVarInt();
    if (added < 0 || added > kMaxComponents) throw DecoderException("bad hashed stack");
    for (int i = 0; i < added; i++) {
        int32_t type = in.readVarInt();
        h.added.emplace_back(type, in.readInt());
    }
    int removed = in.readVarInt();
    if (removed < 0 || removed > kMaxComponents) throw DecoderException("bad hashed stack");
    for (int i = 0; i < removed; i++) h.removed.push_back(in.readVarInt());
    return h;
}

bool HashedStack::matches(const ItemStack& stack) const {
    if (empty || stack.isEmpty()) return empty && stack.isEmpty();
    if (item != stack.item || count != stack.count) return false;
    // HashedPatchMap.matches: the same removed set, and the same hash for every added component.
    std::vector<int32_t> stackRemoved;
    size_t stackAdded = 0;
    for (const auto& c : stack.components) {
        if (c.removed) {
            stackRemoved.push_back(c.type);
            continue;
        }
        stackAdded++;
        auto it = std::find_if(added.begin(), added.end(), [&](const auto& a) { return a.first == c.type; });
        if (it == added.end()) return false;
        auto h = components::hash(c.type, c.value);
        if (!h || *h != it->second) return false;
    }
    if (stackAdded != added.size()) return false;
    std::vector<int32_t> theirs = removed;
    std::sort(theirs.begin(), theirs.end());
    std::sort(stackRemoved.begin(), stackRemoved.end());
    return theirs == stackRemoved;
}

} // namespace mc
