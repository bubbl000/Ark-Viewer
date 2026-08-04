#include "RawDecoder.h"
#include <cstring>
#include <algorithm>
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include "Logger.h"
#ifndef ARK_THUMB_DLL
#include "../ActivityLog.h"  // 步骤计时日志
#include "NvjpegHardDecoder.h"  // RAW 内嵌 JPEG 缩略图硬解（N 卡）
#endif
#include "../FileMapping.h"
#include "../Tiling.h"

using Microsoft::WRL::ComPtr;

// ─── libraw C API 函数指针类型 ───
typedef void* (*fn_libraw_init)(unsigned int flags);
typedef void (*fn_libraw_close)(void* lr);
typedef void (*fn_libraw_recycle)(void* lr);
typedef int (*fn_libraw_open_buffer)(void* lr, const void* buffer, size_t size);
typedef int (*fn_libraw_unpack)(void* lr);
typedef int (*fn_libraw_unpack_thumb)(void* lr);  // 提取内嵌缩略图（不解 demosaic）
typedef int (*fn_libraw_get_iwidth)(void* lr);
typedef int (*fn_libraw_get_iheight)(void* lr);
typedef void (*fn_libraw_set_demosaic)(void* lr, int qual);
typedef void (*fn_libraw_set_output_color)(void* lr, int color);
typedef void (*fn_libraw_set_output_bps)(void* lr, int bps);
typedef void (*fn_libraw_set_no_auto_bright)(void* lr, int val);
typedef void (*fn_libraw_set_adjust_maximum_thr)(void* lr, float val);
typedef void (*fn_libraw_set_highlight)(void* lr, int val);
typedef void (*fn_libraw_set_fbdd_noiserd)(void* lr, int val);
typedef int (*fn_libraw_dcraw_process)(void* lr);
typedef void* (*fn_libraw_make_mem_image)(void* lr, int* errc);
typedef void* (*fn_libraw_make_mem_thumb)(void* lr, int* errc);  // 缩略图内存对象
typedef void (*fn_libraw_clear_mem)(void* img);
typedef const char* (*fn_libraw_strerror)(int err);
// C++ 成员函数：LibRaw::output_params_ptr()，返回 libraw_output_params_t*
// x64 成员函数调用约定：RCX=this，返回值 RAX，与普通函数指针兼容
typedef void* (*fn_libraw_output_params_ptr)(void* lr);

// ─── 全局函数指针（延迟加载） ───
static HMODULE g_librawDll = nullptr;
static fn_libraw_init              g_init = nullptr;
static fn_libraw_close             g_close = nullptr;
static fn_libraw_recycle           g_recycle = nullptr;
static fn_libraw_open_buffer       g_open = nullptr;
static fn_libraw_unpack            g_unpack = nullptr;
static fn_libraw_unpack_thumb      g_unpack_thumb = nullptr;
static fn_libraw_get_iwidth        g_get_iw = nullptr;
static fn_libraw_get_iheight       g_get_ih = nullptr;
static fn_libraw_set_demosaic      g_set_demosaic = nullptr;
static fn_libraw_set_output_color  g_set_color = nullptr;
static fn_libraw_set_output_bps    g_set_bps = nullptr;
static fn_libraw_set_no_auto_bright g_set_no_ab = nullptr;
static fn_libraw_set_adjust_maximum_thr g_set_adj_max = nullptr;
static fn_libraw_set_highlight     g_set_highlight = nullptr;
static fn_libraw_set_fbdd_noiserd  g_set_fbdd = nullptr;
static fn_libraw_dcraw_process     g_process = nullptr;
static fn_libraw_make_mem_image    g_make_mem = nullptr;
static fn_libraw_make_mem_thumb    g_make_mem_thumb = nullptr;
static fn_libraw_clear_mem         g_clear_mem = nullptr;
static fn_libraw_output_params_ptr g_output_params_ptr = nullptr;  // C++ 修饰名，设置 half_size 用

// 加载 libraw.dll
static bool LoadLibRaw() {
    if (g_librawDll) return true;
    g_librawDll = LoadLibraryW(L"libraw.dll");
    if (!g_librawDll) return false;

    g_init         = (fn_libraw_init)              GetProcAddress(g_librawDll, "libraw_init");
    g_close        = (fn_libraw_close)             GetProcAddress(g_librawDll, "libraw_close");
    g_recycle      = (fn_libraw_recycle)           GetProcAddress(g_librawDll, "libraw_recycle");
    g_open         = (fn_libraw_open_buffer)       GetProcAddress(g_librawDll, "libraw_open_buffer");
    g_unpack       = (fn_libraw_unpack)            GetProcAddress(g_librawDll, "libraw_unpack");
    g_unpack_thumb = (fn_libraw_unpack_thumb)      GetProcAddress(g_librawDll, "libraw_unpack_thumb");
    g_get_iw       = (fn_libraw_get_iwidth)        GetProcAddress(g_librawDll, "libraw_get_iwidth");
    g_get_ih       = (fn_libraw_get_iheight)       GetProcAddress(g_librawDll, "libraw_get_iheight");
    g_set_demosaic = (fn_libraw_set_demosaic)      GetProcAddress(g_librawDll, "libraw_set_demosaic");
    g_set_color    = (fn_libraw_set_output_color)  GetProcAddress(g_librawDll, "libraw_set_output_color");
    g_set_bps      = (fn_libraw_set_output_bps)    GetProcAddress(g_librawDll, "libraw_set_output_bps");
    g_set_no_ab    = (fn_libraw_set_no_auto_bright)GetProcAddress(g_librawDll, "libraw_set_no_auto_bright");
    g_set_adj_max  = (fn_libraw_set_adjust_maximum_thr)GetProcAddress(g_librawDll, "libraw_set_adjust_maximum_thr");
    g_set_highlight= (fn_libraw_set_highlight)     GetProcAddress(g_librawDll, "libraw_set_highlight");
    g_set_fbdd     = (fn_libraw_set_fbdd_noiserd)  GetProcAddress(g_librawDll, "libraw_set_fbdd_noiserd");
    g_process      = (fn_libraw_dcraw_process)     GetProcAddress(g_librawDll, "libraw_dcraw_process");
    g_make_mem     = (fn_libraw_make_mem_image)    GetProcAddress(g_librawDll, "libraw_dcraw_make_mem_image");
    g_make_mem_thumb = (fn_libraw_make_mem_thumb)  GetProcAddress(g_librawDll, "libraw_dcraw_make_mem_thumb");
    g_clear_mem    = (fn_libraw_clear_mem)         GetProcAddress(g_librawDll, "libraw_dcraw_clear_mem");
    // C++ 成员函数 output_params_ptr：返回 libraw_output_params_t* 用于设置 half_size
    // 修饰名 ?output_params_ptr@LibRaw@@QEAAPEAUlibraw_output_params_t@@XZ（x64 thiscall=fastcall）
    g_output_params_ptr = (fn_libraw_output_params_ptr)GetProcAddress(
        g_librawDll, "?output_params_ptr@LibRaw@@QEAAPEAUlibraw_output_params_t@@XZ");

    // 缩略图相关符号缺失不阻塞主解码路径（DecodeFull 仍可用）
    return g_init && g_close && g_open && g_unpack && g_process && g_make_mem && g_clear_mem;
}

// ─── TIFF-based RAW 检测 ───
// 扫描 TIFF IFD0 条目，查找 RAW 专有标签
static bool LooksLikeTiffBasedRaw(const uint8_t* data, size_t len) {
    if (len < 8) return false;
    // TIFF 头：II*\0 (小端) 或 MM\0* (大端)
    bool littleEndian;
    if (data[0] == 'I' && data[1] == 'I' && data[2] == 0x2A && data[3] == 0x00)
        littleEndian = true;
    else if (data[0] == 'M' && data[1] == 'M' && data[2] == 0x00 && data[3] == 0x2A)
        littleEndian = false;
    else
        return false;

    // 读取 4 字节 IFD0 偏移
    auto read32 = [&](size_t offset) -> uint32_t {
        if (offset + 4 > len) return 0;
        if (littleEndian)
            return data[offset] | (data[offset + 1] << 8) |
                   (data[offset + 2] << 16) | (data[offset + 3] << 24);
        return (data[offset] << 24) | (data[offset + 1] << 16) |
               (data[offset + 2] << 8) | data[offset + 3];
    };
    auto read16 = [&](size_t offset) -> uint16_t {
        if (offset + 2 > len) return 0;
        return littleEndian
            ? (uint16_t)(data[offset] | (data[offset + 1] << 8))
            : (uint16_t)((data[offset] << 8) | data[offset + 1]);
    };

    uint32_t ifdOffset = read32(4);
    if (ifdOffset == 0 || ifdOffset + 2 > len) return false;

    uint16_t entryCount = read16(ifdOffset);
    // 每条目 12 字节：tag(2) + type(2) + count(4) + value/offset(4)
    size_t ifdEnd = ifdOffset + 2 + (size_t)entryCount * 12;
    if (ifdEnd > len) ifdEnd = len;

    // RAW 专有标签
    const uint16_t RAW_TAGS[] = {
        33422,  // CFA Pattern (Bayer)
        50708,  // UniqueCameraModel (DNG)
        50710,  // CFAPattern2 (DNG)
        50735,  // AsShotNeutral (DNG)
        50806,  // LinearizationTable (DNG)
    };

    // Make 标签（0x010F）值：索尼/佳能等 ARW/CR2 的 CFA Pattern 在 SubIFD 而非 IFD0，
    // 仅靠 RAW_TAGS 会漏检；通过 Make 厂商名兜底识别 TIFF-based RAW
    std::string makeStr;
    for (size_t i = ifdOffset + 2; i + 2 < ifdEnd; i += 12) {
        uint16_t tag = read16(i);
        for (uint16_t rawTag : RAW_TAGS) {
            if (tag == rawTag) return true;
        }
        // 读取 Make 标签的 ASCII 值（type=2，count>4 时值在 offset 指向的数据区）
        if (tag == 0x010F && makeStr.empty() && i + 12 <= len) {
            uint16_t type = read16(i + 2);
            uint32_t count = read32(i + 4);
            if (type == 2 && count > 0 && count < 64) {
                const uint8_t* valPtr;
                if (count <= 4) {
                    valPtr = data + i + 8;  // 内联值
                } else {
                    uint32_t off = read32(i + 8);
                    if ((size_t)off + count > len) continue;
                    valPtr = data + off;
                }
                makeStr.assign((const char*)valPtr, count - 1);  // 去掉 \0
            }
        }
    }

    // Make 匹配主流相机厂商 → 判定为 TIFF-based RAW
    if (!makeStr.empty()) {
        for (auto& c : makeStr) {  // 转大写（ASCII 范围）
            if (c >= 'a' && c <= 'z') c -= 32;
        }
        const char* brands[] = {"SONY","CANON","NIKON","FUJI","PENTAX","OLYMPUS",
                                "PANASONIC","LEICA","RICOH","SAMSUNG","PHASE ONE"};
        for (auto* b : brands) {
            if (makeStr.find(b) != std::string::npos) return true;
        }
    }
    return false;
}

// ─── Open：魔数检测 ───
std::optional<ImageDecoder::OpenResult> RawDecoder::Open(const uint8_t* data, size_t len) {
    OpenResult r;
    r.info.decoderName = "libraw";

    // CR3: ISOBMFF ftyp + "cr3 " / "crx " 品牌（佳能 R5/R6 等新机型为 crx，旧机型为 cr3）
    if (len >= 12 && data[4] == 'f' && data[5] == 't' && data[6] == 'y' && data[7] == 'p'
        && data[8] == 'c' && data[9] == 'r'
        && ((data[10] == '3' && data[11] == ' ') || (data[10] == 'x' && data[11] == ' '))) {
        r.info.format = "CR3";
    }
    // RAF: "FUJIFILM" at offset 0
    else if (len >= 8 && std::memcmp(data, "FUJIFILM", 8) == 0) {
        r.info.format = "RAF";
    }
    // X3F: "FOVb" at offset 0
    else if (len >= 4 && data[0] == 'F' && data[1] == 'O' && data[2] == 'V' && data[3] == 'b') {
        r.info.format = "X3F";
    }
    // TIFF-based RAW（CR2/NEF/ARW/DNG/PEF/RW2/ORF）
    else if (LooksLikeTiffBasedRaw(data, len)) {
        r.info.format = "RAW";
    }
    else {
        return std::nullopt;
    }

    // 获取尺寸：需完整文件，FindDecoder 阶段（≤4096 字节）跳过避免加载 DLL
    if (len > 4096 && LoadLibRaw()) {
        void* lr = g_init(0);
        if (lr) {
            if (g_open(lr, data, len) == 0) {
                r.info.width  = g_get_iw(lr);
                r.info.height = g_get_ih(lr);
            }
            g_close(lr);
        }
    }
    return r;
}

// ─── DecodeFull：完整解码 ───
std::optional<DecodeResult> RawDecoder::DecodeFull(const OpenResult& open) {
    if (!LoadLibRaw()) {
        LOG_WARN("RawDecoder", "无法加载 libraw.dll");
        return std::nullopt;
    }

    auto fileMap = std::static_pointer_cast<FileMapping>(open.state);
    if (!fileMap || !fileMap->Data()) return std::nullopt;

    // 1. init + open_buffer + unpack
    void* lr = g_init(0);
    if (!lr) { LOG_WARN("RawDecoder", "libraw_init 失败"); return std::nullopt; }

    int err = g_open(lr, fileMap->Data(), fileMap->Size());
    if (err != 0) {
        LOG_WARN_STREAM("RawDecoder") << "open_buffer 失败: " << err;
        g_close(lr);
        return std::nullopt;
    }

    // 步骤计时：定位 2 秒瓶颈在 unpack（解压）还是 process（demosaic）
    ULONGLONG t0 = GetTickCount64();
    err = g_unpack(lr);
    double unpackMs = (double)(GetTickCount64() - t0);
    if (err != 0) {
        LOG_WARN_STREAM("RawDecoder") << "unpack 失败: " << err;
        g_close(lr);
        return std::nullopt;
    }

    // 2. 设置处理参数（与 C# 版一致）
    if (g_set_demosaic) {
        g_set_demosaic(lr, 3);       // AHD：本机 libraw 实测 VNG 反慢 2.8 倍（5.3s vs 1.9s），回退 AHD
    } else {
        LOG_WARN("RawDecoder", "libraw_set_demosaic 不可用，demosaic 未生效");
    }
    g_set_color(lr, 1);          // sRGB
    g_set_bps(lr, 8);            // 8-bit
    g_set_no_ab(lr, 0);          // 自动亮度
    g_set_adj_max(lr, 0.0f);     // 自动白点
    g_set_highlight(lr, 0);      // 裁切高光
    g_set_fbdd(lr, 0);           // 无降噪

    // 3. dcraw_process + make_mem_image
    ULONGLONG t1 = GetTickCount64();
    err = g_process(lr);
    double processMs = (double)(GetTickCount64() - t1);
#ifndef ARK_THUMB_DLL
    ActivityLog::Instance().Log(L"RAW",
        L"步骤耗时: unpack=" + std::to_wstring((int)unpackMs) +
        L"ms process=" + std::to_wstring((int)processMs) + L"ms (AHD)");
#endif
    if (err != 0) {
        LOG_WARN_STREAM("RawDecoder") << "dcraw_process 失败: " << err;
        g_close(lr);
        return std::nullopt;
    }

    int errc = 0;
    void* imgPtr = g_make_mem(lr, &errc);
    if (!imgPtr || errc != 0) {
        LOG_WARN_STREAM("RawDecoder") << "make_mem_image 失败: " << errc;
        g_close(lr);
        return std::nullopt;
    }

    // 4. 解析 libraw_processed_image_t
    // struct: { int type; ushort h,w,colors,bits; uint data_size; uchar data[]; }
    const uint8_t* p = (const uint8_t*)imgPtr;
    int type = *(const int*)(p + 0);
    uint16_t height = *(const uint16_t*)(p + 4);
    uint16_t width = *(const uint16_t*)(p + 6);
    uint16_t colors = *(const uint16_t*)(p + 8);
    uint16_t bits = *(const uint16_t*)(p + 10);
    uint32_t dataSize = *(const uint32_t*)(p + 12);
    const uint8_t* src = p + 16;  // data 起始偏移

    DecodeResult result;

    // type=2 (bitmap) 且 colors=3, bits=8 → RGB → BGRA8
    if (type == 2 && colors == 3 && bits == 8 && width > 0 && height > 0) {
        size_t pixelCount = (size_t)width * height;
        size_t expected = pixelCount * 3;
        if (dataSize < expected) expected = dataSize;

        result.width = width;
        result.height = height;
        result.stride = width * 4;
        result.pixels.resize(pixelCount * 4);

        for (size_t i = 0; i < pixelCount; i++) {
            result.pixels[i * 4 + 0] = src[i * 3 + 2];  // B
            result.pixels[i * 4 + 1] = src[i * 3 + 1];  // G
            result.pixels[i * 4 + 2] = src[i * 3 + 0];  // R
            result.pixels[i * 4 + 3] = 0xFF;             // A
        }
    } else {
        LOG_WARN_STREAM("RawDecoder") << "不支持的输出格式: type=" << type
                                       << " colors=" << colors << " bits=" << bits;
        g_clear_mem(imgPtr);
        g_close(lr);
        return std::nullopt;
    }

    g_clear_mem(imgPtr);
    g_recycle(lr);
    g_close(lr);

    LOG_INFO_STREAM("RawDecoder") << "已解码: " << width << "x" << height;
    return result;
}

// DecodeLevel：按 level 选择解码路径
// - level 0：全尺寸 demosaic（用户放大到 100% 才触发，可接受慢）
// - level≥1 且目标 ≤ 内嵌缩略图：复用缩略图降采样（~22ms，跳过 4000 万像素 demosaic）
//   索尼 ARW 缩略图 1616×1080，覆盖 level≥3（996×665 及以下）
// - level 1-2（目标 > 缩略图）：half_size=1 半尺寸 demosaic 后降采样（~0.5s，比全尺寸快 4 倍）
//   half_size 不可用时回退全尺寸 demosaic+ScaleDown
// _cachedThumb/_cachedHalf/_cachedFull 三者独立，避免缓存互相污染
std::optional<DecodeResult> RawDecoder::DecodeLevel(const OpenResult& open, int level) {
    std::lock_guard lock(_cacheMutex);

    // level 0：全尺寸
    if (level == 0) {
        if (!_cachedFull) _cachedFull = DecodeFull(open);
        return _cachedFull ? std::optional<DecodeResult>(*_cachedFull) : std::nullopt;
    }

    int targetW = (std::max)(1, open.info.width  >> level);
    int targetH = (std::max)(1, open.info.height >> level);

    // 预览层快速路径：目标 ≤ 内嵌缩略图 → 缩略图降采样，跳过全尺寸 demosaic
    if (!_cachedThumb) _cachedThumb = DecodeThumbnail(open);
    if (_cachedThumb &&
        targetW <= _cachedThumb->width && targetH <= _cachedThumb->height) {
        return _cachedThumb->ScaleDown(targetW, targetH);
    }

    // level 1-2（目标 > 缩略图）：half_size 半尺寸 demosaic 后降采样
    // half_size=1 输出原图/2，demosaic 像素数降 4 倍（1.9s→~0.5s）
    if (!_cachedHalf) _cachedHalf = DecodeHalfSize(open);
    if (_cachedHalf) {
        return _cachedHalf->ScaleDown(targetW, targetH);
    }

    // half_size 不可用（无 output_params_ptr 或尺寸验证失败）：回退全尺寸 demosaic
    if (!_cachedFull) _cachedFull = DecodeFull(open);
    if (!_cachedFull) return std::nullopt;
    return _cachedFull->ScaleDown(targetW, targetH);
}

// ─── DecodeHalfSize：half_size=1 半尺寸解码 ───
// libraw 的 half_size 在 unpack 阶段生效（只分配 1/4 内存，demosaic 时 2x2 像素合并）
// 必须在 unpack 前设置 imgdata.params.half_size=1
// libraw.dll 无 set_half_size 的 C API，通过 C++ 成员函数 output_params_ptr 获取 params 指针
// half_size 字段偏移 128（ctypes 暴力扫描 DLL 实测确认；0.22.0 头文件 offsetof 算 136 不适用此 DLL，布局差 8 字节）
// 输出尺寸验证：应为原图/2，不符则回退（偏移错误时安全兜底）
std::optional<DecodeResult> RawDecoder::DecodeHalfSize(const OpenResult& open) {
    if (!g_output_params_ptr) return std::nullopt;  // 无 output_params_ptr，调用方回退全尺寸

    auto fileMap = std::static_pointer_cast<FileMapping>(open.state);
    if (!fileMap || !fileMap->Data()) return std::nullopt;

    void* lr = g_init(0);
    if (!lr) return std::nullopt;

    if (g_open(lr, fileMap->Data(), fileMap->Size()) != 0) {
        g_close(lr);
        return std::nullopt;
    }

    // half_size 必须在 unpack 前设置：output_params_ptr 返回 libraw_output_params_t*
    void* params = g_output_params_ptr(lr);
    if (!params) { g_close(lr); return std::nullopt; }
    *(int*)((char*)params + 128) = 1;  // half_size = 1

    // demosaic 等参数与 DecodeFull 一致
    if (g_set_demosaic) g_set_demosaic(lr, 3);  // AHD
    g_set_color(lr, 1);          // sRGB
    g_set_bps(lr, 8);            // 8-bit
    g_set_no_ab(lr, 0);          // 自动亮度
    g_set_adj_max(lr, 0.0f);     // 自动白点
    g_set_highlight(lr, 0);      // 裁切高光
    g_set_fbdd(lr, 0);           // 无降噪

    if (g_unpack(lr) != 0) {
        LOG_WARN("RawDecoder", "half_size unpack 失败");
        g_close(lr);
        return std::nullopt;
    }

    ULONGLONG t0 = GetTickCount64();
    int err = g_process(lr);
    double processMs = (double)(GetTickCount64() - t0);
#ifndef ARK_THUMB_DLL
    ActivityLog::Instance().Log(L"RAW",
        L"half_size process=" + std::to_wstring((int)processMs) + L"ms (AHD 1/2)");
#endif
    if (err != 0) {
        LOG_WARN_STREAM("RawDecoder") << "half_size process 失败: " << err;
        g_close(lr);
        return std::nullopt;
    }

    int errc = 0;
    void* imgPtr = g_make_mem(lr, &errc);
    if (!imgPtr || errc != 0) {
        LOG_WARN_STREAM("RawDecoder") << "half_size make_mem 失败: " << errc;
        g_close(lr);
        return std::nullopt;
    }

    // 解析 libraw_processed_image_t
    const uint8_t* p = (const uint8_t*)imgPtr;
    int type = *(const int*)(p + 0);
    uint16_t height = *(const uint16_t*)(p + 4);
    uint16_t width  = *(const uint16_t*)(p + 6);
    uint16_t colors = *(const uint16_t*)(p + 8);
    uint16_t bits   = *(const uint16_t*)(p + 10);
    uint32_t dataSize = *(const uint32_t*)(p + 12);
    const uint8_t* src = p + 16;

    // 尺寸验证：half_size=1 输出应 ≈ 原图/2
    // 偏移错误时输出尺寸不会减半，返回 nullopt 让调用方回退全尺寸
    int expectedW = open.info.width  / 2;
    int expectedH = open.info.height / 2;
    if (width < expectedW * 2 / 3 || width > expectedW * 3 / 2 ||
        height < expectedH * 2 / 3 || height > expectedH * 3 / 2) {
        LOG_WARN_STREAM("RawDecoder") << "half_size 尺寸不符: " << width << "x" << height
                                       << " 预期~" << expectedW << "x" << expectedH
                                       << "，偏移可能错误，回退全尺寸";
        g_clear_mem(imgPtr);
        g_close(lr);
        return std::nullopt;
    }

    DecodeResult result;
    if (type == 2 && colors == 3 && bits == 8 && width > 0 && height > 0) {
        size_t pixelCount = (size_t)width * height;
        size_t expected = pixelCount * 3;
        if (dataSize < expected) expected = dataSize;

        result.width = width;
        result.height = height;
        result.stride = width * 4;
        result.pixels.resize(pixelCount * 4);

        for (size_t i = 0; i < pixelCount; i++) {
            result.pixels[i * 4 + 0] = src[i * 3 + 2];  // B
            result.pixels[i * 4 + 1] = src[i * 3 + 1];  // G
            result.pixels[i * 4 + 2] = src[i * 3 + 0];  // R
            result.pixels[i * 4 + 3] = 0xFF;             // A
        }
    } else {
        LOG_WARN_STREAM("RawDecoder") << "half_size 不支持的输出格式: type=" << type
                                       << " colors=" << colors << " bits=" << bits;
        g_clear_mem(imgPtr);
        g_close(lr);
        return std::nullopt;
    }

    g_clear_mem(imgPtr);
    g_recycle(lr);
    g_close(lr);

    LOG_INFO_STREAM("RawDecoder") << "half_size 已解码: " << width << "x" << height;
    return result;
}

// DecodeTile：仅 level 0 支持从缓存全尺寸裁剪瓦片（大层路径）
std::optional<DecodeResult> RawDecoder::DecodeTile(const OpenResult& open,
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

// ─── WIC 工厂单例：用于回退解码 JPEG 缩略图（无 N 卡或硬解失败时） ───
static IWICImagingFactory* WIC() {
    static ComPtr<IWICImagingFactory> f;
    if (!f) CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    return f.Get();
}

// 用 WIC 从内存 JPEG 数据解码为 BGRA8（CPU 路径，作为硬解回退）
static std::optional<DecodeResult> DecodeJpegWithWic(const uint8_t* jpeg, size_t len) {
    auto* wic = WIC();
    if (!wic || !jpeg || len == 0) return std::nullopt;

    ComPtr<IWICStream> stream;
    if (FAILED(wic->CreateStream(&stream))) return std::nullopt;
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

// ─── DecodeThumbnail：提取相机内嵌缩略图（避开 demosaic） ───
// libraw 缩略图类型：type=1 JPEG（多数相机），type=0 PPM bitmap（老相机）
// JPEG → N 卡 nvJPEG 硬解，失败回退 WIC；PPM → 直接 RGB→BGRA8 转换
// 全程不调 dcraw_process，预览速度从 demosaic 的几百 ms 降到几十 ms
std::optional<DecodeResult> RawDecoder::DecodeThumbnail(const OpenResult& open) {
    if (!LoadLibRaw() || !g_unpack_thumb || !g_make_mem_thumb) return std::nullopt;

    auto fileMap = std::static_pointer_cast<FileMapping>(open.state);
    if (!fileMap || !fileMap->Data()) return std::nullopt;

    void* lr = g_init(0);
    if (!lr) return std::nullopt;

    if (g_open(lr, fileMap->Data(), fileMap->Size()) != 0) {
        g_close(lr);
        return std::nullopt;
    }

    // 提取内嵌缩略图（不解 raw 数据，不跑 demosaic）
    if (g_unpack_thumb(lr) != 0) {
        g_close(lr);
        return std::nullopt;  // 无缩略图，由 ImageEngine 回退到 DecodeLevel
    }

    int errc = 0;
    void* thumbPtr = g_make_mem_thumb(lr, &errc);
    if (!thumbPtr || errc != 0) {
        g_close(lr);
        return std::nullopt;
    }

    // 解析 libraw_processed_image_t: { int type; ushort h,w,colors,bits; uint data_size; uchar data[]; }
    const uint8_t* p = (const uint8_t*)thumbPtr;
    int type       = *(const int*)(p + 0);
    uint16_t height = *(const uint16_t*)(p + 4);
    uint16_t width  = *(const uint16_t*)(p + 6);
    uint16_t colors = *(const uint16_t*)(p + 8);
    uint16_t bits   = *(const uint16_t*)(p + 10);
    uint32_t dataSize = *(const uint32_t*)(p + 12);
    const uint8_t* data = p + 16;

    std::optional<DecodeResult> result;

    if (type == 1 && dataSize > 2 && data[0] == 0xFF && data[1] == 0xD8) {
        // JPEG 缩略图：硬解优先，失败回退 WIC（无 N 卡用户也能快速预览）
#ifndef ARK_THUMB_DLL
        if (NvjpegHardDecoder::Available()) {
            NvjpegHardDecoder hw;
            result = hw.DecodeFull(data, dataSize);
            if (!result) LOG_WARN("RawDecoder", "缩略图硬解失败，回退 WIC");
        }
#endif
        if (!result) result = DecodeJpegWithWic(data, dataSize);
    } else if (type == 0 && colors == 3 && bits == 8 && width > 0 && height > 0) {
        // PPM bitmap（RGB 顺序）→ BGRA8
        DecodeResult r;
        r.width = width;
        r.height = height;
        r.stride = width * 4;
        r.pixels.resize((size_t)width * height * 4);
        size_t pixelCount = (size_t)width * height;
        size_t avail = (dataSize < pixelCount * 3) ? dataSize : pixelCount * 3;
        for (size_t i = 0; i < pixelCount && i * 3 + 2 < avail; i++) {
            r.pixels[i * 4 + 0] = data[i * 3 + 2];  // B
            r.pixels[i * 4 + 1] = data[i * 3 + 1];  // G
            r.pixels[i * 4 + 2] = data[i * 3 + 0];  // R
            r.pixels[i * 4 + 3] = 0xFF;              // A
        }
        result = r;
    }

    g_clear_mem(thumbPtr);
    g_close(lr);

    if (result) {
        // 诊断：统计缩略图输出亮度（全黑检测——nvJPEG 可能输出黑但误判成功）
        uint64_t sum = 0; size_t npx = 0;
        const auto& px = result->pixels;
        for (size_t i = 0; i + 2 < px.size(); i += 4) { sum += px[i]; npx++; }
        int avgB = npx ? (int)(sum / npx) : 0;
#ifndef ARK_THUMB_DLL
        ActivityLog::Instance().Log(L"缩略图",
            L"提取 " + std::to_wstring(result->width) + L"x" +
            std::to_wstring(result->height) + L" type=" + std::to_wstring(type) +
            L" 平均B=" + std::to_wstring(avgB));
#endif
        LOG_INFO_STREAM("RawDecoder") << "缩略图已提取: " << result->width << "x"
                                       << result->height << " type=" << type;
    }
    return result;
}
