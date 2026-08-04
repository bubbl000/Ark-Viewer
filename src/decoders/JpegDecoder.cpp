#include "JpegDecoder.h"
#include <cstring>
#include <algorithm>
#include "Logger.h"
#include <cstdlib>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "../FileMapping.h"
#include "../Tiling.h"

// turbojpeg C API
typedef void* (__stdcall *fn_tjInit)();
typedef int   (__stdcall *fn_tjHeader)(void*, unsigned char*, unsigned long, int*, int*, int*);
typedef int   (__stdcall *fn_tjDecode)(void*, unsigned char*, unsigned long, unsigned char*, int, int, int, int, int);
typedef int   (__stdcall *fn_tjKill)(void*);
// 编码相关（另存为用）
typedef void* (__stdcall *fn_tjInitC)();
typedef int   (__stdcall *fn_tjCompress)(void*, const unsigned char*, int, int, int, int, unsigned char**, unsigned long*, int, int, int);
typedef void  (__stdcall *fn_tjFree)(unsigned char*);

// turbojpeg 官方枚举：TJPF_BGRA = 8（之前误写 4 = TJPF_XBGR，导致 BGR 通道错位变色）
static const int TJPF_BGRA = 8;
static const int TJFLAG_FASTUPSAMPLE = 256;
static const int TJSAMP_420 = 2;  // 4:2:0 子采样（JPEG 默认，体积/质量平衡）

static HMODULE g_tj = nullptr;
static fn_tjInit    tjI = nullptr;
static fn_tjHeader  tjH = nullptr;
static fn_tjDecode  tjD = nullptr;
static fn_tjKill    tjK = nullptr;
// 编码函数指针（懒加载，缺失不影响解码）
static fn_tjInitC    tjIC = nullptr;
static fn_tjCompress tjC  = nullptr;
static fn_tjFree     tjF  = nullptr;

static bool LoadTJ() {
    if (g_tj) return true;
    g_tj = LoadLibraryW(L"turbojpeg.dll");
    if (!g_tj) return false;
    tjI = (fn_tjInit)GetProcAddress(g_tj, "tjInitDecompress");
    tjH = (fn_tjHeader)GetProcAddress(g_tj, "tjDecompressHeader2");
    tjD = (fn_tjDecode)GetProcAddress(g_tj, "tjDecompress2");
    tjK = (fn_tjKill)GetProcAddress(g_tj, "tjDestroy");
    // 编码接口（turbojpeg.dll 标准导出，加载失败仅影响 EncodeJpeg，不影响解码）
    tjIC = (fn_tjInitC)GetProcAddress(g_tj, "tjInitCompress");
    tjC  = (fn_tjCompress)GetProcAddress(g_tj, "tjCompress2");
    tjF  = (fn_tjFree)GetProcAddress(g_tj, "tjFree");
    return tjI && tjH && tjD && tjK;
}

// BGRA 像素 → JPEG 编码（另存为用）
// stride 为行跨度（GPU pitch 可能 > w*4，tjCompress2 的 pitch 参数支持任意跨度）
bool JpegDecoder::EncodeJpeg(const uint8_t* bgra, int w, int h, int stride,
                             int quality, std::vector<uint8_t>& out) {
    if (!LoadTJ() || !tjIC || !tjC || !tjF) return false;
    if (!bgra || w <= 0 || h <= 0 || stride < w * 4) return false;
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;

    void* hc = tjIC();  // 编码句柄（避开高度参数 h 同名）
    if (!hc) return false;
    unsigned char* jpegBuf = nullptr;
    unsigned long  jpegSize = 0;
    // tjCompress2：jpegBuf 传 nullptr 时内部 tjAlloc 分配，需用 tjFree 释放
    int r = tjC(hc, bgra, w, stride, h, TJPF_BGRA, &jpegBuf, &jpegSize, TJSAMP_420, quality, 0);
    tjK(hc);  // tjDestroy 不释放 jpegBuf
    if (r != 0 || !jpegBuf || jpegSize == 0) {
        if (jpegBuf) tjF(jpegBuf);
        return false;
    }
    out.assign(jpegBuf, jpegBuf + jpegSize);
    tjF(jpegBuf);
    return true;
}

std::optional<ImageDecoder::OpenResult> JpegDecoder::Open(const uint8_t* data, size_t len) {
    if (len < 3 || data[0] != 0xFF || data[1] != 0xD8 || data[2] != 0xFF) return {};
    if (!LoadTJ()) return {};
    void* h = tjI(); if (!h) return {};
    int w=0, hh=0, s=0;
    int r = tjH(h, (unsigned char*)data, (unsigned long)len, &w, &hh, &s);
    tjK(h);
    if (r != 0 || w <= 0 || hh <= 0) return {};
    OpenResult res; res.info.width = w; res.info.height = hh;
    res.info.format = "JPEG"; res.info.decoderName = "libjpeg-turbo";
    return res;
}
std::optional<DecodeResult> JpegDecoder::DecodeFull(const OpenResult& open) {
    if (!LoadTJ()) return {};
    auto fileMap = std::static_pointer_cast<FileMapping>(open.state);
    if (!fileMap || !fileMap->Data()) return {};

    void* h = tjI(); if (!h) return {};
    int w=0,hh=0,s=0;
    // libjpeg-turbo 要求 unsigned char*（非 const），但实际不修改数据，const_cast 安全
    auto* data = const_cast<uint8_t*>(fileMap->Data());
    if (tjH(h, data, (unsigned long)fileMap->Size(), &w, &hh, &s) != 0) { tjK(h); return {}; }

    DecodeResult result;
    result.width = w; result.height = hh; result.stride = w * 4;
    result.pixels.resize((size_t)w * hh * 4);
    tjD(h, data, (unsigned long)fileMap->Size(),
        result.pixels.data(), w, w*4, hh, TJPF_BGRA, TJFLAG_FASTUPSAMPLE);
    tjK(h);
    // JPEG 无 alpha 通道，全部设为 255（不透明）
    for (size_t i = 3; i < result.pixels.size(); i += 4) result.pixels[i] = 255;
    return result;
}

extern "C" __declspec(dllexport)
bool TurboJpegDecode(const uint8_t* jpegData, size_t jpegLen,
    uint8_t** outPixels, int* outW, int* outH, int* outStride)
{
    if (!LoadTJ()) return false;
    void* h = tjI(); if (!h) return false;
    int w=0,hh=0,s=0;
    if (tjH(h,(unsigned char*)jpegData,(unsigned long)jpegLen,&w,&hh,&s)!=0) { tjK(h); return false; }
    *outW=w; *outH=hh; *outStride=w*4;
    *outPixels=(uint8_t*)malloc((size_t)w*hh*4);
    if (!*outPixels) { tjK(h); return false; }
    tjD(h,(unsigned char*)jpegData,(unsigned long)jpegLen,*outPixels,w,w*4,hh,TJPF_BGRA,TJFLAG_FASTUPSAMPLE);
    tjK(h);
    return true;
}
// 最近邻下采样：turbojpeg 1/8 降采样后的二次缩放（level > 3 时使用）
static DecodeResult ScaleDown(const DecodeResult& src, int targetW, int targetH) {
    DecodeResult dst;
    dst.width = targetW; dst.height = targetH; dst.stride = targetW * 4;
    dst.pixels.resize((size_t)targetW * targetH * 4);
    for (int y = 0; y < targetH; y++) {
        int srcY = y * src.height / targetH;
        const uint8_t* srcRow = src.pixels.data() + srcY * src.stride;
        uint8_t* dstRow = dst.pixels.data() + y * dst.stride;
        for (int x = 0; x < targetW; x++) {
            memcpy(dstRow + x * 4, srcRow + (x * src.width / targetW) * 4, 4);
        }
    }
    return dst;
}

std::optional<DecodeResult> JpegDecoder::DecodeLevel(const OpenResult& open, int level) {
    if (!LoadTJ()) return {};
    auto fileMap = std::static_pointer_cast<FileMapping>(open.state);
    if (!fileMap || !fileMap->Data()) return {};

    void* h = tjI(); if (!h) return {};
    auto* data = const_cast<uint8_t*>(fileMap->Data());

    int origW = 0, origH = 0, subsamp = 0;
    if (tjH(h, data, (unsigned long)fileMap->Size(), &origW, &origH, &subsamp) != 0) {
        tjK(h); return {};
    }
    if (origW <= 0 || origH <= 0) { tjK(h); return {}; }

    // turbojpeg 原生支持 1/2^n 降采样（n ≤ 3，即 1/2、1/4、1/8）
    // 通过 DCT 系数丢弃实现，不解码全量像素，极快
    int tjLevel = (level <= 3) ? level : 3;
    // TJSCALED(dim, {1, 2^n}) = (dim + 2^n - 1) >> n
    int scaledW = (origW + (1 << tjLevel) - 1) >> tjLevel;
    int scaledH = (origH + (1 << tjLevel) - 1) >> tjLevel;

    DecodeResult result;
    result.width = scaledW; result.height = scaledH;
    result.stride = scaledW * 4;
    result.pixels.resize((size_t)result.stride * scaledH);

    if (tjD(h, data, (unsigned long)fileMap->Size(),
            result.pixels.data(), scaledW, scaledW * 4, scaledH,
            TJPF_BGRA, TJFLAG_FASTUPSAMPLE) != 0) {
        tjK(h); return {};
    }
    tjK(h);

    // JPEG 无 alpha，全部设为 255
    for (size_t i = 3; i < result.pixels.size(); i += 4) result.pixels[i] = 255;

    // level > 3 时 turbojpeg 降采样不够，CPU 端再缩放
    if (tjLevel < level) {
        int targetW = (std::max)(1, origW >> level);
        int targetH = (std::max)(1, origH >> level);
        result = ScaleDown(result, targetW, targetH);
    }
    return result;
}

std::optional<DecodeResult> JpegDecoder::DecodeTile(const OpenResult& open,
    int level, int col, int row)
{
    // 整层缓存：大层多瓦片共享一次 DecodeLevel，避免每片都重新解码整张图
    // mutex 保证首个线程 DecodeLevel 独占写，后续线程从缓存 SubRegion（memcpy 1MB，极快）
    std::lock_guard lock(_cacheMutex);
    if (_cachedLevel != level || !_cachedLevelResult) {
        auto full = DecodeLevel(open, level);
        if (!full) return {};
        _cachedLevelResult = std::move(*full);
        _cachedLevel = level;
    }

    int tileX = col * TILE_SIZE;
    int tileY = row * TILE_SIZE;
    int tileW = (std::min)(TILE_SIZE, _cachedLevelResult->width - tileX);
    int tileH = (std::min)(TILE_SIZE, _cachedLevelResult->height - tileY);
    if (tileW <= 0 || tileH <= 0) return {};

    return _cachedLevelResult->SubRegion(tileX, tileY, tileW, tileH);
}

// ─── EXIF 缩略图提取 ───
// 解析 JPEG APP1 EXIF，跟随 IFD0 → IFD1，读取 tag 0x0201(偏移) + 0x0202(长度)
// 定位到内嵌 JPEG 数据后用 turbojpeg 解码

static uint16_t ExifReadU16(const uint8_t* p, bool le) {
    return le ? (uint16_t)(p[0] | (p[1] << 8)) : (uint16_t)((p[0] << 8) | p[1]);
}
static uint32_t ExifReadU32(const uint8_t* p, bool le) {
    return le ? (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24))
              : (uint32_t)(((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
}

// 扫描 marker 定位 APP1 EXIF，返回 TIFF header 在 fileMap 中的偏移（"II"/"MM" 处）
// 找不到返回 SIZE_MAX
static size_t FindExifTiffOffset(const uint8_t* data, size_t len) {
    if (len < 4 || data[0] != 0xFF || data[1] != 0xD8) return SIZE_MAX;
    size_t pos = 2;  // 跳过 SOI
    while (pos + 4 <= len) {
        if (data[pos] != 0xFF) return SIZE_MAX;
        uint8_t marker = data[pos + 1];
        // 无 payload 的 standalone marker（RSTn/SOI/EOI）
        if (marker == 0xD8 || marker == 0xD9 || (marker >= 0xD0 && marker <= 0xD7))
            return SIZE_MAX;
        uint16_t segLen = (uint16_t)((data[pos + 2] << 8) | data[pos + 3]);
        if (segLen < 2 || pos + 2 + segLen > len) return SIZE_MAX;

        if (marker == 0xE1 && segLen >= 8 &&
            memcmp(data + pos + 4, "Exif\0\0", 6) == 0) {
            return pos + 4 + 6;  // TIFF header 起始
        }
        pos += 2 + segLen;
    }
    return SIZE_MAX;
}

std::optional<DecodeResult> JpegDecoder::DecodeThumbnail(const OpenResult& open) {
    if (!LoadTJ()) return std::nullopt;
    auto fileMap = std::static_pointer_cast<FileMapping>(open.state);
    if (!fileMap || !fileMap->Data()) return std::nullopt;

    const uint8_t* data = fileMap->Data();
    size_t len = fileMap->Size();

    // 1. 定位 EXIF TIFF header
    size_t tiffOffset = FindExifTiffOffset(data, len);
    if (tiffOffset == SIZE_MAX || tiffOffset + 8 > len) return std::nullopt;

    const uint8_t* tiff = data + tiffOffset;
    bool le = (tiff[0] == 'I' && tiff[1] == 'I');
    if (!le && !(tiff[0] == 'M' && tiff[1] == 'M')) return std::nullopt;
    if (ExifReadU16(tiff + 2, le) != 0x002A) return std::nullopt;

    uint32_t ifd0Offset = ExifReadU32(tiff + 4, le);
    if (ifd0Offset == 0 || (size_t)ifd0Offset + 2 > len - tiffOffset) return std::nullopt;

    // 2. IFD0 末尾取 next IFD offset → IFD1
    const uint8_t* ifd0 = tiff + ifd0Offset;
    uint16_t ifd0Count = ExifReadU16(ifd0, le);
    if ((size_t)ifd0Offset + 2 + (size_t)ifd0Count * 12 + 4 > len - tiffOffset) return std::nullopt;
    uint32_t ifd1Offset = ExifReadU32(ifd0 + 2 + ifd0Count * 12, le);
    if (ifd1Offset == 0 || (size_t)ifd1Offset + 2 > len - tiffOffset) return std::nullopt;

    // 3. 扫描 IFD1 entries，找 0x0201/0x0202
    const uint8_t* ifd1 = tiff + ifd1Offset;
    uint16_t ifd1Count = ExifReadU16(ifd1, le);
    if ((size_t)ifd1Offset + 2 + (size_t)ifd1Count * 12 > len - tiffOffset) return std::nullopt;

    uint32_t jpegOffset = 0, jpegLength = 0;
    bool hasOffset = false, hasLength = false;
    for (int i = 0; i < ifd1Count; i++) {
        const uint8_t* entry = ifd1 + 2 + i * 12;
        uint16_t tag = ExifReadU16(entry, le);
        if (tag == 0x0201) {  // JPEGInterchangeFormat
            jpegOffset = ExifReadU32(entry + 8, le);
            hasOffset = true;
        } else if (tag == 0x0202) {  // JPEGInterchangeFormatLength
            jpegLength = ExifReadU32(entry + 8, le);
            hasLength = true;
        }
    }
    if (!hasOffset || !hasLength || jpegLength < 4) return std::nullopt;
    if ((size_t)jpegOffset + jpegLength > len - tiffOffset) return std::nullopt;

    // 4. 定位缩略图 JPEG 数据（offset 相对 TIFF header 起始）
    const uint8_t* thumbJpeg = tiff + jpegOffset;
    if (thumbJpeg[0] != 0xFF || thumbJpeg[1] != 0xD8) return std::nullopt;

    // 5. turbojpeg 解码
    void* h = tjI(); if (!h) return std::nullopt;
    int w = 0, hh = 0, s = 0;
    auto* jpegData = const_cast<uint8_t*>(thumbJpeg);
    if (tjH(h, jpegData, (unsigned long)jpegLength, &w, &hh, &s) != 0 || w <= 0 || hh <= 0) {
        tjK(h); return std::nullopt;
    }

    DecodeResult result;
    result.width = w; result.height = hh; result.stride = w * 4;
    result.pixels.resize((size_t)w * hh * 4);
    if (tjD(h, jpegData, (unsigned long)jpegLength,
            result.pixels.data(), w, w * 4, hh, TJPF_BGRA, TJFLAG_FASTUPSAMPLE) != 0) {
        tjK(h); return std::nullopt;
    }
    tjK(h);

    // JPEG 无 alpha，全部设为 255
    for (size_t i = 3; i < result.pixels.size(); i += 4) result.pixels[i] = 255;
    return result;
}




