#pragma once
#include "Tiling.h"
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <list>
#include <mutex>
#include <memory>
#include <atomic>

// ─── LRU 瓦片缓存 ───
// 对应原 C# 的 LruTileCache
// 缓存解码后的瓦片像素数据，按使用时间淘汰

class TileCache {
public:
    static constexpr size_t DEFAULT_BUDGET_BYTES = 512ULL * 1024 * 1024; // 512MB

    explicit TileCache(size_t budgetBytes = DEFAULT_BUDGET_BYTES);

    // 存入瓦片
    void Put(TileIndex index, std::vector<uint8_t> pixels);

    // 获取瓦片（命中则更新 LRU 顺序）
    // 返回 nullptr 表示未命中
    std::shared_ptr<std::vector<uint8_t>> Get(TileIndex index);

    // 清除指定层级的所有瓦片
    void ClearLevel(int level);

    // 清除所有
    void Clear();

    // 当前缓存大小
    size_t SizeBytes() const { return _currentBytes; }

    // 设置预算
    void SetBudget(size_t bytes) { _budgetBytes = bytes; }

    // ── 性能遥测：命中/未命中计数（snapshot 读取后清零）──
    uint64_t HitCount() const noexcept { return _hitCount.load(std::memory_order_relaxed); }
    uint64_t MissCount() const noexcept { return _missCount.load(std::memory_order_relaxed); }
    void ResetHitMiss() noexcept {
        _hitCount.store(0, std::memory_order_relaxed);
        _missCount.store(0, std::memory_order_relaxed);
    }

private:
    struct CacheEntry {
        TileIndex index;
        size_t    pixelBytes;
        std::shared_ptr<std::vector<uint8_t>> pixels;
    };

    size_t _budgetBytes;
    size_t _currentBytes = 0;
    std::mutex _mutex;

    // 性能遥测计数器（mutable：const Get 也可写）
    mutable std::atomic<uint64_t> _hitCount{0};
    mutable std::atomic<uint64_t> _missCount{0};

    // LRU list + map
    std::list<CacheEntry> _lruList;
    using ListIter = std::list<CacheEntry>::iterator;
    std::unordered_map<uint64_t, ListIter> _map;

    static uint64_t MakeKey(TileIndex idx) {
        return ((uint64_t)(uint32_t)idx.level << 48) |
               ((uint64_t)(uint32_t)idx.col   << 24) |
               ((uint64_t)(uint32_t)idx.row);
    }

    void Evict(size_t needed);
};
