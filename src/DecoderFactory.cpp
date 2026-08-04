#include "DecoderFactory.h"
#include <algorithm>
#include <cstring>

static std::vector<DecoderFactoryFunc>& Factories() {
    static std::vector<DecoderFactoryFunc> factories;
    return factories;
}

void RegisterDecoder(DecoderFactoryFunc factory) {
    Factories().push_back(std::move(factory));
}

std::unique_ptr<ImageDecoder> FindDecoder(const uint8_t* magic, size_t len) {
    for (auto& factory : Factories()) {
        auto decoder = factory();
        if (!decoder) continue;

        // 尝试用魔数打开
        auto result = decoder->Open(magic, len);
        if (result) {
            decoder.release(); // 需要回收
            return factory();
        }
    }
    return nullptr;
}

// 扩展名 → 解码器名映射（扩展名含点，全小写）
static const char* ExtensionToDecoder(const std::string& ext) {
    // RAW 格式
    if (ext == ".cr2" || ext == ".cr3" || ext == ".nef" || ext == ".arw" ||
        ext == ".dng" || ext == ".raf" || ext == ".x3f" || ext == ".pef" ||
        ext == ".rw2" || ext == ".orf")
        return "RAW";
    // HEIF 格式
    if (ext == ".heic" || ext == ".heif" || ext == ".hif")
        return "HEIF";
    // PSD/PSB
    if (ext == ".psd" || ext == ".psb")
        return "PSD";
    // SVG
    if (ext == ".svg" || ext == ".svgz")
        return "SVG";
    // HDR
    if (ext == ".hdr" || ext == ".pic")
        return "HDR";
    // JPEG
    if (ext == ".jpg" || ext == ".jpeg")
        return "JPEG";
    // WebP
    if (ext == ".webp")
        return "WebP";
    // WIC 原生格式（ICO/CUR/ANI/TGA/DDS 等，魔数未匹配时靠扩展名兜底）
    if (ext == ".ico" || ext == ".cur" || ext == ".ani" ||
        ext == ".tga" || ext == ".dds")
        return "WIC";
    return nullptr;
}

std::unique_ptr<ImageDecoder> FindDecoderByExtension(const std::string& ext) {
    std::string lowerExt = ext;
    std::transform(lowerExt.begin(), lowerExt.end(), lowerExt.begin(), ::tolower);

    const char* targetName = ExtensionToDecoder(lowerExt);
    if (!targetName) return nullptr;

    for (auto& factory : Factories()) {
        auto decoder = factory();
        if (!decoder) continue;
        if (strcmp(decoder->Name(), targetName) == 0) {
            return decoder;
        }
    }
    return nullptr;
}

std::vector<std::string> SupportedExtensions() {
    return { "jpg", "jpeg", "png", "webp", "bmp", "gif", "tif", "tiff",
             "psd", "psb", "hdr", "pic",
             "heic", "heif", "hif",
             "cr2", "cr3", "nef", "arw", "dng", "raf", "x3f", "pef", "rw2", "orf",
             "svg", "svgz",
             "ico", "cur", "ani", "tga", "dds", "dib", "jpe", "jfif" };
}
