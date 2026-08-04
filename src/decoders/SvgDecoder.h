#pragma once
#include "../ImageDecoder.h"

// SVG 矢量图解码器
// 使用 Direct2D ID2D1SvgDocument 原生渲染（Win10 1607+）
// 支持 .svg 和 .svgz（gzip 压缩），输出 BGRA8
class SvgDecoder : public ImageDecoder {
public:
    const char* Name() const override { return "SVG"; }
    std::optional<OpenResult> Open(const uint8_t* data, size_t len) override;
    std::optional<DecodeResult> DecodeFull(const OpenResult& open) override;
    std::optional<DecodeResult> DecodeTile(const OpenResult&, int, int, int) override { return std::nullopt; }
    std::optional<DecodeResult> DecodeLevel(const OpenResult&, int) override { return std::nullopt; }
    bool SupportsTiling() const override { return false; }
};
