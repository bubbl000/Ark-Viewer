# Ark Viewer 2

一款高性能的 Windows 图片浏览器，原生 Win32 + Direct2D + C++20 编写，专为摄影后期与设计工作流打造。
支持专业格式（RAW/PSD/HEIF/AVIF/HDR/TIFF/WebP）、
GPU 硬件解码、超大图分块加载与毫秒级翻页。

## 主要特性
- 专注看图：保持简洁的浏览体验，功能聚焦（浏览/缩放/旋转/翻页/EXIF/缩略图）。
- 支持 Windows x64 图片缩略图扩展（ArkThumbProvider.dll，19 种扩展名：RAW 系/PSD/PSB/HEIC/HEIF/SVG/HDR 等）。
- 添加了 MCP 诊断服务，适合二次开发时 AI 对软件调试。
- F12 查看活动日志

### 格式支持
- **通用格式**: JPG, PNG, GIF, BMP, TIFF, WebP, SVG
- **专业格式**: PSD (Photoshop), HEIC/HEIF, HDR (Radiance)
- **RAW 格式**: ARW (Sony) / CR2 / CR3 (Canon) / NEF (Nikon) / DNG / RAF 等（基于 LibRaw）
- **硬件加速**: NVIDIA nvJPEG 硬解（Baseline JPEG，渐进式自动回退 WIC）
- **格式转换（另存为）**: JPG / PNG / WebP / BMP 互转，JPG 质量可调
## 系统要求

- Windows 10/11 (x64)
- Visual C++ 运行库
- NVIDIA GPU（可选，启用 nvJPEG 硬解；无 N 卡自动回退 CPU/WIC）
- 不支持a卡等非NVIDIA GPU硬解码

## 构建方法

依赖 Visual Studio 2026/18 (MSVC x64) + CMake + Ninja：

```bat
:: 全量构建（推荐，禁增量——增量产物曾损坏）
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

产物：`build/ArkViewer2.exe`（含 libraw.dll / heif.dll / libde265.dll / nvjpeg64_12.dll 等）。


### 性能优化
- **Direct2D GPU 渲染**: 全自绘 UI + 位图渲染，V-Sync 垂直同步
- **瓦片金字塔**: 超大图分块加载，level 0-5 金字塔 + SubRegion 区域解码（>64M 像素自动启用）
- **智能预解码**: 滑动窗口 ±3 张预解码 + 防抖，快速翻页零等待
- **毫秒级翻页**: 导航队列合并 + 缓存命中，翻页延迟 0.1ms 级
- **多级缓存**: PreDecodeCache / DecodedCache / TileCache，瓦片命中率 >80%
- **RAW 快速预览**: half_size 2x2 合并解码，ARW 放大首帧 1.9s → 0.19s
- **动态内存管理**: 缓存按需淘汰，长时间浏览内存稳定

### 界面与交互
- **无边框窗口**: 自绘标题栏 + Win11 圆角 + 8 方向边缘缩放
- **深色主题**: 与 Prism Image Viewer 同源配色（#1A1A1A / 强调绿 #90C208）
- **底部悬浮工具栏**: 信息 / 缩放百分比 / 图片序号 / 旋转 / 适应窗口 / 原始大小
- **智能缩略图条**: 60×60 缩略图，鼠标进入渐显离开渐隐，固定激活框 + 内容滚动
- **鸟瞰图**: 图片超出视口时右下角显示，蓝框拖动同步平移主图
- **边缘导航按钮**: 鼠标靠近左右边缘渐显 ◀ ▶
- **文件夹穿透**: 循环 / 进入相邻文件夹 / 询问 三策略（设置可选）
- **右键菜单**: 重命名 / 另存为（格式转换 + 质量滑条）/ 复制 / 资源管理器中打开 / 复制文件路径 / 设为壁纸 / 删除
- **另存为格式转换**: JPG/PNG/WebP/BMP 互转，JPG 质量滑条（1-100 实时显示预估大小），自动重命名 `原名_ark(n).新扩展名` 防重名
- **EXIF 信息面板**: 20 项拍摄参数（含镜头型号，LibRaw 提取）
- **GIF 控制面板**: 多帧 GIF 右下角面板——帧号实时显示 / 上一帧 / 下一帧 / 播放暂停，可拖动定位
- **OLE 拖出**: 左键平移中拖出窗口边界，将图片拖到 Photoshop 等外部应用打开
- **深色自绘设置面板**: 左侧竖排 Tab（常规/习惯/文件关联），含格式关联注册（文件关联保护）
- **多窗口**: 拖出独立窗口并行浏览

### 性能遥测（特色）
- **ActivityLog 活动日志**: F12 查看，JSONL 落盘
- **MCP 诊断服务**: 9 个工具（性能总览 / 解码统计 / 翻页延迟 / 瓦片命中率 / 内存趋势 / 慢操作 Top10 / 日志检索 / 动态开关遥测）
- **调用栈追踪**: 慢操作自动记录 100ms 阈值 + 模块偏移调用栈

## 快捷键

| 按键 | 功能 |
|---|---|
| ← / → | 上一张 / 下一张 |
| + / - | 放大 / 缩小 |
| F | 适应窗口 |
| R / Shift+R | 左旋 / 右旋 90° |
| Ctrl + 滚轮 | 缩放（滚轮行为可在设置切换）|
| 滚轮 | 缩放（默认）/ 翻页（设置可选）|
| ESC | 退出全屏 / 关闭窗口 |
| F12 | 活动日志窗口 |
| 拖放文件 | 打开图片 |



## 开发与协作说明

本项目由**人类设计 + AI 编码**协作完成，明确分工如下：

### 说明

| 角色 | 负责内容 |
|------|----------|
| **产品设计（人类）** | 软件功能规划、交互逻辑设计、界面布局与操作方式决策、验收标准制定 |
| **代码实现（AI）** | 依据设计文档编写 C++ 代码、构建脚本与文档，实现具体功能与修复 Bug |
| **测试（AI + 人类）** | AI 执行自动化测试与功能验证（COM 直调、性能遥测、回归测试），人类进行手动操作测试与最终验收 |

## 开源致谢

### 直接依赖

| 项目 | 许可证 | 用途 |
|---|---|---|
| [LibRaw](https://github.com/LibRaw/LibRaw) | LGPL-2.1 / CDDL | RAW (ARW) 解码 |
| [libheif](https://github.com/strukturag/libheif) | LGPL-3.0 | HEIF/HEIC 解码 |
| [libde265](https://github.com/strukturag/libde265) | LGPL-3.0 | HEVC 解码（libheif 依赖）|
| [libjpeg-turbo](https://github.com/libjpeg-turbo/libjpeg-turbo) | BSD-3-Clause / IJG | JPEG 解码 |
| [libpng](https://github.com/pnggroup/libpng) | libpng/zlib 式 | PNG 解码 |
| [miniz](https://github.com/richgel999/miniz) | Unlicense | ZIP/PNG 辅助 |
| NVIDIA nvJPEG | NVIDIA 专有 | GPU JPEG 硬解（可选）|

### 参考实现（解码格式参考，未直接引入代码）

| 项目 | 参考内容 |
|---|---|
| [psd-tools](https://github.com/psd-tools/psd-tools) | PSD/PSB 文件格式解析 |
| [psd.js](https://github.com/meltingice/psd.js) | PSD 格式参考 |
| [PSD2HTML](https://github.com/wanxianjia/PSD2HTML) | PSD 解析参考 |
| [PsdPlugin](https://github.com/PsdPlugin/PsdPlugin) | PSD 参考 |
| [libjxl](https://github.com/libjxl/libjxl) | JPEG XL 格式参考 |
| [libtiff](https://github.com/libsdl-org/libtiff) | TIFF 解析参考 |
| [libwebp](https://github.com/webmproject/libwebp) | WebP 格式参考 |
| [libavif](https://github.com/AOMediaCodec/libavif) | AVIF 格式参考 |



## 许可证

本项目采用 MIT 许可证开源。

```
MIT License

Copyright (c) 2026 Ark Viewer 2 Contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
