#include "HdrDecoder.h"
#include "../Logger.h"
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include "../FileMapping.h"
#include "../Tiling.h"

// ─── 内存读取器（基于 open.state 的完整文件字节） ───
struct MemReader {
    const uint8_t* data;
    size_t len;
    size_t pos = 0;

    int ReadByte() { return pos < len ? data[pos++] : -1; }

    // 读取一行（以 \n 结尾），不含换行符；EOF 返回已读内容
    std::string ReadLine() {
        std::string s;
        while (pos < len) {
            char c = (char)data[pos++];
            if (c == '\n') break;
            if (c != '\r') s += c;
        }
        return s;
    }
};

// ─── HDR 文件头解析结果 ───
struct HdrHeader {
    int width = 0;
    int height = 0;
    bool yFlipped = false;   // +Y 表示扫描线自底向顶存储，需垂直翻转
    bool valid = false;
};

// 解析分辨率行，格式 "-Y H +X W" 或 "+Y H -X W"
static bool ParseResolution(const std::string& line, int& w, int& h, bool& yFlipped) {
    const char* p = line.c_str();
    auto skipSpace = [&]() { while (*p == ' ' || *p == '\t') p++; };

    skipSpace();
    if (*p != '-' && *p != '+') return false;
    yFlipped = (*p == '+');
    p++;
    if (*p != 'Y' && *p != 'y') return false;
    p++;
    skipSpace();
    if (*p < '0' || *p > '9') return false;
    h = 0;
    while (*p >= '0' && *p <= '9') { h = h * 10 + (*p - '0'); p++; }

    skipSpace();
    if (*p != '-' && *p != '+') return false;
    p++;
    if (*p != 'X' && *p != 'x') return false;
    p++;
    skipSpace();
    if (*p < '0' || *p > '9') return false;
    w = 0;
    while (*p >= '0' && *p <= '9') { w = w * 10 + (*p - '0'); p++; }

    return w > 0 && h > 0;
}

// 解析 HDR 文件头：魔数 → 元数据行 → 空行 → 分辨率行
// 解析成功后 reader 停在像素数据起始处
static HdrHeader ParseHeader(MemReader& r) {
    HdrHeader h;
    // 魔数行：#?RGBE 或 #?RADIANCE
    std::string magic = r.ReadLine();
    if (magic != "#?RGBE" && magic != "#?RADIANCE") return h;

    // 元数据行直到空行（最多 64 行，防止病态文件）
    for (int i = 0; i < 64; i++) {
        std::string line = r.ReadLine();
        if (line.empty()) break;
    }

    // 分辨率行
    std::string resLine = r.ReadLine();
    if (!ParseResolution(resLine, h.width, h.height, h.yFlipped)) return h;

    h.valid = true;
    return h;
}

// ─── RLE 解码单通道，写入 scanline 的 chanOff 偏移（跨步 4） ───
// Radiance RLE：长度字节 ≤128 为字面量，>128 为重复（重复 len-128 次）
static bool RleReadChannel(MemReader& r, uint8_t* scanline, int chanOff, int width) {
    int written = 0;
    while (written < width) {
        int len = r.ReadByte();
        if (len < 0) return false;
        if (len <= 128) {
            // 字面量：读 len 个独立字节
            if (written + len > width) return false;
            for (int i = 0; i < len; i++) {
                int v = r.ReadByte();
                if (v < 0) return false;
                scanline[chanOff + (written + i) * 4] = (uint8_t)v;
            }
            written += len;
        } else {
            // 重复：下一字节重复 len-128 次
            int rep = len - 128;
            if (written + rep > width) return false;
            int v = r.ReadByte();
            if (v < 0) return false;
            for (int i = 0; i < rep; i++) {
                scanline[chanOff + (written + i) * 4] = (uint8_t)v;
            }
            written += rep;
        }
    }
    return written == width;
}

// ─── 读取一条扫描线 ───
// new-style RLE：开头 4 字节 0x02 0x02 (hi<0x80) (lo)，且 (hi<<8|lo)==width
// 否则按 flat / old-style RLE 处理
static bool ReadScanline(MemReader& r, int width, uint8_t* scanline) {
    if (width >= 8 && width <= 0x7FFF) {
        size_t mark = r.pos;
        int b0 = r.ReadByte(), b1 = r.ReadByte(), b2 = r.ReadByte(), b3 = r.ReadByte();
        if (b0 < 0 || b1 < 0 || b2 < 0 || b3 < 0) return false;

        // new-style RLE 标记：4 字节，无 height 字段（参考 C# 实现注释）
        if (b0 == 2 && b1 == 2 && (b2 & 0x80) == 0 && ((b2 << 8) | b3) == width) {
            for (int c = 0; c < 4; c++) {
                if (!RleReadChannel(r, scanline, c, width)) return false;
            }
            return true;
        }
        r.pos = mark; // 不是 new-style，回退重试 flat
    }

    // flat / old-style RLE
    int x = 0, shift = 0;
    uint8_t pr = 0, pg = 0, pb = 0, pe = 0;
    while (x < width) {
        int b0 = r.ReadByte(), b1 = r.ReadByte(), b2 = r.ReadByte(), b3 = r.ReadByte();
        if (b0 < 0 || b1 < 0 || b2 < 0 || b3 < 0) return false;

        if (b0 == 1 && b1 == 1 && b2 == 1) {
            // old-style 重复标记：重复前一像素 (b3 << shift) 次
            if (x == 0) return false;
            int run = b3 << shift;
            if (x + run > width) return false;
            for (int i = 0; i < run; i++) {
                int o = (x + i) * 4;
                scanline[o] = pr; scanline[o + 1] = pg;
                scanline[o + 2] = pb; scanline[o + 3] = pe;
            }
            x += run;
            shift += 8;
        } else {
            int o = x * 4;
            scanline[o] = (uint8_t)b0; scanline[o + 1] = (uint8_t)b1;
            scanline[o + 2] = (uint8_t)b2; scanline[o + 3] = (uint8_t)b3;
            pr = (uint8_t)b0; pg = (uint8_t)b1; pb = (uint8_t)b2; pe = (uint8_t)b3;
            x++;
            shift = 0;
        }
    }
    return true;
}

// ─── RGBE→linear 缩放表：_expScale[e] = 2^(e-128) / 256 ───
static const float* ExpScaleTable() {
    static float t[256];
    static bool init = false;
    if (!init) {
        for (int e = 0; e < 256; e++)
            t[e] = std::ldexp(1.0f, e - 128) / 256.0f;
        init = true;
    }
    return t;
}

// ─── sRGB 编码查找表（4096 项，线性插值） ───
static const uint8_t* SrgbLut() {
    static uint8_t t[4096];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 4096; i++) {
            float v = i / 4095.0f;
            float e = v <= 0.0031308f ? v * 12.92f
                                        : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
            t[i] = (uint8_t)(e * 255.0f + 0.5f);
        }
        init = true;
    }
    return t;
}

// 线性光 → Reinhard 色调映射 → sRGB 8-bit
static uint8_t SrgbEncode(float v, const uint8_t* lut) {
    if (v <= 0.0f) return 0;
    if (v >= 1.0f) return 255;
    float idx = v * 4095.0f;
    int i0 = (int)idx;
    int i1 = i0 + 1;
    if (i1 >= 4096) return lut[4095];
    float frac = idx - i0;
    return (uint8_t)(lut[i0] + (lut[i1] - lut[i0]) * frac + 0.5f);
}

// ─── Open：魔数检测 + 解析尺寸 ───
std::optional<ImageDecoder::OpenResult> HdrDecoder::Open(const uint8_t* data, size_t len) {
    // 魔数：#?RGBE 或 #?RADIANCE
    if (len < 6) return std::nullopt;
    if (std::memcmp(data, "#?RGBE", 6) != 0 &&
        !(len >= 10 && std::memcmp(data, "#?RADIANCE", 10) == 0))
        return std::nullopt;

    // HDR 文件头很短，前 4096 字节足够解析出尺寸
    MemReader r{data, len, 0};
    HdrHeader h = ParseHeader(r);
    if (!h.valid) return std::nullopt;

    OpenResult result;
    result.info.width = h.width;
    result.info.height = h.height;
    result.info.format = "HDR";
    result.info.decoderName = "HDR";
    return result;
}

// ─── DecodeFull：解析头部 + 解码所有扫描线 → BGRA8 ───
std::optional<DecodeResult> HdrDecoder::DecodeFull(const OpenResult& open) {
    auto fileMap = std::static_pointer_cast<FileMapping>(open.state);
    if (!fileMap || !fileMap->Data()) return std::nullopt;

    MemReader r{fileMap->Data(), fileMap->Size(), 0};
    HdrHeader h = ParseHeader(r);
    if (!h.valid) { LOG_WARN("HdrDecoder", "文件头解析失败"); return std::nullopt; }

    const float* expScale = ExpScaleTable();
    const uint8_t* srgbLut = SrgbLut();

    DecodeResult result;
    result.width = h.width;
    result.height = h.height;
    result.stride = h.width * 4;
    result.pixels.resize((size_t)result.stride * h.height);

    std::vector<uint8_t> scanline(h.width * 4);

    for (int y = 0; y < h.height; y++) {
        if (!ReadScanline(r, h.width, scanline.data())) {
            LOG_WARN_STREAM("HdrDecoder") << "扫描线解码失败 y=" << y;
            return std::nullopt;
        }

        // yFlipped 时扫描线自底向顶，写入时反转 Y 索引
        int writeY = h.yFlipped ? (h.height - 1 - y) : y;
        uint8_t* dst = result.pixels.data() + (size_t)writeY * result.stride;

        // RGBE → linear → Reinhard → sRGB → BGRA
        for (int i = 0; i < h.width; i++) {
            uint8_t rr = scanline[i * 4];
            uint8_t gg = scanline[i * 4 + 1];
            uint8_t bb = scanline[i * 4 + 2];
            uint8_t ee = scanline[i * 4 + 3];

            float fr, fg, fb;
            if (ee == 0) {
                fr = fg = fb = 0.0f;
            } else {
                float scale = expScale[ee];
                fr = (rr + 0.5f) * scale;
                fg = (gg + 0.5f) * scale;
                fb = (bb + 0.5f) * scale;
            }

            // Reinhard 色调映射 x/(1+x)，把 [0,∞) 压缩到 [0,1)
            fr = fr / (1.0f + fr);
            fg = fg / (1.0f + fg);
            fb = fb / (1.0f + fb);

            dst[i * 4]     = SrgbEncode(fb, srgbLut); // B
            dst[i * 4 + 1] = SrgbEncode(fg, srgbLut); // G
            dst[i * 4 + 2] = SrgbEncode(fr, srgbLut); // R
            dst[i * 4 + 3] = 255;                      // A
        }
    }

    LOG_INFO_STREAM("HdrDecoder") << "已解码: " << h.width << "x" << h.height;
    return result;
}

// DecodeLevel：首次调用时全量解码并缓存，后续各层从缓存 CPU 降采样
// HDR 不支持区域解码，全量解码一次后复用
std::optional<DecodeResult> HdrDecoder::DecodeLevel(const OpenResult& open, int level) {
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
std::optional<DecodeResult> HdrDecoder::DecodeTile(const OpenResult& open,
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
