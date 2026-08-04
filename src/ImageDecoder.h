#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <memory>
#include <functional>

// ─── 解码结果 ───
struct DecodeResult {
    int width  = 0;
    int height = 0;
    int stride = 0;               // 每行字节数
    std::vector<uint8_t> pixels;  // BGRA8 像素数据

    // 仅获取该解码结果的部分区域（用于瓦片裁剪）
    DecodeResult SubRegion(int x, int y, int w, int h) const;
    // 最近邻降采样到目标尺寸（用于金字塔层级解码）
    DecodeResult ScaleDown(int targetW, int targetH) const;
};

// ─── 图像格式元信息 ───
struct ImageInfo {
    int width       = 0;
    int height      = 0;
    int bitDepth    = 8;
    bool hasAlpha   = false;
    std::string format;   // "JPEG", "PNG", "WebP", "PSD" ...
    std::string decoderName;
    std::vector<uint8_t> iccProfile;  // ICC profile bytes（可能为空）
};

// ─── 瓦片请求 ───
struct TileRequest {
    int level;
    int col;
    int row;
};

// ─── 解码器接口 ───
class ImageDecoder {
public:
    virtual ~ImageDecoder() = default;

    // 打开图片流，返回基本信息
    // data = 文件全部或部分字节, len = 字节数
    // 返回 nullopt 表示无法解码
    struct OpenResult {
        ImageInfo info;
        // 解码器内部的持久状态（如文件流偏移）
        std::shared_ptr<void> state;
    };
    virtual std::optional<OpenResult> Open(const uint8_t* data, size_t len) = 0;

    // 解码完整图片（非瓦片模式）
    virtual std::optional<DecodeResult> DecodeFull(const OpenResult& open) = 0;

    // 解码指定层级的指定瓦片
    virtual std::optional<DecodeResult> DecodeTile(
        const OpenResult& open, int level, int col, int row) = 0;

    // 解码指定层级完整图片（预览用）
    virtual std::optional<DecodeResult> DecodeLevel(
        const OpenResult& open, int level) = 0;

    // 解码内嵌缩略图（用于切换图片时的快速预览占位）
    // 返回 nullopt 表示该格式不支持缩略图提取
    virtual std::optional<DecodeResult> DecodeThumbnail(const OpenResult& open) {
        (void)open;
        return std::nullopt;
    }

    // 解码器格式名
    virtual const char* Name() const = 0;

    // 是否支持金字塔分层解码
    virtual bool SupportsTiling() const { return false; }
};

// ─── 解码器注册中心  ───
// 仿照原 C# DecoderRegistry 的模式
// 但简化：由主程序在初始化时注册解码器
using DecoderFactoryFunc = std::unique_ptr<ImageDecoder>(*)();
void RegisterDecoder(DecoderFactoryFunc factory);
std::unique_ptr<ImageDecoder> FindDecoder(const uint8_t* magic, size_t len);
std::unique_ptr<ImageDecoder> FindDecoderByExtension(const std::string& ext);
std::vector<std::string> SupportedExtensions();

