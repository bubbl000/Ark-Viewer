#pragma once
#include "../ImageDecoder.h"

// WIC 回退解码器：处理 WIC 支持的所有格式
// 对应原 C# 的 WicDecoder
class WicDecoder : public ImageDecoder {
public:
    const char* Name() const override { return "WIC"; }
    std::optional<OpenResult> Open(const uint8_t* data, size_t len) override;
    std::optional<DecodeResult> DecodeFull(const OpenResult& open) override;
    std::optional<DecodeResult> DecodeTile(const OpenResult& open,
        int level, int col, int row) override;
    std::optional<DecodeResult> DecodeLevel(const OpenResult& open,
        int level) override;
    bool SupportsTiling() const override { return true; }

    // BGRA 像素 → PNG 编码，直接写文件（WIC IWICBitmapEncoder）
    static bool EncodePng(const uint8_t* bgra, int w, int h, int stride,
                          const wchar_t* path);
};
