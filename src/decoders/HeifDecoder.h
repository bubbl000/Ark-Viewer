#pragma once
#include "../ImageDecoder.h"
#include <mutex>

// HEIF/HEIC 解码器
// 动态加载 heif.dll + libde265.dll（移植自 C# Ghde.Heif）
// 支持 HEIC/HEIF 主要品牌，输出 BGRA8
class HeifDecoder : public ImageDecoder {
public:
    const char* Name() const override { return "HEIF"; }
    std::optional<OpenResult> Open(const uint8_t* data, size_t len) override;
    std::optional<DecodeResult> DecodeFull(const OpenResult& open) override;
    std::optional<DecodeResult> DecodeTile(const OpenResult& open, int level, int col, int row) override;
    std::optional<DecodeResult> DecodeLevel(const OpenResult& open, int level) override;
    std::optional<DecodeResult> DecodeThumbnail(const OpenResult& open) override;
    bool SupportsTiling() const override { return true; }
private:
    // 全尺寸解码缓存：libheif 不支持区域解码，全量解码一次后各层从缓存降采样
    std::optional<DecodeResult> _cachedFull;
    std::mutex _cacheMutex;
};
