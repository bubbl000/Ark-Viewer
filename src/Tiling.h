#include <algorithm>
#pragma once
#include <cstdint>

// ─── Tile geometry ───
constexpr int TILE_SIZE = 512;         // 每个瓦片的像素边长（512 减少瓦片数 4 倍）
constexpr int TILE_SIZE_LOG2 = 9;      // log2(TILE_SIZE)
constexpr int MAX_LEVELS = 16;         // 最大金字塔层数

// ─── Tile index ───
struct TileIndex {
    int level = 0;
    int col   = 0;
    int row   = 0;

    auto operator<=>(const TileIndex&) const = default;
};

// ─── 工具函数 ───
inline int TilesForDim(int pixels) {
    return (pixels + TILE_SIZE - 1) >> TILE_SIZE_LOG2;
}
inline int TileStart(int tileIdx) {
    return tileIdx * TILE_SIZE;
}
inline int TileEnd(int tileIdx, int dimPixels) {
    return (std::min)(TileStart(tileIdx) + TILE_SIZE, dimPixels);
}
inline int TileSize(int tileIdx, int dimPixels) { return TileEnd(tileIdx, dimPixels) - TileStart(tileIdx); }

// ─── 2D 矩形 ───
struct RectI { int x, y, w, h; };
struct RectF { float x, y, w, h; };



