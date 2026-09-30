#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "world/level/block/block_state.hpp"

// Port of net.minecraft.world.level.lighting (LevelLightEngine, BlockLightEngine, SkyLightEngine,
// their LayerLightSectionStorage and ChunkSkyLightSources) and DataLayer. Single-threaded: the
// visible and updating section maps of the Java version are one map here, and the work
// ThreadedLevelLightEngine queues runs directly.
namespace mc::light {

enum class LightLayer { Sky, Block };

// SectionPos.asLong / BlockPos.asLong packing, as vanilla uses for its node keys.
inline int64_t sectionNode(int x, int y, int z) {
    return (static_cast<int64_t>(x) & 0x3FFFFF) << 42 | (static_cast<int64_t>(y) & 0xFFFFF) | (static_cast<int64_t>(z) & 0x3FFFFF) << 20;
}
inline int sectionX(int64_t n) { return static_cast<int>(n << 0 >> 42); }
inline int sectionY(int64_t n) { return static_cast<int>(n << 44 >> 44); }
inline int sectionZ(int64_t n) { return static_cast<int>(n << 22 >> 42); }
inline int64_t zeroNode(int64_t n) { return n & ~int64_t{0xFFFFF}; } // SectionPos.getZeroNode
inline int64_t blockNode(int x, int y, int z) {
    return (static_cast<int64_t>(x) & 0x3FFFFFF) << 38 | (static_cast<int64_t>(y) & 0xFFF) | (static_cast<int64_t>(z) & 0x3FFFFFF) << 12;
}
inline int blockX(int64_t n) { return static_cast<int>(n >> 38); }
inline int blockY(int64_t n) { return static_cast<int>(n << 52 >> 52); }
inline int blockZ(int64_t n) { return static_cast<int>(n << 26 >> 38); }
inline int64_t blockToSection(int64_t n) { return sectionNode(blockX(n) >> 4, blockY(n) >> 4, blockZ(n) >> 4); }

// Port of net.minecraft.world.level.chunk.DataLayer: 4096 nibbles, or one value for all.
class DataLayer {
public:
    DataLayer() = default;
    explicit DataLayer(int defaultValue) : defaultValue_(defaultValue) {}
    explicit DataLayer(const std::vector<uint8_t>& bytes);
    DataLayer(const DataLayer& o) : data_(o.data_ ? std::make_unique<std::array<uint8_t, 2048>>(*o.data_) : nullptr), defaultValue_(o.defaultValue_) {}
    DataLayer& operator=(const DataLayer& o) {
        data_ = o.data_ ? std::make_unique<std::array<uint8_t, 2048>>(*o.data_) : nullptr;
        defaultValue_ = o.defaultValue_;
        return *this;
    }
    DataLayer(DataLayer&&) = default;
    DataLayer& operator=(DataLayer&&) = default;

    int get(int x, int y, int z) const {
        int i = y << 8 | z << 4 | x;
        return data_ ? ((*data_)[i >> 1] >> ((i & 1) * 4)) & 15 : defaultValue_;
    }
    void set(int x, int y, int z, int value);
    void fill(int value) {
        defaultValue_ = value;
        data_.reset();
    }
    // DataLayer.getData (materialized).
    const std::array<uint8_t, 2048>& data();
    bool isDefinitelyHomogenous() const { return !data_; }
    bool isEmpty() const { return !data_ && defaultValue_ == 0; }

private:
    std::unique_ptr<std::array<uint8_t, 2048>> data_;
    int defaultValue_ = 0;
};

// Port of ChunkSkyLightSources: per column, the lowest y that still sees the sky.
class ChunkSkyLightSources {
public:
    static constexpr int kNegativeInfinity = INT32_MIN;
    explicit ChunkSkyLightSources(int minY = 0) : minY_(minY - 1) { values_.fill(minY_); }
    // (the column's block states, and the world's height) for fillFrom / update
    struct Column {
        virtual ~Column() = default;
        virtual BlockStateId blockState(int x, int y, int z) const = 0; // local x/z
        virtual int highestFilledSectionIndex() const = 0;
        virtual bool sectionEmpty(int index) const = 0;
        virtual int minY() const = 0;
    };
    void fillFrom(const Column& chunk);
    // A block at local (x, y, z) changed. True if the column's lowest source moved.
    bool update(const Column& chunk, int x, int y, int z);
    int lowestSourceY(int x, int z) const { return extend(values_[x + z * 16]); }
    int highestLowestSourceY() const;

private:
    int findLowestSourceY(const Column& chunk, int topSectionIndex, int x, int z) const;
    int findLowestSourceBelow(const Column& chunk, int x, int startY, BlockStateId startState, int z) const;
    bool updateEdge(int index, int oldTopEdgeY, int topY, BlockStateId topState, BlockStateId bottomState, const Column& chunk, int x, int z);
    int extend(int value) const { return value == minY_ ? kNegativeInfinity : value; }
    int minY_;
    std::array<int, 256> values_{};
};

// What the engines need from the level (LightChunkGetter).
class LightChunkGetter {
public:
    virtual ~LightChunkGetter() = default;
    virtual bool hasChunk(int chunkX, int chunkZ) const = 0;
    // Bedrock for positions in chunks that aren't there (LightEngine.getState).
    virtual BlockStateId blockState(int x, int y, int z) const = 0;
    virtual const ChunkSkyLightSources* skySources(int chunkX, int chunkZ) const = 0;
    // ChunkAccess.findBlockLightSources: every block with a light emission, in world coordinates.
    virtual void findBlockLightSources(int chunkX, int chunkZ, const std::function<void(int, int, int, BlockStateId)>& fn) const = 0;
    virtual void onLightUpdate(LightLayer layer, int64_t sectionNode) = 0;
    virtual int minY() const = 0;
    virtual int sectionCount() const = 0;
};

// LightEngine.hasDifferentLightProperties / isEdgeOccluded helpers (shape occlusion via the
// extracted face masks).
bool hasDifferentLightProperties(BlockStateId oldState, BlockStateId newState);
bool isEdgeOccluded(BlockStateId top, BlockStateId bottom);
// LightEngine.getLightDampeningInto: 16 if the shapes close off the face towards `direction`
// (Direction 3D data value), else `simpleOpacity`.
int lightDampeningInto(BlockStateId from, BlockStateId to, int direction, int simpleOpacity);

class LightEngine;

// LayerLightSectionStorage (plus SkyLightSectionStorage's column tops for the sky layer).
class SectionStorage {
public:
    SectionStorage(LightLayer layer, LightChunkGetter& chunks) : layer(layer), chunks_(chunks) {}
    const LightLayer layer;

    bool storingLightForSection(int64_t section) const { return map_.contains(section); }
    DataLayer* layerFor(int64_t section) {
        auto it = map_.find(section);
        return it == map_.end() ? nullptr : &it->second;
    }
    // getDataLayerData: queued data first (what saving and packets use).
    const DataLayer* dataLayerData(int64_t section) const;
    int storedLevel(int64_t block) const;
    void setStoredLevel(int64_t block, int level);
    int lightValue(int64_t block) const; // getLightValue (sky: getLightValue(pos, false))
    int skyLightValue(int64_t block, bool updating) const;

    bool lightOnInSection(int64_t section) const { return columnsWithSources_.contains(zeroNode(section)); }
    bool lightOnInColumn(int64_t zero) const { return columnsWithSources_.contains(zero); }
    void setLightEnabled(int64_t zero, bool enable);
    void retainData(int64_t zero, bool retain);
    void queueSectionData(int64_t section, std::optional<DataLayer> data);
    void updateSectionStatus(int64_t section, bool sectionEmpty);
    bool hasInconsistencies() const { return hasInconsistencies_; }
    void markNewInconsistencies();
    void swapSectionMap();
    DataLayer* dataLayerToWrite(int64_t section) { return layerFor(section); }

    // Sky only (SkyLightSectionStorage).
    bool hasLightDataAtOrBelow(int sectionY) const { return sectionY >= currentLowestY_; }
    bool isAboveData(int64_t section) const;
    int topSectionY(int64_t zero) const;
    int bottomSectionY() const { return currentLowestY_; }

private:
    void putSectionState(int64_t section, uint8_t state);
    void initializeSection(int64_t section);
    DataLayer createDataLayer(int64_t section);
    void onNodeAdded(int64_t section);
    void onNodeRemoved(int64_t section);
    void markSectionAndNeighborsAsAffected(int64_t section);

    LightChunkGetter& chunks_;
    std::unordered_map<int64_t, uint8_t> sectionStates_;
    std::unordered_set<int64_t> columnsWithSources_;
    std::unordered_map<int64_t, DataLayer> map_;
    std::unordered_map<int64_t, DataLayer> queued_;
    std::unordered_set<int64_t> retain_;
    std::vector<int64_t> toRemove_;
    std::unordered_set<int64_t> toRemoveSet_;
    std::unordered_set<int64_t> affected_;
    bool hasInconsistencies_ = false;
    // SkyDataLayerStorageMap
    std::unordered_map<int64_t, int> topSections_;
    int currentLowestY_ = INT32_MAX;
};

// LightEngine: the decrease/increase queues shared by both layers.
class LightEngine {
public:
    LightEngine(LightLayer layer, LightChunkGetter& chunks) : storage(layer, chunks), chunks_(chunks) {}
    virtual ~LightEngine() = default;
    SectionStorage storage;

    void checkBlock(int64_t block) {
        if (checkSet_.insert(block).second) checks_.push_back(block);
    }
    int runLightUpdates();
    bool hasLightWork() const { return storage.hasInconsistencies() || !checks_.empty() || !decrease_.empty() || !increase_.empty(); }
    virtual void propagateLightSources(int chunkX, int chunkZ) = 0;
    virtual void setLightEnabled(int chunkX, int chunkZ, bool enable) { storage.setLightEnabled(sectionNode(chunkX, 0, chunkZ), enable); }
    int lightValue(int x, int y, int z) const { return storage.lightValue(blockNode(x, y, z)); }

protected:
    virtual void checkNode(int64_t block) = 0;
    virtual void propagateIncrease(int64_t from, int64_t data, int fromLevel) = 0;
    virtual void propagateDecrease(int64_t from, int64_t data) = 0;
    void enqueueDecrease(int64_t node, int64_t data) { decrease_.push_back({node, data}); }
    void enqueueIncrease(int64_t node, int64_t data) { increase_.push_back({node, data}); }
    BlockStateId getState(int64_t block) const { return chunks_.blockState(blockX(block), blockY(block), blockZ(block)); }
    static int getOpacity(BlockStateId state) { return std::max(1, blocks::lightDampening(state)); }
    static bool shapeOccludes(BlockStateId from, BlockStateId to, int direction);

    LightChunkGetter& chunks_;

private:
    int propagateIncreases();
    int propagateDecreases();
    std::vector<int64_t> checks_;
    std::unordered_set<int64_t> checkSet_;
    std::deque<std::pair<int64_t, int64_t>> decrease_, increase_;
};

class BlockLightEngine : public LightEngine {
public:
    explicit BlockLightEngine(LightChunkGetter& chunks) : LightEngine(LightLayer::Block, chunks) {}
    void propagateLightSources(int chunkX, int chunkZ) override;

protected:
    void checkNode(int64_t block) override;
    void propagateIncrease(int64_t from, int64_t data, int fromLevel) override;
    void propagateDecrease(int64_t from, int64_t data) override;

private:
    int emission(int64_t block, BlockStateId state) const;
};

class SkyLightEngine : public LightEngine {
public:
    explicit SkyLightEngine(LightChunkGetter& chunks) : LightEngine(LightLayer::Sky, chunks) {}
    void propagateLightSources(int chunkX, int chunkZ) override;
    void setLightEnabled(int chunkX, int chunkZ, bool enable) override;

protected:
    void checkNode(int64_t block) override;
    void propagateIncrease(int64_t from, int64_t data, int fromLevel) override;
    void propagateDecrease(int64_t from, int64_t data) override;

private:
    int lowestSourceY(int x, int z, int defaultValue) const;
    void updateSourcesInColumn(int x, int z, int lowestSourceY);
    void removeSourcesBelow(int x, int z, int lowestSourceY, int worldBottomY);
    void addSourcesAbove(int x, int z, int lowestSourceY, int worldBottomY);
    int countEmptySectionsBelowIfAtBorder(int64_t block) const;
    void propagateFromEmptySections(int64_t to, int direction, int level, bool increase, int emptySectionsBelow);
};

// LevelLightEngine: both layers (sky only where the dimension has sky light).
class LevelLightEngine {
public:
    LevelLightEngine(LightChunkGetter& chunks, bool hasSkyLight);
    void checkBlock(BlockPos pos);
    int runLightUpdates();
    bool hasLightWork() const;
    void updateSectionStatus(int sectionX, int sectionY, int sectionZ, bool empty);
    void setLightEnabled(int chunkX, int chunkZ, bool enable);
    void propagateLightSources(int chunkX, int chunkZ);
    void queueSectionData(LightLayer layer, int sectionX, int sectionY, int sectionZ, std::optional<DataLayer> data);
    void retainData(int chunkX, int chunkZ, bool retain);
    const DataLayer* dataLayerData(LightLayer layer, int sectionX, int sectionY, int sectionZ) const;
    int brightness(LightLayer layer, BlockPos pos) const;
    int rawBrightness(BlockPos pos, int skyDarken) const;
    bool lightOnInColumn(int chunkX, int chunkZ) const;
    int lightSectionCount() const { return chunks_.sectionCount() + 2; }
    int minLightSection() const { return (chunks_.minY() >> 4) - 1; }

    // ThreadedLevelLightEngine.initializeLight / lightChunk / updateChunkStatus (a chunk going away).
    void initializeLight(int chunkX, int chunkZ, const std::vector<bool>& sectionEmpty, bool lighted);
    void lightChunk(int chunkX, int chunkZ, bool lighted);
    void removeChunk(int chunkX, int chunkZ);

private:
    LightChunkGetter& chunks_;
    std::unique_ptr<BlockLightEngine> block_;
    std::unique_ptr<SkyLightEngine> sky_;
};

} // namespace mc::light
