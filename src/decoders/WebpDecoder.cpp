#include "WebpDecoder.h"
#include <cstring>
#include "Logger.h"
#include <cstdlib>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "../FileMapping.h"

typedef int   (__stdcall *fn_WI)(const uint8_t*, size_t, int*, int*);
typedef uint8_t* (__stdcall *fn_WD)(const uint8_t*, size_t, int*, int*);
typedef void  (__stdcall *fn_WF)(void*);
// WebP 编码：返回编码后字节数（0=失败），*output 为分配的缓冲（WebPFree 释放）
typedef size_t (__stdcall *fn_WE)(const uint8_t*, int, int, int, float, uint8_t**);

static HMODULE g_wp = nullptr;
static fn_WI wpInfo = nullptr;
static fn_WD wpDecode = nullptr;
static fn_WF wpFree = nullptr;
static fn_WE wpEncode = nullptr;  // 编码函数（libwebp.dll 含编码导出）

static bool LoadWP() {
    if (g_wp) return true;
    g_wp = LoadLibraryW(L"libwebp.dll");
    if (!g_wp) return false;
    wpInfo   = (fn_WI)GetProcAddress(g_wp, "WebPGetInfo");
    wpDecode = (fn_WD)GetProcAddress(g_wp, "WebPDecodeBGRA");
    wpFree   = (fn_WF)GetProcAddress(g_wp, "WebPFree");
    wpEncode = (fn_WE)GetProcAddress(g_wp, "WebPEncodeBGRA");
    return wpInfo && wpDecode && wpFree;
}

std::optional<ImageDecoder::OpenResult> WebpDecoder::Open(const uint8_t* d, size_t l) {
    if (l < 12 || memcmp(d, "RIFF", 4) != 0 || memcmp(d+8, "WEBP", 4) != 0) return {};
    if (!LoadWP()) return {};
    int w=0, h=0;
    if (!wpInfo(d, l, &w, &h) || w <= 0 || h <= 0) return {};
    OpenResult res; res.info.width = w; res.info.height = h;
    res.info.format = "WebP"; res.info.decoderName = "libwebp";
    return res;
}
std::optional<DecodeResult> WebpDecoder::DecodeFull(const OpenResult& open) {
    if (!LoadWP()) return {};
    auto fileMap = std::static_pointer_cast<FileMapping>(open.state);
    if (!fileMap || !fileMap->Data()) return {};

    int w=0,h=0;
    uint8_t* pixels = wpDecode(fileMap->Data(), fileMap->Size(), &w, &h);
    if (!pixels || w <= 0 || h <= 0) return {};

    DecodeResult result;
    result.width = w; result.height = h; result.stride = w * 4;
    result.pixels.resize((size_t)w * h * 4);
    memcpy(result.pixels.data(), pixels, (size_t)w * h * 4);
    wpFree(pixels);
    // WebP 可能没有 alpha，确保不透明
    for (size_t i = 3; i < result.pixels.size(); i += 4)
        if (result.pixels[i] == 0) result.pixels[i] = 255;
    return result;
}

extern "C" __declspec(dllexport)
bool WebpDecode(const uint8_t* data, size_t len,
    uint8_t** outPixels, int* outW, int* outH)
{
    if (!LoadWP()) return false;
    int w=0,h=0;
    uint8_t* pixels = wpDecode(data, len, &w, &h);
    if (!pixels) return false;
    *outPixels = (uint8_t*)malloc((size_t)w*h*4);
    if (!*outPixels) { wpFree(pixels); return false; }
    memcpy(*outPixels, pixels, (size_t)w*h*4);
    wpFree(pixels);
    *outW=w; *outH=h;
    return true;
}
std::optional<DecodeResult> WebpDecoder::DecodeTile(const OpenResult&, int, int, int) { return {}; }
std::optional<DecodeResult> WebpDecoder::DecodeLevel(const OpenResult&, int) { return {}; }

// BGRA → WebP 有损编码（quality 1-100）
bool WebpDecoder::EncodeWebp(const uint8_t* bgra, int w, int h, int stride,
                             int quality, std::vector<uint8_t>& out) {
    if (!LoadWP() || !wpEncode) return false;
    if (!bgra || w <= 0 || h <= 0 || stride < w * 4) return false;
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;

    uint8_t* encBuf = nullptr;
    size_t sz = wpEncode(bgra, w, h, stride, (float)quality, &encBuf);
    if (sz == 0 || !encBuf) { if (encBuf) wpFree(encBuf); return false; }
    out.assign(encBuf, encBuf + sz);
    wpFree(encBuf);
    return true;
}




