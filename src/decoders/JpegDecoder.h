#pragma once
#include "../ImageDecoder.h"
#include <mutex>
#include <optional>
#include <vector>
#include <cstdint>

// libjpeg-turbo JPEG 解码器
class JpegDecoder : public ImageDecoder {
public:
    const char* Name() const override { return "JPEG"; }

    std::optional<OpenResult> Open(const uint8_t* data, size_t len) override;
    std::optional<DecodeResult> DecodeFull(const OpenResult& open) override;
    std::optional<DecodeResult> DecodeTile(const OpenResult& open,
        int level, int col, int row) override;
    std::optional<DecodeResult> DecodeLevel(const OpenResult& open,
        int level) override;
    std::optional<DecodeResult> DecodeThumbnail(const OpenResult& open) override;
    bool SupportsTiling() const override { return true; }

    // BGRA 像素 → JPEG 编码（另存为用）
    // bgra: 源像素，stride 为行跨度（可能 > w*4，GPU 对齐）
    // quality: 1-100，out: 输出 JPEG 字节流。失败返回 false
    static bool EncodeJpeg(const uint8_t* bgra, int w, int h, int stride,
                           int quality, std::vector<uint8_t>& out);
private:
    // 整层解码缓存：大层（>64M 像素）走 DecodeTile 时，同层多瓦片共享一次 DecodeLevel
    // 避免 turbojpeg 每片都重新解码整张图（1210ms/片 → 首片 ~269ms + 后续 SubRegion <1ms）
    int _cachedLevel = -1;
    std::optional<DecodeResult> _cachedLevelResult;
    std::mutex _cacheMutex;
};
