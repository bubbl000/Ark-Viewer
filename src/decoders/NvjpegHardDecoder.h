#pragma once
#include "../ImageDecoder.h"
#include <optional>
#include <cstdint>
#include <cstddef>

// NVIDIA nvJPEG 硬件解码工具（不继承 ImageDecoder，纯工具类）
// 动态加载 nvjpeg64_12.dll + cudart64_12.dll，无 N 卡/无 CUDA 时 Available() 返回 false
// 线程安全：每线程独立 nvjpeg 句柄（nvjpeg 文档要求 bitstream/state 句柄每线程独立）
// 失败返回 std::nullopt，由调用方（NvjpegDecoder）回退到 WicDecoder
class NvjpegHardDecoder {
public:
    NvjpegHardDecoder() = default;
    ~NvjpegHardDecoder() = default;

    // 进程级可用性检测（首次调用时懒加载 DLL + 初始化，结果缓存）
    // 无 N 卡 / DLL 缺失 / nvjpegCreateSimple 失败 → false
    static bool Available();

    // 全图解码（scale=NONE），输出 BGRA8 像素
    // data/len = JPEG 完整文件数据（内存映射）
    std::optional<DecodeResult> DecodeFull(const uint8_t* data, size_t len);

    // 金字塔层级解码：level n 对应 1/2^n 降采样
    // level 0 = 原图；level 1/2/3 = 1/2/1/4/1/8；level>3 = 1/8 + CPU 二次缩放
    // 输出 BGRA8，与 JpegDecoder/WicDecoder 的 DecodeLevel 语义一致
    std::optional<DecodeResult> DecodeLevel(const uint8_t* data, size_t len, int level);
};
