#include "PsdDecoder.h"
#include <cstring>
#include <cmath>
#include <algorithm>
#include "Logger.h"
#include "miniz.h"
#include "../FileMapping.h"
#include "../Tiling.h"
#ifndef ARK_THUMB_DLL
#include "NvjpegHardDecoder.h"  // PSD 缩略图 JPEG 硬解（N 卡）
#endif

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

// WIC 工厂单例：用 IWICImagingFactory 解码 PSD 内嵌的 JPEG 缩略图
// 不依赖 turbojpeg.dll，避免与 JpegDecoder 的全局函数指针耦合
static IWICImagingFactory* WIC() {
    static ComPtr<IWICImagingFactory> f;
    if (!f) CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    return f.Get();
}

// 用 WIC 从内存 JPEG 数据解码为 BGRA8 像素
// 用于 PSD 缩略图（资源 ID 1036/1033 内嵌 JPEG）
static std::optional<DecodeResult> DecodeJpegWithWic(const uint8_t* jpeg, size_t len) {
    auto* wic = WIC();
    if (!wic || !jpeg || len == 0) return std::nullopt;

    ComPtr<IWICStream> stream;
    if (FAILED(wic->CreateStream(&stream))) return std::nullopt;
    // InitializeFromMemory 要求 BYTE*，但 WIC 不会修改数据，const_cast 安全
    if (FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(jpeg), (DWORD)len)))
        return std::nullopt;

    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr,
        WICDecodeMetadataCacheOnDemand, &dec)))
        return std::nullopt;

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return std::nullopt;

    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    if (w == 0 || h == 0) return std::nullopt;

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic->CreateFormatConverter(&converter))) return std::nullopt;
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
        return std::nullopt;

    DecodeResult result;
    result.width = (int)w;
    result.height = (int)h;
    result.stride = (int)w * 4;
    result.pixels.resize((size_t)result.stride * h);
    if (FAILED(converter->CopyPixels(nullptr, result.stride,
        (UINT)result.pixels.size(), result.pixels.data())))
        return std::nullopt;
    return result;
}

// PSD 缩略图 JPEG 解码：N 卡硬解优先，失败/无 N 卡回退 WIC（CPU）
// 硬解对缩略图这种小尺寸 JPEG 收益有限，但避免 CPU JPEG 解码开销
// DLL 模式（ARK_THUMB_DLL）无 CUDA，直接走 WIC
static std::optional<DecodeResult> DecodeJpegPreferHw(const uint8_t* jpeg, size_t len) {
#ifndef ARK_THUMB_DLL
    if (NvjpegHardDecoder::Available()) {
        NvjpegHardDecoder hw;
        if (auto r = hw.DecodeFull(jpeg, len)) return r;
        LOG_WARN("PsdDecoder", "缩略图硬解失败，回退 WIC");
    }
#endif
    return DecodeJpegWithWic(jpeg, len);
}

// ─── PSD/PSB 解码器（移植自 C# Ghde.Psd） ───
// 支持：PSD v1 / PSB v2
// 颜色模式：RGB(3) / Grayscale(1) / CMYK(4) / Bitmap(0)
// 位深：1 / 8 / 16 / 32 bit
// 压缩：Raw(0) / RLE(1) / ZIP(2) / ZIP+Prediction(3)
// 输出统一为 BGRA8

namespace {

// 大端读取辅助
struct MemReader {
    const uint8_t* data;
    size_t len;
    size_t pos;

    bool Eof() const { return pos >= len; }
    size_t Remaining() const { return pos < len ? len - pos : 0; }

    uint8_t ReadU8() {
        if (pos >= len) return 0;
        return data[pos++];
    }
    uint16_t ReadBE16() {
        if (pos + 2 > len) { pos = len; return 0; }
        uint16_t v = (uint16_t)((data[pos] << 8) | data[pos + 1]);
        pos += 2;
        return v;
    }
    int32_t ReadBE32() {
        if (pos + 4 > len) { pos = len; return 0; }
        int32_t v = (data[pos] << 24) | (data[pos + 1] << 16) |
                    (data[pos + 2] << 8) | data[pos + 3];
        pos += 4;
        return v;
    }
    int64_t ReadBE64() {
        if (pos + 8 > len) { pos = len; return 0; }
        int64_t v = 0;
        for (int i = 0; i < 8; i++) v = (v << 8) | data[pos + i];
        pos += 8;
        return v;
    }
    // 跳过指定字节数
    void Skip(size_t n) {
        pos = (pos + n <= len) ? pos + n : len;
    }
};

// PSD 头结构
struct PsdHeader {
    uint16_t version;     // 1=PSD, 2=PSB
    int width;
    int height;
    int16_t channels;
    int16_t depth;        // 1/8/16/32
    int16_t colorMode;    // 0=Bitmap,1=Gray,3=RGB,4=CMYK
};

// 颜色模式对应的颜色通道数（不含 alpha）
int ChannelCountForMode(int16_t colorMode) {
    switch (colorMode) {
        case 0: return 1;  // Bitmap
        case 1: return 1;  // Grayscale
        case 3: return 3;  // RGB
        case 4: return 4;  // CMYK
        case 7: return 4;  // Multichannel
        case 9: return 3;  // Lab
        default: return 3;
    }
}

// PackBits 解码（PSD RLE）
// n∈[0,127]: 复制后 n+1 字节；n∈[-127,-1]: 重复下一字节 -n+1 次；n=-128: 跳过
size_t UnpackBits(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen) {
    size_t si = 0, di = 0;
    while (si < srcLen && di < dstLen) {
        int8_t n = (int8_t)src[si++];
        if (n >= 0) {
            size_t count = (size_t)n + 1;
            for (size_t j = 0; j < count && di < dstLen && si < srcLen; j++)
                dst[di++] = src[si++];
        } else if (n > -128) {
            size_t count = (size_t)(-n) + 1;
            if (si >= srcLen) break;
            uint8_t b = src[si++];
            for (size_t j = 0; j < count && di < dstLen; j++)
                dst[di++] = b;
        }
        // n == -128: 无操作
    }
    return di;
}

// Paeth 预测器（PNG 滤镜用）
int PaethPredictor(int a, int b, int c) {
    int p = a + b - c;
    int pa = std::abs(p - a);
    int pb = std::abs(p - b);
    int pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

// 反转 PSD "ZIP with prediction" 的 PNG 滤镜
// 每行布局：[1字节滤镜类型][rowBytes 像素数据]
void ApplyPredictionReversal(uint8_t* data, int rowBytes, int height, int bytesPerSample) {
    int rowStride = rowBytes + 1;  // +1 为滤镜字节
    std::vector<uint8_t> prevRow(rowBytes, 0), curRow(rowBytes);

    for (int y = 0; y < height; y++) {
        int rowStart = y * rowStride;
        uint8_t filter = data[rowStart];
        uint8_t* pixelStart = data + rowStart + 1;

        std::memcpy(curRow.data(), pixelStart, rowBytes);

        switch (filter) {
            case 0:  // None
                break;
            case 1:  // Sub: cur[x] += cur[x - bpp]
                for (int i = bytesPerSample; i < rowBytes; i++)
                    curRow[i] = (uint8_t)(curRow[i] + curRow[i - bytesPerSample]);
                break;
            case 2:  // Up: cur[x] += prev[x]
                for (int i = 0; i < rowBytes; i++)
                    curRow[i] = (uint8_t)(curRow[i] + prevRow[i]);
                break;
            case 3:  // Average: cur[x] += (prev[x] + cur[x - bpp]) / 2
                for (int i = 0; i < rowBytes; i++) {
                    int left = (i < bytesPerSample) ? 0 : curRow[i - bytesPerSample];
                    int up = prevRow[i];
                    curRow[i] = (uint8_t)(curRow[i] + ((left + up) >> 1));
                }
                break;
            case 4:  // Paeth
                for (int i = 0; i < rowBytes; i++) {
                    int left = (i < bytesPerSample) ? 0 : curRow[i - bytesPerSample];
                    int up = prevRow[i];
                    int upLeft = (i < bytesPerSample) ? 0 : prevRow[i - bytesPerSample];
                    curRow[i] = (uint8_t)(curRow[i] + PaethPredictor(left, up, upLeft));
                }
                break;
            default:
                break;
        }

        std::memcpy(pixelStart, curRow.data(), rowBytes);
        std::swap(prevRow, curRow);
    }
}

// 用 miniz 解压 raw DEFLATE 流（无 zlib 头）
// 返回解压后的字节数，失败返回 0
size_t InflateRaw(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstLen) {
    mz_stream stream = {};
    stream.next_in = src;
    stream.avail_in = (mz_uint32)srcLen;
    stream.next_out = dst;
    stream.avail_out = (mz_uint32)dstLen;

    // 负 window_bits = raw DEFLATE（无 zlib 头）
    if (mz_inflateInit2(&stream, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK)
        return 0;

    int status = mz_inflate(&stream, MZ_FINISH);
    mz_inflateEnd(&stream);

    if (status != MZ_STREAM_END && status != MZ_OK)
        return 0;

    return (size_t)stream.total_out;
}

// 32-bit float → 8-bit：Reinhard tone map + 线性 sRGB
uint8_t FloatTo8Bit(float v) {
    if (v <= 0.0f) return 0;
    // Reinhard tone map（处理 HDR 值 > 1.0）
    float mapped = v / (1.0f + v);
    // 线性 → sRGB gamma 近似
    float srgb = std::pow(mapped, 1.0f / 2.2f);
    int val = (int)(srgb * 255.0f + 0.5f);
    return (uint8_t)std::clamp(val, 0, 255);
}

}  // namespace

// 前向声明（定义在后面，供 DecodeFull 调用）
static bool DecodeRaw(MemReader& r, int width, int height, int totalChans,
                      int bytesPerSample, std::vector<std::vector<uint8_t>>& out);
static bool DecodeRle(MemReader& r, uint16_t version, int width, int height,
                      int totalChans, int bytesPerSample,
                      std::vector<std::vector<uint8_t>>& out);
static bool DecodeZip(MemReader& r, uint16_t version, int width, int height,
                      int totalChans, int bytesPerSample, bool withPrediction,
                      size_t pixelDataStart, size_t fileSize,
                      std::vector<std::vector<uint8_t>>& out);
static void InterleaveToBGRA(const std::vector<std::vector<uint8_t>>& chans,
                             int width, int height, int depth, int colorMode,
                             int colorChans, int totalChans, bool hasAlpha,
                             std::vector<uint8_t>& out);

// ─── Open：魔数检测 + 头解析 ───
std::optional<ImageDecoder::OpenResult> PsdDecoder::Open(const uint8_t* data, size_t len) {
    if (len < 26) return std::nullopt;
    // 魔数 "8BPS"
    if (data[0] != '8' || data[1] != 'B' || data[2] != 'P' || data[3] != 'S')
        return std::nullopt;

    MemReader r{data, len, 0};
    r.pos = 4;
    uint16_t version = r.ReadBE16();
    if (version != 1 && version != 2) return std::nullopt;

    r.Skip(6);  // reserved
    int16_t channels = (int16_t)r.ReadBE16();
    int height = r.ReadBE32();
    int width = r.ReadBE32();
    int16_t depth = (int16_t)r.ReadBE16();
    int16_t colorMode = (int16_t)r.ReadBE16();

    if (width <= 0 || height <= 0 || channels <= 0) return std::nullopt;
    if (depth != 1 && depth != 8 && depth != 16 && depth != 32) return std::nullopt;

    int colorChans = ChannelCountForMode(colorMode);
    bool hasAlpha = channels > colorChans;

    OpenResult result;
    result.info.width = width;
    result.info.height = height;
    result.info.bitDepth = depth;
    result.info.hasAlpha = hasAlpha;
    result.info.format = (version == 2) ? "PSB" : "PSD";
    result.info.decoderName = "PSD Parser";
    return result;
}

// ─── DecodeFull：完整解码 ───
std::optional<DecodeResult> PsdDecoder::DecodeFull(const OpenResult& open) {
    auto fileMap = std::static_pointer_cast<FileMapping>(open.state);
    if (!fileMap || !fileMap->Data()) return std::nullopt;

    MemReader r{fileMap->Data(), fileMap->Size(), 0};

    // 重新解析头部（Open 只传了 4096 字节，这里用完整数据）
    r.pos = 4;
    uint16_t version = r.ReadBE16();
    r.Skip(6);
    int16_t channels = (int16_t)r.ReadBE16();
    int height = r.ReadBE32();
    int width = r.ReadBE32();
    int16_t depth = (int16_t)r.ReadBE16();
    int16_t colorMode = (int16_t)r.ReadBE16();

    int colorChans = ChannelCountForMode(colorMode);
    int totalChans = channels;
    bool hasAlpha = totalChans > colorChans;
    int bytesPerSample = depth / 8;
    if (bytesPerSample == 0 && depth == 1) bytesPerSample = 1;  // 1-bit 特殊处理

    // 跳过 Color Mode Data / Image Resources / Layer/Mask Info
    // Color Mode Data: 4 字节长度（PSB 也是 4 字节）
    r.Skip(r.ReadBE32());
    // Image Resources: 4 字节长度
    r.Skip(r.ReadBE32());
    // Layer/Mask Info: PSB 用 8 字节长度，PSD 用 4 字节
    if (version == 2)
        r.Skip((size_t)r.ReadBE64());
    else
        r.Skip((size_t)r.ReadBE32());

    // Image Data Section
    uint16_t compression = r.ReadBE16();
    if (compression > 3) {
        LOG_WARN("PsdDecoder", "不支持的压缩方法");
        return std::nullopt;
    }

    // PSB + ZIP 时有 8 字节长度前缀
    size_t pixelDataStart = r.pos;
    if (version == 2 && (compression == 2 || compression == 3)) {
        r.ReadBE64();  // 跳过长度前缀
        pixelDataStart = r.pos;
    }

    int rowBytes = width * bytesPerSample;
    size_t chanSize = (size_t)width * height * bytesPerSample;

    // 每通道解压后的像素数据
    std::vector<std::vector<uint8_t>> channelPixels(totalChans);
    for (int c = 0; c < totalChans; c++)
        channelPixels[c].resize(chanSize);

    bool ok = false;
    switch (compression) {
        case 0:  // Raw
            ok = DecodeRaw(r, width, height, totalChans, bytesPerSample, channelPixels);
            break;
        case 1:  // RLE
            ok = DecodeRle(r, version, width, height, totalChans, bytesPerSample, channelPixels);
            break;
        case 2:  // ZIP
        case 3:  // ZIP + Prediction
            ok = DecodeZip(r, version, width, height, totalChans, bytesPerSample,
                          compression == 3, pixelDataStart, fileMap->Size(), channelPixels);
            break;
    }

    if (!ok) {
        LOG_WARN("PsdDecoder", "解码失败");
        return std::nullopt;
    }

    // 交错通道 → BGRA8
    DecodeResult result;
    result.width = width;
    result.height = height;
    result.stride = width * 4;
    result.pixels.resize((size_t)width * height * 4);

    InterleaveToBGRA(channelPixels, width, height, depth, colorMode,
                     colorChans, totalChans, hasAlpha, result.pixels);

    LOG_INFO_STREAM("PsdDecoder") << "已解码: " << width << "x" << height
                                   << " depth=" << depth << " mode=" << colorMode
                                   << " comp=" << compression;
    return result;
}

// ─── Raw 解码：平面布局，直接读取 ───
static bool DecodeRaw(MemReader& r, int width, int height, int totalChans,
                      int bytesPerSample, std::vector<std::vector<uint8_t>>& out) {
    size_t chanSize = (size_t)width * height * bytesPerSample;
    for (int c = 0; c < totalChans; c++) {
        if (r.Remaining() < chanSize) {
            // 截断文件：零填充剩余部分
            size_t avail = r.Remaining();
            if (avail > 0) std::memcpy(out[c].data(), r.data + r.pos, avail);
            r.pos = r.len;
            break;
        }
        std::memcpy(out[c].data(), r.data + r.pos, chanSize);
        r.pos += chanSize;
    }
    return true;
}

// ─── RLE 解码：PackBits，每行独立压缩 ───
static bool DecodeRle(MemReader& r, uint16_t version, int width, int height,
                      int totalChans, int bytesPerSample,
                      std::vector<std::vector<uint8_t>>& out) {
    size_t totalRows = (size_t)height * totalChans;
    int entrySize = (version == 2) ? 4 : 2;

    // 读取行长度表
    size_t tableBytes = totalRows * entrySize;
    if (r.Remaining() < tableBytes) return false;

    std::vector<int> rowLengths(totalRows);
    for (size_t i = 0; i < totalRows; i++) {
        if (entrySize == 4) {
            rowLengths[i] = (r.data[r.pos] << 24) | (r.data[r.pos + 1] << 16) |
                            (r.data[r.pos + 2] << 8) | r.data[r.pos + 3];
        } else {
            rowLengths[i] = (r.data[r.pos] << 8) | r.data[r.pos + 1];
        }
        r.pos += entrySize;
    }

    // 压缩数据紧随其后
    const uint8_t* compData = r.data + r.pos;
    size_t compRemaining = r.Remaining();

    // 计算每行的压缩偏移
    std::vector<size_t> compOffsets(totalRows);
    size_t cumOffset = 0;
    for (size_t i = 0; i < totalRows; i++) {
        compOffsets[i] = cumOffset;
        cumOffset += rowLengths[i];
    }

    int rowBytes = width * bytesPerSample;
    std::vector<uint8_t> rowScratch(rowBytes);

    for (int c = 0; c < totalChans; c++) {
        size_t channelRowBase = (size_t)c * height;
        for (int row = 0; row < height; row++) {
            size_t rowIdx = channelRowBase + row;
            size_t offset = compOffsets[rowIdx];
            int length = rowLengths[rowIdx];

            if (offset + length > compRemaining) return false;

            size_t decoded = UnpackBits(compData + offset, length, rowScratch.data(), rowBytes);
            if (decoded < (size_t)rowBytes) {
                // 不足部分零填充
                std::memset(rowScratch.data() + decoded, 0, rowBytes - decoded);
            }

            size_t dstOffset = (size_t)row * rowBytes;
            std::memcpy(out[c].data() + dstOffset, rowScratch.data(), rowBytes);
        }
    }

    return true;
}

// ─── ZIP 解码：每通道独立 raw DEFLATE ───
static bool DecodeZip(MemReader& r, uint16_t version, int width, int height,
                      int totalChans, int bytesPerSample, bool withPrediction,
                      size_t pixelDataStart, size_t fileSize,
                      std::vector<std::vector<uint8_t>>& out) {
    int rowBytes = width * bytesPerSample;
    // withPrediction 时每行多 1 字节滤镜类型
    size_t rawBufSize = withPrediction
        ? (size_t)(rowBytes + 1) * height
        : (size_t)rowBytes * height;

    std::vector<uint8_t> rawBuf(rawBufSize);
    bool hasLengthPrefix = (version == 2);

    size_t pos = pixelDataStart;

    for (int c = 0; c < totalChans; c++) {
        size_t compressedLen = 0;
        if (hasLengthPrefix) {
            // PSB: 4 字节大端长度前缀
            if (pos + 4 > fileSize) return false;
            compressedLen = (size_t)((r.data[pos] << 24) | (r.data[pos + 1] << 16) |
                                     (r.data[pos + 2] << 8) | r.data[pos + 3]);
            pos += 4;
        }

        if (pos >= fileSize) return false;
        size_t avail = fileSize - pos;
        if (hasLengthPrefix && compressedLen < avail)
            avail = compressedLen;

        // 用 miniz 解压 raw DEFLATE
        size_t decompressed = InflateRaw(r.data + pos, avail, rawBuf.data(), rawBufSize);
        if (decompressed < rawBufSize) {
            // 截断：零填充
            std::memset(rawBuf.data() + decompressed, 0, rawBufSize - decompressed);
        }

        if (hasLengthPrefix) {
            pos += compressedLen;
        } else {
            // PSD v1: 无法确定精确消耗量，跳到剩余数据末尾
            // 实际中 PSD v1 + ZIP 极少见
            pos = fileSize;
        }

        // 反转 PNG 滤镜预测
        if (withPrediction) {
            ApplyPredictionReversal(rawBuf.data(), rowBytes, height, bytesPerSample);
        }

        // 提取像素行到通道缓冲
        int srcPixelOffset = withPrediction ? 1 : 0;
        size_t srcRowStride = withPrediction ? (size_t)rowBytes + 1 : (size_t)rowBytes;
        for (int row = 0; row < height; row++) {
            size_t srcOffset = (size_t)row * srcRowStride + srcPixelOffset;
            size_t dstOffset = (size_t)row * rowBytes;
            std::memcpy(out[c].data() + dstOffset, rawBuf.data() + srcOffset, rowBytes);
        }
    }

    return true;
}

// ─── 交错通道 → BGRA8 ───
static void InterleaveToBGRA(const std::vector<std::vector<uint8_t>>& chans,
                             int width, int height, int depth, int colorMode,
                             int colorChans, int totalChans, bool hasAlpha,
                             std::vector<uint8_t>& out) {
    int bytesPerSample = depth / 8;
    if (bytesPerSample == 0) bytesPerSample = 1;
    size_t pixelCount = (size_t)width * height;

    auto getSample = [&](int c, size_t pixelIdx) -> uint8_t {
        if (c >= totalChans) return 255;
        const uint8_t* p = chans[c].data() + pixelIdx * bytesPerSample;
        if (depth == 1) {
            // 1-bit: 每字节 8 像素，高位在前
            size_t byteIdx = pixelIdx / 8;
            int bitIdx = 7 - (int)(pixelIdx % 8);
            if (byteIdx >= chans[c].size()) return 0;
            return (chans[c][byteIdx] >> bitIdx) & 1 ? 255 : 0;
        }
        if (depth == 8) return *p;
        if (depth == 16) return p[0];  // 取高字节（大端）
        if (depth == 32) {
            // 32-bit float
            float v;
            std::memcpy(&v, p, 4);
            return FloatTo8Bit(v);
        }
        return *p;
    };

    int alphaChan = colorChans;  // alpha 通道索引

    for (size_t i = 0; i < pixelCount; i++) {
        uint8_t b, g, r, a;
        switch (colorMode) {
            case 0:  // Bitmap
            case 1:  // Grayscale
                g = getSample(0, i);
                b = g; r = g;
                a = hasAlpha ? getSample(alphaChan, i) : 255;
                break;
            case 3:  // RGB
                r = getSample(0, i);
                g = getSample(1, i);
                b = getSample(2, i);
                a = hasAlpha ? getSample(alphaChan, i) : 255;
                break;
            case 4:  // CMYK
            case 7: { // Multichannel
                // 简单 CMYK → RGB：反相后合成
                int C = 255 - getSample(0, i);
                int M = 255 - getSample(1, i);
                int Y = 255 - getSample(2, i);
                int K = 255 - getSample(3, i);
                r = (uint8_t)std::clamp((C * K) / 255, 0, 255);
                g = (uint8_t)std::clamp((M * K) / 255, 0, 255);
                b = (uint8_t)std::clamp((Y * K) / 255, 0, 255);
                a = hasAlpha ? getSample(alphaChan, i) : 255;
                break;
            }
            case 9:  // Lab（简化处理，直接取值）
                r = getSample(0, i);
                g = getSample(1, i);
                b = getSample(2, i);
                a = hasAlpha ? getSample(alphaChan, i) : 255;
                break;
            default:
                r = getSample(0, i);
                g = getSample(1, i);
                b = getSample(2, i);
                a = hasAlpha ? getSample(alphaChan, i) : 255;
                break;
        }
        size_t off = i * 4;
        out[off] = b;
        out[off + 1] = g;
        out[off + 2] = r;
        out[off + 3] = a;
    }
}

// DecodeLevel：首次调用时全量解码并缓存，后续各层从缓存 CPU 降采样
// PSD 不支持区域解码，全量解码一次后复用，避免每个瓦片重复全量解码
std::optional<DecodeResult> PsdDecoder::DecodeLevel(const OpenResult& open, int level) {
    std::lock_guard lock(_cacheMutex);
    if (!_cachedFull) {
        _cachedFull = DecodeFull(open);
        if (!_cachedFull) return std::nullopt;
    }
    int targetW = (std::max)(1, _cachedFull->width  >> level);
    int targetH = (std::max)(1, _cachedFull->height >> level);
    if (level == 0) return *_cachedFull;
    return _cachedFull->ScaleDown(targetW, targetH);
}

// DecodeTile：仅 level 0 支持从缓存全尺寸裁剪瓦片（大层路径）
// level > 0 的大层返回 nullopt（中小层走 ImageEngine 本层缓存路径，不调此方法）
std::optional<DecodeResult> PsdDecoder::DecodeTile(const OpenResult& open,
    int level, int col, int row) {
    if (level != 0) return std::nullopt;
    std::lock_guard lock(_cacheMutex);
    if (!_cachedFull) {
        _cachedFull = DecodeFull(open);
        if (!_cachedFull) return std::nullopt;
    }
    int tileX = col * TILE_SIZE;
    int tileY = row * TILE_SIZE;
    int tileW = (std::min)(TILE_SIZE, _cachedFull->width  - tileX);
    int tileH = (std::min)(TILE_SIZE, _cachedFull->height - tileY);
    if (tileW <= 0 || tileH <= 0) return std::nullopt;
    return _cachedFull->SubRegion(tileX, tileY, tileW, tileH);
}

// ─── DecodeThumbnail：解析 Image Resources 提取内嵌 JPEG 缩略图 ───
// 资源块格式（Adobe PSD 规范）：
//   "8BIM"(4) + id(2 BE) + pascal 名称(填充至偶数) + 数据长度(4 BE) + 数据(填充至偶数)
// 缩略图资源（ID 1036 = Photoshop 5.0+，1033 = 4.0）数据布局：
//   格式(4 BE) + 宽(4 BE) + 高(4 BE) + 行字节数(4 BE) + 总大小(4 BE) +
//   压缩大小(4 BE) + 每像素位数(2 BE) + 平面数(2 BE) + JPEG 数据
// 格式=1 表示 JPEG RGB（1036 用 RGB；1033 用 BGR，但极少见，此处只处理 JPEG）
std::optional<DecodeResult> PsdDecoder::DecodeThumbnail(const OpenResult& open) {
    auto fileMap = std::static_pointer_cast<FileMapping>(open.state);
    if (!fileMap || !fileMap->Data()) return std::nullopt;

    const uint8_t* data = fileMap->Data();
    size_t len = fileMap->Size();
    if (len < 26) return std::nullopt;

    // 头部已由 Open 验证过 "8BPS"，此处直接用 MemReader 从头遍历
    MemReader r{data, len, 0};
    r.pos = 4;                          // 跳过 "8BPS"
    r.ReadBE16();                       // version（1=PSD, 2=PSB，此处不用）
    r.Skip(6);                          // reserved
    r.ReadBE16();                       // channels
    r.ReadBE32();                       // height
    r.ReadBE32();                       // width
    r.ReadBE16();                       // depth
    r.ReadBE16();                       // colorMode

    // Color Mode Data：4 字节长度 + 数据（PSD/PSB 均为 4 字节）
    int32_t colorModeLen = r.ReadBE32();
    if (colorModeLen < 0) return std::nullopt;
    r.Skip((size_t)colorModeLen);

    // Image Resources：4 字节长度 + 资源块序列
    int32_t irLen = r.ReadBE32();
    if (irLen <= 0) return std::nullopt;
    size_t irEnd = r.pos + (size_t)irLen;
    if (irEnd > len) return std::nullopt;

    // 遍历资源块查找缩略图（ID 1036 优先，1033 次之）
    // 第一遍找 1036，未找到再扫一遍 1033（避免一次扫描误命中 1033 而错过 1036）
    for (int pass = 0; pass < 2; pass++) {
        uint16_t targetId = (pass == 0) ? 0x040C : 0x0409;  // 1036 / 1033
        size_t pos = r.pos;  // 重置扫描位置（每遍从 Image Resources 起点开始）
        while (pos + 12 <= irEnd) {
            // 签名 "8BIM"
            if (data[pos] != '8' || data[pos + 1] != 'B' ||
                data[pos + 2] != 'I' || data[pos + 3] != 'M') break;
            size_t p = pos + 4;
            uint16_t id = (uint16_t)((data[p] << 8) | data[p + 1]);
            p += 2;

            // Pascal 字符串：1 字节长度 + 内容，整体填充至偶数
            // 名称长度 + 1（长度字节本身）后向上取偶
            uint8_t nameLen = data[p++];
            size_t paddedName = ((size_t)nameLen + 1 + 1) & ~(size_t)1;
            p += paddedName - 1;  // 已读 1 字节长度
            if (p + 4 > irEnd) break;

            // 数据长度（4 字节 BE），数据本身也要填充至偶数
            int32_t dataLen = (data[p] << 24) | (data[p + 1] << 16) |
                              (data[p + 2] << 8) | data[p + 3];
            p += 4;
            if (dataLen < 0 || p + (size_t)dataLen > irEnd) break;
            size_t paddedData = ((size_t)dataLen + 1) & ~(size_t)1;

            if (id == targetId && dataLen > 28) {
                // 缩略图资源头：28 字节
                // 格式(4) + 宽(4) + 高(4) + 行字节(4) + 总大小(4) + 压缩大小(4) + 位数(2) + 平面(2)
                int format = (data[p] << 24) | (data[p + 1] << 16) |
                             (data[p + 2] << 8) | data[p + 3];
                // compressedSize 字段（offset 20-23）：JPEG 数据的实际字节数
                // 用 compressedSize 而非 dataLen-28，避免 dataLen 包含偶数填充字节
                // 导致 "extraneous bytes before marker 0xd9" 解码错误
                int32_t compressedSize = (data[p + 20] << 24) | (data[p + 21] << 16) |
                                         (data[p + 22] << 8) | data[p + 23];
                // JPEG 数据紧跟 28 字节头之后
                const uint8_t* jpeg = data + p + 28;
                // 优先用 compressedSize；异常时回退 dataLen-28
                size_t jpegLen = (compressedSize > 0 && compressedSize <= dataLen - 28)
                    ? (size_t)compressedSize
                    : (size_t)dataLen - 28;

                // 校验 JPEG 起始魔数 FFD8
                if (format == 1 && jpegLen > 2 &&
                    jpeg[0] == 0xFF && jpeg[1] == 0xD8) {
                    auto result = DecodeJpegPreferHw(jpeg, jpegLen);
                    if (result) {
                        LOG_INFO_STREAM("PsdDecoder")
                            << "缩略图已提取: " << result->width << "x"
                            << result->height << " (资源 ID=" << (int)id << ")";
                        return result;
                    }
                }
                // format=0（raw RGB，PS 4.0 BGR）或解码失败：继续扫描
            }

            pos = p + paddedData;  // 跳到下一个资源块
        }
    }

    return std::nullopt;  // 无缩略图资源
}
