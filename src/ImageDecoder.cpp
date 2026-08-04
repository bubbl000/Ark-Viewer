#include "ImageDecoder.h"

DecodeResult DecodeResult::SubRegion(int x, int y, int w, int h) const {
    DecodeResult region;
    // 边界检查：越界或空数据返回空 region，避免 memcpy 越界崩溃（防御性兜底）
    if (w <= 0 || h <= 0 || pixels.empty()) return region;
    if (x < 0 || y < 0 || x + w > width || y + h > height) return region;
    if (stride < width * 4) return region;  // stride 异常

    region.width  = w;
    region.height = h;
    region.stride = w * 4;  // BGRA8

    region.pixels.resize((size_t)w * h * 4);
    const uint8_t* src = pixels.data() + (size_t)y * stride + (size_t)x * 4;
    uint8_t* dst = region.pixels.data();

    for (int row = 0; row < h; row++) {
        memcpy(dst, src, (size_t)w * 4);
        src += stride;
        dst += w * 4;
    }

    return region;
}

// 最近邻降采样：用于全量解码后生成金字塔各层（PSD/HEIF/RAW/HDR 等不支持区域解码的格式）
DecodeResult DecodeResult::ScaleDown(int targetW, int targetH) const {
    DecodeResult dst;
    dst.width = targetW;
    dst.height = targetH;
    dst.stride = targetW * 4;
    dst.pixels.resize((size_t)targetW * targetH * 4);
    for (int y = 0; y < targetH; y++) {
        int srcY = y * height / targetH;
        const uint8_t* srcRow = pixels.data() + (size_t)srcY * stride;
        uint8_t* dstRow = dst.pixels.data() + (size_t)y * dst.stride;
        for (int x = 0; x < targetW; x++) {
            memcpy(dstRow + (size_t)x * 4, srcRow + (size_t)(x * width / targetW) * 4, 4);
        }
    }
    return dst;
}
