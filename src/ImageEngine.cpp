#include "ImageEngine.h"
#include "DecoderFactory.h"
#include "PreDecodeCache.h"
#include "ActivityLog.h"
#include "Logger.h"
#include "Config.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <Shlwapi.h>

namespace fs = std::filesystem;

// RAII 守卫：取到瓦片任务时标记忙（预解码让步），作用域结束自动清除
// 多线程下用计数器，最后一个线程退出时才清除 _tileBusy
namespace {
struct TileBusyGuard {
    std::atomic<int>&  counter;
    std::atomic<bool>& busy;
    TileBusyGuard(std::atomic<int>& c, std::atomic<bool>& b) : counter(c), busy(b) {
        counter.fetch_add(1, std::memory_order_relaxed);
        busy.store(true, std::memory_order_release);
    }
    ~TileBusyGuard() {
        int prev = counter.fetch_sub(1, std::memory_order_acq_rel);
        if (prev == 1) busy.store(false, std::memory_order_release);
    }
};

// 判断路径是否为 GIF：动画需主线程逐帧 CreateBitmap，不能进全异步后台路径
bool IsGifPath(const std::wstring& path) {
    auto dotPos = path.find_last_of(L'.');
    if (dotPos == std::wstring::npos) return false;
    std::wstring ext = path.substr(dotPos);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    return ext == L".gif";
}
}

ImageEngine::ImageEngine() = default;
ImageEngine::~ImageEngine() {
    _tileRunning = false;
    _tileCV.notify_all();  // 唤醒所有等待的瓦片线程
    for (auto& t : _tileThreads) if (t.joinable()) t.join();
    Unload();
    // Unload 已 detach _bgThread；线程持有 shared_ptr 副本，安全后台退出
    // 此处再次确认 detach，防止 ~thread() 调用 std::terminate
    if (_bgThread.joinable()) _bgThread.detach();
}

void ImageEngine::Initialize(D2DRenderer* renderer, PreDecodeCache* preDecodeCache) {
    _renderer = renderer;
    _preDecodeCache = preDecodeCache;
    _colorManager.SetRenderer(renderer);
    _tileRunning = true;
    // 多线程并行解码：CPU 核数 - 1（留 1 核给主线程渲染+UI），至少 2 线程
    int n = (std::max)(2, (int)std::thread::hardware_concurrency() - 1);
    _tileThreads.reserve(n);
    for (int i = 0; i < n; i++)
        _tileThreads.emplace_back(&ImageEngine::TileWorker, this);
}

void ImageEngine::FitToWindow(int clientW, int clientH) {
    // 仅依赖 _srcWidth/_srcHeight：瓦片模式下 _sourceBitmap 等后台解码才就绪，
    // 但尺寸已先设置，FitToWindow 必须能提前计算缩放，否则 _scaleX 保持 1.0
    // 会让 UpdateViewport 的 SelectLevel 误选 level 0
    if (_srcWidth <= 0 || _srcHeight <= 0) return;
    LOG_DBG_STREAM("ImgEngine") << "FitToWindow: client=" << clientW << "x" << clientH;

    // 旋转 90/270 时显示宽高互换：按互换后的尺寸适配窗口
    int dispW = IsRotatedSideways() ? _srcHeight : _srcWidth;
    int dispH = IsRotatedSideways() ? _srcWidth  : _srcHeight;

    double sx = (double)clientW / dispW;
    double sy = (double)clientH / dispH;
    double scale = (std::min)(sx, sy);
    if (scale <= 0) scale = 1.0;

    _scaleX = scale;
    _scaleY = scale;
    _offsetX = (clientW - dispW * scale) / 2.0;
    _offsetY = (clientH - dispH * scale) / 2.0;
}

void ImageEngine::ZoomTo100() {
    _scaleX = 1.0;
    _scaleY = 1.0;
    if (_renderer) {
        // 旋转 90/270 时居中用互换后的尺寸
        int dispW = IsRotatedSideways() ? _srcHeight : _srcWidth;
        int dispH = IsRotatedSideways() ? _srcWidth  : _srcHeight;
        _offsetX = (_renderer->Width()  - dispW) / 2.0;
        _offsetY = (_renderer->Height() - dispH) / 2.0;
    }
}

void ImageEngine::RotateLeft() {
    RotatePreservingCenter(90);
}

void ImageEngine::RotateRight() {
    RotatePreservingCenter(270);  // 顺时针 90° = 逆时针 270°
}

// 旋转以图片中心为原点：缩放不变，旋转前后调整偏移使图片中心停留在同一屏幕坐标
// 不调 FitToWindow（避免缩放/位置跳变）；90/270 时显示宽高互换，中心偏移随之重算
void ImageEngine::RotatePreservingCenter(int delta) {
    int oldRot = _rotation;
    _rotation = ((_rotation + delta) % 360 + 360) % 360;
    if (!_renderer || _srcWidth <= 0 || _srcHeight <= 0) {
        if (_renderer) _renderer->NotifyInteraction();
        return;
    }
    bool oldSide = (oldRot == 90 || oldRot == 270);
    bool newSide = IsRotatedSideways();
    double s = _scaleX;
    double oldDispW = (oldSide ? _srcHeight : _srcWidth)  * s;
    double oldDispH = (oldSide ? _srcWidth  : _srcHeight) * s;
    double newDispW = (newSide ? _srcHeight : _srcWidth)  * s;
    double newDispH = (newSide ? _srcWidth  : _srcHeight) * s;
    // 图片中心屏幕坐标旋转前后保持不变
    double cx = _offsetX + oldDispW / 2.0;
    double cy = _offsetY + oldDispH / 2.0;
    _offsetX = cx - newDispW / 2.0;
    _offsetY = cy - newDispH / 2.0;
    if (_renderer) _renderer->NotifyInteraction();
}

// ── 统一坐标变换（屏幕↔原图，含旋转分支）──
// 与 DrawBitmapRotated 矩阵严格对应：旋转 90/270 时屏幕轴↔原图轴互换并带符号翻转。
// Zoom 锚定 / Pan 等价证明 / QueueTiles 视口 / 鸟瞰蓝框均以此为唯一变换来源。
void ImageEngine::ScreenToImage(double sx, double sy, double& ix, double& iy) const {
    double s = _scaleX > 0 ? _scaleX : 1.0;
    switch (_rotation) {
        case 90:   // 逆时针90°：屏幕X→原图Y，屏幕Y→原图X(翻转)
            ix = _srcWidth  - (sy - _offsetY) / s;
            iy = (sx - _offsetX) / s;
            break;
        case 180:  // 逆时针180°：屏幕X→原图X(翻转)，屏幕Y→原图Y(翻转)
            ix = _srcWidth  - (sx - _offsetX) / s;
            iy = _srcHeight - (sy - _offsetY) / s;
            break;
        case 270:  // 逆时针270°：屏幕X→原图Y(翻转)，屏幕Y→原图X
            ix = (sy - _offsetY) / s;
            iy = _srcHeight - (sx - _offsetX) / s;
            break;
        default:   // 0°：屏幕X→原图X，屏幕Y→原图Y
            ix = (sx - _offsetX) / s;
            iy = (sy - _offsetY) / s;
            break;
    }
}

// 旋转映射项（不含 offset）：ImageToScreen = (offsetX+rx, offsetY+ry)
// rx/ry 只依赖旋转与缩放，不依赖 offset——这正是 Pan 直接累加 offset 即可的数学根因
void ImageEngine::ImageToScreenRel(double ix, double iy, double& rx, double& ry) const {
    double s = _scaleX > 0 ? _scaleX : 1.0;
    switch (_rotation) {
        case 90:  rx = iy * s;                  ry = (_srcWidth - ix) * s;  break;
        case 180: rx = (_srcWidth - ix) * s;    ry = (_srcHeight - iy) * s; break;
        case 270: rx = (_srcHeight - iy) * s;   ry = ix * s;                break;
        default:  rx = ix * s;                  ry = iy * s;                break;
    }
}

void ImageEngine::ImageToScreen(double ix, double iy, double& sx, double& sy) const {
    double rx, ry;
    ImageToScreenRel(ix, iy, rx, ry);
    sx = _offsetX + rx;
    sy = _offsetY + ry;
}

void ImageEngine::RenameCurrentPath(const std::wstring& newPath) {
    // 仅更新路径与目录列表项，已解码纹理/金字塔/缓存全部复用
    if (_currentIndex >= 0 && _currentIndex < (int)_dirFiles.size()) {
        _dirFiles[_currentIndex] = newPath;
    }
    _currentPath = newPath;
}

void ImageEngine::ToggleFlipH() {
    _flipH = !_flipH;
    if (_renderer) _renderer->NotifyInteraction();
}

void ImageEngine::ToggleFlipV() {
    _flipV = !_flipV;
    if (_renderer) _renderer->NotifyInteraction();
}

void ImageEngine::Zoom(double factor, double mouseX, double mouseY) {
    if (!_sourceBitmap) return;
    LOG_DBG_STREAM("ImgEngine") << "Zoom: factor=" << factor << " scale=" << _scaleX * factor;

    double newScale = (std::clamp)(_scaleX * factor, 0.05, 40.0);

    // 锚定鼠标位置：鼠标对应的原图点在缩放前后保持不动
    // 1) 旧 scale 下用统一 ScreenToImage 求鼠标对应的原图点（必须在改 scale 之前）
    double ix, iy;
    ScreenToImage(mouseX, mouseY, ix, iy);
    // 2) 切到新 scale，用 ImageToScreenRel 求新 scale 下该原图点的旋转映射项
    _scaleX = newScale;
    _scaleY = newScale;
    double rx, ry;
    ImageToScreenRel(ix, iy, rx, ry);
    // 3) 反求 offset：使该原图点仍映射到鼠标屏幕坐标（ImageToScreen = offset + rel）
    _offsetX = mouseX - rx;
    _offsetY = mouseY - ry;

    // 交互节流：标记交互中并刷新停手截止时间
    // 放大过程中不切层，保持当前层高清纹理 GPU 拉伸显示（清晰，避免切到目标层占位变糊）
    // 停手 200ms 后由 CheckInteractionTimeout 触发 UpdateViewport 切层+解码
    _interacting = true;
    _interactionDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    _lastBrowseTick = std::chrono::steady_clock::now();  // 浏览操作：重置 3 分钟空闲计时
    if (_renderer) _renderer->NotifyInteraction();  // V-Sync 切换：缩放期间用 Present(0)
}

// 滚轮停手超时检查：由主线程 30ms 定时器调用
// 两阶段渐进过渡：落差大时先切中间层，200ms 后再切目标层，分两步消化落差
void ImageEngine::CheckInteractionTimeout() {
    auto now = std::chrono::steady_clock::now();

    // 延迟 UpdateViewport：ApplyBgResultIfReady 设标志，此处非绘制期安全执行
    // 优先级最高：异步解码完成后需尽快切层+排瓦片，否则用户看到低分辨率顶层预览
    FlushViewportUpdate();

    // 预解码防抖：停手 30ms 后才触发，避免快速切换时每次都 SetCurrentIndex
    if (_preDecodePending && now >= _preDecodeDeadline) {
        _preDecodePending = false;
        TriggerPreDecode(_preDecodeFwd, _preDecodeBwd);
    }

    // 切换防抖：快速连续切换时停手 200ms 后才启动正式解码
    // 优先级最高：解码未启动时 _sourceBitmap 为空，其他切层逻辑无意义
    if (_bgDecodeScheduled && now >= _bgDecodeDeadline) {
        int level = _bgDecodeLevel;
        bool skipThumb = _bgDecodeSkipThumb;
        std::wstring pendingPath = _bgDecodePendingPath;
        _bgDecodeScheduled = false;
        _bgDecodePendingPath.clear();

        // 缓存未命中：延迟到此处启动全异步加载（非 OnPaint，不阻塞绘制）
        // GIF 走同步 LoadFile：动画需主线程逐帧 CreateBitmap，且 GIF 体积小不冻结
        // 其他格式走 StartBgFullLoad：FileMapping+Open+Pyramid+解码 全部后台，UI 零阻塞
        if (_bgCacheMiss) {
            _bgCacheMiss = false;
            if (IsGifPath(pendingPath)) {
                ActivityLog::Instance().Log(L"防抖", L"GIF 未命中，同步加载");
                LoadFile(pendingPath, _bgFwd, _bgBwd, true);
            } else {
                ActivityLog::Instance().Log(L"防抖", L"缓存未命中，全异步加载");
                StartBgFullLoad(pendingPath, _bgFwd, _bgBwd);
            }
            return;  // 预解码已由 Navigate 未命中分支设 _preDecodePending 触发
        }

        // 延迟文件打开：LoadFromCache 跳过了 FileMapping + decoder.Open，
        // 此处（停手后）才打开，避免快速切换时每张都打开文件（~15ms/次）
        if (!_fileMapping || _currentPath != pendingPath) {
            _fileMapping = std::make_shared<FileMapping>(pendingPath);
            if (_fileMapping->Data()) {
                size_t headerLen = (std::min)(_fileMapping->Size(), (size_t)4096);
                _decoder = FindDecoder(_fileMapping->Data(), headerLen);
                if (!_decoder) {
                    std::wstring extW = pendingPath.substr(pendingPath.find_last_of(L'.'));
                    std::string extA(extW.begin(), extW.end());
                    _decoder = FindDecoderByExtension(extA);
                }
                if (_decoder) {
                    auto openResult = _decoder->Open(_fileMapping->Data(), _fileMapping->Size());
                    if (openResult) {
                        _openResult = std::move(*openResult);
                        _openResult.state = _fileMapping;
                        _imageInfo = _openResult.info;
                    }
                }
            }
        }

        // 缓存命中时 _sourceBitmap 已就绪，跳过顶层解码
        if (_sourceBitmap && _decoder && _decoder->SupportsTiling()
            && _pyramid && _pyramid->LevelCount() > 1) {
            if (_cacheFullQuality) {
                // 高清缓存：纹理已含瓦片，仅打开文件供后续缩放，不重新排瓦片
                _cacheFullQuality = false;
                ActivityLog::Instance().Log(L"防抖", L"缓存命中（高清），跳过瓦片解码");
            } else {
                // 低清缓存（仅顶层）：排瓦片补充高清像素
                ActivityLog::Instance().Log(L"防抖", L"缓存命中，直接排瓦片");
                UpdateViewport();
            }
        } else if (_decoder) {
            ActivityLog::Instance().Log(L"防抖", L"停手超时，启动正式解码");
            StartBgTopLevelDecode(level, skipThumb);
        } else {
            ActivityLog::Instance().Log(L"防抖", L"文件打开失败，仅显示缩略图");
        }
        return;  // 本帧已启动解码，下一帧再处理其他
    }

    if (now < _interactionDeadline) return;

    // 阶段 2：渐进过渡收尾——切到最终目标层
    // 阶段 1 已切中间层并排瓦片，此处切剩余的目标层
    if (_pendingTargetLevel >= 0) {
        int target = _pendingTargetLevel;
        _pendingTargetLevel = -1;
        if (_pyramid && target >= 0 && target < _pyramid->LevelCount()
            && _decoder && _decoder->SupportsTiling() && _renderer && _sourceBitmap) {
            if (target != _displayedLevel) SwitchToLevel(target);
            QueueTiles(target);
        }
        return;
    }

    if (!_interacting) return;
    _interacting = false;

    // 停手后切层：落差大（≥2 级）时渐进过渡，先切中间层消化部分落差
    // 中间层占位+瓦片先到位，200ms 后再切目标层，避免一次跨多层视觉跳跃
    if (_pyramid && _decoder && _decoder->SupportsTiling() && _pyramid->LevelCount() > 1) {
        int targetLevel = _pyramid->SelectLevel(_scaleX);
        if (targetLevel >= 0 && _displayedLevel - targetLevel >= 2) {
            int mid = (_displayedLevel + targetLevel) / 2;
            SwitchToLevel(mid);
            QueueTiles(mid);
            _pendingTargetLevel = targetLevel;
            _interactionDeadline = now + std::chrono::milliseconds(200);
            return;
        }
    }
    UpdateViewport();
}

// 3 分钟空闲超时：窗口在前台且无浏览操作时，移除非当前顶层预览
// 由主线程 30ms 定时器调用，内部早期 return 保证开销极低
void ImageEngine::CheckIdleTimeout(HWND hwnd) {
    EvictDecodedCache();  // 顺带清理已解码缓存过期条目
    if (!_preDecodeCache) return;
    // 窗口最小化或后台：保留缓存不移除（用户可能恢复查看）
    if (IsIconic(hwnd)) return;
    if (GetForegroundWindow() != hwnd) return;

    auto now = std::chrono::steady_clock::now();
    auto idleSec = std::chrono::duration_cast<std::chrono::seconds>(
        now - _lastBrowseTick).count();
    if (idleSec < 180) return;  // 3 分钟 = 180 秒

    // 空闲超时：仅保留当前图的顶层预览
    if (_currentIndex >= 0) {
        _preDecodeCache->ClearExcept(_currentIndex);
    }
}

void ImageEngine::Pan(double dx, double dy) {
    LOG_DBG_STREAM("ImgEngine") << "Pan: dx=" << dx << " dy=" << dy;
    // Pan 直接累加屏幕偏移，与 Zoom/QueueTiles 共用同一套变换体系且数学等价：
    // ImageToScreen = (offsetX + rx, offsetY + ry)，其中 rx/ry=ImageToScreenRel 只依赖
    // 旋转与缩放、不含 offset（见 DrawBitmapRotated 的 trans 为纯屏幕平移项）。
    // 故 d(offsetX)=dx、d(offsetY)=dy 对任意旋转都使图像整体平移 (dx,dy)——
    // 即"鼠标向右拖 → 图像向右动"。无需按旋转互换 dx/dy（那反而会引入轴错乱）。
    _offsetX += dx;
    _offsetY += dy;
    _lastBrowseTick = std::chrono::steady_clock::now();  // 浏览操作：重置 3 分钟空闲计时
    if (_renderer) _renderer->NotifyInteraction();  // V-Sync 切换：拖拽期间用 Present(0)
    UpdateViewport();
}

void ImageEngine::OnResize(int w, int h) {
    // 中心保持逻辑（对应原 CanvasGrid_SizeChanged 非 FitMode 中心保持）
    if (_sourceBitmap) {
        double imgW = _srcWidth  * _scaleX;
        double imgH = _srcHeight * _scaleY;
        // 保持旧中心在新画布中的相对位置不变
        double centerX = _offsetX + imgW / 2;
        double centerY = _offsetY + imgH / 2;

        // 如果 change ratio 很大则重置到中心
        // 否则按比例调整
        if (centerX > 0 && centerY > 0) {
            double relX = centerX / (centerX + _offsetX); // 简化
            double relY = centerY / (centerY + _offsetY);
        }
    }
    UpdateViewport();
}

void ImageEngine::RenderFrame() {
    if (!_renderer) return;

    PerfScope perfRender(L"渲染", "RenderFrame");  // 性能遥测：UI 线程渲染耗时

    // 检测后台顶层解码结果就绪：主线程换入 _sourceBitmap，触发 UpdateViewport 排瓦片
    ApplyBgResultIfReady();

    // 处理待更新的瓦片纹理：主线程执行，D2D 操作安全
    // TileWorker 后台解码后存入 pending 队列，此处统一贴入 GPU 纹理并存缓存
    // 避免后台线程操作 D2D 单线程资源 + _sourceBitmap 生命周期竞态
    if (!_pendingTiles.empty()) {
        std::vector<PendingTile> pending;
        {
            std::lock_guard lock(_pendingMutex);
            pending.swap(_pendingTiles);
        }
        for (auto& pt : pending) {
            if (pt.level != _displayedLevel) continue;  // 层级已切换，瓦片坐标不匹配
            if (!_sourceBitmap) continue;
            int dstX = pt.col * TILE_SIZE;
            int dstY = pt.row * TILE_SIZE;
            _renderer->UpdateBitmapRegion(
                _sourceBitmap.Get(), dstX, dstY,
                pt.width, pt.height,
                pt.pixels.data(), pt.stride);
            // 新解码的瓦片存入缓存（fromCache=true 的已在缓存，跳过避免重复）
            if (!pt.fromCache && _tileCache) {
                TileIndex idx = { pt.level, pt.col, pt.row };
                _tileCache->Put(idx, std::move(pt.pixels));
            }
            (void)dstX; (void)dstY;
            ActivityLog::Instance().Log(L"瓦片",
                L"贴入 [" + std::to_wstring(pt.col) + L"," + std::to_wstring(pt.row) +
                L"] L" + std::to_wstring(pt.level) + L" -> disp L" +
                std::to_wstring(_displayedLevel) + L" src=" +
                std::to_wstring((int)_sourceBitmap->GetPixelSize().width) + L"x" +
                std::to_wstring((int)_sourceBitmap->GetPixelSize().height));
        }
    }

    // 计算目标矩形（原始尺寸 × 缩放）
    // 旋转 90/270 时显示宽高互换：destW 用 srcHeight，destH 用 srcWidth
    bool sideways = IsRotatedSideways();
    float w = (float)((sideways ? _srcHeight : _srcWidth)  * _scaleX);
    float h = (float)((sideways ? _srcWidth  : _srcHeight) * _scaleY);
    float x = (float)_offsetX;
    float y = (float)_offsetY;

    // 有旋转/翻转时走矩阵变换路径（不改变原图，仅视图变换）
    // 注：ICC 色彩管理 effect 与旋转矩阵组合较复杂，旋转时暂跳过 ICC（少见场景）
    bool hasTransform = (_rotation != 0) || _flipH || _flipV;

    // 优先绘制 _sourceBitmap（高清），否则用 _blurBitmap 模糊占位
    if (_sourceBitmap) {
        // 棋盘格背景：开启设置时在图片区域先铺棋盘格，图片绘制其上，透明处露出棋盘格
        if (Config::Instance().Get().checkerboard)
            _renderer->DrawCheckerboard(x, y, w, h, 8.0f,
                Config::Instance().Get().checkerboardOpacity / 100.0f);
        if (hasTransform) {
            _renderer->DrawBitmapRotated(_sourceBitmap.Get(), x, y, w, h,
                _rotation, _flipH, _flipV);
        } else if (_colorManager.HasIcc() && _colorManager.IccEffect()) {
            _renderer->DrawImageWithTransform(
                _colorManager.IccEffect(), (float)_scaleX, (float)_scaleY, x, y);
        } else {
            _renderer->DrawBitmap(_sourceBitmap.Get(), x, y, w, h);
        }
    } else if (_blurBitmap) {
        // 模糊占位：缩略图/旧图拉伸铺满视口，高斯模糊掩盖锯齿
        // 标准差自适应：放大倍数越大越模糊（最少 2.0 保证视觉柔和）
        D2D1_SIZE_F blurSize = _blurBitmap->GetSize();
        float maxScale = (std::max)(w / blurSize.width, h / blurSize.height);
        float blurRadius = (std::max)(2.0f, maxScale * 0.4f);
        _renderer->DrawBitmapBlurred(_blurBitmap.Get(), x, y, w, h, blurRadius);
    }
}

void ImageEngine::UpdateAnimation() {
    if (!_animPlayer.IsPlaying()) return;
    int curr = _animPlayer.NextFrame();
    if (curr == _lastAnimFrame) return;  // 帧未变，无需更新
    _lastAnimFrame = curr;
    auto* frame = _animPlayer.CurrentFrame();
    if (frame && _sourceBitmap && _renderer) {
        // 增量更新 GPU 纹理（复用同一张位图，避免每帧重建）
        _renderer->UpdateBitmapRegion(
            _sourceBitmap.Get(), 0, 0,
            frame->width, frame->height,
            frame->pixels.data(), frame->stride);
    }
}

int ImageEngine::AnimFrameCount() const {
    return _animPlayer.FrameCount();
}

int ImageEngine::AnimCurrentFrame() const {
    return _animPlayer.CurrentIndex();
}

bool ImageEngine::AnimSetFrame(int i) {
    if (!_animPlayer.SetFrame(i)) return false;
    _lastAnimFrame = _animPlayer.CurrentIndex();
    auto* frame = _animPlayer.CurrentFrame();
    if (frame && _sourceBitmap && _renderer) {
        // 步进帧像素更新到 GPU 纹理（复用 UpdateAnimation 的增量更新逻辑）
        _renderer->UpdateBitmapRegion(
            _sourceBitmap.Get(), 0, 0,
            frame->width, frame->height,
            frame->pixels.data(), frame->stride);
    }
    return true;
}

void ImageEngine::AnimTogglePlay() {
    _animPlayer.SetPlaying(!_animPlayer.IsPlaying());
}

bool ImageEngine::LoadFile(const std::wstring& path, int fwd, int bwd, bool forceAsync) {
    LOG_INFO_STREAM("ImgEngine") << "加载文件: " << std::string(path.begin(), path.end());
    ActivityLog::Instance().Log(L"加载",
        L"开始: " + ActivityFmt::ShortName(path));

    // 保留旧图 GPU 纹理作模糊占位 fallback（无缩略图时使用）
    // oldBlur 备份占位图：大文件快速连点时 _sourceBitmap 还是 null（phase 2 未完），
    // 此时实际显示的是 _blurBitmap（上一张缩略图/旧图），Unload 会清掉它，
    // 必须先备份才能在新图占位建立前维持画面连续
    ComPtr<ID2D1Bitmap1> oldBitmap = _sourceBitmap;
    ComPtr<ID2D1Bitmap1> oldBlur   = _blurBitmap;

    // 事务快照：另存为写入触发的重载可能因文件正在写而 LoadFile 失败，
    // 失败时回滚到加载前状态，避免 Unload 已清空 _sourceBitmap 导致画面消失
    struct TxnSnap {
        ImageEngine& e;
        ComPtr<ID2D1Bitmap1> src, top, blur;
        std::shared_ptr<FileMapping> fm;
        std::shared_ptr<TileCache> tc;
        std::shared_ptr<Pyramid> py;
        std::shared_ptr<ImageDecoder> dec;
        ImageDecoder::OpenResult open;
        ImageInfo info;
        std::wstring curPath;
        int idx, sw, sh, dispLvl, cachedLvl;
        std::wstring cachedPath;
        DecodeResult cachedResult;
        double sx, sy, ox, oy;
        int rot; bool fh, fv;
        bool committed = false;
        TxnSnap(ImageEngine& eng) : e(eng),
            src(e._sourceBitmap), top(e._topLevelBitmap), blur(e._blurBitmap),
            fm(e._fileMapping), tc(e._tileCache), py(e._pyramid), dec(e._decoder),
            open(e._openResult), info(e._imageInfo),
            curPath(e._currentPath), idx(e._currentIndex),
            sw(e._srcWidth), sh(e._srcHeight), dispLvl(e._displayedLevel),
            cachedLvl(e._cachedLevel.load()),
            cachedPath(e._cachedLevelPath), cachedResult(e._cachedLevelResult),
            sx(e._scaleX), sy(e._scaleY), ox(e._offsetX), oy(e._offsetY),
            rot(e._rotation), fh(e._flipH), fv(e._flipV) {}
        ~TxnSnap() {
            if (committed) return;
            e._sourceBitmap = src;
            e._topLevelBitmap = top;
            e._blurBitmap = blur;
            e._fileMapping = fm;
            e._tileCache = tc;
            e._pyramid = py;
            e._decoder = dec;
            e._openResult = std::move(open);
            e._imageInfo = std::move(info);
            e._currentPath = std::move(curPath);
            e._currentIndex = idx;
            e._srcWidth = sw; e._srcHeight = sh;
            e._displayedLevel = dispLvl;
            e._cachedLevel.store(cachedLvl);
            e._cachedLevelPath = std::move(cachedPath);
            e._cachedLevelResult = std::move(cachedResult);
            e._scaleX = sx; e._scaleY = sy;
            e._offsetX = ox; e._offsetY = oy;
            e._rotation = rot; e._flipH = fh; e._flipV = fv;
            LOG_WARN("ImgEngine", "LoadFile 失败已回滚，保持当前显示");
        }
    } snap(*this);

    Unload();

    _currentPath = path;
    // 优化：Navigate 切换时 _dirFiles 已有该文件，跳过目录遍历省 5-20ms
    bool found = false;
    for (size_t i = 0; i < _dirFiles.size(); i++) {
        if (_dirFiles[i] == path) {
            _currentIndex = (int)i;
            found = true;
            break;
        }
    }
    if (!found) {
        LoadDirectoryFiles();
    }

    // 内存映射文件：大文件按需分页加载，避免全量读入内存
    _fileMapping = std::make_shared<FileMapping>(path);
    if (!_fileMapping->Data()) { LOG_WARN("ImgEngine", "无法映射文件"); return false; }

    const uint8_t* fileData = _fileMapping->Data();
    size_t fileSize = _fileMapping->Size();

    // 解码器匹配（直接用 MMF 指针读 header，无需拷贝）
    size_t headerLen = (std::min)(fileSize, (size_t)4096);
    _decoder = FindDecoder(fileData, headerLen);
    if (!_decoder) {
        std::wstring ext = path.substr(path.find_last_of(L'.'));
        std::string extA(ext.begin(), ext.end());
        _decoder = FindDecoderByExtension(extA);
    }
    if (!_decoder) { LOG_WARN("ImgEngine", "无匹配解码器"); return false; }

    // GIF 动画检测：多帧 GIF 走 AnimationPlayer，跳过普通单帧解码路径
    {
        std::wstring ext = path.substr(path.find_last_of(L'.'));
        std::string extA(ext.begin(), ext.end());
        if (_animPlayer.Open(fileData, fileSize, extA)) {
            auto* frame = _animPlayer.CurrentFrame();
            if (frame && _renderer) {
                _srcWidth  = frame->width;
                _srcHeight = frame->height;
                _displayedLevel = 0;
                _sourceBitmap = _renderer->CreateBitmap(
                    frame->width, frame->height,
                    frame->pixels.data(), frame->stride);
                _lastAnimFrame = 0;
                LOG_INFO_STREAM("ImgEngine") << "GIF动画: " << _srcWidth << "x" << _srcHeight
                    << " 帧数=" << _animPlayer.FrameCount();
                FitToWindow(_renderer->Width(), _renderer->Height());
                TriggerPreDecode(fwd, bwd);
                LogNavLatency(false);  // 性能遥测：GIF 单帧同步显示（占位+完整同时）
                LogNavLatency(true);
                snap.committed = true;  // GIF 加载成功，提交事务（禁止失败回滚）
                return true;
            }
            _animPlayer.Reset();  // 取帧失败，回退普通解码
        }
    }

    auto openResult = _decoder->Open(fileData, fileSize);
    if (!openResult) { LOG_WARN("ImgEngine", "Open 失败"); return false; }
    _openResult = std::move(*openResult);
    _openResult.state = _fileMapping;  // 解码器通过 state 持有 MMF 引用
    _imageInfo = _openResult.info;

    {
        auto name = _decoder->Name();
        std::wstring nameW(name, name + strlen(name));
        ActivityLog::Instance().Log(L"解码器",
            L"匹配: " + nameW +
            L" " + std::to_wstring(_imageInfo.width) + L"x" + std::to_wstring(_imageInfo.height) +
            L" " + ActivityFmt::SizeStr(fileSize));
    }

    // 创建金字塔（shared_ptr：TileWorker 持有副本，Unload 时无需等待）
    _pyramid = std::make_shared<Pyramid>(
        Pyramid::ForSize(_imageInfo.width, _imageInfo.height));

    // 创建瓦片缓存
    _tileCache = std::make_shared<TileCache>();

    // 瓦片模式：缩略图模糊占位 + 后台异步解顶层
    // 主线程不阻塞，HEIF/RAW 大图顶层解码在后台进行，期间用缩略图/旧图模糊铺满视口
    if (_decoder->SupportsTiling()) {
        int topLevel = _pyramid->TopLevel();
        _srcWidth  = _imageInfo.width;
        _srcHeight = _imageInfo.height;

        // 大文件/大像素走异步：PSD/PSB 大文件 DecodeLevel 分配数百 MB；
        // JPEG 高压缩比，16-20MB 文件可能 40M+ 像素，顶层 DCT 解码仍耗 266ms 冻结 UI
        // 小图走同步：<100ms，比异步轮询（30ms×N帧）更快且无占位延迟
        // forceAsync: _bgCacheMiss 场景强制异步，避免 OnTimer 同步阻塞
        constexpr size_t MAX_SYNC_DECODE_SIZE = 30 * 1024 * 1024;  // 30MB（PSD/PSB 等）
        constexpr size_t MAX_SYNC_PIXELS = 20 * 1000 * 1000;       // 20M 像素（JPEG 等）
        bool isLargeFile = forceAsync ||
            _fileMapping->Size() > MAX_SYNC_DECODE_SIZE ||
            (size_t)_imageInfo.width * _imageInfo.height > MAX_SYNC_PIXELS;

        if (isLargeFile) {
            // 大文件：立即用旧图/缩略图占位，防抖 200ms 后启动正式解码
            // 快速切换时只显示占位不做 DecodeLevel，停手后才解码，避免 CPU 浪费
            // 占位优先级：旧纹理（高清）> 旧占位（缩略图/上上张）> 同步提缩略图
            // oldBlur 兜底：快速连点时 _sourceBitmap 还是 null（phase 2 未完），
            //              此时 _blurBitmap 才是实际显示的占位，必须接住避免黑屏
            if (_renderer && oldBitmap) {
                _blurBitmap = oldBitmap;  // 旧纹理优先（最高清）
                ActivityLog::Instance().Log(L"占位", L"使用旧纹理占位");
            } else if (_renderer && oldBlur) {
                _blurBitmap = oldBlur;    // 旧占位次之：大文件连点时维持画面连续
                ActivityLog::Instance().Log(L"占位", L"使用旧占位图");
            } else if (_renderer) {
                // 首次加载无旧图：同步提取缩略图避免黑屏（首次不追求速度）
                PerfScope perfThumb(L"解码", _decoder->Name());  // 性能遥测：缩略图解码耗时
                auto thumb = _decoder->DecodeThumbnail(_openResult);
                perfThumb.SetExtra({
                    Perf::S("file", ActivityFmt::NarrowUtf8(ActivityFmt::ShortName(path))),
                    Perf::N("w", (double)_imageInfo.width),
                    Perf::N("h", (double)_imageInfo.height),
                    Perf::S("op_detail", "DecodeThumbnail")
                });
                if (thumb) {
                    _blurBitmap = _renderer->CreateBitmap(
                        thumb->width, thumb->height,
                        thumb->pixels.data(), thumb->stride);
                    ActivityLog::Instance().Log(L"缩略图",
                        L"占位: " + std::to_wstring(thumb->width) + L"x" + std::to_wstring(thumb->height));
                    LogNavLatency(false);  // 性能遥测：占位帧就绪（同步缩略图）
                }
            }
            LOG_INFO_STREAM("ImgEngine") << "大文件防抖解码: " << _fileMapping->Size()
                << " bytes topLevel=" << topLevel;
            _displayedLevel = topLevel;
            if (forceAsync) {
                // _bgCacheMiss 路径：防抖已在 Navigate 的 200ms 完成，直接异步解码
                // 缩略图就绪后立即更新 _blurBitmap，画面从旧图占位变为新图缩略图
                ActivityLog::Instance().Log(L"解码",
                    L"缓存未命中异步解码（跳过防抖）");
                StartBgTopLevelDecode(topLevel, false);
            } else {
                // 大文件路径：设防抖，200ms 后由 CheckInteractionTimeout 启动 StartBgTopLevelDecode
                // 快速切换时每次重置 deadline，停手后才真正解码
                ActivityLog::Instance().Log(L"解码",
                    L"异步模式（大文件/大像素），防抖 200ms");
                _bgDecodeScheduled = true;
                _bgDecodeLevel = topLevel;
                _bgDecodeSkipThumb = false;  // 大文件路径需要后台提取缩略图
                _bgDecodePendingPath = path;  // LoadFile 已打开文件，防抖处检测到路径一致会跳过重复打开
                _bgDecodeDeadline = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(200);
            }
        } else {
            // 小文件：同步解码（<100ms），立即显示，比异步轮询更快
            ActivityLog::Instance().Log(L"解码",
                L"同步顶层 Level " + std::to_wstring(topLevel));
            std::optional<DecodeResult> preview;
            {
                PerfScope perfLevel(L"解码", _decoder->Name());  // 性能遥测：顶层解码耗时
                preview = _decoder->DecodeLevel(_openResult, topLevel);
                perfLevel.SetExtra({
                    Perf::S("file", ActivityFmt::NarrowUtf8(ActivityFmt::ShortName(path))),
                    Perf::N("w", (double)_imageInfo.width),
                    Perf::N("h", (double)_imageInfo.height),
                    Perf::N("level", (double)topLevel),
                    Perf::S("op_detail", "DecodeLevel_sync")
                });
            }
            if (preview) {
                _sourceBitmap = _renderer->CreateBitmap(
                    preview->width, preview->height,
                    preview->pixels.data(), preview->stride);
                _topLevelBitmap = _sourceBitmap;
                _displayedLevel = topLevel;
                _blurBitmap.Reset();  // 同步解码成功，清除占位释放 GPU 内存
                ActivityLog::Instance().Log(L"解码",
                    L"顶层就绪: " + std::to_wstring(preview->width) + L"x" + std::to_wstring(preview->height));
                LogNavLatency(true);  // 性能遥测：完整帧就绪（同步顶层）
            } else {
                // 顶层解码失败，回退全量解码
                LOG_WARN("ImgEngine", "DecodeLevel 返回 nullopt，回退 DecodeFull");
                ActivityLog::Instance().Log(L"解码", L"顶层失败，回退全量解码");
                std::optional<DecodeResult> full;
                {
                    PerfScope perfFull(L"解码", _decoder->Name());  // 性能遥测：回退全量解码耗时
                    full = _decoder->DecodeFull(_openResult);
                    perfFull.SetExtra({
                        Perf::S("file", ActivityFmt::NarrowUtf8(ActivityFmt::ShortName(path))),
                        Perf::S("op_detail", "DecodeFull_fallback")
                    });
                }
                if (!full) { LOG_WARN("ImgEngine", "DecodeFull 失败"); return false; }
                _sourceBitmap = _renderer->CreateBitmap(
                    full->width, full->height,
                    full->pixels.data(), full->stride);
                _displayedLevel = 0;
                _blurBitmap.Reset();
                LogNavLatency(true);  // 性能遥测：完整帧就绪（回退全量）
            }
        }
    } else {
        // 不支持瓦片 → 直接解码全尺寸
        std::optional<DecodeResult> full;
        {
            PerfScope perfFull(L"解码", _decoder->Name());  // 性能遥测：全量解码耗时
            full = _decoder->DecodeFull(_openResult);
            perfFull.SetExtra({
                Perf::S("file", ActivityFmt::NarrowUtf8(ActivityFmt::ShortName(path))),
                Perf::N("w", (double)_imageInfo.width),
                Perf::N("h", (double)_imageInfo.height),
                Perf::S("op_detail", "DecodeFull_nontile")
            });
        }
        if (!full) { LOG_WARN("ImgEngine", "DecodeFull 失败"); return false; }

        _srcWidth  = full->width;
        _srcHeight = full->height;
        _displayedLevel = 0;

        _sourceBitmap = _renderer->CreateBitmap(
            _srcWidth, _srcHeight,
            full->pixels.data(), full->stride);
        if (!_sourceBitmap) {
            return false;
        }
        LOG_INFO_STREAM("ImgEngine") << "已解码: " << _srcWidth << "x" << _srcHeight;
        LogNavLatency(false);  // 性能遥测：非瓦片单层图（占位+完整同时）
        LogNavLatency(true);
    }

    // ICC 色彩管理：仅单层图启用
    // 瓦片模式（多层）切层会重建 _sourceBitmap，ICC effect 绑定的源位图失效，故跳过
    // 大图通常无 ICC profile，小图单层有 ICC 时正常生效
    if (!_imageInfo.iccProfile.empty() && _sourceBitmap && _pyramid->LevelCount() <= 1) {
        _colorManager.SetIccProfile(
            _imageInfo.iccProfile.data(), _imageInfo.iccProfile.size());
        _colorManager.SetSourceBitmap(_sourceBitmap.Get());
        LOG_INFO_STREAM("ImgEngine") << "ICC profile 已加载: "
            << _imageInfo.iccProfile.size() << " bytes";
    }

    // 适配窗口
    if (_renderer) {
        FitToWindow(_renderer->Width(), _renderer->Height());
    }

    // 启动瓦片解码（如果支持且 _sourceBitmap 已就绪）
    // 瓦片模式下 _sourceBitmap 等后台解出顶层后才就绪，由 ApplyBgResultIfReady 内部触发 UpdateViewport
    if (_decoder->SupportsTiling() && _pyramid->LevelCount() > 1 && _sourceBitmap) {
        UpdateViewport();
    }

    TriggerPreDecode(fwd, bwd);

    ActivityLog::Instance().Log(L"就绪",
        ActivityFmt::ShortName(path) + L" " +
        std::to_wstring(_srcWidth) + L"x" + std::to_wstring(_srcHeight));
    snap.committed = true;  // 加载成功，提交事务（禁止失败回滚）
    return true;
}

void ImageEngine::Unload() {
    PerfScope perfUnload(L"消息", "ImageEngine.Unload");  // 细分计时：Unload 耗时（GPU 资源释放嫌疑）
    // 1. 取消后台解码线程（不 join，detach 让其后台安全退出）
    // 线程持有 _bgState + decoder + openResult 的 shared_ptr 副本，可安全完成
    // cancel 标志通知线程丢弃结果并不进入 DecodeLevel 阶段
    if (_bgState) {
        _bgState->cancel.store(true, std::memory_order_release);
        _bgState->pending.store(false, std::memory_order_release);
    }
    if (_bgThread.joinable()) _bgThread.detach();
    _bgState = nullptr;  // 释放主线程引用，线程持有自己的副本保活
    _bgDecodeScheduled = false;  // 清除待启动的防抖解码任务
    _bgCacheMiss = false;        // 清除缓存未命中延迟加载标志
    _bgDecodePendingPath.clear();
    _preDecodePending = false;   // 清除待触发的预解码
    _needsViewportUpdate = false;  // 清除延迟 UpdateViewport

    // 2. 标记卸载中 + 清空瓦片队列 + 唤醒等待的 TileWorker
    // TileWorker 检测 _unloading 后跳过新任务；进行中的任务持有 shared_ptr 副本
    // 资源通过 shared_ptr 保活，Unload 无需等待 TileWorker 完成（不阻塞主线程）
    _unloading.store(true, std::memory_order_release);
    {
        std::lock_guard lock(_tileMutex);
        std::queue<TileTask>().swap(_tileQueue);
    }
    _tileCV.notify_all();

    // 3. 清空待更新瓦片队列：避免切换图片后旧瓦片贴入新纹理
    {
        std::lock_guard lock(_pendingMutex);
        _pendingTiles.clear();
    }

    // 4. 释放资源（shared_ptr reset 不阻塞：TileWorker 持有副本保持资源存活）
    _animPlayer.Reset();
    _animPlayer.SetPlaying(false);
    _lastAnimFrame = -1;
    _colorManager.Reset();
    _sourceBitmap.Reset();
    _topLevelBitmap.Reset();
    _blurBitmap.Reset();
    _fileMapping.reset();
    _tileCache.reset();
    _pyramid.reset();
    _decoder.reset();
    _currentPath.clear();
    _currentIndex = -1;
    _srcWidth = _srcHeight = 0;
    _displayedLevel = -1;
    _cachedLevel.store(-1);  // 清空本层解码缓存标志
    // try_lock 清空缓存数据：若 TileWorker 正在解码（持锁），跳过清空
    // _cachedLevel=-1 已使缓存失效，TileWorker 会重新解码覆盖
    {
        std::unique_lock ulock(_cachedLevelMutex, std::try_to_lock);
        if (ulock.owns_lock()) {
            _cachedLevelResult = {};
            _cachedLevelPath.clear();  // 一并清文件标识，跨图必不匹配
        }
    }
    _scaleX = _scaleY = 1.0;
    _offsetX = _offsetY = 0.0;
    _rotation = 0;  // 重置视图变换
    _flipH = _flipV = false;
    _interacting = false;        // 重置交互节流状态
    _pendingTargetLevel = -1;    // 重置渐进过渡状态
    _fwdFrontier = -1;           // 重置滑动窗口前沿
    _bwdFrontier = -1;
    _lastNavIndex = -1;
    _lastBrowseTick = std::chrono::steady_clock::now();  // 初始化浏览时间戳

    // 5. 解除卸载标志，允许后续 TileWorker 处理新图片任务
    _unloading.store(false, std::memory_order_release);
}

void ImageEngine::Navigate(int steps) {
    LOG_INFO_STREAM("ImgEngine") << "Navigate: steps=" << steps
        << " files=" << _dirFiles.size() << " idx=" << _currentIndex;
    if (_dirFiles.empty()) return;
    // Clamp 边界：快速连按合并后 steps 可能超出范围，钳制到首尾
    int newIdx = _currentIndex + steps;
    if (newIdx < 0) newIdx = 0;
    if (newIdx >= (int)_dirFiles.size()) newIdx = (int)_dirFiles.size() - 1;
    if (newIdx == _currentIndex) return;

    // 性能遥测：翻页起点（占位/完整两阶段后续在结果应用处上报）
    _navStart = std::chrono::steady_clock::now();
    _navTargetIndex = newIdx;
    _navPlaceholderLogged = false;
    _navFullLogged = false;
    if (_renderer) _renderer->NotifyInteraction();  // V-Sync 切换：翻页期间用 Present(0)

    // 翻页速度检测：间隔<300ms 累加 _navSpeedBurst，≥2 视为快速连按
    // burst 时预解码 ±3 下限 + 防抖降为 0 立即触发，降低快速翻页未命中率
    auto navNow = std::chrono::steady_clock::now();
    int intervalMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
        navNow - _lastNavStart).count();
    _lastNavStart = navNow;
    _navSpeedBurst = (intervalMs < 300) ? _navSpeedBurst + 1 : 0;
    bool burst = (_navSpeedBurst >= 2);

    ActivityLog::Instance().Log(L"切换",
        (steps > 0 ? L"下一张: " : L"上一张: ") + ActivityFmt::ShortName(_dirFiles[newIdx]) +
        (std::abs(steps) > 1 ? L" (×" + std::to_wstring(std::abs(steps)) + L")" : L""));

    // ── 滑动窗口批次预解码 ──
    // 每推进 5 张触发再预解码 10 张：frontier=10，当前推进到 5 时再解到 20
    // 方向切换（_lastNavIndex != _currentIndex）时重置前沿，开启新批次
    int fwd, bwd;
    int total = (int)_dirFiles.size();
    if (steps > 0) {
        // 下一张：向后预解码
        if (_fwdFrontier < 0 || _lastNavIndex != _currentIndex) {
            _fwdFrontier = (std::min)(newIdx + 10, total - 1);  // 首次/方向切换：新批次 10 张
        } else if (newIdx + 5 >= _fwdFrontier && _fwdFrontier < total - 1) {
            _fwdFrontier = (std::min)(_fwdFrontier + 10, total - 1);  // 距前沿≤5：再推进 10 张
        }
        fwd = (std::max)(0, _fwdFrontier - newIdx);
        bwd = 0;
        _bwdFrontier = -1;  // 切方向重置反向前沿
    } else {
        // 上一张：向前预解码（对称逻辑）
        if (_bwdFrontier < 0 || _lastNavIndex != _currentIndex) {
            _bwdFrontier = (std::max)(newIdx - 10, 0);
        } else if (newIdx - 5 <= _bwdFrontier && _bwdFrontier > 0) {
            _bwdFrontier = (std::max)(_bwdFrontier - 10, 0);
        }
        fwd = 0;
        bwd = (std::max)(0, newIdx - _bwdFrontier);
        _fwdFrontier = -1;
    }
    // 快速连按：预解码窗口扩到 ±3 下限，让 worker 尽早把邻近图入队
    if (burst) {
        fwd = (std::max)(fwd, 3);
        bwd = (std::max)(bwd, 3);
    }
    _lastNavIndex = newIdx;
    _lastBrowseTick = std::chrono::steady_clock::now();  // 浏览操作：重置 3 分钟空闲计时

    // 缓存命中：直接从预解码数据创建 GPU 纹理，零等待切换
    if (LoadFromCache(_dirFiles[newIdx], newIdx)) {
        // 预解码延迟到停手后触发：避免每次切换都 SetCurrentIndex（mutex锁+文件列表复制+worker重启）
        // burst 时防抖降为 0：下一帧 OnTimer 立即触发，让 worker 尽早跟上快速翻页
        _preDecodePending = true;
        _preDecodeFwd = fwd;
        _preDecodeBwd = bwd;
        _preDecodeDeadline = burst ? navNow : navNow + std::chrono::milliseconds(30);
        return;
    }
    // 缓存未命中：设占位 + 200ms 防抖，延迟到 CheckInteractionTimeout 执行 LoadFile
    // 避免在 OnPaint 里同步执行 LoadFile 阻塞 UI（50~500ms）
    // 快速连点时被防抖合并，停手后只执行一次 LoadFile
    {
        ComPtr<ID2D1Bitmap1> oldBitmap = _sourceBitmap;
        ComPtr<ID2D1Bitmap1> oldBlur   = _blurBitmap;
        // 保存旧图几何：Unload 会清零，占位图需按旧图位置/缩放显示维持视觉连续
        int oldSrcW = _srcWidth, oldSrcH = _srcHeight;
        double oldScaleX = _scaleX, oldScaleY = _scaleY;
        double oldOffsetX = _offsetX, oldOffsetY = _offsetY;
        Unload();
        _currentPath = _dirFiles[newIdx];
        _currentIndex = newIdx;
        _srcWidth = oldSrcW;  _srcHeight = oldSrcH;
        _scaleX = oldScaleX;  _scaleY = oldScaleY;
        _offsetX = oldOffsetX; _offsetY = oldOffsetY;
        // 占位：旧纹理优先（高清），旧占位次之（缩略图/上上张）
        if (_renderer && oldBitmap) _blurBitmap = oldBitmap;
        else if (oldBlur) _blurBitmap = oldBlur;
        // 延迟加载：200ms 后由 CheckInteractionTimeout 启动全异步加载（GIF 例外走同步）
        _bgDecodeScheduled = true;
        _bgCacheMiss = true;
        _bgFwd = fwd;
        _bgBwd = bwd;
        _bgDecodePendingPath = _dirFiles[newIdx];
        _bgDecodeDeadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(200);
        // 预解码同步触发：当前图后台加载期间，worker 并行预解码邻近图
        // burst 时立即触发（navNow），否则 30ms 防抖，与命中分支一致
        _preDecodePending = true;
        _preDecodeFwd = fwd;
        _preDecodeBwd = bwd;
        _preDecodeDeadline = burst ? navNow : navNow + std::chrono::milliseconds(30);
        ActivityLog::Instance().Log(L"切换", L"缓存未命中，延迟加载（防抖 200ms）");
    }
}

void ImageEngine::NavigateTo(int newIndex) {
    // 跳帧调用：OnPaint 取最新 _targetIndex 直接跳转，复用 Navigate 全部逻辑
    if (_dirFiles.empty()) return;
    if (newIndex < 0) newIndex = 0;
    if (newIndex >= (int)_dirFiles.size()) newIndex = (int)_dirFiles.size() - 1;
    if (newIndex == _currentIndex) return;
    Navigate(newIndex - _currentIndex);
}

void ImageEngine::LoadDirectoryFiles() {
    _decodedCache.clear();      // 目录变化使索引失效，清空已解码缓存
    _decodedCacheBytes = 0;
    _dirFiles.clear();
    _currentIndex = -1;

    if (_currentPath.empty()) return;
    fs::path dir = fs::path(_currentPath).parent_path();
    _currentDir = dir.wstring();

    // 支持的扩展名
    std::vector<std::wstring> exts = {
        L".jpg", L".jpeg", L".jpe", L".jfif", L".png", L".webp", L".bmp", L".dib",
        L".gif", L".tif", L".tiff",
        L".psd", L".psb",
        L".hdr", L".pic",
        L".heic", L".heif", L".hif",
        L".cr2", L".cr3", L".nef", L".arw", L".dng", L".raf",
        L".x3f", L".pef", L".rw2", L".orf",
        L".svg", L".svgz",
        L".ico", L".cur", L".ani", L".tga", L".dds"
    };

    std::wstring targetName = fs::path(_currentPath).filename().wstring();
    int idx = 0;
    for (auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        if (std::find(exts.begin(), exts.end(), ext) != exts.end()) {
            _dirFiles.push_back(entry.path().wstring());
            if (entry.path().filename().wstring() == targetName) {
                _currentIndex = idx;
            }
            idx++;
        }
    }
}

// 跨文件夹导航：枚举父目录的子文件夹（按名称排序），取相邻兄弟
// dir=+1 下一文件夹（取其第一张图），-1 上一文件夹（取其最后一张图）
bool ImageEngine::NavigateToSiblingFolder(int dir) {
    if (_currentPath.empty()) return false;
    fs::path curDir = fs::path(_currentPath).parent_path();
    fs::path parent = curDir.parent_path();
    if (parent.empty()) return false;

    // 枚举父目录下所有子文件夹（按名称排序）
    std::vector<fs::path> subDirs;
    try {
        for (auto& entry : fs::directory_iterator(parent)) {
            if (entry.is_directory()) subDirs.push_back(entry.path());
        }
    } catch (...) { return false; }
    std::sort(subDirs.begin(), subDirs.end());

    // 找当前目录位置
    auto it = std::find(subDirs.begin(), subDirs.end(), curDir);
    if (it == subDirs.end()) return false;
    int pos = (int)(it - subDirs.begin());

    // 取相邻兄弟（循环：到头则回到另一端）
    int target = pos + dir;
    if (target < 0) target = (int)subDirs.size() - 1;
    if (target >= (int)subDirs.size()) target = 0;
    if (target == pos) return false;  // 仅一个子目录

    fs::path targetDir = subDirs[target];

    // 枚举目标目录下支持的图片文件
    std::vector<std::wstring> exts = {
        L".jpg", L".jpeg", L".jpe", L".jfif", L".png", L".webp", L".bmp", L".dib",
        L".gif", L".tif", L".tiff", L".psd", L".psb", L".hdr", L".pic",
        L".heic", L".heif", L".hif", L".cr2", L".cr3", L".nef", L".arw", L".dng",
        L".raf", L".x3f", L".pef", L".rw2", L".orf", L".svg", L".svgz",
        L".ico", L".cur", L".ani", L".tga", L".dds"
    };
    std::vector<std::wstring> targetFiles;
    try {
        for (auto& entry : fs::directory_iterator(targetDir)) {
            if (!entry.is_regular_file()) continue;
            auto ext = entry.path().extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (std::find(exts.begin(), exts.end(), ext) != exts.end()) {
                targetFiles.push_back(entry.path().wstring());
            }
        }
    } catch (...) { return false; }

    if (targetFiles.empty()) return false;  // 目标文件夹无图片
    std::sort(targetFiles.begin(), targetFiles.end());

    // dir=+1 取第一张，dir=-1 取最后一张
    const std::wstring& targetFile = (dir > 0) ? targetFiles.front() : targetFiles.back();
    ActivityLog::Instance().Log(L"导航",
        (dir > 0 ? L"进入下一文件夹: " : L"进入上一文件夹: ") +
        targetDir.filename().wstring());
    return LoadFile(targetFile);
}

// 切层占位：用当前层已解码高清纹理拉伸到目标层尺寸（含真实瓦片像素，比顶层预览清晰得多）
// 仅做纹理重建，不排瓦片——UpdateViewport 和 CheckInteractionTimeout 渐进过渡负责排队
void ImageEngine::SwitchToLevel(int level) {
    if (!_pyramid || !_renderer) return;
    int levelW = _pyramid->WidthAt(level);
    int levelH = _pyramid->HeightAt(level);
    if (levelW <= 0 || levelH <= 0) return;

    ActivityLog::Instance().Log(L"切层",
        L"Level " + std::to_wstring(level) + L" (" +
        std::to_wstring(levelW) + L"x" + std::to_wstring(levelH) + L")");

    // 优先用当前层纹理（含真实瓦片像素）拉伸作占位，首次切层或丢失时兜底用顶层预览
    // 当前层有真实像素，拉伸到目标层仅放大/缩小，视觉落差远小于顶层预览拉伸
    ID2D1Bitmap1* srcBitmap = _sourceBitmap.Get();
    if (!srcBitmap) srcBitmap = _topLevelBitmap.Get();

    if (srcBitmap) {
        _sourceBitmap = _renderer->CreateStretchedBitmap(srcBitmap, levelW, levelH);
    }
    if (!_sourceBitmap) {
        // 拉伸失败兜底：空位图（黑屏但不崩溃）
        _sourceBitmap = _renderer->CreateBitmap(levelW, levelH, nullptr, 0);
    }
    if (_sourceBitmap) {
        _displayedLevel = level;
        // 不清空缓存：不同层瓦片 TileIndex 不同可共存，512MB LRU 自动淘汰
    }
}

void ImageEngine::UpdateViewport() {
    if (!_pyramid || !_decoder || !_decoder->SupportsTiling()) return;
    if (!_renderer || !_sourceBitmap) return;
    if (_pyramid->LevelCount() <= 1) return;  // 单层无需切换

    int level = _pyramid->SelectLevel(_scaleX);
    if (level < 0) level = 0;
    if (level >= _pyramid->LevelCount()) level = _pyramid->TopLevel();

    // 层级切换：GPU 拉伸顶层预览作模糊占位，瓦片到达后由 UpdateBitmapRegion 逐块替换为高清像素
    if (level != _displayedLevel) {
        SwitchToLevel(level);
    }

    QueueTiles(level);
}

// 排队视口可见瓦片：按距视口中心的曼哈顿距离排序，注视点先解码
// 中心瓦片排队首，多线程先取先解，用户最先看到注视区域变清晰
void ImageEngine::QueueTiles(int level) {
    int canvasW = _renderer->Width();
    int canvasH = _renderer->Height();
    // 视口可见区域 → 原图坐标：旋转时屏幕↔原图坐标轴映射改变
    // 由 DrawBitmapRotated 矩阵反推：90/270 屏幕宽↔原图高、屏幕高↔原图宽，且起点随象限翻转
    // 修复旋转后拖动平移时瓦片解码区域错位（高清区不跟随）
    double s = _scaleX;  // 统一缩放（_scaleX==_scaleY）
    double visX, visY, visW, visH;
    switch (_rotation) {
        case 90:   // 逆时针90°：屏幕Y→原图X(翻转)，屏幕X→原图Y
            visX = _srcWidth + _offsetY / s - canvasH / s; visW = canvasH / s;
            visY = -_offsetX / s;                     visH = canvasW / s;
            break;
        case 180:  // 逆时针180°：屏幕X→原图X(翻转)，屏幕Y→原图Y(翻转)
            visX = _srcWidth + _offsetX / s - canvasW / s; visW = canvasW / s;
            visY = _srcHeight + _offsetY / s - canvasH / s; visH = canvasH / s;
            break;
        case 270:  // 逆时针270°：屏幕Y→原图X，屏幕X→原图Y(翻转)
            visX = -_offsetY / s;                     visW = canvasH / s;
            visY = _srcHeight + _offsetX / s - canvasW / s; visH = canvasW / s;
            break;
        default:   // 0°：屏幕X→原图X，屏幕Y→原图Y
            visX = -_offsetX / s; visW = canvasW / s;
            visY = -_offsetY / s; visH = canvasH / s;
            break;
    }

    int levelScale = 1 << level;
    int tileX0 = (std::max)(0, (int)(visX / levelScale / TILE_SIZE));
    int tileY0 = (std::max)(0, (int)(visY / levelScale / TILE_SIZE));
    int tileX1 = (std::min)(_pyramid->TilesXAt(level),
                            (int)((visX + visW) / levelScale / TILE_SIZE) + 1);
    int tileY1 = (std::min)(_pyramid->TilesYAt(level),
                            (int)((visY + visH) / levelScale / TILE_SIZE) + 1);

    {
        std::lock_guard lock(_tileMutex);
        // 清空旧请求，只保留新视口范围内的瓦片
        std::queue<TileTask>().swap(_tileQueue);
        // 按距视口中心的曼哈顿距离排序，注视点先解码，用户最先看到注视区域变清晰
        int centerTX = (tileX0 + tileX1) / 2;
        int centerTY = (tileY0 + tileY1) / 2;
        std::vector<TileTask> tasks;
        tasks.reserve((size_t)(tileX1 - tileX0) * (tileY1 - tileY0));
        for (int ty = tileY0; ty < tileY1; ty++) {
            for (int tx = tileX0; tx < tileX1; tx++) {
                tasks.push_back({ level, tx, ty });
            }
        }
        std::sort(tasks.begin(), tasks.end(),
            [&](const TileTask& a, const TileTask& b) {
                int da = std::abs(a.col - centerTX) + std::abs(a.row - centerTY);
                int db = std::abs(b.col - centerTX) + std::abs(b.row - centerTY);
                return da < db;
            });
        for (auto& t : tasks) _tileQueue.push(std::move(t));
        // 推入任务标记忙，预解码让步
        if (!_tileQueue.empty()) _tileBusy.store(true, std::memory_order_release);
        // 重置瓦片进度计数器（活动日志进度显示用）
        _tileTotal.store((int)tasks.size(), std::memory_order_relaxed);
        _tileDone.store(0, std::memory_order_relaxed);
    }
    _tileCV.notify_all();  // 唤醒所有瓦片线程并行处理

    int total = _tileTotal.load(std::memory_order_relaxed);
    ActivityLog::Instance().Log(L"瓦片",
        L"排队 " + std::to_wstring(total) +
        L" 块 (Level " + std::to_wstring(level) + L")");
    // 性能遥测：瓦片队列深度
    ActivityLog::Instance().LogTimed(L"瓦片", "queue_tiles", 0, {
        Perf::N("level", (double)level),
        Perf::N("count", (double)total)
    });
}

void ImageEngine::EncodeTiles(int level) {
    // 实际解码在 TileWorker 中完成
}

void ImageEngine::RequestViewport(const Viewport& vp) {
    // 供外部调度使用
}

void ImageEngine::TileWorker() {
    ActivityLog::SetThreadName("TileWorker");  // 性能遥测：线程角色名
    // WIC 解码器需要 COM 初始化（工作线程不继承主线程 COM apartment）
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (_tileRunning) {
        TileTask task;
        {
            std::unique_lock lock(_tileMutex);
            // _unloading 也唤醒等待：Unload 清队列后需让 Worker 重新检测条件
            _tileCV.wait(lock, [this]() {
                return !_tileQueue.empty() || !_tileRunning
                    || _unloading.load(std::memory_order_acquire);
            });
            if (!_tileRunning) break;
            // 卸载中或队列空：跳过，回循环重新等待
            if (_unloading.load(std::memory_order_acquire)) continue;
            if (_tileQueue.empty()) continue;

            task = _tileQueue.front();
            _tileQueue.pop();
        }
        // 取任务后再次检测：Unloading 可能在 pop 之后、guard 之前开始
        if (_unloading.load(std::memory_order_acquire)) continue;

        // RAII：取到任务标记忙（预解码让步），作用域结束自动清除
        TileBusyGuard guard(_activeTileWorkers, _tileBusy);

        // 卸载中或资源已释放：不访问 _decoder/_pyramid，直接跳过
        if (_unloading.load(std::memory_order_acquire)) continue;

        // 捕获 shared_ptr 副本：Unload 可能在此之后 reset 成员变量
        // 副本保持资源存活，本任务可安全完成解码
        auto decoder = _decoder;
        auto pyramid = _pyramid;
        auto tileCache = _tileCache;
        auto openResult = _openResult;  // 值拷贝：state(shared_ptr) 共享，info 复制
        if (!decoder || !pyramid) continue;

        TileIndex idx = { task.level, task.col, task.row };

        // 缓存命中：瓦片像素已有，推入 pending 让主线程贴纹理（省去重新解码）
        // 来回切层时旧层瓦片仍在缓存，命中即秒出
        if (tileCache) {
            auto cached = tileCache->Get(idx);
            if (cached) {
                int levelW = pyramid->WidthAt(task.level);
                int levelH = pyramid->HeightAt(task.level);
                if (levelW > 0 && levelH > 0) {
                    int tileX = task.col * TILE_SIZE;
                    int tileY = task.row * TILE_SIZE;
                    int tileW = (std::min)(TILE_SIZE, levelW - tileX);
                    int tileH = (std::min)(TILE_SIZE, levelH - tileY);
                    {
                        std::lock_guard lock(_pendingMutex);
                        _pendingTiles.push_back({
                            task.level, task.col, task.row,
                            tileW, tileH, tileW * 4,
                            *cached, true  // fromCache=true，RenderFrame 不重复 Put
                        });
                    }
                    _tileUpdated.store(true, std::memory_order_release);
                    int done = _tileDone.fetch_add(1, std::memory_order_relaxed) + 1;
                    int total = _tileTotal.load(std::memory_order_relaxed);
                    ActivityLog::Instance().Log(L"瓦片",
                        L"缓存命中 [" + std::to_wstring(task.col) + L"," + std::to_wstring(task.row) +
                        L"] (" + std::to_wstring(done) + L"/" + std::to_wstring(total) + L")");
                }
                continue;
            }
        }

        // 未命中：解码瓦片
        int levelW = pyramid->WidthAt(task.level);
        int levelH = pyramid->HeightAt(task.level);
        const size_t MAX_LEVEL_CACHE = 4096 * 4096;  // 64M 像素上限，大层不缓存避免爆内存

        std::vector<uint8_t> tilePixels;
        int tileW = 0, tileH = 0;

        if (levelW > 0 && levelH > 0 && (size_t)levelW * levelH <= MAX_LEVEL_CACHE) {
            // 中小层：本层缓存，同层多瓦片共享一次 DecodeLevel
            // 多线程 double-checked locking：首个线程 DecodeLevel 独占写，后续线程 shared 并行 SubRegion
            if (_cachedLevel.load(std::memory_order_relaxed) != task.level
                || _cachedLevelPath != _currentPath) {
                std::unique_lock ulock(_cachedLevelMutex);
                if (_cachedLevel.load(std::memory_order_relaxed) != task.level
                    || _cachedLevelPath != _currentPath) {
                    // 用本地副本解码：Unload 已 reset 成员变量也不影响本次调用
                    PerfScope perfLevel(L"解码", decoder->Name());  // 性能遥测：整层解码耗时
                    auto full = decoder->DecodeLevel(openResult, task.level);
                    perfLevel.SetExtra({
                        Perf::S("file", ActivityFmt::NarrowUtf8(ActivityFmt::ShortName(_currentPath))),
                        Perf::N("level", (double)task.level),
                        Perf::S("op_detail", "DecodeLevel_tile")
                    });
                    if (!full) continue;
                    _cachedLevelResult = std::move(*full);
                    _cachedLevel.store(task.level, std::memory_order_relaxed);
                    _cachedLevelPath = _currentPath;  // 绑定文件标识，跨图必不匹配强制重解
                }
            }
            int tileX = task.col * TILE_SIZE;
            int tileY = task.row * TILE_SIZE;
            tileW = (std::min)(TILE_SIZE, levelW - tileX);
            tileH = (std::min)(TILE_SIZE, levelH - tileY);
            // shared_lock：多线程并行 SubRegion 裁剪（memcpy 256KB，极快）
            std::shared_lock slock(_cachedLevelMutex);
            // 双重检查：Unload 可能已置 _cachedLevel=-1 并清空 _cachedLevelResult
            // （外层的 != 检查与 shared_lock 之间存在窗口，必须持锁再验一次）
            if (_cachedLevel.load(std::memory_order_relaxed) != task.level
                || _cachedLevelPath != _currentPath) continue;
            auto tile = _cachedLevelResult.SubRegion(tileX, tileY, tileW, tileH);
            tilePixels = std::move(tile.pixels);
            if (tilePixels.empty()) continue;  // SubRegion 边界检查失败（防御性兜底）
        } else {
            // 大层：不缓存整层，直接 DecodeTile 区域解码（WIC MCU 级跳过，每块只解 512×512）
            PerfScope perfTile(L"解码", decoder->Name());  // 性能遥测：单瓦片解码耗时
            auto tile = decoder->DecodeTile(openResult, task.level, task.col, task.row);
            perfTile.SetExtra({
                Perf::S("file", ActivityFmt::NarrowUtf8(ActivityFmt::ShortName(_currentPath))),
                Perf::N("level", (double)task.level),
                Perf::N("col", (double)task.col),
                Perf::N("row", (double)task.row),
                Perf::S("op_detail", "DecodeTile")
            });
            if (!tile) continue;
            tileW = tile->width;
            tileH = tile->height;
            tilePixels = std::move(tile->pixels);
        }

        // 存入待更新队列：主线程 RenderFrame 处理 GPU 纹理更新和缓存写入
        {
            std::lock_guard lock(_pendingMutex);
            _pendingTiles.push_back({
                task.level, task.col, task.row,
                tileW, tileH, tileW * 4,
                std::move(tilePixels), false  // fromCache=false，RenderFrame Put 存缓存
            });
        }
        _tileUpdated.store(true, std::memory_order_release);
        int done = _tileDone.fetch_add(1, std::memory_order_relaxed) + 1;
        int total = _tileTotal.load(std::memory_order_relaxed);
        ActivityLog::Instance().Log(L"瓦片",
            L"解码完成 [" + std::to_wstring(task.col) + L"," + std::to_wstring(task.row) +
            L"] " + std::to_wstring(tileW) + L"x" + std::to_wstring(tileH) +
            L" (" + std::to_wstring(done) + L"/" + std::to_wstring(total) + L")");
    }
    CoUninitialize();
}

// ─── 预解码缓存 ───

bool ImageEngine::LoadFromCache(const std::wstring& path, int newIndex) {
    // 保存当前图片的已解码纹理到缓存（Unload 前调用）
    StoreDecodedCache();

    // 优先检查已解码缓存：命中则直接复用 GPU 纹理，跳过模糊预览和顶层解码
    if (LoadFromDecodedCache(path, newIndex)) return true;

    if (!_preDecodeCache) return false;
    auto cached = _preDecodeCache->Get(newIndex);
    if (!cached || !cached->pixels) return false;

    LOG_INFO_STREAM("ImgEngine") << "缓存命中: idx=" << newIndex
        << " 预览 " << cached->width << "x" << cached->height
        << " (orig " << cached->origWidth << "x" << cached->origHeight
        << " level " << cached->topLevel << "/" << cached->levelCount << ")";

    ActivityLog::Instance().Log(L"缓存",
        L"命中: " + ActivityFmt::ShortName(path) + L" " +
        std::to_wstring(cached->width) + L"x" + std::to_wstring(cached->height));

    // 保存旧 _blurBitmap 供复用（避免每次新建 GPU 对象，8ms→0.5ms）
    ComPtr<ID2D1Bitmap1> oldBlur = _blurBitmap;

    {
        PerfScope perfSetup(L"翻页", "nav.cache_setup");  // 细分计时：旧图缓存保存+资源释放
        StoreDecodedCache();
        Unload();  // 清理上一张的 GPU 资源、解码器、动画状态（_blurBitmap.Reset 只减引用，oldBlur 保活）
    }
    _currentPath = path;
    _currentIndex = newIndex;
    // _dirFiles 保持不变（由上次 LoadFile 填充，索引仍有效）

    // 复用 _blurBitmap：缩略图尺寸一致时只更新像素，避免新建 GPU 对象（8ms→0.5ms）
    D2D1_SIZE_F oldSize = oldBlur ? oldBlur->GetSize() : D2D1::SizeF(0, 0);
    {
        PerfScope perfBmp(L"翻页", "nav.cache_bitmap");  // 细分计时：GPU 纹理上传嫌疑
        if (oldBlur && (int)oldSize.width == cached->width
                     && (int)oldSize.height == cached->height) {
            _renderer->UpdateBitmapRegion(oldBlur.Get(), 0, 0,
                cached->width, cached->height,
                cached->pixels->data(), cached->stride);
            _blurBitmap = oldBlur;  // 复用
        } else {
            _blurBitmap = _renderer->CreateBitmap(
                cached->width, cached->height,
                cached->pixels->data(), cached->stride);
        }
    }
    if (!_blurBitmap) return false;

    LogNavLatency(false);  // 性能遥测：占位帧就绪（缩略图/缓存像素）

    // 路径 A：支持瓦片 → 缓存像素用作占位，延迟文件打开到防抖后
    if (cached->supportsTiling && cached->levelCount > 1) {
        _pyramid = std::make_shared<Pyramid>(
            Pyramid::ForSize(cached->origWidth, cached->origHeight));
        _tileCache = std::make_shared<TileCache>();

        _srcWidth = cached->origWidth;
        _srcHeight = cached->origHeight;
        _displayedLevel = cached->topLevel;

        {
            PerfScope perfFit(L"翻页", "nav.cache_fit");  // 细分计时：适窗计算
            if (_renderer) FitToWindow(_renderer->Width(), _renderer->Height());
        }

        // 判断缓存像素是否为顶层尺寸（DecodeLevel 结果而非缩略图）
        // 顶层像素可直接显示，跳过 200ms 后的 StartBgTopLevelDecode
        // CheckInteractionTimeout 检测 _sourceBitmap 就绪 → 走 UpdateViewport 排瓦片
        int topLevelW = _pyramid->WidthAt(cached->topLevel);
        int topLevelH = _pyramid->HeightAt(cached->topLevel);
        if (cached->width == topLevelW && cached->height == topLevelH) {
            // 缓存有顶层像素：直接用作 _sourceBitmap，用户立即看到清晰顶层
            _sourceBitmap = _blurBitmap;
            _topLevelBitmap = _sourceBitmap;
            _blurBitmap.Reset();
            ActivityLog::Instance().Log(L"缓存",
                L"顶层像素直接显示，跳过后台解码");
            LogNavLatency(true);  // 性能遥测：完整帧就绪（顶层即完整）
        }
        // 缩略图（尺寸 < 顶层）：保留为 _blurBitmap 模糊占位，200ms 后 DecodeLevel

        // 防抖 200ms 后打开文件：_sourceBitmap 就绪则排瓦片，否则启动后台解码
        _bgDecodeScheduled = true;
        _bgDecodeLevel = cached->topLevel;
        _bgDecodeSkipThumb = true;
        _bgDecodePendingPath = path;
        _bgDecodeDeadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(200);
        return true;
    }

    // 路径 B：不支持瓦片 或 单层图 → 缓存像素即完整图，直接用作 _sourceBitmap
    _sourceBitmap = _blurBitmap;
    _blurBitmap.Reset();
    _srcWidth = cached->origWidth;
    _srcHeight = cached->origHeight;
    _displayedLevel = 0;
    if (_renderer) FitToWindow(_renderer->Width(), _renderer->Height());
    LogNavLatency(true);  // 性能遥测：完整帧就绪（缓存直通）
    return true;
}

// ─── 已解码图片缓存：GPU 纹理复用 ───

// 保存当前已解码纹理到缓存（Unload 前调用）
// 优先缓存 _sourceBitmap（含瓦片的高清纹理），切回时直接显示无需重解瓦片
// _sourceBitmap 尺寸过大（缩放到低层级）时回退缓存 _topLevelBitmap（顶层低清）
void ImageEngine::StoreDecodedCache() {
    if (_currentIndex < 0) return;
    if (_animPlayer.IsPlaying()) return;  // GIF 动画不缓存（恢复后无法继续动画）

    // 选择缓存目标：优先高清 _sourceBitmap，尺寸超 4M 像素则回退顶层
    ComPtr<ID2D1Bitmap1> bitmapToCache;
    int   cacheLevel = 0;
    bool  hasFullQuality = false;

    if (_sourceBitmap) {
        D2D1_SIZE_F srcSize = _sourceBitmap->GetSize();
        size_t srcPixels = (size_t)srcSize.width * (size_t)srcSize.height;
        // 高清判定：像素数合适 且 纹理分辨率满足窗口显示需求
        // （仅像素数会误标：高速扫过时 _sourceBitmap 是 level5 缩略图 241x161，
        //   像素数小但分辨率不足，切回时被当"高清缓存"跳过瓦片补全 → 一直模糊）
        if (srcPixels <= MAX_FULLQUALITY_PIXELS
            && srcSize.width >= _renderer->Width()
            && srcSize.height >= _renderer->Height()) {
            // 适窗层级（如 Level 2）的高清纹理，含已解码瓦片
            bitmapToCache   = _sourceBitmap;
            cacheLevel      = _displayedLevel;
            hasFullQuality  = true;
        }
    }
    if (!bitmapToCache && _topLevelBitmap) {
        // _sourceBitmap 太大（缩放到 Level 0/1）或不存在，缓存顶层低清
        bitmapToCache   = _topLevelBitmap;
        cacheLevel      = _pyramid ? _pyramid->TopLevel() : 0;
        hasFullQuality  = false;
    }
    if (!bitmapToCache) return;  // 非瓦片大图且无顶层，跳过

    D2D1_SIZE_F size = bitmapToCache->GetSize();
    if (size.width <= 0 || size.height <= 0) return;

    size_t bytes = (size_t)size.width * (size_t)size.height * 4;
    // 替换旧条目时先减去旧字节数
    auto it = _decodedCache.find(_currentIndex);
    if (it != _decodedCache.end()) _decodedCacheBytes -= it->second.bytes;

    auto& e = _decodedCache[_currentIndex];
    e.bitmap       = bitmapToCache;            // ComPtr 引用 +1，Unload 不释放
    e.origWidth    = _srcWidth;
    e.origHeight   = _srcHeight;
    e.level        = cacheLevel;
    e.levelCount   = _pyramid ? _pyramid->LevelCount() : 1;
    // 基于 _pyramid 而非 _decoder：缓存命中路径 _decoder 为空（延迟打开），
    // 但 _pyramid 已创建，应记 supportsTiling=true，否则下次命中不创建 _pyramid（恶性循环）
    e.supportsTiling = _pyramid != nullptr;
    e.hasFullQuality = hasFullQuality;
    e.lastAccess   = std::chrono::steady_clock::now();
    e.bytes        = bytes;
    _decodedCacheBytes += bytes;

    ActivityLog::Instance().Log(L"缓存",
        L"存入已解码" + std::wstring(hasFullQuality ? L"(高清)" : L"(顶层)") +
        L" idx=" + std::to_wstring(_currentIndex) +
        L" " + std::to_wstring((int)size.width) + L"x" + std::to_wstring((int)size.height) +
        L" (共 " + std::to_wstring(_decodedCacheBytes / 1024 / 1024) + L"MB)");

    EvictDecodedCache();
}

// 从已解码缓存恢复：命中则直接用 GPU 纹理，跳过模糊预览和顶层解码
// 瓦片图延迟 200ms 打开文件供缩放时瓦片解码（不阻塞当前显示）
bool ImageEngine::LoadFromDecodedCache(const std::wstring& path, int newIndex) {
    auto it = _decodedCache.find(newIndex);
    if (it == _decodedCache.end()) return false;

    auto& entry = it->second;
    entry.lastAccess = std::chrono::steady_clock::now();
    // 引用计数 +1，Unload 不会释放缓存纹理
    ComPtr<ID2D1Bitmap1> cached = entry.bitmap;

    Unload();
    _currentIndex = newIndex;
    _currentPath  = path;
    _sourceBitmap = cached;       // 直接复用已解码纹理，无需 CreateBitmap
    _topLevelBitmap = cached;     // 切层时作拉伸源
    _srcWidth  = entry.origWidth;
    _srcHeight = entry.origHeight;
    _displayedLevel = entry.level;
    _cacheFullQuality = entry.hasFullQuality;  // 高清缓存：打开文件后跳过排瓦片

    // 瓦片图：延迟打开文件供缩放时瓦片解码，已有纹理不阻塞显示
    // CheckInteractionTimeout 检测到 _sourceBitmap 已就绪会跳过 StartBgTopLevelDecode
    // 仅检查 levelCount：旧缓存可能 supportsTiling=false（_decoder 为空时误存），
    // levelCount>1 足够判断需要金字塔，打破恶性循环
    if (entry.levelCount > 1) {
        _pyramid = std::make_shared<Pyramid>(
            Pyramid::ForSize(entry.origWidth, entry.origHeight));
        _tileCache = std::make_shared<TileCache>();
        _bgDecodeScheduled   = true;
        _bgDecodeLevel       = entry.level;
        _bgDecodeSkipThumb   = true;
        _bgDecodePendingPath = path;
        _bgDecodeDeadline    = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(200);
    }

    if (_renderer) FitToWindow(_renderer->Width(), _renderer->Height());

    ActivityLog::Instance().Log(L"缓存",
        L"命中已解码" + std::wstring(entry.hasFullQuality ? L"(高清)" : L"(顶层)") +
        L": " + ActivityFmt::ShortName(path) +
        L" " + std::to_wstring(entry.origWidth) + L"x" + std::to_wstring(entry.origHeight));
    // 性能遥测：已解码缓存直通，占位与完整同时达到
    LogNavLatency(false);
    LogNavLatency(true);
    return true;
}

// 淘汰：时间（3 分钟未访问）+ 大小（总量超 200MB 按 LRU 淘汰）
void ImageEngine::EvictDecodedCache() {
    auto now = std::chrono::steady_clock::now();
    // 时间淘汰：超 3 分钟未访问
    for (auto it = _decodedCache.begin(); it != _decodedCache.end();) {
        auto age = std::chrono::duration_cast<std::chrono::seconds>(
            now - it->second.lastAccess).count();
        if (age >= DECODED_CACHE_EXPIRE_SEC) {
            _decodedCacheBytes -= it->second.bytes;
            it = _decodedCache.erase(it);
        } else {
            ++it;
        }
    }
    // 大小淘汰：总量超 200MB，按最久未访问淘汰
    while (_decodedCacheBytes > MAX_DECODED_CACHE_BYTES && !_decodedCache.empty()) {
        auto oldest = _decodedCache.begin();
        for (auto it = _decodedCache.begin(); it != _decodedCache.end(); ++it) {
            if (it->second.lastAccess < oldest->second.lastAccess) oldest = it;
        }
        _decodedCacheBytes -= oldest->second.bytes;
        _decodedCache.erase(oldest);
    }
}

void ImageEngine::TriggerPreDecode(int fwd, int bwd) {
    if (!_preDecodeCache) return;
    if (_dirFiles.empty() || _currentIndex < 0) return;
    ActivityLog::Instance().Log(L"预解码",
        L"触发: 向后" + std::to_wstring(fwd) + L"张 向前" + std::to_wstring(bwd) + L"张");
    _preDecodeCache->SetCurrentIndex(
        _currentIndex, _dirFiles, DecodeForCache, fwd, bwd);
}

// 静态解码回调：工作线程执行，创建独立 FileMapping + decoder，不访问实例状态
// 优先提取缩略图（DecodeThumbnail，极快），无缩略图回退 DecodeLevel(topLevel)
// 缩略图提取不涉及全量解码，大文件也能快速缓存
bool ImageEngine::DecodeForCache(const std::wstring& path, CachedImage& out) {
    // GIF 跳过：动画需多帧，预解码只存单帧
    auto dotPos = path.find_last_of(L'.');
    if (dotPos != std::wstring::npos) {
        std::wstring ext = path.substr(dotPos);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
        if (ext == L".gif") return false;
    }

    // 独立 FileMapping：不共享 ImageEngine::_fileMapping（线程安全）
    auto fm = std::make_shared<FileMapping>(path);
    if (!fm->Data()) return false;

    // 独立 decoder 实例
    size_t headerLen = (std::min)(fm->Size(), (size_t)4096);
    auto decoder = FindDecoder(fm->Data(), headerLen);
    if (!decoder && dotPos != std::wstring::npos) {
        std::wstring extW = path.substr(dotPos);
        std::string extA(extW.begin(), extW.end());
        decoder = FindDecoderByExtension(extA);
    }
    if (!decoder) return false;

    auto openResult = decoder->Open(fm->Data(), fm->Size());
    if (!openResult) return false;
    openResult->state = fm;  // 解码器通过 state 持有 MMF，OpenResult 析构时释放

    int origW = openResult->info.width;
    int origH = openResult->info.height;
    Pyramid py = Pyramid::ForSize(origW, origH);
    int topLevel = py.TopLevel();

    // ── 优先提取内嵌缩略图（极快：JPEG EXIF ~1ms，PSD ~5-50ms，HEIF ~10ms）──
    // 缩略图不涉及全量解码，大文件也能快速缓存
    std::optional<DecodeResult> thumb;
    {
        PerfScope perfThumb(L"解码", decoder->Name());  // 性能遥测：预解码缩略图耗时
        thumb = decoder->DecodeThumbnail(*openResult);
        perfThumb.SetExtra({
            Perf::S("file", ActivityFmt::NarrowUtf8(ActivityFmt::ShortName(path))),
            Perf::S("op_detail", "DecodeThumbnail_predecode")
        });
    }
    if (thumb) {
        out.width = thumb->width;
        out.height = thumb->height;
        out.stride = thumb->stride;
        out.pixels = std::make_shared<std::vector<uint8_t>>(std::move(thumb->pixels));
        out.origWidth = origW;
        out.origHeight = origH;
        out.topLevel = topLevel;
        out.levelCount = py.LevelCount();
        out.supportsTiling = decoder->SupportsTiling();
        return true;
    }

    // ── 无内嵌缩略图回退：DecodeLevel(topLevel) 作为预览 ──
    // 顶层预览通常 128x128（~64KB），解码较快（PNG/BMP <50ms，TIFF 视压缩而定）
    // 超大文件（>200MB）跳过：DecodeLevel 内部可能 DecodeFull 分配数百 MB
    constexpr size_t MAX_LEVEL_CACHE_SIZE = 200 * 1024 * 1024;  // 200MB
    if (decoder->SupportsTiling() && fm->Size() <= MAX_LEVEL_CACHE_SIZE) {
        std::optional<DecodeResult> decoded;
        {
            PerfScope perfLevel(L"解码", decoder->Name());  // 性能遥测：预解码顶层耗时
            decoded = decoder->DecodeLevel(*openResult, topLevel);
            perfLevel.SetExtra({
                Perf::S("file", ActivityFmt::NarrowUtf8(ActivityFmt::ShortName(path))),
                Perf::N("level", (double)topLevel),
                Perf::S("op_detail", "DecodeLevel_predecode")
            });
        }
        if (decoded) {
            out.width = decoded->width;
            out.height = decoded->height;
            out.stride = decoded->stride;
            out.pixels = std::make_shared<std::vector<uint8_t>>(std::move(decoded->pixels));
            out.origWidth = origW;
            out.origHeight = origH;
            out.topLevel = topLevel;
            out.levelCount = py.LevelCount();
            out.supportsTiling = true;
            return true;
        }
    }

    // 不支持瓦片或大文件：仅小图（<2M 像素）用 DecodeFull 缓存
    constexpr size_t MAX_FULL_CACHE_PIXELS = 2 * 1024 * 1024;
    if ((size_t)origW * origH > MAX_FULL_CACHE_PIXELS) return false;

    std::optional<DecodeResult> decoded;
    {
        PerfScope perfFull(L"解码", decoder->Name());  // 性能遥测：预解码全量耗时
        decoded = decoder->DecodeFull(*openResult);
        perfFull.SetExtra({
            Perf::S("file", ActivityFmt::NarrowUtf8(ActivityFmt::ShortName(path))),
            Perf::S("op_detail", "DecodeFull_predecode")
        });
    }
    if (!decoded) return false;
    out.width = decoded->width;
    out.height = decoded->height;
    out.stride = decoded->stride;
    out.pixels = std::make_shared<std::vector<uint8_t>>(std::move(decoded->pixels));
    out.origWidth = origW;
    out.origHeight = origH;
    out.topLevel = 0;
    out.levelCount = 1;
    out.supportsTiling = false;
    return true;
}

// 盒式平均降采样：把 BGRA 图像缩到 targetW×targetH（等比≤80×110，避免最近邻锯齿）
// 用于缩略图生成：解码大图→一次缩到最终显示尺寸，内存小且画质平滑
static void BoxAverageShrink(const uint8_t* src, int sw, int sh, int stride,
                             std::vector<uint8_t>& dst, int dw, int dh) {
    dst.resize((size_t)dw * dh * 4);
    for (int y = 0; y < dh; y++) {
        int y0 = (int)((int64_t)y * sh / dh), y1 = (int)((int64_t)(y + 1) * sh / dh);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < dw; x++) {
            int x0 = (int)((int64_t)x * sw / dw), x1 = (int)((int64_t)(x + 1) * sw / dw);
            if (x1 <= x0) x1 = x0 + 1;
            int b = 0, g = 0, r = 0, a = 0, n = 0;
            for (int sy = y0; sy < y1; sy++) {
                const uint8_t* p = src + (size_t)sy * stride + (size_t)x0 * 4;
                for (int sx = x0; sx < x1; sx++, p += 4) {
                    b += p[0]; g += p[1]; r += p[2]; a += p[3]; n++;
                }
            }
            uint8_t* q = dst.data() + ((size_t)y * dw + x) * 4;
            q[0] = (uint8_t)(b / n); q[1] = (uint8_t)(g / n);
            q[2] = (uint8_t)(r / n); q[3] = (uint8_t)(a / n);
        }
    }
}

// 静态缩略图解码回调：底部条/侧边栏共享，解码后一步缩到 80×110 等比小图
// 复用 DecodeForCache 的解码源（内嵌缩略图优先，回退顶层/全量），仅最终尺寸缩小
bool ImageEngine::DecodeForThumbnail(const std::wstring& path, CachedImage& out) {
    CachedImage full;
    if (!DecodeForCache(path, full)) return false;
    if (!full.pixels || full.pixels->empty() || full.width <= 0 || full.height <= 0) return false;

    // 等比 ≤ 80×110 框（避免上采样）
    constexpr int kThumbW = 80, kThumbH = 110;
    float s = std::min((float)kThumbW / full.width, (float)kThumbH / full.height);
    if (s >= 1.0f) {  // 源已足够小：直接复用
        out = std::move(full);
        return true;
    }
    int tw = (std::max)(1, (int)std::lround(full.width * s));
    int th = (std::max)(1, (int)std::lround(full.height * s));
    std::vector<uint8_t> thumb;
    BoxAverageShrink(full.pixels->data(), full.width, full.height, full.stride, thumb, tw, th);
    out = std::move(full);  // 保留 origWidth/topLevel 等元数据
    out.width = tw; out.height = th; out.stride = tw * 4;
    out.pixels = std::make_shared<std::vector<uint8_t>>(std::move(thumb));
    return true;
}

// ─── 异步顶层解码 ───

// 全异步加载：缓存未命中路径专用
// FileMapping+FindDecoder+Open+Pyramid+缩略图+顶层 全部在后台线程完成
// 主线程仅由 ApplyBgResultIfReady 应用结果（CreateBitmap 等 D2D 操作），UI 线程零阻塞
// 三阶段：阶段0 文件打开 → 阶段1 缩略图 → 阶段2 顶层/全量
// 每阶段后查 cancel，快速连点时旧任务尽早丢弃，避免 CPU 浪费
void ImageEngine::StartBgFullLoad(const std::wstring& path, int fwd, int bwd) {
    if (_bgThread.joinable()) _bgThread.detach();

    _bgState = std::make_shared<BgState>();
    _bgState->pending.store(true, std::memory_order_release);
    _bgState->hasOpenStage = true;           // 标记含阶段0，ApplyBgResultIfReady 先应用 openResult
    _bgState->targetIndex = _currentIndex;  // 索引一致性校验：结果应用时若已切换则丢弃

    auto bgState = _bgState;
    ActivityLog::Instance().Log(L"异步",
        L"全异步加载: 向后" + std::to_wstring(fwd) + L" 向前" + std::to_wstring(bwd));

    _bgThread = std::thread([path, bgState, this]() {
        ActivityLog::SetThreadName("BgLoad");
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        // work 用普通 return 早退，CoUninitialize 统一在末尾调用
        auto work = [&]() {
            // ── 阶段0：FileMapping + FindDecoder + Open + Pyramid ──
            auto fm = std::make_shared<FileMapping>(path);
            if (!fm->Data()) {
                std::lock_guard lock(bgState->mutex);
                bgState->openResult.failed = true;
                bgState->openResult.ready  = true;
                return;
            }
            if (bgState->cancel.load(std::memory_order_acquire)) return;

            size_t headerLen = (std::min)(fm->Size(), (size_t)4096);
            // 显式 shared_ptr：FindDecoder 返回 unique_ptr，借此转换构造为共享所有权
            // 后续阶段复用同一 decoder，且 bgState->openResult.decoder 需 shared_ptr 副本
            std::shared_ptr<ImageDecoder> decoder = FindDecoder(fm->Data(), headerLen);
            if (!decoder) {
                auto dotPos = path.find_last_of(L'.');
                if (dotPos != std::wstring::npos) {
                    std::wstring extW = path.substr(dotPos);
                    std::string extA(extW.begin(), extW.end());
                    decoder = FindDecoderByExtension(extA);
                }
            }
            if (!decoder) {
                std::lock_guard lock(bgState->mutex);
                bgState->openResult.failed = true;
                bgState->openResult.ready  = true;
                return;
            }
            if (bgState->cancel.load(std::memory_order_acquire)) return;

            auto openOpt = decoder->Open(fm->Data(), fm->Size());
            if (!openOpt) {
                std::lock_guard lock(bgState->mutex);
                bgState->openResult.failed = true;
                bgState->openResult.ready  = true;
                return;
            }
            openOpt->state = fm;  // 解码器通过 state 持有 MMF 引用
            if (bgState->cancel.load(std::memory_order_acquire)) return;

            auto pyramid = std::make_shared<Pyramid>(
                Pyramid::ForSize(openOpt->info.width, openOpt->info.height));
            bool tiling   = decoder->SupportsTiling();
            int  topLevel = pyramid->TopLevel();

            // 提交阶段0 结果：shared_ptr 共享所有权 + OpenResult 副本
            // 后续阶段用本地 decoder/openOpt，与主线程读 bgState->openResult 无竞态
            {
                std::lock_guard lock(bgState->mutex);
                if (bgState->cancel.load(std::memory_order_acquire)) return;
                bgState->openResult.fileMapping    = fm;
                bgState->openResult.decoder        = decoder;
                bgState->openResult.openResult     = *openOpt;
                bgState->openResult.pyramid        = pyramid;
                bgState->openResult.topLevel       = topLevel;
                bgState->openResult.supportsTiling = tiling;
                bgState->openResult.ready          = true;
            }
            ActivityLog::Instance().Log(L"异步",
                L"阶段0 就绪: " + std::to_wstring(openOpt->info.width) + L"x" +
                std::to_wstring(openOpt->info.height) + (tiling ? L" (瓦片)" : L" (非瓦片)"));

            // ── 阶段1：缩略图（仅瓦片格式，毫秒级）──
            // 非瓦片跳过：DecodeFull 一次出图，无需缩略图占位
            if (tiling) {
                PerfScope perfThumb(L"解码", decoder->Name());
                auto thumb = decoder->DecodeThumbnail(*openOpt);
                perfThumb.SetExtra({
                    Perf::S("file", ActivityFmt::NarrowUtf8(ActivityFmt::ShortName(path))),
                    Perf::S("op_detail", "DecodeThumbnail_bgfull")
                });
                if (!bgState->cancel.load(std::memory_order_acquire) && thumb) {
                    std::lock_guard lock(bgState->mutex);
                    if (!bgState->cancel.load(std::memory_order_acquire)) {
                        bgState->thumbResult.ready   = true;
                        bgState->thumbResult.width   = thumb->width;
                        bgState->thumbResult.height  = thumb->height;
                        bgState->thumbResult.stride  = thumb->stride;
                        bgState->thumbResult.pixels  = std::move(thumb->pixels);
                    }
                }
                if (bgState->cancel.load(std::memory_order_acquire)) return;
            }

            // ── 阶段2：顶层预览（瓦片）或全量（非瓦片）──
            {
                std::optional<DecodeResult> result;
                PerfScope perfLevel(L"解码", decoder->Name());
                if (tiling) {
                    result = decoder->DecodeLevel(*openOpt, topLevel);
                    perfLevel.SetExtra({
                        Perf::S("file", ActivityFmt::NarrowUtf8(ActivityFmt::ShortName(path))),
                        Perf::N("level", (double)topLevel),
                        Perf::S("op_detail", "DecodeLevel_bgfull")
                    });
                } else {
                    result = decoder->DecodeFull(*openOpt);
                    perfLevel.SetExtra({
                        Perf::S("file", ActivityFmt::NarrowUtf8(ActivityFmt::ShortName(path))),
                        Perf::S("op_detail", "DecodeFull_bgfull")
                    });
                }
                if (!bgState->cancel.load(std::memory_order_acquire) && result) {
                    std::lock_guard lock(bgState->mutex);
                    if (!bgState->cancel.load(std::memory_order_acquire)) {
                        bgState->topResult.ready   = true;
                        bgState->topResult.level   = tiling ? topLevel : 0;
                        bgState->topResult.width   = result->width;
                        bgState->topResult.height  = result->height;
                        bgState->topResult.stride  = result->stride;
                        bgState->topResult.pixels  = std::move(result->pixels);
                    }
                }
            }
        };
        work();

        CoUninitialize();
    });
}

// 启动后台线程解码顶层预览
// 主线程立即返回，结果通过 _bgState 传递，由 ApplyBgResultIfReady 在 RenderFrame 中换入
// 线程持有 shared_ptr 副本（bgState + decoder + openResult），Unload 时 detach 不阻塞
// 两阶段：先 DecodeThumbnail（快）→ 更新 _blurBitmap，再 DecodeLevel（慢）→ 更新 _sourceBitmap
// skipThumb=true 跳过缩略图阶段（缓存命中时 _blurBitmap 已有缩略图）
void ImageEngine::StartBgTopLevelDecode(int level, bool skipThumb) {
    // detach 旧线程（如有）：线程持有自己的 shared_ptr 副本，可安全后台运行
    if (_bgThread.joinable()) _bgThread.detach();

    // 创建新 BgState：旧线程持有旧 BgState 副本，互不干扰
    _bgState = std::make_shared<BgState>();
    _bgState->pending.store(true, std::memory_order_release);  // 标记异步解码中
    _bgState->targetIndex = _currentIndex;  // 记录解码任务对应的图片索引
    if (skipThumb) _bgState->thumbApplied = true;  // 跳过缩略图阶段

    // 捕获 shared_ptr 副本：Unload 可能在此之后 reset 成员变量
    auto bgState = _bgState;
    auto decoder = _decoder;
    auto openResult = _openResult;

    _bgThread = std::thread([level, skipThumb, bgState, decoder, openResult, this]() {
        ActivityLog::SetThreadName("Decode");  // 性能遥测：线程角色名
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (!decoder) { CoUninitialize(); return; }

        // ── 阶段1：提取缩略图（快，毫秒级）──
        // skipThumb=true 时跳过（缓存命中，_blurBitmap 已有缩略图）
        if (!skipThumb) {
            ActivityLog::Instance().Log(L"异步", L"阶段1: 提取缩略图");
            PerfScope perfThumb(L"解码", decoder->Name());  // 性能遥测：缩略图解码耗时
            auto thumb = decoder->DecodeThumbnail(openResult);
            perfThumb.SetExtra({
                Perf::S("file", ActivityFmt::NarrowUtf8(ActivityFmt::ShortName(_currentPath))),
                Perf::S("op_detail", "DecodeThumbnail_bg")
            });
            if (!bgState->cancel.load(std::memory_order_acquire) && thumb) {
                std::lock_guard lock(bgState->mutex);
                if (!bgState->cancel.load(std::memory_order_acquire)) {
                    bgState->thumbResult.ready = true;
                    bgState->thumbResult.width = thumb->width;
                    bgState->thumbResult.height = thumb->height;
                    bgState->thumbResult.stride = thumb->stride;
                    bgState->thumbResult.pixels = std::move(thumb->pixels);
                }
            }

            // cancel 检测：用户已切换到下一张，不进入重量级 DecodeLevel
            if (bgState->cancel.load(std::memory_order_acquire)) {
                CoUninitialize();
                return;
            }
        }

        // ── 阶段2：解码顶层预览（慢，百毫秒到秒级）──
        ActivityLog::Instance().Log(L"异步", L"阶段2: 解码顶层 Level " + std::to_wstring(level));
        PerfScope perfLevel(L"解码", decoder->Name());  // 性能遥测：顶层解码耗时
        auto result = decoder->DecodeLevel(openResult, level);
        perfLevel.SetExtra({
            Perf::S("file", ActivityFmt::NarrowUtf8(ActivityFmt::ShortName(_currentPath))),
            Perf::N("level", (double)level),
            Perf::S("op_detail", "DecodeLevel_bg")
        });
        if (!bgState->cancel.load(std::memory_order_acquire) && result) {
            std::lock_guard lock(bgState->mutex);
            if (!bgState->cancel.load(std::memory_order_acquire)) {
                bgState->topResult.ready = true;
                bgState->topResult.level = level;
                bgState->topResult.width = result->width;
                bgState->topResult.height = result->height;
                bgState->topResult.stride = result->stride;
                bgState->topResult.pixels = std::move(result->pixels);
            }
        }
        CoUninitialize();
    });
}

// 主线程调用：检测后台两阶段结果就绪则上传 GPU 纹理
// 阶段1：缩略图就绪 → 更新 _blurBitmap（用户立即看到当前图缩略图）
// 阶段2：顶层就绪 → 更新 _sourceBitmap，清空 _blurBitmap，触发 UpdateViewport
void ImageEngine::ApplyBgResultIfReady() {
    if (!_bgState) return;

    // 索引一致性验证：_bgState 对应的图片已被切换走，丢弃所有过期结果
    // 防止旧图片的后台解码结果错误应用到当前图片，导致"图显示与名称不一致"
    if (_bgState->targetIndex != _currentIndex) {
        LOG_INFO_STREAM("ImgEngine") << "丢弃过期解码结果: bgTarget=" << _bgState->targetIndex
            << " current=" << _currentIndex;
        _bgState->pending.store(false, std::memory_order_release);
        _bgState.reset();
        return;
    }

    // ── 阶段0：文件打开结果应用（仅 StartBgFullLoad 路径，hasOpenStage=true）──
    // 后台 FileMapping+Open+Pyramid 结果换入主线程成员；未就绪则等下一帧
    // StartBgTopLevelDecode 不置 hasOpenStage，跳过本块直接进阶段1/2
    if (_bgState->hasOpenStage && !_bgState->openApplied) {
        BgOpenResult open;
        bool openReady = false;
        {
            std::lock_guard lock(_bgState->mutex);
            if (_bgState->openResult.ready) {
                open = std::move(_bgState->openResult);
                _bgState->openResult = {};
                _bgState->openApplied = true;
                openReady = true;
            }
        }
        if (!openReady) return;  // 阶段0 未就绪，等下一帧
        if (open.failed) {
            LOG_WARN("ImgEngine", "全异步文件打开失败");
            ActivityLog::Instance().Log(L"异步", L"文件打开失败");
            _bgState->pending.store(false, std::memory_order_release);
            _bgState.reset();
            return;
        }
        _fileMapping = open.fileMapping;
        _decoder     = open.decoder;
        _openResult  = std::move(open.openResult);
        _imageInfo   = _openResult.info;
        _pyramid     = open.pyramid;
        _tileCache   = std::make_shared<TileCache>();
        _srcWidth    = _imageInfo.width;
        _srcHeight   = _imageInfo.height;
        _displayedLevel = open.topLevel;
        if (!open.supportsTiling) _bgState->thumbApplied = true;  // 非瓦片跳过缩略图阶段
        if (_renderer) FitToWindow(_renderer->Width(), _renderer->Height());
        LOG_INFO_STREAM("ImgEngine") << "文件就绪(异步): " << _srcWidth << "x" << _srcHeight
            << (open.supportsTiling ? " (瓦片)" : " (非瓦片)");
        ActivityLog::Instance().Log(L"异步",
            L"文件就绪: " + std::to_wstring(_srcWidth) + L"x" + std::to_wstring(_srcHeight));
    }

    // ── 阶段1：缩略图就绪 → 更新 _blurBitmap ──
    if (!_bgState->thumbApplied) {
        BgResult thumb;
        bool hasThumb = false;
        {
            std::lock_guard lock(_bgState->mutex);
            if (_bgState->thumbResult.ready) {
                thumb = std::move(_bgState->thumbResult);
                _bgState->thumbResult = {};
                _bgState->thumbApplied = true;
                hasThumb = true;
            }
        }
        if (hasThumb && _renderer && thumb.pixels.size() > 0) {
            _blurBitmap = _renderer->CreateBitmap(
                thumb.width, thumb.height, thumb.pixels.data(), thumb.stride);
            LOG_INFO_STREAM("ImgEngine") << "缩略图就绪: " << thumb.width << "x" << thumb.height;
            ActivityLog::Instance().Log(L"异步",
                L"缩略图就绪: " + std::to_wstring(thumb.width) + L"x" + std::to_wstring(thumb.height));
            LogNavLatency(false);  // 性能遥测：占位帧就绪（异步缩略图）
        }
    }

    // ── 阶段2：顶层就绪 → 更新 _sourceBitmap ──
    BgResult top;
    {
        std::lock_guard lock(_bgState->mutex);
        if (!_bgState->topResult.ready) return;  // 顶层未就绪，等下一帧
        top = std::move(_bgState->topResult);
        _bgState->topResult = {};
    }
    _bgState->pending.store(false, std::memory_order_release);  // 异步解码全部完成

    if (!_renderer || !_pyramid) return;

    {
        PerfScope perfBmp(L"翻页", "nav.apply_bitmap");  // 细分计时：顶层纹理上传到 GPU
        _sourceBitmap = _renderer->CreateBitmap(
            top.width, top.height, top.pixels.data(), top.stride);
    }
    if (!_sourceBitmap) return;

    _displayedLevel = top.level;
    _topLevelBitmap = _sourceBitmap;
    _blurBitmap.Reset();  // 顶层就绪，模糊占位使命完成

    // 非瓦片单层图：异步路径补 ICC（瓦片模式切层会重建位图，ICC effect 绑定失效故跳过）
    if (_decoder && !_decoder->SupportsTiling()
        && _pyramid && _pyramid->LevelCount() <= 1
        && !_imageInfo.iccProfile.empty() && _sourceBitmap) {
        _colorManager.SetIccProfile(
            _imageInfo.iccProfile.data(), _imageInfo.iccProfile.size());
        _colorManager.SetSourceBitmap(_sourceBitmap.Get());
    }

    LogNavLatency(true);  // 性能遥测：完整帧就绪（异步顶层）

    LOG_INFO_STREAM("ImgEngine") << "顶层就绪: " << top.width << "x" << top.height
        << " (level " << top.level << ")";
    ActivityLog::Instance().Log(L"异步",
        L"顶层就绪: " + std::to_wstring(top.width) + L"x" + std::to_wstring(top.height) +
        L" (Level " + std::to_wstring(top.level) + L")");

    // 延迟 UpdateViewport：当前处于 RenderFrame→BeginDraw 内，
    // UpdateViewport→SwitchToLevel→CreateStretchedBitmap 会再次 BeginDraw 导致嵌套崩溃
    if (_decoder && _decoder->SupportsTiling() && _pyramid->LevelCount() > 1) {
        _needsViewportUpdate = true;
    }
}

// OnTimer 调用：非绘制期执行延迟的 UpdateViewport（SwitchToLevel 的 CreateStretchedBitmap 安全）
void ImageEngine::FlushViewportUpdate() {
    if (!_needsViewportUpdate) return;
    _needsViewportUpdate = false;
    if (_decoder && _decoder->SupportsTiling() && _pyramid && _pyramid->LevelCount() > 1) {
        UpdateViewport();
    }
}

// ─── 性能遥测实现 ───

// 上报翻页延迟：isFull=true 记 nav_to_full_ms，false 记 nav_to_placeholder_ms
// 守卫 _navTargetIndex==_currentIndex 且未重复记，避免快速切换时旧任务结果污染
void ImageEngine::LogNavLatency(bool isFull) {
    if (_navTargetIndex == -1) return;
    if (_navTargetIndex != _currentIndex) return;  // 已切换到其他图，丢弃
    if (isFull && _navFullLogged) return;
    if (!isFull && _navPlaceholderLogged) return;

    double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - _navStart).count();
    const char* op = isFull ? "nav_to_full_ms" : "nav_to_placeholder_ms";
    ActivityLog::Instance().LogTimed(L"翻页", op, ms, {
        Perf::S("file", ActivityFmt::NarrowUtf8(ActivityFmt::ShortName(_currentPath))),
        Perf::N("idx", (double)_currentIndex)
    });
    if (isFull) _navFullLogged = true;
    else _navPlaceholderLogged = true;
}

// 估算当前持有 ID2D1Bitmap 总像素 ×4B（显存近似）
// 遍历 _sourceBitmap/_topLevelBitmap/_blurBitmap/_decodedCache，指针去重避免重复计数
size_t ImageEngine::EstimateBitmapBytes() const {
    std::vector<ID2D1Bitmap1*> seen;
    auto add = [&](const ComPtr<ID2D1Bitmap1>& b) -> size_t {
        if (!b) return 0;
        ID2D1Bitmap1* p = b.Get();
        for (auto* s : seen) if (s == p) return 0;
        seen.push_back(p);
        D2D1_SIZE_F s = b->GetSize();
        return (size_t)s.width * (size_t)s.height * 4;
    };
    size_t total = add(_sourceBitmap) + add(_topLevelBitmap) + add(_blurBitmap);
    for (const auto& kv : _decodedCache) total += add(kv.second.bitmap);
    return total;
}

// 性能遥测：瓦片缓存命中/未命中计数（透传 TileCache）
uint64_t ImageEngine::TileHitCount() const noexcept {
    return _tileCache ? _tileCache->HitCount() : 0;
}
uint64_t ImageEngine::TileMissCount() const noexcept {
    return _tileCache ? _tileCache->MissCount() : 0;
}
void ImageEngine::TileResetHitMiss() noexcept {
    if (_tileCache) _tileCache->ResetHitMiss();
}

// 读回当前显示纹理像素（GPU→CPU staging bitmap），用于复制图片到剪贴板
bool ImageEngine::ReadDisplayPixels(std::vector<uint8_t>& pixels, int& width, int& height,
                                    int* stride) {
    if (!_sourceBitmap || !_renderer) return false;
    D2D1_SIZE_U size = _sourceBitmap->GetPixelSize();
    width = (int)size.width;
    height = (int)size.height;
    if (width <= 0 || height <= 0) return false;

    // 创建 CPU 可读 staging bitmap
    D2D1_BITMAP_PROPERTIES1 props = {};
    props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
    props.bitmapOptions = D2D1_BITMAP_OPTIONS_CANNOT_DRAW | D2D1_BITMAP_OPTIONS_CPU_READ;
    props.dpiX = 96.0f;
    props.dpiY = 96.0f;
    ComPtr<ID2D1Bitmap1> staging;
    HRESULT hr = _renderer->Context()->CreateBitmap(size, nullptr, 0, &props, &staging);
    if (FAILED(hr) || !staging) return false;

    // 从 GPU 纹理拷贝到 staging
    D2D1_POINT_2U dstPoint = { 0, 0 };
    D2D1_RECT_U srcRect = { 0, 0, size.width, size.height };
    hr = staging->CopyFromBitmap(&dstPoint, _sourceBitmap.Get(), &srcRect);
    if (FAILED(hr)) return false;

    // Map 读取像素（top-down，row 0 = 图片顶部）
    D2D1_MAPPED_RECT mapped = {};
    hr = staging->Map(D2D1_MAP_OPTIONS_READ, &mapped);
    if (FAILED(hr)) return false;
    pixels.resize((size_t)mapped.pitch * height);
    memcpy(pixels.data(), mapped.bits, pixels.size());
    if (stride) *stride = (int)mapped.pitch;  // GPU pitch，可能 > width*4
    staging->Unmap();
    return true;
}

// 读原图全分辨率像素（另存为用）
// 瓦片大图 _sourceBitmap 是缩放后的显示层（如 fit-to-window 时取顶层小图），
// 直接回读会得到缩小分辨率，另存为等于丢数据。此处按需重新解码原图全分辨率。
bool ImageEngine::ReadSourcePixels(std::vector<uint8_t>& pixels, int& width, int& height, int& stride) {
    // 非瓦片小图：_sourceBitmap 即原图全分辨率，直接 GPU 回读省去重新解码
    if (_sourceBitmap && _renderer) {
        D2D1_SIZE_U sz = _sourceBitmap->GetPixelSize();
        if ((int)sz.width == _srcWidth && (int)sz.height == _srcHeight) {
            return ReadDisplayPixels(pixels, width, height, &stride);
        }
    }
    // 瓦片大图：重新解码原图全分辨率（DecodeFull 从 FileMapping 独立解码，不依赖 GPU 纹理）
    if (_decoder && _openResult.state) {
        auto result = _decoder->DecodeFull(_openResult);
        if (result) {
            width  = result->width;
            height = result->height;
            stride = result->stride;
            pixels = std::move(result->pixels);
            return true;
        }
    }
    // 兜底：缓存命中且文件尚未打开时退化为显示分辨率（好过失败）
    return ReadDisplayPixels(pixels, width, height, &stride);
}

// 文件删除后：从目录列表移除并导航到相邻文件
void ImageEngine::OnFileDeleted() {
    if (_currentIndex < 0 || _currentIndex >= (int)_dirFiles.size()) return;
    _dirFiles.erase(_dirFiles.begin() + _currentIndex);
    // 索引已偏移，清除已解码缓存避免错位命中
    _decodedCache.clear();
    _decodedCacheBytes = 0;
    if (_dirFiles.empty()) {
        Unload();
        return;
    }
    if (_currentIndex >= (int)_dirFiles.size()) {
        _currentIndex = (int)_dirFiles.size() - 1;
    }
    NavigateTo(_currentIndex);
}









