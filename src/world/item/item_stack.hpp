#pragma once

#include <cstdint>
#include <vector>

#include "generated/behaviour.hpp"
#include "nbt/tag.hpp"
#include "network/friendly_byte_buf.hpp"

namespace mc {

// Port of ItemStack. Data components are kept as their network-encoded patch; values are decoded
// on demand through core/component/data_components (Codec/StreamCodec schemas), which lets us
// store any value a client sends and send it back verbatim.
struct ItemStack {
    int32_t item = 0; // minecraft:item id, 0 = air
    int32_t count = 0;

    struct Component {
        int32_t type;               // minecraft:data_component_type id
        bool removed = false;       // "!type" entry of the patch
        std::vector<uint8_t> value; // value as encoded by the component's stream codec
        bool operator==(const Component&) const = default;
    };
    std::vector<Component> components;
    // Saved components that don't decode (unknown types, malformed values); kept verbatim so saving
    // doesn't lose them. Not visible to clients.
    nbt::Compound diskComponents;
    // BundleContents.selectedItem: the bundle item the player points at (ServerboundSelectBundleItemPacket).
    // Part of the component object in vanilla but neither saved, sent nor compared.
    int bundleSelected = -1;

    ItemStack() = default;
    ItemStack(int32_t item, int32_t count) : item(item), count(count) {}

    bool isEmpty() const { return item == 0 || count <= 0; }
    const gen::ItemInfo& info() const { return gen::kItems[item]; }
    int maxStackSize() const { return info().maxStack; }
    bool isStackable() const { return maxStackSize() > 1; }

    ItemStack copy() const { return *this; }
    ItemStack copyWithCount(int n) const {
        ItemStack s = *this;
        s.count = n;
        return s;
    }
    // ItemStack.split: removes up to n items and returns them.
    ItemStack split(int n);
    void grow(int n) { count += n; }
    void shrink(int n) { count -= n; }
    void clearIfEmpty() {
        if (isEmpty()) *this = ItemStack();
    }

    static bool isSameItem(const ItemStack& a, const ItemStack& b) { return a.item == b.item; }
    static bool isSameItemSameComponents(const ItemStack& a, const ItemStack& b) {
        return a.item == b.item && a.components == b.components && sameDisk(a, b);
    }
    static bool matches(const ItemStack& a, const ItemStack& b) {
        if (a.isEmpty() || b.isEmpty()) return a.isEmpty() && b.isEmpty();
        return a.count == b.count && isSameItemSameComponents(a, b);
    }

    static bool sameDisk(const ItemStack& a, const ItemStack& b);

    // ItemStack.OPTIONAL_STREAM_CODEC (server -> client).
    void write(ByteWriter& out) const;
    // ItemStack.OPTIONAL_UNTRUSTED_STREAM_CODEC (client -> server).
    static ItemStack readUntrusted(ByteReader& in);
};

// HashedStack (ServerboundContainerClickPacket): the client's view of a slot, with each component
// reduced to its HashOps hash (HashedPatchMap).
struct HashedStack {
    bool empty = true;
    int32_t item = 0;
    int32_t count = 0;
    std::vector<std::pair<int32_t, int32_t>> added; // component type -> hash
    std::vector<int32_t> removed;

    static HashedStack read(ByteReader& in);
    // HashedStack.matches: same item, count and component patch (compared by hash).
    bool matches(const ItemStack& stack) const;
};

} // namespace mc
