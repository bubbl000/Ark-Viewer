#include "TileCache.h"

TileCache::TileCache(size_t budgetBytes)
    : _budgetBytes(budgetBytes) {}

void TileCache::Put(TileIndex index, std::vector<uint8_t> pixels) {
    auto key = MakeKey(index);
    size_t size = pixels.size();
    std::lock_guard lock(_mutex);

    // 已存在则更新
    auto it = _map.find(key);
    if (it != _map.end()) {
        auto& entry = it->second;
        _currentBytes -= entry->pixelBytes;
        entry->pixels = std::make_shared<std::vector<uint8_t>>(std::move(pixels));
        entry->pixelBytes = size;
        _currentBytes += size;
        // 移到 LRU 前端
        _lruList.splice(_lruList.begin(), _lruList, it->second);
        return;
    }

    // 淘汰
    Evict(size);

    // 插入到 LRU 前端
    _lruList.emplace_front(CacheEntry{
        index, size,
        std::make_shared<std::vector<uint8_t>>(std::move(pixels))
    });
    _map[key] = _lruList.begin();
    _currentBytes += size;
}

std::shared_ptr<std::vector<uint8_t>> TileCache::Get(TileIndex index) {
    auto key = MakeKey(index);
    std::lock_guard lock(_mutex);

    auto it = _map.find(key);
    if (it == _map.end()) {
        _missCount.fetch_add(1, std::memory_order_relaxed);  // 性能遥测：未命中
        return nullptr;
    }

    // 移到 LRU 前端
    _lruList.splice(_lruList.begin(), _lruList, it->second);
    _hitCount.fetch_add(1, std::memory_order_relaxed);  // 性能遥测：命中
    return it->second->pixels;
}

void TileCache::ClearLevel(int level) {
    std::lock_guard lock(_mutex);
    for (auto it = _lruList.begin(); it != _lruList.end(); ) {
        if (it->index.level == level) {
            _currentBytes -= it->pixelBytes;
            _map.erase(MakeKey(it->index));
            it = _lruList.erase(it);
        } else {
            ++it;
        }
    }
}

void TileCache::Clear() {
    std::lock_guard lock(_mutex);
    _lruList.clear();
    _map.clear();
    _currentBytes = 0;
}

void TileCache::Evict(size_t needed) {
    while (_currentBytes + needed > _budgetBytes && !_lruList.empty()) {
        auto& entry = _lruList.back();
        _currentBytes -= entry.pixelBytes;
        _map.erase(MakeKey(entry.index));
        _lruList.pop_back();
    }
}
