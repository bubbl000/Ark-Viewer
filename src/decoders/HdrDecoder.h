#pragma once
#include "../ImageDecoder.h"
#include <mutex>

// Radiance HDR (.hdr / .pic) 解码器
// 纯算法实现（移植自 C# Ghde.Hdr），不依赖外部 DLL
// 支持 32-bit_rle_rgbe 格式：new-style RLE / old-style RLE / flat
class HdrDecoder : public ImageDecoder {
public:
    const char* Name() const override { return "HDR"; }
    std::optional<OpenResult> Open(const uint8_t* data, size_t len) override;
    std::optional<DecodeResult> DecodeFull(const OpenResult& open) override;
    std::optional<DecodeResult> DecodeTile(const OpenResult& open, int level, int col, int row) override;
    std::optional<DecodeResult> DecodeLevel(const OpenResult& open, int level) override;
    bool SupportsTiling() const override { return true; }
private:
    // 全尺寸解码缓存：HDR 不支持区域解码，全量解码一次后各层从缓存降采样
    std::optional<DecodeResult> _cachedFull;
    std::mutex _cacheMutex;
};
