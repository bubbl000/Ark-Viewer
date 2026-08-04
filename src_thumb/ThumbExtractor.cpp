#include "ThumbExtractor.h"
#include <algorithm>
#include <cwctype>     // std::towlower
#include <memory>
#include <optional>
#include <string>

// 复用主程序解码器（ARK_THUMB_DLL 宏隔离主程序独有依赖）
#include "FileMapping.h"
#include "ImageDecoder.h"
#include "decoders/RawDecoder.h"
#include "decoders/PsdDecoder.h"
#include "decoders/HeifDecoder.h"
#include "decoders/SvgDecoder.h"
#include "decoders/HdrDecoder.h"

namespace ThumbExtractor {

// 扩展名 → 解码器类型
enum class DecKind { None, Raw, Psd, Heif, Svg, Hdr };

static DecKind ExtToKind(std::wstring ext) {
    // 转小写
    for (auto& c : ext) c = (wchar_t)std::towlower(c);

    if (ext == L".cr2" || ext == L".cr3" || ext == L".nef" || ext == L".arw" ||
        ext == L".dng" || ext == L".raf" || ext == L".x3f" || ext == L".pef" ||
        ext == L".rw2" || ext == L".orf")
        return DecKind::Raw;
    if (ext == L".psd" || ext == L".psb")
        return DecKind::Psd;
    if (ext == L".heic" || ext == L".heif" || ext == L".hif")
        return DecKind::Heif;
    if (ext == L".svg" || ext == L".svgz")
        return DecKind::Svg;
    if (ext == L".hdr" || ext == L".pic")
        return DecKind::Hdr;
    return DecKind::None;
}

// 全黑缩略图检测：内嵌缩略图可能因解码异常输出纯黑（如 _AHY8030）
// 采样像素计算平均亮度，接近纯黑视为无效（让系统显示默认图标）
static bool IsAllBlack(const std::vector<uint8_t>& bgra) {
    if (bgra.size() < 4) return true;
    uint64_t sum = 0;
    size_t count = 0;
    // 采样：每隔 37 像素取一个，避免全量扫描
    for (size_t i = 0; i + 2 < bgra.size(); i += 4 * 37) {
        sum += bgra[i] + bgra[i + 1] + bgra[i + 2];
        count++;
    }
    if (count == 0) return true;
    // 平均每通道亮度 < 3 → 纯黑
    return (sum / (count * 3)) < 3;
}

// 创建解码器实例（按类型，避免依赖 DecoderFactory 注册机制）
static std::unique_ptr<ImageDecoder> CreateDecoder(DecKind kind) {
    switch (kind) {
        case DecKind::Raw:  return std::make_unique<RawDecoder>();
        case DecKind::Psd:  return std::make_unique<PsdDecoder>();
        case DecKind::Heif: return std::make_unique<HeifDecoder>();
        case DecKind::Svg:  return std::make_unique<SvgDecoder>();
        case DecKind::Hdr:  return std::make_unique<HdrDecoder>();
        default:            return nullptr;
    }
}

bool Extract(const wchar_t* path, uint32_t maxDim,
             std::vector<uint8_t>& bgra, uint32_t& w, uint32_t& h) {
    // 全程 try/catch：dllhost 崩溃两次会被系统禁用 provider，任何异常都必须吞掉
    try {
        if (!path || !maxDim) return false;

        // 取扩展名
        std::wstring pathStr(path);
        auto dot = pathStr.find_last_of(L'.');
        if (dot == std::wstring::npos) return false;
        DecKind kind = ExtToKind(pathStr.substr(dot));
        if (kind == DecKind::None) return false;

        // 内存映射文件（遵守项目硬约束：禁全量 ifstream 读取）
        auto fm = std::make_shared<FileMapping>(pathStr);
        if (!fm->Data() || fm->Size() == 0) return false;

        auto dec = CreateDecoder(kind);
        if (!dec) return false;

        // Open + 绑定 FileMapping 到 state（解码器通过 state 持有 MMF）
        auto openResult = dec->Open(fm->Data(), fm->Size());
        if (!openResult) return false;
        openResult->state = fm;

        // 优先内嵌缩略图（快），无则回退全量解码
        bool fromThumb = true;
        std::optional<DecodeResult> result = dec->DecodeThumbnail(*openResult);
        if (!result) {
            fromThumb = false;
            result = dec->DecodeFull(*openResult);
        }
        if (!result || result->width <= 0 || result->height <= 0 ||
            result->pixels.empty()) {
            return false;
        }

        // 黑缩略图处理：仅内嵌缩略图可能误判全黑
        // 非 RAW 回退全量解码（PSD/HEIF 较快）；RAW 全量 demosaic 太慢，直接放弃
        if (fromThumb && IsAllBlack(result->pixels)) {
            if (kind == DecKind::Raw) return false;
            if (auto full = dec->DecodeFull(*openResult)) {
                result = full;
            } else {
                return false;
            }
        }

        // 按比例缩放到 maxDim 框内（最近邻，缩略图质量足够）
        int sw = result->width;
        int sh = result->height;
        int maxSrc = (std::max)(sw, sh);
        if ((uint32_t)maxSrc > maxDim) {
            int tw = (std::max)(1, (int)((uint64_t)sw * maxDim / maxSrc));
            int th = (std::max)(1, (int)((uint64_t)sh * maxDim / maxSrc));
            *result = result->ScaleDown(tw, th);
        }

        bgra = std::move(result->pixels);
        w = (uint32_t)result->width;
        h = (uint32_t)result->height;
        return !bgra.empty() && w > 0 && h > 0;
    } catch (...) {
        return false;
    }
}

}  // namespace ThumbExtractor
