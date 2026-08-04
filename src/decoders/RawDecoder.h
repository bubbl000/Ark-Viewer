#pragma once
#include "../ImageDecoder.h"
#include <mutex>

// 相机 RAW 解码器
// 动态加载 libraw.dll（移植自 C# Ghde.Raw）
// 支持 CR3/CR2/NEF/ARW/DNG/RAF/X3F/PEF/RW2/ORF，输出 BGRA8
class RawDecoder : public ImageDecoder {
public:
    const char* Name() const override { return "RAW"; }
    std::optional<OpenResult> Open(const uint8_t* data, size_t len) override;
    std::optional<DecodeResult> DecodeFull(const OpenResult& open) override;
    std::optional<DecodeResult> DecodeTile(const OpenResult& open, int level, int col, int row) override;
    std::optional<DecodeResult> DecodeLevel(const OpenResult& open, int level) override;
    // 提取相机内嵌的 JPEG/PPM 缩略图：避开 demosaic，用于快速预览占位
    // JPEG 缩略图走 N 卡 nvJPEG 硬解，无 N 卡回退 WIC；PPM 直接转 BGRA8
    std::optional<DecodeResult> DecodeThumbnail(const OpenResult& open) override;
    bool SupportsTiling() const override { return true; }
private:
    // 全尺寸解码缓存：libraw 不支持区域解码，全量解码一次后各层从缓存降采样
    std::optional<DecodeResult> _cachedFull;
    // 内嵌缩略图缓存：预览层(level≥3)复用，避免全尺寸 demosaic
    // 与 _cachedFull 独立，互不污染
    std::optional<DecodeResult> _cachedThumb;
    // 半尺寸解码缓存：level 1-2 专用，demosaic 像素数降 4 倍（half_size=1）
    // 与 _cachedFull/_cachedThumb 独立，避免缓存互相污染
    std::optional<DecodeResult> _cachedHalf;
    std::mutex _cacheMutex;

    // 半尺寸解码：half_size=1 输出原图/2，demosaic 快约 4 倍
    // 用于 level 1-2（目标 > 缩略图）场景，替代全尺寸 demosaic+ScaleDown
    std::optional<DecodeResult> DecodeHalfSize(const OpenResult& open);
};
