#include "NvjpegDecoder.h"
#include "NvjpegHardDecoder.h"
#include "../FileMapping.h"
#include "../Tiling.h"
#include "../ActivityLog.h"
#include "Logger.h"
#include <algorithm>

std::optional<ImageDecoder::OpenResult> NvjpegDecoder::Open(const uint8_t* data, size_t len) {
    // JPEG 魔数检测（FF D8 FF）
    if (len < 3 || data[0] != 0xFF || data[1] != 0xD8 || data[2] != 0xFF) return {};

    // 硬解不可用 → 返回 nullopt，让 DecoderFactory 继续到 WicDecoder 接管
    if (!NvjpegHardDecoder::Available()) return {};

    // 委托 WicDecoder.Open 获取 ImageInfo（宽高/格式）
    // state 由 ImageEngine 在 LoadFile 时注入 FileMapping
    auto result = _wic.Open(data, len);
    if (result) {
        result->info.decoderName = "nvJPEG";
        ActivityLog::Instance().Log(L"加载", L"nvJPEG 硬解接管 JPEG");
    }
    return result;
}

std::optional<DecodeResult> NvjpegDecoder::DecodeFull(const OpenResult& open) {
    // 硬解优先：从 FileMapping 取完整 JPEG 数据
    auto fileMap = std::static_pointer_cast<FileMapping>(open.state);
    if (fileMap && fileMap->Data()) {
        if (auto r = _hw.DecodeFull(fileMap->Data(), fileMap->Size())) return r;
        LOG_WARN("NVJPEG", "硬解 DecodeFull 失败，回退 WicDecoder");
    }
    // 回退 WicDecoder（CPU）
    return _wic.DecodeFull(open);
}

std::optional<DecodeResult> NvjpegDecoder::DecodeLevel(const OpenResult& open, int level) {
    auto fileMap = std::static_pointer_cast<FileMapping>(open.state);
    if (fileMap && fileMap->Data()) {
        if (auto r = _hw.DecodeLevel(fileMap->Data(), fileMap->Size(), level)) return r;
        LOG_WARN_STREAM("NVJPEG") << "硬解 DecodeLevel(" << level << ") 失败，回退 WicDecoder";
    }
    return _wic.DecodeLevel(open, level);
}

std::optional<DecodeResult> NvjpegDecoder::DecodeTile(const OpenResult& open,
    int level, int col, int row)
{
    // 硬解路径：整层解码 + 裁剪瓦片（与 JpegDecoder turbojpeg 现状一致，nvJPEG 不做区域解码）
    // GPU 整层硬解速度快于 CPU 区域解码，瓦片由 TileCache 缓存避免重复解码
    auto fileMap = std::static_pointer_cast<FileMapping>(open.state);
    if (fileMap && fileMap->Data()) {
        auto full = _hw.DecodeLevel(fileMap->Data(), fileMap->Size(), level);
        if (full) {
            int tileX = col * TILE_SIZE;
            int tileY = row * TILE_SIZE;
            int tileW = (std::min)(TILE_SIZE, full->width - tileX);
            int tileH = (std::min)(TILE_SIZE, full->height - tileY);
            if (tileW > 0 && tileH > 0) return full->SubRegion(tileX, tileY, tileW, tileH);
        }
    }
    // 回退 WicDecoder：CopyPixels 区域解码（只解相关 MCU，CPU 路径更高效）
    return _wic.DecodeTile(open, level, col, row);
}
