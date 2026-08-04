# Debug Session: drag-image-blank-level2
- **Status**: [OPEN]
- **Issue**: 拖入图片后状态栏显示"已加载"，但屏幕看不到图，标题/日志显示从 Level 2 开始加载
- **Debug Server**: N/A（C++ Win32 桌面应用，使用项目内置 Logger）
- **Log File**: %LOCALAPPDATA%\ArkViewer2\logs\arkviewer2-YYYY-MM-DD.log

## Reproduction Steps
1. 启动 ArkViewer2.exe（首实例，无前置图片）
2. 拖入一张 JPEG 大图到窗口
3. 状态栏显示"已加载: <路径>"
4. 屏幕看不到图（黑屏/灰屏）
5. 标题或日志显示 Level 2

## Hypotheses & Verification
| ID | Hypothesis | Likelihood | Effort | Evidence |
|----|------------|------------|--------|----------|
| A | SwitchToLevel 中 CreateStretchedBitmap 失败返回 nullptr，fallback 创建空黑位图，用户看到黑屏 | High | Low | **Rejected** - 日志显示 CreateStretchedBitmap=OK |
| B | WicDecoder::DecodeLevel(topLevel) 返回 nullopt，DecodeFull fallback 也失败，_sourceBitmap 为 null | Med | Low | **Rejected** - preview=OK, _sourceBitmap=OK |
| C | SelectLevel 返回的 level 超出范围，SwitchToLevel 中 WidthAt/HeightAt 返回 0 提前 return，_sourceBitmap 未更新 | Med | Low | **Rejected** - level=2 在范围内 |
| D | RenderFrame 中 _colorManager.HasIcc() 为 true 但 IccEffect 为 null，走 DrawImageWithTransform 但不画图 | Low | Low | **Rejected** - icc=N iccEffect=NULL |
| E | LoadFile 中 _renderer 为 null，_sourceBitmap 未创建 | Low | Low | **Rejected** - renderer=OK |
| F | **连续切换时主线程同步解码大 PSD 文件阻塞，与 PreDecodeCache 后台 DecodeFull 同时分配大量内存导致崩溃** | High | Med | **Confirmed** - 日志显示崩溃在"同步解顶层 begin"后，加载 1.1GB .psb 文件 |
| G | **Unload 未等待活跃瓦片线程归零，瓦片线程访问已释放的 _decoder 导致竞态崩溃** | Med | Low | Pending - 需验证 |

## Log Evidence

### 拖入第一张图（正常）
```
[03:56:40.224] [INFO] ImgEngine: [DBG] 同步解顶层 begin: topLevel=6 renderer=OK
[03:56:40.529] [INFO] ImgEngine: [DBG] DecodeLevel preview=OK
[03:56:40.530] [INFO] ImgEngine: [DBG] _sourceBitmap=OK size=129x86 (level 6)
[03:56:40.530] [INFO] ImgEngine: [DBG] UpdateViewport: SelectLevel(0.145349)=2 displayedLevel=6 levelCount=7 topLevel=6
[03:56:40.530] [INFO] ImgEngine: [DBG] SwitchToLevel(2): levelW=2064 levelH=1376 sourceBitmap=OK topLevelBitmap=OK
[03:56:40.530] [INFO] D2D: [DBG] Stretch src=129x86 -> dst=2064x1376 endHr=0
[03:56:40.530] [INFO] ImgEngine: [DBG] CreateStretchedBitmap=OK srcBitmap=OK oldSrc=OK 拉伸到 2064x1376
[03:56:40.534] [INFO] ImgEngine: [DBG] RenderFrame DrawBitmap: dest=40,0 1200x800 bmpSize=2064x1376 icc=N iccEffect=NULL
```
**结论**：拖入第一张图完全正常，假设 A-E 全部证伪。

### 连续切换崩溃
```
[03:56:43.601] [INFO] ImgEngine: Navigate: dir=1 files=19 idx=2
[03:56:43.601] [INFO] ImgEngine: 加载文件: ...0116fee918c3135c5f762d6a20848083.psb  ← 1.1GB PSD
[03:56:43.642] [INFO] PsdDecoder: 已解码: 5504x8256 depth=8 mode=3 comp=1  ← PreDecodeCache 后台 DecodeFull
[03:56:43.686] [INFO] FileMapping: 已映射: 1154371875 bytes  ← 1.1GB 映射
[03:56:43.689] [INFO] ImgEngine: [DBG] 同步解顶层 begin: topLevel=6 renderer=OK  ← 主线程同步 DecodeLevel
[之后无日志 - 崩溃]
```

**崩溃原因分析**：
1. idx=1 走异步路径（_blurBitmap=oldBitmap），_sourceBitmap 未设置
2. idx=2 Navigate 时 oldBitmap=nullptr（idx=1 异步未完成）
3. idx=2 走同步路径，主线程调用 PsdDecoder::DecodeLevel → DecodeFull
4. DecodeFull 分配 360MB（5504x8256x4x2），同时 PreDecodeCache 后台也在 DecodeFull
5. 主线程阻塞 + 内存竞争 → 崩溃

## Verification Conclusion
[待修复后对比]
