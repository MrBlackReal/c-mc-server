#include "world/level/lighting/level_light_engine.hpp"

#include <algorithm>
#include <cstring>

#include "generated/behaviour.hpp"

namespace mc::light {

namespace {

// Direction.values(): DOWN, UP, NORTH, SOUTH, WEST, EAST.
constexpr int kDx[6] = {0, 0, 0, 0, -1, 1}, kDy[6] = {-1, 1, 0, 0, 0, 0}, kDz[6] = {0, 0, -1, 1, 0, 0};
constexpr int kDown = 0, kUp = 1, kNorth = 2, kSouth = 3, kWest = 4, kEast = 5;
int opposite(int d) { return d ^ 1; }
int64_t offset(int64_t block, int d) { return blockNode(blockX(block) + kDx[d], blockY(block) + kDy[d], blockZ(block) + kDz[d]); }
int64_t sectionOffset(int64_t s, int dx, int dy, int dz) { return sectionNode(sectionX(s) + dx, sectionY(s) + dy, sectionZ(s) + dz); }

// LightEngine.QueueEntry
namespace entry {
constexpr int64_t kLevelMask = 15, kDirections = 1008, kFromEmptyShape = 1024, kFromEmission = 2048;
int64_t withLevel(int64_t e, int level) { return (e & -16) | (level & 15); }
int64_t withDirection(int64_t e, int d) { return e | (int64_t{1} << (d + 4)); }
int64_t withoutDirection(int64_t e, int d) { return e & ~(int64_t{1} << (d + 4)); }
int64_t decreaseSkipOneDirection(int level, int skip) { return withLevel(withoutDirection(kDirections, skip), level); }
int64_t decreaseAllDirections(int level) { return withLevel(kDirections, level); }
int64_t increaseLightFromEmission(int level, bool fromEmpty) { return withLevel(kDirections | kFromEmission | (fromEmpty ? kFromEmptyShape : 0), level); }
int64_t increaseSkipOneDirection(int level, bool fromEmpty, int skip) {
    return withLevel(withoutDirection(kDirections, skip) | (fromEmpty ? kFromEmptyShape : 0), level);
}
int64_t increaseOnlyOneDirection(int level, bool fromEmpty, int d) { return withLevel(withDirection(fromEmpty ? kFromEmptyShape : 0, d), level); }
int64_t increaseSkySourceInDirections(bool down, bool north, bool south, bool west, bool east) {
    int64_t e = withLevel(0, 15);
    if (down) e = withDirection(e, kDown);
    if (north) e = withDirection(e, kNorth);
    if (south) e = withDirection(e, kSouth);
    if (west) e = withDirection(e, kWest);
    if (east) e = withDirection(e, kEast);
    return e;
}
int fromLevel(int64_t e) { return static_cast<int>(e & kLevelMask); }
bool isFromEmptyShape(int64_t e) { return (e & kFromEmptyShape) != 0; }
bool isIncreaseFromEmission(int64_t e) { return (e & kFromEmission) != 0; }
bool shouldPropagate(int64_t e, int d) { return (e & (int64_t{1} << (d + 4))) != 0; }
} // namespace entry

const int64_t kPullLightInEntry = entry::decreaseAllDirections(1);

// LightEngine.isEmptyShape: the shape doesn't matter for light.
bool isEmptyShape(BlockStateId state) {
    uint32_t f = blocks::flags(state);
    return !(f & blocks::kCanOcclude) || !(f & blocks::kUseShapeForLightOcclusion);
}
// LightEngine.getOcclusionShape(state, direction) as a 16x16 mask; null for an empty shape.
const uint64_t* occlusionFace(BlockStateId state, int d) {
    if (isEmptyShape(state)) return nullptr;
    int16_t i = gen::kStateLightFaces[state];
    return i < 0 ? nullptr : gen::kLightFaces[i].face[d];
}
bool isFull(const uint64_t* m) { return m && m[0] == ~0ull && m[1] == ~0ull && m[2] == ~0ull && m[3] == ~0ull; }
bool isEmptyMask(const uint64_t* m) { return !m || (m[0] | m[1] | m[2] | m[3]) == 0; }
// Shapes.faceShapeOccludes
bool faceShapeOccludes(const uint64_t* a, const uint64_t* b) {
    if (isFull(a) || isFull(b)) return true;
    if (isEmptyMask(a) && isEmptyMask(b)) return false;
    for (int i = 0; i < 4; i++)
        if (((a ? a[i] : 0) | (b ? b[i] : 0)) != ~0ull) return false;
    return true;
}

bool isSourceLevel(int value) { return value == 15; }

} // namespace

bool hasDifferentLightProperties(BlockStateId oldState, BlockStateId newState) {
    if (newState == oldState) return false;
    return blocks::lightDampening(newState) != blocks::lightDampening(oldState) || blocks::lightEmission(newState) != blocks::lightEmission(oldState) ||
           (blocks::flags(newState) & blocks::kUseShapeForLightOcclusion) || (blocks::flags(oldState) & blocks::kUseShapeForLightOcclusion);
}

bool isEdgeOccluded(BlockStateId top, BlockStateId bottom) {
    if (blocks::lightDampening(bottom) != 0) return true;
    return faceShapeOccludes(occlusionFace(top, kDown), occlusionFace(bottom, kUp));
}

int lightDampeningInto(BlockStateId from, BlockStateId to, int d, int simpleOpacity) {
    return faceShapeOccludes(occlusionFace(from, d), occlusionFace(to, opposite(d))) ? 16 : simpleOpacity;
}

bool LightEngine::shapeOccludes(BlockStateId from, BlockStateId to, int d) {
    return faceShapeOccludes(occlusionFace(from, d), occlusionFace(to, opposite(d)));
}

// ---------------------------------------------------------------- DataLayer

DataLayer::DataLayer(const std::vector<uint8_t>& bytes) {
    data_ = std::make_unique<std::array<uint8_t, 2048>>();
    std::memcpy(data_->data(), bytes.data(), std::min<size_t>(bytes.size(), 2048));
}

void DataLayer::set(int x, int y, int z, int value) {
    data();
    int i = y << 8 | z << 4 | x;
    uint8_t& b = (*data_)[i >> 1];
    int shift = (i & 1) * 4;
    b = static_cast<uint8_t>((b & ~(15 << shift)) | ((value & 15) << shift));
}

const std::array<uint8_t, 2048>& DataLayer::data() {
    if (!data_) {
        data_ = std::make_unique<std::array<uint8_t, 2048>>();
        data_->fill(static_cast<uint8_t>(defaultValue_ | defaultValue_ << 4));
    }
    return *data_;
}

// ---------------------------------------------------------------- ChunkSkyLightSources

void ChunkSkyLightSources::fillFrom(const Column& chunk) {
    int top = chunk.highestFilledSectionIndex();
    if (top == -1) {
        values_.fill(minY_);
        return;
    }
    for (int z = 0; z < 16; z++)
        for (int x = 0; x < 16; x++) values_[x + z * 16] = std::max(findLowestSourceY(chunk, top, x, z), minY_);
}

int ChunkSkyLightSources::findLowestSourceY(const Column& chunk, int topSectionIndex, int x, int z) const {
    int sectionBase = chunk.minY() >> 4;
    int topY = (sectionBase + topSectionIndex + 1) * 16;
    BlockStateId topState = blocks::defaultState(gen::block::AIR);
    for (int index = topSectionIndex; index >= 0; index--) {
        if (chunk.sectionEmpty(index)) {
            topState = blocks::defaultState(gen::block::AIR);
            topY = (sectionBase + index) * 16;
            continue;
        }
        int bottomBase = (sectionBase + index) * 16;
        for (int y = 15; y >= 0; y--) {
            BlockStateId bottom = chunk.blockState(x, bottomBase + y, z);
            if (isEdgeOccluded(topState, bottom)) return topY;
            topState = bottom;
            topY = bottomBase + y;
        }
    }
    return minY_;
}

bool ChunkSkyLightSources::update(const Column& chunk, int x, int y, int z) {
    int index = x + z * 16;
    int current = values_[index];
    if (y + 1 < current) return false;
    BlockStateId top = chunk.blockState(x, y + 1, z), middle = chunk.blockState(x, y, z);
    if (updateEdge(index, current, y + 1, top, middle, chunk, x, z)) return true;
    BlockStateId bottom = chunk.blockState(x, y - 1, z);
    return updateEdge(index, current, y, middle, bottom, chunk, x, z);
}

bool ChunkSkyLightSources::updateEdge(int index, int oldTopEdgeY, int topY, BlockStateId topState, BlockStateId bottomState, const Column& chunk, int x,
                                      int z) {
    if (isEdgeOccluded(topState, bottomState)) {
        if (topY > oldTopEdgeY) {
            values_[index] = topY;
            return true;
        }
    } else if (topY == oldTopEdgeY) {
        values_[index] = findLowestSourceBelow(chunk, x, topY - 1, bottomState, z);
        return true;
    }
    return false;
}

int ChunkSkyLightSources::findLowestSourceBelow(const Column& chunk, int x, int startY, BlockStateId startState, int z) const {
    int topY = startY;
    BlockStateId topState = startState;
    for (int bottomY = startY - 1; bottomY >= minY_; bottomY--) {
        BlockStateId bottom = chunk.blockState(x, bottomY, z);
        if (isEdgeOccluded(topState, bottom)) return topY;
        topState = bottom;
        topY = bottomY;
    }
    return minY_;
}

int ChunkSkyLightSources::highestLowestSourceY() const {
    int max = INT32_MIN;
    for (int v : values_) max = std::max(max, v);
    return extend(max);
}

// ---------------------------------------------------------------- SectionStorage

namespace {
constexpr uint8_t kHasDataBit = 32, kNeighborBits = 31;
}

const DataLayer* SectionStorage::dataLayerData(int64_t section) const {
    if (auto it = queued_.find(section); it != queued_.end()) return &it->second;
    auto it = map_.find(section);
    return it == map_.end() ? nullptr : &it->second;
}

int SectionStorage::storedLevel(int64_t block) const {
    auto it = map_.find(blockToSection(block));
    return it == map_.end() ? 0 : it->second.get(blockX(block) & 15, blockY(block) & 15, blockZ(block) & 15);
}

void SectionStorage::setStoredLevel(int64_t block, int level) {
    auto it = map_.find(blockToSection(block));
    if (it == map_.end()) return;
    int x = blockX(block), y = blockY(block), z = blockZ(block);
    it->second.set(x & 15, y & 15, z & 15, level);
    // SectionPos.aroundAndAtBlockPos
    int minX = (x - 1) >> 4, maxX = (x + 1) >> 4, minY = (y - 1) >> 4, maxY = (y + 1) >> 4, minZ = (z - 1) >> 4, maxZ = (z + 1) >> 4;
    for (int sx = minX; sx <= maxX; sx++)
        for (int sy = minY; sy <= maxY; sy++)
            for (int sz = minZ; sz <= maxZ; sz++) affected_.insert(sectionNode(sx, sy, sz));
}

int SectionStorage::lightValue(int64_t block) const {
    if (layer == LightLayer::Sky) return skyLightValue(block, false);
    auto it = map_.find(blockToSection(block));
    return it == map_.end() ? 0 : it->second.get(blockX(block) & 15, blockY(block) & 15, blockZ(block) & 15);
}

int SectionStorage::skyLightValue(int64_t block, bool updating) const {
    int64_t section = blockToSection(block);
    int sy = sectionY(section);
    int top = topSectionY(zeroNode(section));
    if (top != currentLowestY_ && sy < top) {
        auto it = map_.find(section);
        int localY = blockY(block) & 15;
        while (it == map_.end()) {
            localY = 0; // BlockPos.getFlatIndex
            if (++sy >= top) return 15;
            section = sectionOffset(section, 0, 1, 0);
            it = map_.find(section);
        }
        return it->second.get(blockX(block) & 15, localY, blockZ(block) & 15);
    }
    return updating && !lightOnInSection(section) ? 0 : 15;
}

void SectionStorage::setLightEnabled(int64_t zero, bool enable) {
    if (enable) columnsWithSources_.insert(zero);
    else columnsWithSources_.erase(zero);
}

void SectionStorage::retainData(int64_t zero, bool retain) {
    if (retain) retain_.insert(zero);
    else retain_.erase(zero);
}

void SectionStorage::queueSectionData(int64_t section, std::optional<DataLayer> data) {
    if (data) {
        queued_[section] = std::move(*data);
        hasInconsistencies_ = true;
    } else {
        queued_.erase(section);
    }
}

void SectionStorage::updateSectionStatus(int64_t section, bool sectionEmpty) {
    uint8_t state = sectionStates_.contains(section) ? sectionStates_[section] : 0;
    uint8_t newState = sectionEmpty ? state & ~kHasDataBit : state | kHasDataBit;
    if (state == newState) return;
    putSectionState(section, newState);
    int increment = sectionEmpty ? -1 : 1;
    for (int dx = -1; dx <= 1; dx++)
        for (int dy = -1; dy <= 1; dy++)
            for (int dz = -1; dz <= 1; dz++) {
                if (dx == 0 && dy == 0 && dz == 0) continue;
                int64_t neighbor = sectionOffset(section, dx, dy, dz);
                uint8_t ns = sectionStates_.contains(neighbor) ? sectionStates_[neighbor] : 0;
                int count = std::clamp((ns & kNeighborBits) + increment, 0, 26);
                putSectionState(neighbor, static_cast<uint8_t>((ns & ~kNeighborBits) | count));
            }
}

void SectionStorage::putSectionState(int64_t section, uint8_t state) {
    if (state != 0) {
        auto [it, inserted] = sectionStates_.try_emplace(section, state);
        bool wasZero = inserted || it->second == 0;
        it->second = state;
        if (wasZero) initializeSection(section);
    } else if (auto it = sectionStates_.find(section); it != sectionStates_.end()) {
        bool wasNonZero = it->second != 0;
        sectionStates_.erase(it);
        if (wasNonZero) { // removeSection
            if (toRemoveSet_.insert(section).second) toRemove_.push_back(section);
            hasInconsistencies_ = true;
        }
    }
}

void SectionStorage::initializeSection(int64_t section) {
    if (toRemoveSet_.erase(section)) {
        std::erase(toRemove_, section);
        return;
    }
    map_[section] = createDataLayer(section);
    onNodeAdded(section);
    markSectionAndNeighborsAsAffected(section);
    hasInconsistencies_ = true;
}

DataLayer SectionStorage::createDataLayer(int64_t section) {
    if (auto it = queued_.find(section); it != queued_.end()) return it->second;
    if (layer == LightLayer::Block) return DataLayer();
    int top = topSectionY(zeroNode(section));
    if (top != currentLowestY_ && sectionY(section) < top) {
        int64_t above = sectionOffset(section, 0, 1, 0);
        const DataLayer* aboveData;
        while (!(aboveData = layerFor(above))) above = sectionOffset(above, 0, 1, 0);
        if (aboveData->isDefinitelyHomogenous()) return *aboveData; // repeatFirstLayer
        DataLayer copy = *aboveData;
        const auto& input = copy.data();
        std::vector<uint8_t> output(2048);
        for (int i = 0; i < 16; i++) std::memcpy(output.data() + i * 128, input.data(), 128);
        return DataLayer(output);
    }
    return lightOnInSection(section) ? DataLayer(15) : DataLayer();
}

void SectionStorage::onNodeAdded(int64_t section) {
    if (layer != LightLayer::Sky) return;
    int y = sectionY(section);
    if (currentLowestY_ > y) currentLowestY_ = y; // (the map's default follows)
    int64_t zero = zeroNode(section);
    if (topSectionY(zero) < y + 1) topSections_[zero] = y + 1;
}

void SectionStorage::onNodeRemoved(int64_t section) {
    if (layer != LightLayer::Sky) return;
    int64_t zero = zeroNode(section);
    int y = sectionY(section);
    if (topSectionY(zero) != y + 1) return;
    int64_t newTop = section;
    while (!storingLightForSection(newTop) && hasLightDataAtOrBelow(y)) {
        y--;
        newTop = sectionOffset(newTop, 0, -1, 0);
    }
    if (storingLightForSection(newTop)) topSections_[zero] = y + 1;
    else topSections_.erase(zero);
}

int SectionStorage::topSectionY(int64_t zero) const {
    auto it = topSections_.find(zero);
    return it == topSections_.end() ? currentLowestY_ : it->second;
}

bool SectionStorage::isAboveData(int64_t section) const {
    int top = topSectionY(zeroNode(section));
    return top == currentLowestY_ || sectionY(section) >= top;
}

void SectionStorage::markSectionAndNeighborsAsAffected(int64_t section) {
    for (int dz = -1; dz <= 1; dz++)
        for (int dx = -1; dx <= 1; dx++)
            for (int dy = -1; dy <= 1; dy++) affected_.insert(sectionOffset(section, dx, dy, dz));
}

void SectionStorage::markNewInconsistencies() {
    if (!hasInconsistencies_) return;
    hasInconsistencies_ = false;
    for (int64_t node : toRemove_) {
        std::optional<DataLayer> queued, stored;
        if (auto it = queued_.find(node); it != queued_.end()) queued = std::move(it->second), queued_.erase(it);
        if (auto it = map_.find(node); it != map_.end()) stored = std::move(it->second), map_.erase(it);
        if (retain_.contains(zeroNode(node))) {
            if (queued) queued_[node] = std::move(*queued);
            else if (stored) queued_[node] = std::move(*stored);
        }
    }
    for (int64_t node : toRemove_) onNodeRemoved(node);
    toRemove_.clear();
    toRemoveSet_.clear();
    for (auto it = queued_.begin(); it != queued_.end();) {
        if (storingLightForSection(it->first)) {
            map_[it->first] = std::move(it->second);
            it = queued_.erase(it);
        } else {
            ++it;
        }
    }
}

void SectionStorage::swapSectionMap() {
    for (int64_t section : affected_) chunks_.onLightUpdate(layer, section);
    affected_.clear();
}

// ---------------------------------------------------------------- LightEngine

int LightEngine::runLightUpdates() {
    std::vector<int64_t> checks = std::move(checks_);
    checks_.clear();
    checkSet_.clear();
    for (int64_t node : checks) checkNode(node);
    int count = propagateDecreases();
    count += propagateIncreases();
    storage.markNewInconsistencies();
    storage.swapSectionMap();
    return count;
}

int LightEngine::propagateIncreases() {
    int count = 0;
    for (; !increase_.empty(); count++) {
        auto [from, data] = increase_.front();
        increase_.pop_front();
        int level = storage.storedLevel(from);
        int target = entry::fromLevel(data);
        if (entry::isIncreaseFromEmission(data) && level < target) {
            storage.setStoredLevel(from, target);
            level = target;
        }
        if (level == target) propagateIncrease(from, data, level);
    }
    return count;
}

int LightEngine::propagateDecreases() {
    int count = 0;
    for (; !decrease_.empty(); count++) {
        auto [from, data] = decrease_.front();
        decrease_.pop_front();
        propagateDecrease(from, data);
    }
    return count;
}

// ---------------------------------------------------------------- BlockLightEngine

int BlockLightEngine::emission(int64_t block, BlockStateId state) const {
    int e = blocks::lightEmission(state);
    return e > 0 && storage.lightOnInSection(blockToSection(block)) ? e : 0;
}

void BlockLightEngine::checkNode(int64_t block) {
    if (!storage.storingLightForSection(blockToSection(block))) return;
    BlockStateId state = getState(block);
    int e = emission(block, state);
    int old = storage.storedLevel(block);
    if (e < old) {
        storage.setStoredLevel(block, 0);
        enqueueDecrease(block, entry::decreaseAllDirections(old));
    } else {
        enqueueDecrease(block, kPullLightInEntry);
    }
    if (e > 0) enqueueIncrease(block, entry::increaseLightFromEmission(e, isEmptyShape(state)));
}

void BlockLightEngine::propagateIncrease(int64_t from, int64_t data, int fromLevel) {
    std::optional<BlockStateId> fromState;
    for (int d = 0; d < 6; d++) {
        if (!entry::shouldPropagate(data, d)) continue;
        int64_t to = offset(from, d);
        if (!storage.storingLightForSection(blockToSection(to))) continue;
        int toLevel = storage.storedLevel(to);
        if (fromLevel - 1 <= toLevel) continue;
        BlockStateId toState = getState(to);
        int newLevel = fromLevel - getOpacity(toState);
        if (newLevel <= toLevel) continue;
        if (!fromState) fromState = entry::isFromEmptyShape(data) ? blocks::defaultState(gen::block::AIR) : getState(from);
        if (shapeOccludes(*fromState, toState, d)) continue;
        storage.setStoredLevel(to, newLevel);
        if (newLevel > 1) enqueueIncrease(to, entry::increaseSkipOneDirection(newLevel, isEmptyShape(toState), opposite(d)));
    }
}

void BlockLightEngine::propagateDecrease(int64_t from, int64_t data) {
    int oldFromLevel = entry::fromLevel(data);
    for (int d = 0; d < 6; d++) {
        if (!entry::shouldPropagate(data, d)) continue;
        int64_t to = offset(from, d);
        if (!storage.storingLightForSection(blockToSection(to))) continue;
        int toLevel = storage.storedLevel(to);
        if (toLevel == 0) continue;
        if (toLevel <= oldFromLevel - 1) {
            BlockStateId toState = getState(to);
            int e = emission(to, toState);
            storage.setStoredLevel(to, 0);
            if (e < toLevel) enqueueDecrease(to, entry::decreaseSkipOneDirection(toLevel, opposite(d)));
            if (e > 0) enqueueIncrease(to, entry::increaseLightFromEmission(e, isEmptyShape(toState)));
        } else {
            enqueueIncrease(to, entry::increaseOnlyOneDirection(toLevel, false, opposite(d)));
        }
    }
}

void BlockLightEngine::propagateLightSources(int chunkX, int chunkZ) {
    setLightEnabled(chunkX, chunkZ, true);
    if (!chunks_.hasChunk(chunkX, chunkZ)) return;
    chunks_.findBlockLightSources(chunkX, chunkZ, [&](int x, int y, int z, BlockStateId state) {
        enqueueIncrease(blockNode(x, y, z), entry::increaseLightFromEmission(blocks::lightEmission(state), isEmptyShape(state)));
    });
}

// ---------------------------------------------------------------- SkyLightEngine

namespace {
const int64_t kRemoveTopSkySource = entry::decreaseAllDirections(15);
const int64_t kRemoveSkySource = entry::decreaseSkipOneDirection(15, kUp);
const int64_t kAddSkySource = entry::increaseSkipOneDirection(15, false, kUp);
} // namespace

int SkyLightEngine::lowestSourceY(int x, int z, int defaultValue) const {
    const ChunkSkyLightSources* sources = chunks_.skySources(x >> 4, z >> 4);
    return sources ? sources->lowestSourceY(x & 15, z & 15) : defaultValue;
}

void SkyLightEngine::checkNode(int64_t block) {
    int x = blockX(block), y = blockY(block), z = blockZ(block);
    int64_t section = blockToSection(block);
    int lowest = storage.lightOnInSection(section) ? lowestSourceY(x, z, INT32_MAX) : INT32_MAX;
    if (lowest != INT32_MAX) updateSourcesInColumn(x, z, lowest);
    if (!storage.storingLightForSection(section)) return;
    if (y >= lowest) {
        enqueueDecrease(block, kRemoveSkySource);
        enqueueIncrease(block, kAddSkySource);
    } else {
        int old = storage.storedLevel(block);
        if (old > 0) {
            storage.setStoredLevel(block, 0);
            enqueueDecrease(block, entry::decreaseAllDirections(old));
        } else {
            enqueueDecrease(block, kPullLightInEntry);
        }
    }
}

void SkyLightEngine::updateSourcesInColumn(int x, int z, int lowest) {
    int worldBottomY = storage.bottomSectionY() * 16;
    removeSourcesBelow(x, z, lowest, worldBottomY);
    addSourcesAbove(x, z, lowest, worldBottomY);
}

void SkyLightEngine::removeSourcesBelow(int x, int z, int lowest, int worldBottomY) {
    if (lowest <= worldBottomY) return;
    int sx = x >> 4, sz = z >> 4;
    int startY = lowest - 1;
    for (int sy = startY >> 4; storage.hasLightDataAtOrBelow(sy); sy--) {
        if (!storage.storingLightForSection(sectionNode(sx, sy, sz))) continue;
        int bottom = sy * 16, top = bottom + 15;
        for (int y = std::min(top, startY); y >= bottom; y--) {
            int64_t block = blockNode(x, y, z);
            if (!isSourceLevel(storage.storedLevel(block))) return;
            storage.setStoredLevel(block, 0);
            enqueueDecrease(block, y == lowest - 1 ? kRemoveTopSkySource : kRemoveSkySource);
        }
    }
}

void SkyLightEngine::addSourcesAbove(int x, int z, int lowest, int worldBottomY) {
    int sx = x >> 4, sz = z >> 4;
    int neighborLowest = std::max(std::max(lowestSourceY(x - 1, z, INT32_MIN), lowestSourceY(x + 1, z, INT32_MIN)),
                                  std::max(lowestSourceY(x, z - 1, INT32_MIN), lowestSourceY(x, z + 1, INT32_MIN)));
    int startY = std::max(lowest, worldBottomY);
    for (int64_t section = sectionNode(sx, startY >> 4, sz); !storage.isAboveData(section); section = sectionOffset(section, 0, 1, 0)) {
        if (!storage.storingLightForSection(section)) continue;
        int bottom = sectionY(section) * 16, top = bottom + 15;
        for (int y = std::max(bottom, startY); y <= top; y++) {
            int64_t block = blockNode(x, y, z);
            if (isSourceLevel(storage.storedLevel(block))) return;
            storage.setStoredLevel(block, 15);
            if (y < neighborLowest || y == lowest) enqueueIncrease(block, kAddSkySource);
        }
    }
}

void SkyLightEngine::propagateIncrease(int64_t from, int64_t data, int fromLevel) {
    std::optional<BlockStateId> fromState;
    int emptyBelow = countEmptySectionsBelowIfAtBorder(from);
    for (int d = 0; d < 6; d++) {
        if (!entry::shouldPropagate(data, d)) continue;
        int64_t to = offset(from, d);
        if (!storage.storingLightForSection(blockToSection(to))) continue;
        int toLevel = storage.storedLevel(to);
        if (fromLevel - 1 <= toLevel) continue;
        BlockStateId toState = getState(to);
        int newLevel = fromLevel - getOpacity(toState);
        if (newLevel <= toLevel) continue;
        if (!fromState) fromState = entry::isFromEmptyShape(data) ? blocks::defaultState(gen::block::AIR) : getState(from);
        if (shapeOccludes(*fromState, toState, d)) continue;
        storage.setStoredLevel(to, newLevel);
        if (newLevel > 1) enqueueIncrease(to, entry::increaseSkipOneDirection(newLevel, isEmptyShape(toState), opposite(d)));
        propagateFromEmptySections(to, d, newLevel, true, emptyBelow);
    }
}

void SkyLightEngine::propagateDecrease(int64_t from, int64_t data) {
    int emptyBelow = countEmptySectionsBelowIfAtBorder(from);
    int oldFromLevel = entry::fromLevel(data);
    for (int d = 0; d < 6; d++) {
        if (!entry::shouldPropagate(data, d)) continue;
        int64_t to = offset(from, d);
        if (!storage.storingLightForSection(blockToSection(to))) continue;
        int toLevel = storage.storedLevel(to);
        if (toLevel == 0) continue;
        if (toLevel <= oldFromLevel - 1) {
            storage.setStoredLevel(to, 0);
            enqueueDecrease(to, entry::decreaseSkipOneDirection(toLevel, opposite(d)));
            propagateFromEmptySections(to, d, toLevel, false, emptyBelow);
        } else {
            enqueueIncrease(to, entry::increaseOnlyOneDirection(toLevel, false, opposite(d)));
        }
    }
}

int SkyLightEngine::countEmptySectionsBelowIfAtBorder(int64_t block) const {
    int y = blockY(block);
    if ((y & 15) != 0) return 0;
    int x = blockX(block), z = blockZ(block);
    int lx = x & 15, lz = z & 15;
    if (lx != 0 && lx != 15 && lz != 0 && lz != 15) return 0;
    int sx = x >> 4, sy = y >> 4, sz = z >> 4;
    int empty = 0;
    while (!storage.storingLightForSection(sectionNode(sx, sy - empty - 1, sz)) && storage.hasLightDataAtOrBelow(sy - empty - 1)) empty++;
    return empty;
}

void SkyLightEngine::propagateFromEmptySections(int64_t to, int d, int level, bool increase, int emptyBelow) {
    if (emptyBelow == 0) return;
    int x = blockX(to), z = blockZ(to);
    int lx = x & 15, lz = z & 15;
    bool crossed = (d == kNorth && lz == 15) || (d == kSouth && lz == 0) || (d == kWest && lx == 15) || (d == kEast && lx == 0);
    if (!crossed) return;
    int sx = x >> 4, sz = z >> 4;
    int sy = (blockY(to) >> 4) - 1;
    int bottom = sy - emptyBelow + 1;
    for (; sy >= bottom; sy--) {
        if (!storage.storingLightForSection(sectionNode(sx, sy, sz))) continue;
        int minY = sy * 16;
        for (int ly = 15; ly >= 0; ly--) {
            int64_t block = blockNode(x, minY + ly, z);
            if (increase) {
                storage.setStoredLevel(block, level);
                if (level > 1) enqueueIncrease(block, entry::increaseSkipOneDirection(level, true, opposite(d)));
            } else {
                storage.setStoredLevel(block, 0);
                enqueueDecrease(block, entry::decreaseSkipOneDirection(level, opposite(d)));
            }
        }
    }
}

void SkyLightEngine::setLightEnabled(int chunkX, int chunkZ, bool enable) {
    LightEngine::setLightEnabled(chunkX, chunkZ, enable);
    if (!enable) return;
    const ChunkSkyLightSources* found = chunks_.skySources(chunkX, chunkZ);
    ChunkSkyLightSources empty(chunks_.minY());
    const ChunkSkyLightSources& sources = found ? *found : empty;
    int highestNonSourceY = sources.highestLowestSourceY() - 1;
    int lowestFullySourceSectionY = (highestNonSourceY >> 4) + 1;
    int64_t zero = sectionNode(chunkX, 0, chunkZ);
    int top = storage.topSectionY(zero);
    int bottom = std::max(storage.bottomSectionY(), lowestFullySourceSectionY);
    for (int sy = top - 1; sy >= bottom; sy--) {
        DataLayer* layer = storage.dataLayerToWrite(sectionNode(chunkX, sy, chunkZ));
        if (layer && layer->isEmpty()) layer->fill(15);
    }
}

void SkyLightEngine::propagateLightSources(int chunkX, int chunkZ) {
    int64_t zero = sectionNode(chunkX, 0, chunkZ);
    storage.setLightEnabled(zero, true);
    ChunkSkyLightSources emptySources(chunks_.minY());
    auto sourcesAt = [&](int cx, int cz) -> const ChunkSkyLightSources& {
        const ChunkSkyLightSources* s = chunks_.skySources(cx, cz);
        return s ? *s : emptySources;
    };
    const ChunkSkyLightSources &sources = sourcesAt(chunkX, chunkZ), &north = sourcesAt(chunkX, chunkZ - 1), &south = sourcesAt(chunkX, chunkZ + 1),
                               &west = sourcesAt(chunkX - 1, chunkZ), &east = sourcesAt(chunkX + 1, chunkZ);
    int top = storage.topSectionY(zero);
    int bottom = storage.bottomSectionY();
    int minX = chunkX * 16, minZ = chunkZ * 16;
    for (int sy = top - 1; sy >= bottom; sy--) {
        DataLayer* layer = storage.dataLayerToWrite(sectionNode(chunkX, sy, chunkZ));
        if (!layer) continue;
        int sectionMinY = sy * 16, sectionMaxY = sectionMinY + 15;
        bool sourcesBelow = false;
        for (int z = 0; z < 16; z++) {
            for (int x = 0; x < 16; x++) {
                int lowest = sources.lowestSourceY(x, z);
                if (lowest > sectionMaxY) continue;
                int n = z == 0 ? north.lowestSourceY(x, 15) : sources.lowestSourceY(x, z - 1);
                int s = z == 15 ? south.lowestSourceY(x, 0) : sources.lowestSourceY(x, z + 1);
                int w = x == 0 ? west.lowestSourceY(15, z) : sources.lowestSourceY(x - 1, z);
                int e = x == 15 ? east.lowestSourceY(0, z) : sources.lowestSourceY(x + 1, z);
                int neighborLowest = std::max(std::max(n, s), std::max(w, e));
                for (int y = sectionMaxY; y >= std::max(sectionMinY, lowest); y--) {
                    layer->set(x, y & 15, z, 15);
                    if (y == lowest || y < neighborLowest)
                        enqueueIncrease(blockNode(minX + x, y, minZ + z), entry::increaseSkySourceInDirections(y == lowest, y < n, y < s, y < w, y < e));
                }
                if (lowest < sectionMinY) sourcesBelow = true;
            }
        }
        if (!sourcesBelow) break;
    }
}

// ---------------------------------------------------------------- LevelLightEngine

LevelLightEngine::LevelLightEngine(LightChunkGetter& chunks, bool hasSkyLight)
    : chunks_(chunks), block_(std::make_unique<BlockLightEngine>(chunks)), sky_(hasSkyLight ? std::make_unique<SkyLightEngine>(chunks) : nullptr) {}

void LevelLightEngine::checkBlock(BlockPos pos) {
    int64_t node = blockNode(pos.x, pos.y, pos.z);
    block_->checkBlock(node);
    if (sky_) sky_->checkBlock(node);
}

int LevelLightEngine::runLightUpdates() {
    int count = block_->runLightUpdates();
    if (sky_) count += sky_->runLightUpdates();
    return count;
}

bool LevelLightEngine::hasLightWork() const { return (sky_ && sky_->hasLightWork()) || block_->hasLightWork(); }

void LevelLightEngine::updateSectionStatus(int sx, int sy, int sz, bool empty) {
    block_->storage.updateSectionStatus(sectionNode(sx, sy, sz), empty);
    if (sky_) sky_->storage.updateSectionStatus(sectionNode(sx, sy, sz), empty);
}

void LevelLightEngine::setLightEnabled(int chunkX, int chunkZ, bool enable) {
    block_->setLightEnabled(chunkX, chunkZ, enable);
    if (sky_) sky_->setLightEnabled(chunkX, chunkZ, enable);
}

void LevelLightEngine::propagateLightSources(int chunkX, int chunkZ) {
    block_->propagateLightSources(chunkX, chunkZ);
    if (sky_) sky_->propagateLightSources(chunkX, chunkZ);
}

void LevelLightEngine::queueSectionData(LightLayer layer, int sx, int sy, int sz, std::optional<DataLayer> data) {
    if (layer == LightLayer::Block) block_->storage.queueSectionData(sectionNode(sx, sy, sz), std::move(data));
    else if (sky_) sky_->storage.queueSectionData(sectionNode(sx, sy, sz), std::move(data));
}

void LevelLightEngine::retainData(int chunkX, int chunkZ, bool retain) {
    block_->storage.retainData(sectionNode(chunkX, 0, chunkZ), retain);
    if (sky_) sky_->storage.retainData(sectionNode(chunkX, 0, chunkZ), retain);
}

const DataLayer* LevelLightEngine::dataLayerData(LightLayer layer, int sx, int sy, int sz) const {
    if (layer == LightLayer::Block) return block_->storage.dataLayerData(sectionNode(sx, sy, sz));
    return sky_ ? sky_->storage.dataLayerData(sectionNode(sx, sy, sz)) : nullptr;
}

int LevelLightEngine::brightness(LightLayer layer, BlockPos pos) const {
    if (layer == LightLayer::Block) return block_->lightValue(pos.x, pos.y, pos.z);
    return sky_ ? sky_->lightValue(pos.x, pos.y, pos.z) : 0;
}

int LevelLightEngine::rawBrightness(BlockPos pos, int skyDarken) const {
    int sky = sky_ ? sky_->lightValue(pos.x, pos.y, pos.z) - skyDarken : 0;
    return std::max(block_->lightValue(pos.x, pos.y, pos.z), sky);
}

bool LevelLightEngine::lightOnInColumn(int chunkX, int chunkZ) const {
    int64_t zero = sectionNode(chunkX, 0, chunkZ);
    return block_->storage.lightOnInColumn(zero) && (!sky_ || sky_->storage.lightOnInColumn(zero));
}

void LevelLightEngine::initializeLight(int chunkX, int chunkZ, const std::vector<bool>& sectionEmpty, bool lighted) {
    int minSection = chunks_.minY() >> 4;
    for (size_t i = 0; i < sectionEmpty.size(); i++)
        if (!sectionEmpty[i]) updateSectionStatus(chunkX, minSection + static_cast<int>(i), chunkZ, false);
    runLightUpdates(); // (ThreadedLevelLightEngine.runUpdate: the pre-update tasks, the updates, then these)
    setLightEnabled(chunkX, chunkZ, lighted);
    retainData(chunkX, chunkZ, false);
}

void LevelLightEngine::lightChunk(int chunkX, int chunkZ, bool lighted) {
    if (!lighted) propagateLightSources(chunkX, chunkZ);
    runLightUpdates();
}

void LevelLightEngine::removeChunk(int chunkX, int chunkZ) {
    retainData(chunkX, chunkZ, false);
    setLightEnabled(chunkX, chunkZ, false);
    int minLight = minLightSection();
    for (int sy = minLight; sy < minLight + lightSectionCount(); sy++) {
        queueSectionData(LightLayer::Block, chunkX, sy, chunkZ, std::nullopt);
        queueSectionData(LightLayer::Sky, chunkX, sy, chunkZ, std::nullopt);
    }
    int minSection = chunks_.minY() >> 4;
    for (int i = 0; i < chunks_.sectionCount(); i++) updateSectionStatus(chunkX, minSection + i, chunkZ, true);
    runLightUpdates();
}

} // namespace mc::light
