#pragma once
#include <cstdint>
#include <vector>

// 缩略图提取器：按扩展名分发到各解码器，输出 BGRA8 像素
// 运行在 dllhost.exe（资源管理器缩略图进程），稳定性第一：任何失败静默返回 false
namespace ThumbExtractor {

// 提取缩略图。maxDim 为目标最大边长，输出按比例缩放到 maxDim×maxDim 框内。
// 成功时 bgra 填充 BGRA8 像素，w/h 为输出尺寸；失败返回 false（系统显示默认图标）。
bool Extract(const wchar_t* path, uint32_t maxDim,
             std::vector<uint8_t>& bgra, uint32_t& w, uint32_t& h);

}  // namespace ThumbExtractor
