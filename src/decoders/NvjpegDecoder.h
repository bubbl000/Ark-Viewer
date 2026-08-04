#pragma once
#include "../ImageDecoder.h"
#include "NvjpegHardDecoder.h"
#include "WicDecoder.h"

// JPEG 硬解解码器：硬解优先，失败回退 WicDecoder（CPU）
// 注册在 JpegDecoder 之后、WicDecoder 之前：
//   - N 卡可用 → 本类接管 JPEG，GPU 硬解加速
//   - 无 N 卡 → Open 返回 nullopt，DecoderFactory 继续到 WicDecoder（与改造前一致）
// DecodeThumbnail 不 override：用基类默认 nullopt，ImageEngine 自动用 DecodeLevel 顶层做缩略图
class NvjpegDecoder : public ImageDecoder {
public:
    std::optional<OpenResult> Open(const uint8_t* data, size_t len) override;
    std::optional<DecodeResult> DecodeFull(const OpenResult& open) override;
    std::optional<DecodeResult> DecodeTile(const OpenResult& open,
        int level, int col, int row) override;
    std::optional<DecodeResult> DecodeLevel(const OpenResult& open, int level) override;
    const char* Name() const override { return "JPEG"; }  // 与 JpegDecoder 同名，扩展名映射兼容
    bool SupportsTiling() const override { return true; }

private:
    NvjpegHardDecoder _hw;   // 硬解工具
    WicDecoder        _wic;  // CPU 回退解码器
};
