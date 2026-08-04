#pragma once
#include "../ImageDecoder.h"

class PngDecoder : public ImageDecoder {
public:
    const char* Name() const override { return "PNG"; }
    std::optional<OpenResult> Open(const uint8_t* data, size_t len) override;
    std::optional<DecodeResult> DecodeFull(const OpenResult& open) override;
    std::optional<DecodeResult> DecodeTile(const OpenResult& open,
        int level, int col, int row) override;
    std::optional<DecodeResult> DecodeLevel(const OpenResult& open,
        int level) override;
    bool SupportsTiling() const override { return true; }
};
