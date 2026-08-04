#pragma once
#include "Tiling.h"
#include <vector>
#include <cstdint>

// ─── 多级分辨率金字塔 ───
// level 0 = 原始分辨率
// level n = 原始分辨率 >> n
//
// 选择显示哪层的规则：
//   displayScale = 缩放比
//   当 displayScale 落在某层的 2x 窗口内时切换
//
// 例：256x256 原图，level 0 = 256x256
//   level 1 = 128x128, level 2 = 64x64 ...

class Pyramid {
public:
    struct LevelInfo {
        int  index;
        int  width;
        int  height;
        int  tilesX;
        int  tilesY;
        int  area;            // width * height（用于排序/预算）
    };

    Pyramid() = default;

    // 根据图片尺寸构建金字塔
    static Pyramid ForSize(int imageW, int imageH, int minLevelSize = 256);

    // 根据缩放比选择最佳层
    // displayScale = 1.0 表示原始尺寸
    int SelectLevel(double displayScale) const;

    // 根据目标显示分辨率选择层
    int SelectLevelForDisplay(int displayW, int displayH) const;

    // 获取指定层的尺寸
    int  WidthAt(int level)  const { return level < (int)_levels.size() ? _levels[level].width  : 0; }
    int  HeightAt(int level) const { return level < (int)_levels.size() ? _levels[level].height : 0; }

    // 层的瓦片数
    int  TilesXAt(int level) const { return level < (int)_levels.size() ? _levels[level].tilesX : 0; }
    int  TilesYAt(int level) const { return level < (int)_levels.size() ? _levels[level].tilesY : 0; }
    int  TilesTotalAt(int level) const { return TilesXAt(level) * TilesYAt(level); }

    // 总层数
    int  LevelCount() const { return (int)_levels.size(); }

    // 顶层（最小分辨率）
    int  TopLevel() const { return (int)_levels.size() - 1; }

    // 获取所有层信息
    const std::vector<LevelInfo>& Levels() const { return _levels; }

    // 单层多级分辨率系数
    static int LevelScale(int level) { return 1 << level; }

private:
    std::vector<LevelInfo> _levels;
};

// ─── 视口描述 ───
struct Viewport {
    int x = 0;       // level-0 坐标
    int y = 0;
    int w = 0;       // level-0 宽度
    int h = 0;       // level-0 高度
    double scale = 1.0;  // 原图像素 / 屏幕像素
};
