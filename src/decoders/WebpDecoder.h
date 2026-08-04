#pragma once
#include "../ImageDecoder.h"

class WebpDecoder : public ImageDecoder {
public:
    const char* Name() const override { return "WebP"; }
    std::optional<OpenResult> Open(const uint8_t* data, size_t len) override;
    std::optional<DecodeResult> DecodeFull(const OpenResult& open) override;
    std::optional<DecodeResult> DecodeTile(const OpenResult& open,
        int level, int col, int row) override;
    std::optional<DecodeResult> DecodeLevel(const OpenResult& open,
        int level) override;
    bool SupportsTiling() const override { return false; }

    // BGRA 像素 → WebP 编码（有损，quality 1-100），输出到 out
    static bool EncodeWebp(const uint8_t* bgra, int w, int h, int stride,
                           int quality, std::vector<uint8_t>& out);
};
