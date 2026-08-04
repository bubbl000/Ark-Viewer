#include "Pyramid.h"
#include <algorithm>

Pyramid Pyramid::ForSize(int imageW, int imageH, int minLevelSize) {
    Pyramid pyramid;
    int w = imageW, h = imageH;
    int level = 0;

    while (w > 0 && h > 0) {
        Pyramid::LevelInfo info;
        info.index  = level;
        info.width  = w;
        info.height = h;
        info.tilesX = TilesForDim(w);
        info.tilesY = TilesForDim(h);
        info.area   = w * h;
        pyramid._levels.push_back(info);

        if (w <= minLevelSize && h <= minLevelSize) break;

        level++;
        w = std::max(1, w >> 1);
        h = std::max(1, h >> 1);
    }

    return pyramid;
}

int Pyramid::SelectLevel(double displayScale) const {
    // 原理：选择最接近但不大于 displayScale 的层
    // 每层对应 2^x 的缩放系数
    // displayScale = 1.0 时选 level 0（原始尺寸）
    // displayScale = 0.5 时可能在 level 1
    if (_levels.empty()) return 0;

    // displayScale 大于 1（放大）-> 始终选 level 0
    if (displayScale >= 1.0) return 0;

    // 缩小：找到"最接近但实际上小于" displayScale 比例的层
    // level n 的等效显示比例 = displayScale * 2^n
    for (int i = 0; i < (int)_levels.size() - 1; i++) {
        double levelScale = displayScale * (1 << (i + 1));
        if (levelScale >= 1.0) {
            // 选择 level i+1 时视觉效果不会显著劣化
            return i;
        }
    }
    return TopLevel();
}

int Pyramid::SelectLevelForDisplay(int displayW, int displayH) const {
    if (_levels.empty()) return 0;
    double scaleX = (double)displayW / _levels[0].width;
    double scaleY = (double)displayH / _levels[0].height;
    double scale = std::min(scaleX, scaleY);
    return SelectLevel(scale);
}
