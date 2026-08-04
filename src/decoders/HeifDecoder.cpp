#include "HeifDecoder.h"
#include <cstring>
#include <algorithm>
#include <Windows.h>
#include "Logger.h"
#include "../FileMapping.h"
#include "../Tiling.h"

// ─── libheif C API 常量 ───
// heif_colorspace_RGB = 1
// heif_chroma_interleaved_RGBA = 19
// heif_channel_interleaved = 0
static const int HEIF_COLORSPACE_RGB = 1;
static const int HEIF_CHROMA_RGBA = 19;
static const int HEIF_CHANNEL_INTERLEAVED = 0;

// heif_error 结构（x64 ABI 16 字节）：{ int code; int subcode; const char* message; }
struct HeifError {
    int code;
    int subcode;
    const char* message;
};

// ─── heif.dll 函数指针类型 ───
typedef void* (*fn_heif_context_alloc)();
typedef void (*fn_heif_context_free)(void* ctx);
typedef HeifError (*fn_heif_read_mem)(void* ctx, const void* mem, size_t size, const void* options);
typedef HeifError (*fn_heif_get_primary_handle)(void* ctx, void** handle);
typedef void (*fn_heif_handle_release)(void* handle);
typedef int (*fn_heif_handle_get_width)(void* handle);
typedef int (*fn_heif_handle_get_height)(void* handle);
typedef int (*fn_heif_handle_has_alpha)(void* handle);
typedef void* (*fn_heif_decoding_options_alloc)();
typedef void (*fn_heif_decoding_options_free)(void* options);
typedef HeifError (*fn_heif_decode_image)(void* handle, void** out_img,
    int colorspace, int chroma, const void* options);
typedef void (*fn_heif_image_release)(void* img);
typedef const uint8_t* (*fn_heif_image_get_plane)(void* img, int channel, int* out_stride);
typedef int (*fn_heif_image_get_width)(void* img, int channel);
typedef int (*fn_heif_image_get_height)(void* img, int channel);
// 缩略图提取（libheif 1.4+）
typedef int (*fn_heif_handle_get_thumbnail_count)(void* handle);
typedef HeifError (*fn_heif_handle_get_nth_thumbnail)(void* handle, int n, void** out);

// ─── 全局函数指针（延迟加载） ───
static HMODULE g_heifDll = nullptr;
static fn_heif_context_alloc        g_ctx_alloc = nullptr;
static fn_heif_context_free         g_ctx_free = nullptr;
static fn_heif_read_mem             g_read_mem = nullptr;
static fn_heif_get_primary_handle   g_get_handle = nullptr;
static fn_heif_handle_release       g_handle_release = nullptr;
static fn_heif_handle_get_width     g_handle_get_w = nullptr;
static fn_heif_handle_get_height    g_handle_get_h = nullptr;
static fn_heif_handle_has_alpha     g_handle_has_alpha = nullptr;
static fn_heif_decoding_options_alloc g_opts_alloc = nullptr;
static fn_heif_decoding_options_free  g_opts_free = nullptr;
static fn_heif_decode_image         g_decode = nullptr;
static fn_heif_image_release        g_img_release = nullptr;
static fn_heif_image_get_plane      g_get_plane = nullptr;
static fn_heif_image_get_width      g_img_get_w = nullptr;
static fn_heif_image_get_height     g_img_get_h = nullptr;
static fn_heif_handle_get_thumbnail_count g_thumb_count = nullptr;
static fn_heif_handle_get_nth_thumbnail   g_thumb_nth = nullptr;

// 加载 heif.dll，返回是否成功
static bool LoadHeif() {
    if (g_heifDll) return true;
    g_heifDll = LoadLibraryW(L"heif.dll");
    if (!g_heifDll) return false;

    g_ctx_alloc      = (fn_heif_context_alloc)      GetProcAddress(g_heifDll, "heif_context_alloc");
    g_ctx_free       = (fn_heif_context_free)       GetProcAddress(g_heifDll, "heif_context_free");
    g_read_mem       = (fn_heif_read_mem)           GetProcAddress(g_heifDll, "heif_context_read_from_memory_without_copy");
    g_get_handle     = (fn_heif_get_primary_handle) GetProcAddress(g_heifDll, "heif_context_get_primary_image_handle");
    g_handle_release = (fn_heif_handle_release)     GetProcAddress(g_heifDll, "heif_image_handle_release");
    g_handle_get_w   = (fn_heif_handle_get_width)   GetProcAddress(g_heifDll, "heif_image_handle_get_width");
    g_handle_get_h   = (fn_heif_handle_get_height)  GetProcAddress(g_heifDll, "heif_image_handle_get_height");
    g_handle_has_alpha = (fn_heif_handle_has_alpha) GetProcAddress(g_heifDll, "heif_image_handle_has_alpha_channel");
    g_opts_alloc     = (fn_heif_decoding_options_alloc) GetProcAddress(g_heifDll, "heif_decoding_options_alloc");
    g_opts_free      = (fn_heif_decoding_options_free)  GetProcAddress(g_heifDll, "heif_decoding_options_free");
    g_decode         = (fn_heif_decode_image)       GetProcAddress(g_heifDll, "heif_decode_image");
    g_img_release    = (fn_heif_image_release)      GetProcAddress(g_heifDll, "heif_image_release");
    g_get_plane      = (fn_heif_image_get_plane)    GetProcAddress(g_heifDll, "heif_image_get_plane_readonly2");
    g_img_get_w      = (fn_heif_image_get_width)    GetProcAddress(g_heifDll, "heif_image_get_width");
    g_img_get_h      = (fn_heif_image_get_height)   GetProcAddress(g_heifDll, "heif_image_get_height");
    // 缩略图相关（libheif 1.4+），可能为空
    g_thumb_count    = (fn_heif_handle_get_thumbnail_count) GetProcAddress(g_heifDll, "heif_image_handle_get_number_of_thumbnails");
    g_thumb_nth      = (fn_heif_handle_get_nth_thumbnail)   GetProcAddress(g_heifDll, "heif_image_handle_get_nth_thumbnail");

    return g_ctx_alloc && g_ctx_free && g_read_mem && g_get_handle &&
           g_handle_release && g_handle_get_w && g_handle_get_h &&
           g_decode && g_img_release && g_get_plane;
}

// HEIF 品牌检测（heic/heix/mif1/msf1/heim/heis）
static bool IsHeifBrand(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3) {
    if (b0 == 'h' && b1 == 'e' && b2 == 'i' && (b3 == 'c' || b3 == 'x' || b3 == 'm' || b3 == 's'))
        return true;
    if (b0 == 'm' && b1 == 'i' && b2 == 'f' && b3 == '1') return true;
    if (b0 == 'm' && b1 == 's' && b2 == 'f' && b3 == '1') return true;
    return false;
}

// ─── Open：ISOBMFF ftyp box 魔数检测 ───
std::optional<ImageDecoder::OpenResult> HeifDecoder::Open(const uint8_t* data, size_t len) {
    // ftyp box: bytes[4..7]="ftyp", bytes[8..11]=brand
    if (len < 12) return std::nullopt;
    if (data[4] != 'f' || data[5] != 't' || data[6] != 'y' || data[7] != 'p')
        return std::nullopt;
    if (!IsHeifBrand(data[8], data[9], data[10], data[11]))
        return std::nullopt;

    OpenResult result;
    result.info.format = "HEIF";
    result.info.decoderName = "libheif";

    // 获取尺寸：需完整文件，FindDecoder 阶段（≤4096 字节）跳过避免加载 DLL
    if (len > 4096 && LoadHeif()) {
        void* ctx = g_ctx_alloc();
        if (ctx) {
            if (g_read_mem(ctx, data, len, nullptr).code == 0) {
                void* handle = nullptr;
                if (g_get_handle(ctx, &handle).code == 0 && handle) {
                    result.info.width  = g_handle_get_w(handle);
                    result.info.height = g_handle_get_h(handle);
                    g_handle_release(handle);
                }
            }
            g_ctx_free(ctx);
        }
    }
    return result;
}

// ─── DecodeFull：完整解码 ───
std::optional<DecodeResult> HeifDecoder::DecodeFull(const OpenResult& open) {
    if (!LoadHeif()) {
        LOG_WARN("HeifDecoder", "无法加载 heif.dll");
        return std::nullopt;
    }

    auto fileMap = std::static_pointer_cast<FileMapping>(open.state);
    if (!fileMap || !fileMap->Data()) return std::nullopt;

    // 1. 创建 context 并读入内存
    void* ctx = g_ctx_alloc();
    if (!ctx) { LOG_WARN("HeifDecoder", "heif_context_alloc 失败"); return std::nullopt; }

    HeifError err = g_read_mem(ctx, fileMap->Data(), fileMap->Size(), nullptr);
    if (err.code != 0) {
        LOG_WARN_STREAM("HeifDecoder") << "read_from_memory 失败: code=" << err.code;
        g_ctx_free(ctx);
        return std::nullopt;
    }

    // 2. 获取主图像句柄
    void* handle = nullptr;
    err = g_get_handle(ctx, &handle);
    if (err.code != 0 || !handle) {
        LOG_WARN_STREAM("HeifDecoder") << "get_primary_handle 失败: code=" << err.code;
        g_ctx_free(ctx);
        return std::nullopt;
    }

    // context 在获取 handle 后即可释放（handle 是引用计数的）
    g_ctx_free(ctx);

    // 3. 解码图像为 RGBA
    void* options = g_opts_alloc();
    void* image = nullptr;
    err = g_decode(handle, &image, HEIF_COLORSPACE_RGB, HEIF_CHROMA_RGBA, options);
    if (options) g_opts_free(options);
    g_handle_release(handle);

    if (err.code != 0 || !image) {
        LOG_WARN_STREAM("HeifDecoder") << "heif_decode_image 失败: code=" << err.code;
        return std::nullopt;
    }

    // 4. 获取像素平面
    int w = g_img_get_w(image, HEIF_CHANNEL_INTERLEAVED);
    int h = g_img_get_h(image, HEIF_CHANNEL_INTERLEAVED);
    if (w <= 0 || h <= 0) {
        LOG_WARN("HeifDecoder", "无效尺寸");
        g_img_release(image);
        return std::nullopt;
    }

    int stride = 0;
    const uint8_t* plane = g_get_plane(image, HEIF_CHANNEL_INTERLEAVED, &stride);
    if (!plane) {
        LOG_WARN("HeifDecoder", "get_plane 返回 NULL");
        g_img_release(image);
        return std::nullopt;
    }

    // 5. 拷贝并转换 RGBA → BGRA
    DecodeResult result;
    result.width = w;
    result.height = h;
    result.stride = w * 4;
    result.pixels.resize((size_t)w * h * 4);

    int rowBytes = w * 4;
    for (int y = 0; y < h; y++) {
        const uint8_t* srcRow = plane + (size_t)y * stride;
        uint8_t* dstRow = result.pixels.data() + (size_t)y * rowBytes;
        for (int x = 0; x < w; x++) {
            // src: R G B A → dst: B G R A
            dstRow[x * 4 + 0] = srcRow[x * 4 + 2];  // B
            dstRow[x * 4 + 1] = srcRow[x * 4 + 1];  // G
            dstRow[x * 4 + 2] = srcRow[x * 4 + 0];  // R
            dstRow[x * 4 + 3] = srcRow[x * 4 + 3];  // A
        }
    }

    g_img_release(image);

    LOG_INFO_STREAM("HeifDecoder") << "已解码: " << w << "x" << h;
    return result;
}

// 拷贝 libheif RGBA 平面 → BGRA DecodeResult（DecodeFull 与 DecodeThumbnail 共用）
static DecodeResult RgbaPlaneToBgra(void* image) {
    int w = g_img_get_w(image, HEIF_CHANNEL_INTERLEAVED);
    int h = g_img_get_h(image, HEIF_CHANNEL_INTERLEAVED);
    if (w <= 0 || h <= 0) { g_img_release(image); return {}; }

    int stride = 0;
    const uint8_t* plane = g_get_plane(image, HEIF_CHANNEL_INTERLEAVED, &stride);
    if (!plane) { g_img_release(image); return {}; }

    DecodeResult result;
    result.width = w; result.height = h; result.stride = w * 4;
    result.pixels.resize((size_t)w * h * 4);
    int rowBytes = w * 4;
    for (int y = 0; y < h; y++) {
        const uint8_t* srcRow = plane + (size_t)y * stride;
        uint8_t* dstRow = result.pixels.data() + (size_t)y * rowBytes;
        for (int x = 0; x < w; x++) {
            dstRow[x * 4 + 0] = srcRow[x * 4 + 2];  // B
            dstRow[x * 4 + 1] = srcRow[x * 4 + 1];  // G
            dstRow[x * 4 + 2] = srcRow[x * 4 + 0];  // R
            dstRow[x * 4 + 3] = srcRow[x * 4 + 3];  // A
        }
    }
    g_img_release(image);
    return result;
}

// ─── DecodeThumbnail：提取第 0 个内嵌缩略图 ───
// libheif 在主图像 handle 上挂载缩略图轨道，先取主 handle 再取缩略图 handle
std::optional<DecodeResult> HeifDecoder::DecodeThumbnail(const OpenResult& open) {
    if (!LoadHeif() || !g_thumb_count || !g_thumb_nth) return std::nullopt;

    auto fileMap = std::static_pointer_cast<FileMapping>(open.state);
    if (!fileMap || !fileMap->Data()) return std::nullopt;

    void* ctx = g_ctx_alloc();
    if (!ctx) return std::nullopt;
    if (g_read_mem(ctx, fileMap->Data(), fileMap->Size(), nullptr).code != 0) {
        g_ctx_free(ctx); return std::nullopt;
    }

    void* handle = nullptr;
    if (g_get_handle(ctx, &handle).code != 0 || !handle) {
        g_ctx_free(ctx); return std::nullopt;
    }
    g_ctx_free(ctx);  // handle 引用计数独立持有

    // 无缩略图直接返回（不 fallback 全量解码，避免主线程阻塞）
    int count = g_thumb_count(handle);
    if (count <= 0) { g_handle_release(handle); return std::nullopt; }

    void* thumbHandle = nullptr;
    HeifError err = g_thumb_nth(handle, 0, &thumbHandle);
    g_handle_release(handle);
    if (err.code != 0 || !thumbHandle) return std::nullopt;

    void* options = g_opts_alloc();
    void* image = nullptr;
    err = g_decode(thumbHandle, &image, HEIF_COLORSPACE_RGB, HEIF_CHROMA_RGBA, options);
    if (options) g_opts_free(options);
    g_handle_release(thumbHandle);
    if (err.code != 0 || !image) return std::nullopt;

    DecodeResult result = RgbaPlaneToBgra(image);
    if (result.width <= 0) return std::nullopt;
    LOG_INFO_STREAM("HeifDecoder") << "缩略图已解码: " << result.width << "x" << result.height;
    return result;
}

// DecodeLevel：首次调用时全量解码并缓存，后续各层从缓存 CPU 降采样
// libheif 不支持区域解码，全量解码一次后复用
std::optional<DecodeResult> HeifDecoder::DecodeLevel(const OpenResult& open, int level) {
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
std::optional<DecodeResult> HeifDecoder::DecodeTile(const OpenResult& open,
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
