#include "WicDecoder.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <cstring>
#include <algorithm>
#include "Logger.h"
#include "../FileMapping.h"
#include "../Tiling.h"

using Microsoft::WRL::ComPtr;

// ─── WIC 工厂（全局单例延迟初始化） ───
static IWICImagingFactory* GetWicFactory() {
    static ComPtr<IWICImagingFactory> factory;
    if (!factory) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    }
    return factory.Get();
}

// 魔数预筛：只让 WIC 可能支持的格式进入流解码，避免对 PSD 等无谓创建流
static bool IsWicFormat(const uint8_t* data, size_t len) {
    // BMP: "BM"
    if (len >= 2 && data[0] == 'B' && data[1] == 'M') return true;
    // PNG: 89 50 4E 47
    if (len >= 4 && data[0] == 0x89 && data[1] == 0x50 &&
        data[2] == 0x4E && data[3] == 0x47) return true;
    // JPEG: FF D8 FF
    if (len >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) return true;
    // TIFF: II 2A 00 / MM 00 2A
    if (len >= 4) {
        if ((data[0] == 'I' && data[1] == 'I' && data[2] == 0x2A && data[3] == 0) ||
            (data[0] == 'M' && data[1] == 'M' && data[2] == 0 && data[3] == 0x2A))
            return true;
    }
    // GIF: "GIF8"
    if (len >= 4 && data[0] == 'G' && data[1] == 'I' &&
        data[2] == 'F' && data[3] == '8') return true;
    // WebP: RIFF....WEBP（Win10 1809+ WIC 内置支持）
    if (len >= 12 && memcmp(data, "RIFF", 4) == 0 &&
        memcmp(data + 8, "WEBP", 4) == 0) return true;
    // ICO: 00 00 01 00
    if (len >= 4 && data[0] == 0x00 && data[1] == 0x00 &&
        data[2] == 0x01 && data[3] == 0x00) return true;
    // CUR: 00 00 02 00
    if (len >= 4 && data[0] == 0x00 && data[1] == 0x00 &&
        data[2] == 0x02 && data[3] == 0x00) return true;
    // DDS: "DDS "（Win8+ WIC 内置）
    if (len >= 4 && memcmp(data, "DDS ", 4) == 0) return true;
    // ANI: RIFF....ACON
    if (len >= 12 && memcmp(data, "RIFF", 4) == 0 &&
        memcmp(data + 8, "ACON", 4) == 0) return true;
    return false;
}

// 从内存数据创建 WIC 解码器（Open 与 DecodeFull 共用）
// 注意：InitializeFromMemory 要求 BYTE*，但 WIC 不会修改数据，const_cast 安全
static ComPtr<IWICBitmapDecoder> CreateWicDecoder(IWICImagingFactory* factory,
    const uint8_t* data, size_t len) {
    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream))) return nullptr;
    if (FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(data), (DWORD)len)))
        return nullptr;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr,
        WICDecodeMetadataCacheOnDemand, &decoder)))
        return nullptr;
    return decoder;
}

std::optional<ImageDecoder::OpenResult> WicDecoder::Open(const uint8_t* data, size_t len) {
    // 魔数预筛 + 兜底 WIC 探测，任一命中即认作 WIC 格式
    bool matched = IsWicFormat(data, len);
    if (!matched) {
        auto* factory = GetWicFactory();
        if (!factory || !CreateWicDecoder(factory, data, len)) return std::nullopt;
    }

    OpenResult result;
    result.info.format = "WIC";
    result.info.decoderName = "WIC";

    // 解析尺寸：金字塔构建与瓦片分层都依赖 width/height
    // 注意 FindDecoder 阶段只传 4096 字节，多数格式头足以解析尺寸；
    // 解析失败时留 0，由 LoadFile 用完整数据二次 Open 补齐
    auto* factory = GetWicFactory();
    if (factory) {
        auto decoder = CreateWicDecoder(factory, data, len);
        if (decoder) {
            ComPtr<IWICBitmapFrameDecode> frame;
            if (SUCCEEDED(decoder->GetFrame(0, &frame))) {
                UINT w = 0, h = 0;
                frame->GetSize(&w, &h);
                result.info.width = (int)w;
                result.info.height = (int)h;
            }
        }
    }
    // TIFF 魔数命中但 WIC 取不到有效尺寸：可能是 RAW（ARW/CR2/NEF 等），
    // WIC 的 TIFF 解码器读不懂压缩 RAW → GetSize 返回 0×0
    // 拒绝让 RawDecoder 接手，避免 ARW 显示空白 0×0 图
    bool isTiff = (len >= 4) &&
        ((data[0] == 'I' && data[1] == 'I' && data[2] == 0x2A && data[3] == 0) ||
         (data[0] == 'M' && data[1] == 'M' && data[2] == 0 && data[3] == 0x2A));
    if (isTiff && result.info.width == 0 && result.info.height == 0) {
        return std::nullopt;
    }

    LOG_INFO_STREAM("WicDecoder") << "Open 匹配 " << result.info.width << "x" << result.info.height;
    return result;
}

std::optional<DecodeResult> WicDecoder::DecodeFull(const OpenResult& open) {
    auto* factory = GetWicFactory();
    if (!factory) { LOG_WARN("WicDecoder", "WIC 工厂不可用"); return std::nullopt; }

    // open.state 由 ImageEngine 注入，持有 FileMapping 内存映射
    auto fileMap = std::static_pointer_cast<FileMapping>(open.state);
    if (!fileMap || !fileMap->Data()) { LOG_WARN("WicDecoder", "无文件数据"); return std::nullopt; }

    auto decoder = CreateWicDecoder(factory, fileMap->Data(), fileMap->Size());
    if (!decoder) { LOG_WARN_STREAM("WicDecoder") << "CreateWicDecoder 失败 size=" << fileMap->Size(); return std::nullopt; }

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) { LOG_WARN("WicDecoder", "GetFrame 失败"); return std::nullopt; }

    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    if (w == 0 || h == 0) { LOG_WARN("WicDecoder", "尺寸为 0"); return std::nullopt; }

    // 统一转 BGRA8，下游无需关心 WIC 原生格式
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter))) { LOG_WARN("WicDecoder", "CreateFormatConverter 失败"); return std::nullopt; }
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        LOG_WARN("WicDecoder", "Initialize converter 失败"); return std::nullopt;
    }

    DecodeResult result;
    result.width = (int)w;
    result.height = (int)h;
    result.stride = (int)w * 4;
    result.pixels.resize((size_t)result.stride * result.height);

    if (FAILED(converter->CopyPixels(nullptr, result.stride,
        (UINT)result.pixels.size(), result.pixels.data()))) {
        LOG_WARN("WicDecoder", "CopyPixels 失败"); return std::nullopt;
    }

    LOG_INFO_STREAM("WicDecoder") << "已解码: " << w << "x" << h;
    return result;
}

std::optional<DecodeResult> WicDecoder::DecodeLevel(const OpenResult& open, int level) {
    auto* factory = GetWicFactory();
    if (!factory) return std::nullopt;
    auto fileMap = std::static_pointer_cast<FileMapping>(open.state);
    if (!fileMap || !fileMap->Data()) return std::nullopt;

    auto decoder = CreateWicDecoder(factory, fileMap->Data(), fileMap->Size());
    if (!decoder) return std::nullopt;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return std::nullopt;

    UINT origW = 0, origH = 0;
    frame->GetSize(&origW, &origH);
    if (origW == 0 || origH == 0) return std::nullopt;

    // 目标层尺寸：origW >> level，至少 1 像素
    int targetW = (std::max)(1, (int)origW >> level);
    int targetH = (std::max)(1, (int)origH >> level);

    // 统一转 BGRA8
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter))) return std::nullopt;
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
        return std::nullopt;

    DecodeResult result;
    result.width = targetW;
    result.height = targetH;
    result.stride = targetW * 4;
    result.pixels.resize((size_t)result.stride * targetH);

    // level 0 直接拷贝原始尺寸；level > 0 用 BitmapScaler 降采样
    // Fant 插值质量优于最近邻，降采样时无明显锯齿
    if (level == 0) {
        if (FAILED(converter->CopyPixels(nullptr, result.stride,
            (UINT)result.pixels.size(), result.pixels.data())))
            return std::nullopt;
    } else {
        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(factory->CreateBitmapScaler(&scaler))) return std::nullopt;
        if (FAILED(scaler->Initialize(converter.Get(), (UINT)targetW, (UINT)targetH,
            WICBitmapInterpolationModeFant)))
            return std::nullopt;
        if (FAILED(scaler->CopyPixels(nullptr, result.stride,
            (UINT)result.pixels.size(), result.pixels.data())))
            return std::nullopt;
    }
    return result;
}

std::optional<DecodeResult> WicDecoder::DecodeTile(const OpenResult& open,
    int level, int col, int row)
{
    // 区域解码：直接 CopyPixels(瓦片矩形)，WIC 对 JPEG 内部用 libjpeg MCU 级跳过，
    // level 0 大图无需解全图，每块瓦片只解 512×512 相关 MCU
    auto* factory = GetWicFactory();
    if (!factory) return std::nullopt;
    auto fileMap = std::static_pointer_cast<FileMapping>(open.state);
    if (!fileMap || !fileMap->Data()) return std::nullopt;

    auto decoder = CreateWicDecoder(factory, fileMap->Data(), fileMap->Size());
    if (!decoder) return std::nullopt;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return std::nullopt;

    UINT origW = 0, origH = 0;
    frame->GetSize(&origW, &origH);
    if (origW == 0 || origH == 0) return std::nullopt;

    int targetW = (std::max)(1, (int)origW >> level);
    int targetH = (std::max)(1, (int)origH >> level);

    int tileX = col * TILE_SIZE;
    int tileY = row * TILE_SIZE;
    int tileW = (std::min)(TILE_SIZE, targetW - tileX);
    int tileH = (std::min)(TILE_SIZE, targetH - tileY);
    if (tileW <= 0 || tileH <= 0) return std::nullopt;

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter))) return std::nullopt;
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
        return std::nullopt;

    DecodeResult result;
    result.width = tileW;
    result.height = tileH;
    result.stride = tileW * 4;
    result.pixels.resize((size_t)result.stride * tileH);

    WICRect rect = { tileX, tileY, tileW, tileH };

    if (level == 0) {
        // level 0：converter 直接区域 CopyPixels，WIC JPEG 内部只解相关 MCU
        if (FAILED(converter->CopyPixels(&rect, result.stride,
            (UINT)result.pixels.size(), result.pixels.data())))
            return std::nullopt;
    } else {
        // level > 0：scaler 降采样后区域 CopyPixels
        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(factory->CreateBitmapScaler(&scaler))) return std::nullopt;
        if (FAILED(scaler->Initialize(converter.Get(), (UINT)targetW, (UINT)targetH,
            WICBitmapInterpolationModeFant)))
            return std::nullopt;
        if (FAILED(scaler->CopyPixels(&rect, result.stride,
            (UINT)result.pixels.size(), result.pixels.data())))
            return std::nullopt;
    }
    return result;
}

// ─── 辅助函数：用 WIC 从文件路径解码到 BGRA8（保留外部导出） ───
extern "C" __declspec(dllexport)
bool WicDecodeFile(const wchar_t* path, uint8_t** outPixels,
    int* outW, int* outH, int* outStride)
{
    auto* factory = GetWicFactory();
    if (!factory) return false;

    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = factory->CreateDecoderFromFilename(
        path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) return false;

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) return false;

    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    if (w == 0 || h == 0) return false;

    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr)) return false;

    hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return false;

    *outW = (int)w;
    *outH = (int)h;
    *outStride = (int)w * 4;

    *outPixels = (uint8_t*)malloc((size_t)(*outStride) * (*outH));
    hr = converter->CopyPixels(nullptr, *outStride, (UINT)(*outStride) * (*outH), *outPixels);
    return SUCCEEDED(hr);
}

// BGRA 像素 → PNG 文件（WIC 编码器 + 内存位图源）
bool WicDecoder::EncodePng(const uint8_t* bgra, int w, int h, int stride,
                           const wchar_t* path) {
    auto* factory = GetWicFactory();
    if (!factory || !bgra || w <= 0 || h <= 0 || stride < w * 4 || !path) return false;

    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)))
        return false;

    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream))) return false;
    if (FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE))) return false;
    if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> bag;
    if (FAILED(encoder->CreateNewFrame(&frame, &bag))) return false;
    if (FAILED(frame->Initialize(bag.Get()))) return false;
    if (FAILED(frame->SetSize(w, h))) return false;

    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&fmt))) return false;

    // 从内存建位图源（引用传入像素，WriteSource 期间立即拷贝入编码器）
    ComPtr<IWICBitmap> bitmap;
    if (FAILED(factory->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppBGRA,
            (UINT)stride, (UINT)(stride * h), const_cast<BYTE*>(bgra), &bitmap)))
        return false;
    if (FAILED(frame->WriteSource(bitmap.Get(), nullptr))) return false;
    if (FAILED(frame->Commit())) return false;
    if (FAILED(encoder->Commit())) return false;
    return true;
}
