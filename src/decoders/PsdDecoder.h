#pragma once
#include "../ImageDecoder.h"
#include <mutex>

class PsdDecoder : public ImageDecoder {
public:
    const char* Name() const override { return "PSD"; }
    std::optional<OpenResult> Open(const uint8_t* data, size_t len) override;
    std::optional<DecodeResult> DecodeFull(const OpenResult& open) override;
    std::optional<DecodeResult> DecodeTile(const OpenResult& open,
        int level, int col, int row) override;
    std::optional<DecodeResult> DecodeLevel(const OpenResult& open,
        int level) override;
    // 从 Image Resources 区块提取内嵌 JPEG 缩略图（资源 ID 1036 优先，1033 次之）
    // 用于切换 PSD/PSB 时的快速预览占位，避免主线程等待全量解码
    std::optional<DecodeResult> DecodeThumbnail(const OpenResult& open) override;
    bool SupportsTiling() const override { return true; }
private:
    // 全尺寸解码缓存：DecodeLevel 首次调用时 DecodeFull 并缓存，后续各层从缓存降采样
    // PSD 不支持区域解码，全量解码一次后复用避免重复解码
    std::optional<DecodeResult> _cachedFull;
    std::mutex _cacheMutex;
};
