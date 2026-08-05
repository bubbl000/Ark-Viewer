#include "WindowManager.h"
#include "ActivityLog.h"
#include "Logger.h"
#include "Config.h"
#include "ExifParser.h"
#include "DragSource.h"
#include "FileAssoc.h"
#include "MessageBoxDlg.h"
#include "DecoderFactory.h"
#include "SettingsPanel.h"
#include "SaveAsDialog.h"
#include "decoders/JpegDecoder.h"
#include "decoders/WicDecoder.h"
#include "decoders/WebpDecoder.h"
#include "../resources/resource.h"
#include <commdlg.h>    // OPENFILENAMEW / GetOpenFileNameW / GetSaveFileNameW
#include <shellapi.h>   // SHFileOperationW / ShellExecuteW
#include <shlwapi.h>    // PathFileExistsW（另存为自动名存在性检查）
#include <psapi.h>      // GetProcessMemoryInfo（性能遥测 snapshot 用）
#include <filesystem>   // file_size（标题栏文件大小同步）
#include <strsafe.h>    // StringCchCopyW（重命名 InputBox）
#include <commctrl.h>   // Tab/ListView 公共控件 + SetWindowSubclass
#include <dwmapi.h>     // DwmSetWindowAttribute（设置弹窗深色标题栏）
#include <uxtheme.h>    // SetWindowTheme（禁用控件 visual styles 用经典深色样式）
#include <algorithm>
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")

// GIF 动画 + 瓦片重绘 定时器
constexpr UINT_PTR ANIM_TIMER_ID = 1001;
constexpr UINT     ANIM_TIMER_INTERVAL_MS = 30;  // 约 33fps 轮询

// ─── WindowContext ───

bool WindowContext::Create(int cmdShow, const std::wstring& path) {
    // 窗口初始尺寸随 DPI 缩放：系统 DPI 因子 × 用户手动界面缩放（100~200%）
    UINT dpi = GetDpiForSystem();
    auto& cfg = Config::Instance().Get();
    float scale = (dpi / 96.0f) * (cfg.uiScale / 100.0f);
    if (!window.Create(L"Ark Viewer 2", (int)(1280 * scale), (int)(800 * scale))) return false;
    if (!renderer.Initialize() || !renderer.Attach(window.Handle())) return false;
    ui.SetDpiScale(scale);   // UI 布局/字号随 DPI × 手动缩放
    ui.Initialize(renderer.DWrite());
    // GIF 控制面板位置从配置恢复（-1=默认右下角）
    {
        auto& cfg = Config::Instance().Get();
        ui.SetGifPanelPos(cfg.gifPanelX, cfg.gifPanelY);
    }
    imageEngine.Initialize(&renderer, &preDecodeCache);
    // 预解码线程依据本窗口 _tileBusy 让步（多窗口互不干扰）
    preDecodeCache.SetBusyFlag(&imageEngine.TileBusy());

    SetupHandlers();
    window.EnableDropFiles(true);
    window.SetTimer(ANIM_TIMER_ID, ANIM_TIMER_INTERVAL_MS);
    window.Show(cmdShow);

    if (!path.empty()) LoadPath(path);
    return true;
}

void WindowContext::LoadPath(const std::wstring& path) {
    if (!imageEngine.LoadFile(path)) {
        auto dotPos = path.find_last_of(L'.');
        std::wstring ext = (dotPos != std::wstring::npos) ? path.substr(dotPos) : L"";
        MessageBoxDlg::Show(window.Handle(),
            L"不支持此文件格式: " + ext,
            L"提示", false);
        return;
    }
    fileWatcher.Watch(path);
    std::wstring fname = path.substr(path.find_last_of(L"\\/") + 1);
    window.SetTitle((L"Ark Viewer 2 - " + fname).c_str());
    ui.SetTitleText(fname);  // 自绘标题栏显示文件名
    ui.SetStatusText(L"已加载: " + path);
    window.Invalidate();
}

void WindowContext::RefreshAfterNavigate() {
    // Navigate 切换图片后刷新文件监视（标题栏已在 RequestNavigate 更新）
    // 不调 window.Update()：同步发送 WM_PAINT 会阻塞 UI 线程，违背"点击响应与渲染解耦"
    // OnPaint 已在渲染，无需额外 Invalidate
    const auto& path = imageEngine.FilePath();
    if (!path.empty()) {
        fileWatcher.Watch(path);
        // EXIF 面板开启时跟随刷新（用户开面板即在看信息，翻页应同步）
        if (ui.IsExifPanelVisible()) {
            ui.SetExifInfo(ExifParser::Parse(path,
                imageEngine.SrcWidth(), imageEngine.SrcHeight()));
        }
    }
}

// 点击响应与渲染解耦（qimgv/Honeyview 架构）：
// 点击只更新 _targetIndex + 标题栏 + Invalidate（O(1)，不阻塞 UI 线程）
// 真正的 Navigate 在 OnPaint 中执行：快速连点 5 次，UI 线程瞬间完成 5 次 _targetIndex++
// WM_PAINT 只在队列空闲时触发一次，取最新 _targetIndex 渲染，中间帧自动跳过
// 标题栏与画面分离更新：标题栏每次点击都跟随（0.1ms），画面跳帧（避免 8ms CreateBitmap 堆积）
void WindowContext::RequestNavigate(int dir) {
    const auto& files = imageEngine.GetDirFiles();
    if (files.empty()) return;

    // PeekMessage 消息压制：吃掉队列里后续连续的同方向 WM_KEYDOWN
    // 键盘按住自动重复时消息队列会积压，此处一次性合并，只更新一次标题+Invalidate
    // 鼠标点击频率低不处理；慢速点击时队列空，PeekMessage 是空操作
    int vk = (dir > 0) ? VK_RIGHT : VK_LEFT;
    MSG msg;
    int extra = 0;
    while (PeekMessageW(&msg, window.Handle(), WM_KEYDOWN, WM_KEYDOWN, PM_NOREMOVE)) {
        if (msg.wParam == (WPARAM)vk) {
            PeekMessageW(&msg, window.Handle(), WM_KEYDOWN, WM_KEYDOWN, PM_REMOVE);
            extra++;
        } else {
            break;
        }
    }

    // 基准：优先用 _targetIndex（快速连点时 CurrentIndex 尚未更新，必须基于目标累加）
    int base = (_targetIndex != -1) ? _targetIndex : imageEngine.CurrentIndex();
    int newIdx = base + dir * (1 + extra);

    // 文件夹边界处理：第一张按"上一张" / 最后一张按"下一张"
    // 按 Config.folderNavPolicy 决定行为：0=本文件夹循环(默认) 1=穿透 2=询问
    auto& cfg = Config::Instance().Get();
    if (newIdx < 0) {
        // 第一张按"上一张"
        if (cfg.folderNavPolicy == 1) {
            // 穿透：直接进入上一文件夹
            FlushPendingNavigate();
            if (imageEngine.NavigateToSiblingFolder(-1)) return;
            newIdx = (int)files.size() - 1;  // 失败则循环到末尾
        } else if (cfg.folderNavPolicy == 2) {
            // 询问（自绘深色弹窗，与主界面样式统一）
            FlushPendingNavigate();
            if (MessageBoxDlg::Show(window.Handle(), L"是否进入上一个文件夹？", L"提示", true) == IDYES) {
                if (imageEngine.NavigateToSiblingFolder(-1)) return;
            }
            newIdx = (int)files.size() - 1;  // 选否则循环到末尾
        } else {
            newIdx = (int)files.size() - 1;  // 策略0：本文件夹循环
        }
    } else if (newIdx >= (int)files.size()) {
        // 最后一张按"下一张"
        if (cfg.folderNavPolicy == 1) {
            FlushPendingNavigate();
            if (imageEngine.NavigateToSiblingFolder(1)) return;
            newIdx = 0;
        } else if (cfg.folderNavPolicy == 2) {
            // 询问（自绘深色弹窗）
            FlushPendingNavigate();
            if (MessageBoxDlg::Show(window.Handle(), L"是否进入下一个文件夹？", L"提示", true) == IDYES) {
                if (imageEngine.NavigateToSiblingFolder(1)) return;
            }
            newIdx = 0;
        } else {
            newIdx = 0;  // 策略0：本文件夹循环
        }
    }
    _targetIndex = newIdx;
    // 标题栏立即跟随：PeekMessage 已合并后续点击，这里只调 1 次 SetTitle
    const std::wstring& path = files[newIdx];
    std::wstring fname = path.substr(path.find_last_of(L"\\/") + 1);
    window.SetTitle((L"Ark Viewer 2 - " + fname).c_str());
    ui.SetTitleText(fname);  // 自绘标题栏跟随
    ui.SetStatusText(L"已加载: " + path);
    window.Invalidate();  // 异步通知，不调 UpdateWindow（避免同步阻塞）
}

// 同步消费 _targetIndex：缩放/拖拽等操作前调用，确保索引已切换到最新目标
// 避免缩放作用在旧图上（如连点右箭头后立即滚轮缩放）
void WindowContext::FlushPendingNavigate() {
    if (_targetIndex == -1 || _targetIndex == imageEngine.CurrentIndex()) return;
    imageEngine.NavigateTo(_targetIndex);
    _targetIndex = -1;
    RefreshAfterNavigate();
}

void WindowContext::SetupHandlers() {
    // 注意：lambda 捕获 this，WindowContext 由 unique_ptr 持有，地址在 vector 中稳定（对象不发生拷贝移动）

    window.OnResize([this](int w, int h) {
        renderer.Resize(w, h);
        imageEngine.OnResize(w, h);
        window.Invalidate();
    });

    window.OnPaint([this](HDC, int, int) {
        PerfScope perfPaint(L"消息", "WM_PAINT");  // 性能遥测：消息处理耗时（整体对照）
        // 分段计时：定位快速翻页 500ms 卡顿具体落点（NavigateTo/BeginDraw/RenderFrame/ui/EndDraw）
        // 各段用花括号隔离作用域，PerfScope 析构即记录，互不重叠

        // 段1：NavigateTo（含 RefreshAfterNavigate 文件监视刷新）
        {
            PerfScope perfNav(L"消息", "WM_PAINT.nav");
            // 跳帧：消费最新 _targetIndex，中间帧自动跳过
            // 快速连点 5 次：UI 线程瞬间完成 5 次 _targetIndex++，WM_PAINT 只触发 1 次
            // 此处取最新 _targetIndex 调 NavigateTo，中间 4 帧的 CreateBitmap 被跳过
            if (_targetIndex != -1 && _targetIndex != imageEngine.CurrentIndex()) {
                imageEngine.NavigateTo(_targetIndex);
                _targetIndex = -1;
                RefreshAfterNavigate();  // 更新文件监视（标题栏已在 RequestNavigate 更新）
            }
        }

        // 段2：BeginDraw + Clear
        {
            PerfScope perfBegin(L"消息", "WM_PAINT.begin");
            renderer.BeginDraw();
            renderer.Clear(0.118f, 0.118f, 0.118f);   // #1E1E1E
        }

        // 段3：RenderFrame（图片绘制 + 瓦片纹理上传）
        {
            PerfScope perfRender(L"消息", "WM_PAINT.render");
            imageEngine.RenderFrame();
        }

        // 段4：层级信息更新 + UI 绘制
        {
            PerfScope perfUi(L"消息", "WM_PAINT.ui");
            // 更新右上角层级信息（仅变化时更新，避免每帧构造 wstring）
            int level = imageEngine.DisplayedLevel();
            int count = imageEngine.LevelCount();
            if (level != _lastLevel || count != _lastCount) {
                _lastLevel = level;
                _lastCount = count;
                if (count > 1) {
                    ui.SetImageInfo(L"Level " + std::to_wstring(level)
                        + L"/" + std::to_wstring(count - 1));
                } else {
                    ui.SetImageInfo(L"");
                }
            }
            // 同步缩略图条/鸟瞰图状态（每帧从 Config + ImageEngine 取最新）
            auto& cfg = Config::Instance().Get();
            ui.SetThumbBarEnabled(cfg.thumbnailBarVisible);
            ui.SetBirdsEyeEnabled(cfg.birdsEyeVisible);
            ui.SetThumbSource(&preDecodeCache, imageEngine.CurrentIndex(),
                              &imageEngine.GetDirFiles());
            ViewportInfo vp;
            vp.hasImage = imageEngine.HasImage();
            vp.imgW = imageEngine.SrcWidth();
            vp.imgH = imageEngine.SrcHeight();
            vp.winW = window.ClientWidth();
            vp.winH = window.ClientHeight();
            vp.scaleX = imageEngine.Scale();
            vp.scaleY = imageEngine.ScaleY();
            vp.offsetX = imageEngine.OffsetX();
            vp.offsetY = imageEngine.OffsetY();
            vp.rotation = imageEngine.Rotation();
            vp.flipH = imageEngine.FlipH();
            vp.flipV = imageEngine.FlipV();
            ui.SetViewportInfo(vp);
            // 同步标题栏中间信息（文件名|尺寸|大小）+ 工具栏序号（每帧复用此机制）
            // 文件大小按路径缓存：仅路径变化时查 file_size，避免每帧 IO
            const std::wstring& curPath = imageEngine.FilePath();
            if (!curPath.empty()) {
                std::wstring fname = curPath.substr(curPath.find_last_of(L"\\/") + 1);
                std::wstring dims = std::to_wstring(imageEngine.SrcWidth()) + L"x"
                                  + std::to_wstring(imageEngine.SrcHeight());
                if (curPath != _lastSyncPath) {
                    _lastSyncPath = curPath;
                    std::error_code ec;
                    auto sz = std::filesystem::file_size(curPath, ec);
                    if (!ec) {
                        if (sz < 1024ULL * 1024) {
                            _lastSizeStr = std::to_wstring(sz / 1024) + L" KB";
                        } else {
                            _lastSizeStr = std::to_wstring(sz / (1024ULL * 1024)) + L" MB";
                        }
                    } else {
                        _lastSizeStr.clear();
                    }
                }
                ui.SetCenterInfo(fname, dims, _lastSizeStr);
                int idx = imageEngine.CurrentIndex();
                const auto& files = imageEngine.GetDirFiles();
                if (!files.empty() && idx >= 0 && idx < (int)files.size()) {
                    ui.SetIndexText(std::to_wstring(idx + 1) + L"/"
                                  + std::to_wstring(files.size()));
                }
                // 缩放百分比：scale*100 四舍五入，供工具栏缩放框 + 状态栏同步显示
                ui.SetZoomText(std::to_wstring(
                    (int)(imageEngine.Scale() * 100.0 + 0.5)) + L"%");
            } else {
                ui.SetCenterInfo(L"", L"", L"");
                ui.SetIndexText(L"");
                ui.SetZoomText(L"");
                _lastSyncPath.clear();
            }
            // GIF 控制面板状态同步（每帧从 ImageEngine 取最新）
            int animCount = imageEngine.AnimFrameCount();
            ui.SetGifPanelState(animCount > 1, imageEngine.AnimCurrentFrame(),
                                animCount, imageEngine.IsPlayingAnimation());
            ui.Draw(renderer);
        }

        // 段5：EndDraw（含 Present）
        {
            PerfScope perfEnd(L"消息", "WM_PAINT.end");
            renderer.EndDraw();
        }
    });

    window.OnMouseWheel([this](int delta, int x, int y, int fwKeys) {
        PerfScope perfWheel(L"消息", "WM_MOUSEWHEEL");  // 性能遥测：滚轮消息耗时
        FlushPendingNavigate();  // 操作前先刷新待处理导航
        // 滚轮行为：
        //   鼠标在缩略图条上（条开启时）→ 默认切换图片，Ctrl+滚轮缩放（覆盖 wheelBehavior）
        //   否则由 Config.wheelBehavior 决定，Ctrl+滚轮始终执行另一项：
        //     0=缩放(默认)：滚轮缩放，Ctrl+滚轮切换图片
        //     1=切换图片  ：滚轮切换图片，Ctrl+滚轮缩放
        auto& cfg = Config::Instance().Get();
        bool ctrl = (fwKeys & MK_CONTROL) != 0;
        bool onThumbBar = cfg.thumbnailBarVisible &&
                          y > ui.ThumbBarTopY((float)window.ClientHeight());
        bool zoom = onThumbBar ? ctrl : ((cfg.wheelBehavior == 0) ? !ctrl : ctrl);
        if (zoom) {
            double factor = (delta > 0) ? 1.1 : 1.0 / 1.1;
            imageEngine.Zoom(factor, (double)x, (double)y);
        } else {
            // 上滚上一张，下滚下一张
            RequestNavigate(delta > 0 ? -1 : 1);
        }
        window.Invalidate();
    });

    window.OnMouseDown([this](int x, int y, int) {
        PerfScope perfDown(L"消息", "WM_LBUTTONDOWN");  // 性能遥测：点击消息耗时
        // "更多"面板：可见时优先处理（点 toggle 切换；点外部关闭，不触发其他操作）
        if (ui.IsMorePanelVisible()) {
            if (ui.MorePanelHitTest(x, y)) {
                int t = ui.MorePanelToggleAt(x, y);
                if (t >= 0) {
                    auto cfg = Config::Instance().Get();
                    if (t == 0) cfg.birdsEyeVisible = !cfg.birdsEyeVisible;
                    else        cfg.thumbnailBarVisible = !cfg.thumbnailBarVisible;
                    Config::Instance().Set(cfg);
                    window.Invalidate();
                }
                return;  // 面板内点击（含 toggle 间空白）不触发其他操作
            }
            ui.ToggleMorePanel();  // 点面板外 → 关闭面板
            window.Invalidate();
            return;
        }
        // EXIF 面板拖动：点击面板内任意位置可拖动移动（优先于图片平移）
        if (ui.IsExifPanelVisible() && ui.ExifPanelHitTest(x, y)) {
            ui.StartExifDrag(x, y);
            return;
        }
        // GIF 控制面板：按钮点击 + 面板拖动（优先于图片平移）
        if (ui.IsGifPanelVisible()) {
            int gifCmd = ui.GifPanelHitTest(x, y);
            if (gifCmd >= 0) {
                PostMessage(window.Handle(), WM_COMMAND, gifCmd, 0);
                return;
            }
            if (ui.GifPanelDragHitTest(x, y)) {
                ui.StartGifPanelDrag(x, y);
                return;
            }
        }
        // 缩略图条点击：跳转到对应图片（优先于其他命中）
        int thumbIdx = ui.ThumbBarHitTest(x, y);
        if (thumbIdx >= 0) {
            FlushPendingNavigate();
            imageEngine.NavigateTo(thumbIdx);
            RefreshAfterNavigate();
            window.Invalidate();
            return;
        }
        // 鸟瞰图蓝框拖动：进入拖动状态并立即定位一次
        if (ui.BirdsEyeHitTest(x, y)) {
            _draggingBirdsEye = true;
            double dx, dy;
            if (ui.BirdsEyeDrag(x, y, dx, dy)) {
                imageEngine.Pan(dx, dy);
                window.Invalidate();
            }
            return;
        }
        int cmd = ui.HitTest(x, y);
        if (cmd == IDM_VIEW_NEXT) { RequestNavigate(1);  return; }
        if (cmd == IDM_VIEW_PREV) { RequestNavigate(-1); return; }
        FlushPendingNavigate();  // 其他操作前先刷新待处理导航
        if (cmd > 0) {
            PostMessage(window.Handle(), WM_COMMAND, cmd, 0);  // 复用菜单命令处理
            return;
        }
        _dragOutState = DragOutState::Arming;
        _dragStartX = x; _dragStartY = y;
        _lastX = x; _lastY = y;
        SetCapture(window.Handle());  // 移出窗口仍收 WM_MOUSEMOVE，OOB 检测必需
    });

    window.OnMouseMove([this](int x, int y, int) {
        ui.SetMousePos(x, y);  // 全屏渐隐 UI 跟随鼠标位置
        if (ui.IsFullscreen()) {
            window.Invalidate();
        } else if ((_dragOutState == DragOutState::Idle || _dragOutState == DragOutState::Returned) && !_draggingBirdsEye && !ui.IsExifDragging()) {
            // 非全屏下工具栏/缩略图/标题栏按钮 hover 反馈：
            // 仅当 hover 目标变化（进入/离开按钮或缩略图格）时重绘，避免每帧重绘开销
            int key = ui.HitTest(x, y);
            if (key < 0) {
                int th = ui.ThumbBarHitTest(x, y);
                if (th >= 0) key = 100000 + th;  // 缩略图索引加偏移避开命令 ID 段
            }
            if (key != _lastHoverKey) {
                _lastHoverKey = key;
                window.Invalidate();
            }
        }
        // 边缘导航按钮：鼠标靠近左/右 40px 渐显，离开渐隐
        int winW = window.ClientWidth();
        int winH = window.ClientHeight();
        constexpr int EDGE_ZONE = 40;
        ui.SetEdgeNavTargetL(x < EDGE_ZONE ? 1.0f : 0.0f);
        ui.SetEdgeNavTargetR(x > winW - EDGE_ZONE ? 1.0f : 0.0f);
        // 缩略图条智能隐藏：鼠标进入条区域（含工具栏，避免移到按钮时突然消失）渐显，离开渐隐
        // 触发区上沿对齐 ThumbBarTopY（=winH-142），修复原硬编码 winH-100 导致条上半部 42px 为"显示盲区"
        auto& cfg = Config::Instance().Get();
        ui.SetThumbBarTarget((cfg.thumbnailBarVisible && y > ui.ThumbBarTopY((float)winH)) ? 1.0f : 0.0f);
        // 鸟瞰图蓝框拖动：跟随鼠标平移主图
        if (_draggingBirdsEye) {
            double dx, dy;
            if (ui.BirdsEyeDrag(x, y, dx, dy)) {
                imageEngine.Pan(dx, dy);
                window.Invalidate();
            }
            return;
        }
        // EXIF 面板拖动：更新自定义位置并重绘
        if (ui.IsExifDragging()) {
            ui.UpdateExifDrag(x, y, window.ClientWidth(), window.ClientHeight());
            window.Invalidate();
            return;
        }
        // GIF 控制面板拖动：更新位置并重绘
        if (ui.IsGifDragging()) {
            ui.UpdateGifPanelDrag(x, y, window.ClientWidth(), window.ClientHeight());
            window.Invalidate();
            return;
        }
        // ── 左键拖拽状态机 ──
        // Idle：无左键拖拽
        if (_dragOutState == DragOutState::Idle) return;
        // Returned：拖回窗口后等待释放。窗口内图片静止（不 Pan）；
        //           再次出界则 CheckDragOutTrigger 重新发起拖出（冷却防反复横跳）
        if (_dragOutState == DragOutState::Returned) {
            CheckDragOutTrigger();
            return;
        }

        if (_dragOutState == DragOutState::Arming) {
            // 阈值检测：移动超 SM_CXDRAG（默认 ~4px）才进入平移，避免点击误触发
            int dx = x - _dragStartX, dy = y - _dragStartY;
            int thr = GetSystemMetrics(SM_CXDRAG);
            if (thr < 1) thr = 4;
            if (dx * dx + dy * dy < thr * thr) return;  // 未过阈值，继续 Arming
            _dragOutState = DragOutState::Panning;      // 过阈值，落 Panning 块
        }

        if (_dragOutState == DragOutState::Panning) {
            PerfScope perfMove(L"消息", "WM_MOUSEMOVE");  // 性能遥测：拖拽消息耗时
            imageEngine.Pan((double)(x - _lastX), (double)(y - _lastY));
            _lastX = x; _lastY = y;
            window.Invalidate();
            CheckDragOutTrigger();  // OOB+100ms+冷却 → 转 DragOut + 调 DoDragOut()
            return;
        }
        // DragOut 状态：DoDragDrop 模态循环派发的消息，忽略（IDropSource 控制流程）
    });

    window.OnMouseUp([this](int, int, int) {
        _dragOutState = DragOutState::Idle;
        _oobSince = 0;
        ReleaseCapture();
        _draggingBirdsEye = false; ui.EndExifDrag();
        // GIF 面板拖动结束 → 持久化位置到 Config
        if (ui.IsGifDragging()) {
            ui.EndGifPanelDrag();
            auto cfg = Config::Instance().Get();
            cfg.gifPanelX = ui.GifPanelPosX();
            cfg.gifPanelY = ui.GifPanelPosY();
            Config::Instance().Set(cfg);
        }
    });

    fileWatcher.OnChanged = [this]() {
        lastFileChange = GetTickCount();  // 工作线程仅设时间戳，重载在定时器主线程执行（D2D 安全）
    };

    window.OnDropFile([this](const std::vector<std::wstring>& paths) {
        WindowManager::Instance().OnDropFiles(this, paths);
    });

    // 右键菜单：复制路径/图片、打开文件夹、旋转/翻转、EXIF、删除
    window.OnContextMenu([this](int x, int y) { ShowContextMenu(x, y); });

    window.OnKeyDown([this](int vk, bool down) {
        if (!down) return;
        PerfScope perfKey(L"消息", "WM_KEYDOWN");  // 性能遥测：键盘消息耗时
        switch (vk) {
        case VK_OEM_MINUS: case VK_SUBTRACT:
            FlushPendingNavigate();
            imageEngine.Zoom(1.0 / 1.25, 0, 0); break;
        case VK_OEM_PLUS: case VK_ADD:
            FlushPendingNavigate();
            imageEngine.Zoom(1.25, 0, 0); break;
        case VK_PRIOR:  // PageUp 上一张（Prism 标准翻页键）
            RequestNavigate(-1); break;
        case VK_NEXT:   // PageDown 下一张
            RequestNavigate(1);  break;
        case VK_LEFT:
            RequestNavigate(-1); break;
        case VK_RIGHT:
            RequestNavigate(1);  break;
        case 'F':
            imageEngine.FitToWindow(window.ClientWidth(), window.ClientHeight()); break;
        // ── 视图变换（只改变视图不改变原图）──
        case 'R':
            FlushPendingNavigate();
            imageEngine.RotateLeft(); break;
        case 'H':
            FlushPendingNavigate();
            imageEngine.ToggleFlipH(); break;
        case 'V':
            FlushPendingNavigate();
            imageEngine.ToggleFlipV(); break;
        case 'I':
            ToggleExifPanel(); return;  // 已在内部 Invalidate
        case 'O':
            OpenContainingFolder(); break;
        case VK_F11:
            ToggleFullscreen(); return;  // 已在内部 Invalidate
        case VK_DELETE:
            DeleteCurrentFile(); return;  // 已在内部处理
        case VK_F12:
            // F12：切换活动日志窗口显示/隐藏
            ActivityLog::Instance().Toggle();
            break;
        case VK_ESCAPE:
            // 全屏时 ESC 退出全屏，否则关闭窗口
            if (ui.IsFullscreen()) ToggleFullscreen();
            else window.Close();
            break;
        }
        window.Invalidate();
    });

    window.OnCommand([this](int cmdId) {
        switch (cmdId) {
        case IDM_FILE_OPEN: {
            wchar_t file[MAX_PATH] = {};
            OPENFILENAMEW ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = window.Handle();
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrFilter = L"所有图片\0*.jpg;*.jpeg;*.jpe;*.jfif;*.png;*.webp;*.bmp;*.dib;*.gif;*.tif;*.tiff;*.psd;*.psb;*.hdr;*.pic;*.heic;*.heif;*.hif;*.cr2;*.cr3;*.nef;*.arw;*.dng;*.raf;*.x3f;*.pef;*.rw2;*.orf;*.svg;*.svgz;*.ico;*.cur;*.ani;*.tga;*.dds\0JPEG\0*.jpg;*.jpeg;*.jpe;*.jfif\0PNG\0*.png\0WebP\0*.webp\0BMP\0*.bmp;*.dib\0GIF\0*.gif\0TIFF\0*.tif;*.tiff\0ICO\0*.ico;*.cur\0ANI\0*.ani\0TGA\0*.tga\0DDS\0*.dds\0HEIF\0*.heic;*.heif;*.hif\0RAW\0*.cr2;*.cr3;*.nef;*.arw;*.dng;*.raf;*.x3f;*.pef;*.rw2;*.orf\0PSD\0*.psd;*.psb\0SVG\0*.svg;*.svgz\0HDR\0*.hdr;*.pic\0所有文件\0*.*\0";
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
            if (GetOpenFileNameW(&ofn)) LoadPath(file);  // 菜单打开=替换当前窗口
            break;
        }
        case IDM_FILE_EXIT:
            window.Close(); break;
        case IDM_ZOOM_FIT:
            imageEngine.FitToWindow(window.ClientWidth(), window.ClientHeight()); window.Invalidate(); break;
        case IDM_ZOOM_100:
            imageEngine.ZoomTo100(); window.Invalidate(); break;
        case IDM_VIEW_PREV:
            RequestNavigate(-1); break;
        case IDM_VIEW_NEXT:
            RequestNavigate(1);  break;
        // ── 新增操作功能 ──
        case IDM_VIEW_FULLSCREEN:
            ToggleFullscreen(); break;
        case IDM_VIEW_ROTATE_LEFT:
            FlushPendingNavigate();
            imageEngine.RotateLeft(); window.Invalidate(); break;
        case IDM_VIEW_FLIP_H:
            FlushPendingNavigate();
            imageEngine.ToggleFlipH(); window.Invalidate(); break;
        case IDM_VIEW_FLIP_V:
            FlushPendingNavigate();
            imageEngine.ToggleFlipV(); window.Invalidate(); break;
        case IDM_VIEW_EXIF:
            ToggleExifPanel(); break;
        case IDM_FILE_DELETE:
            DeleteCurrentFile(); break;
        case IDM_EDIT_COPY_PATH:
            CopyPathToClipboard(); break;
        case IDM_EDIT_COPY_IMAGE:
            CopyImageToClipboard(); break;
        case IDM_FILE_OPEN_FOLDER:
            OpenContainingFolder(); break;
        // ── Prism UI 还原：右键菜单扩展 ──
        case IDM_VIEW_ROTATE_RIGHT:
            FlushPendingNavigate();
            imageEngine.RotateRight(); window.Invalidate(); break;
        case IDM_FILE_RENAME:
            RenameCurrentFile(); break;
        case IDM_FILE_SAVEAS:
            SaveAs(); break;
        case IDM_FILE_WALLPAPER:
            SetAsWallpaper(); break;
        // ── 自绘标题栏按钮 ──
        case IDM_TITLEBAR_CLOSE:
            window.Close(); break;
        case IDM_TITLEBAR_MIN:
            ShowWindow(window.Handle(), SW_MINIMIZE); break;
        case IDM_TITLEBAR_MAX:
            // 最大化/还原切换
            if (IsZoomed(window.Handle())) ShowWindow(window.Handle(), SW_RESTORE);
            else ShowWindow(window.Handle(), SW_MAXIMIZE);
            break;
        case IDM_TITLEBAR_MENU:
            // 设置弹窗挂到标题栏 ≡ 按钮（含穿透策略/鸟瞰图/缩略图条/滚轮行为）
            ShowSettingsDialog(); break;
        // ── 工具栏信息/更多按钮（P6 预留，此处先接 EXIF 切换） ──
        case IDM_VIEW_INFO:
            ToggleExifPanel(); break;
        case IDM_VIEW_MORE:
            // 底部 ⋮ 弹出"更多"面板（鸟瞰图/缩略图 toggle 开关）
            ui.ToggleMorePanel();
            window.Invalidate();
            break;
        case IDM_TOGGLE_THUMBBAR: {
            auto cfg = Config::Instance().Get();
            cfg.thumbnailBarVisible = !cfg.thumbnailBarVisible;
            Config::Instance().Set(cfg);
            window.Invalidate();
            break;
        }
        case IDM_TOGGLE_BIRDSEYE: {
            auto cfg = Config::Instance().Get();
            cfg.birdsEyeVisible = !cfg.birdsEyeVisible;
            Config::Instance().Set(cfg);
            window.Invalidate();
            break;
        }
        // ── 文件夹穿透策略设置 ──
        case IDM_FOLDERNAV_LOOP:
        case IDM_FOLDERNAV_PENETRATE:
        case IDM_FOLDERNAV_PROMPT: {
            auto cfg = Config::Instance().Get();
            cfg.folderNavPolicy = cmdId - IDM_FOLDERNAV_LOOP;  // 0/1/2
            Config::Instance().Set(cfg);
            break;
        }
        // ── GIF 动画控制 ──
        case IDM_GIF_PREV:
            imageEngine.AnimSetFrame(imageEngine.AnimCurrentFrame() - 1);
            window.Invalidate();
            break;
        case IDM_GIF_NEXT:
            imageEngine.AnimSetFrame(imageEngine.AnimCurrentFrame() + 1);
            window.Invalidate();
            break;
        case IDM_GIF_PLAYPAUSE:
            imageEngine.AnimTogglePlay();
            window.Invalidate();
            break;
        }
    });

    window.OnTimer([this](UINT_PTR id) {
        if (id != ANIM_TIMER_ID) return;

        // 透明度动画插值（缩略图条/边缘按钮渐显渐隐）
        if (ui.UpdateAnimations()) window.Invalidate();

        // 交互节流：滚轮停手超时则切层+排瓦片，触发真正解码
        imageEngine.CheckInteractionTimeout();
        // 3 分钟空闲超时：移除非当前顶层预览
        imageEngine.CheckIdleTimeout(window.Handle());

        // 底部状态栏：显示当前活动摘要（活动日志窗口的最近一条）
        auto activity = ActivityLog::Instance().LastActivity();
        if (!activity.empty()) ui.SetStatusText(activity);

        // 文件变更检测：工作线程设时间戳，主线程在此重载（D2D 安全），防抖 200ms
        DWORD lastChange = lastFileChange.load();
        if (lastChange > 0 && GetTickCount() - lastChange >= 200) {
            lastFileChange = 0;
            // 抑制自身另存为引发的目录变更通知（避免无谓重载导致画面闪退）
            if (GetTickCount() < watchSuppressUntil.load(std::memory_order_acquire)) {
                return;  // 静默忽略：保存后短窗内的变更由本程序写入，不需重载
            }
            const auto& path = imageEngine.FilePath();
            if (!path.empty()) {
                imageEngine.LoadFile(path);
                fileWatcher.Watch(path);
                window.Invalidate();
            }
            return;  // 本帧已重载，跳过动画与瓦片刷新
        }

        // GIF 动画帧推进
        if (imageEngine.IsPlayingAnimation()) {
            imageEngine.UpdateAnimation();
            window.Invalidate();
            return;
        }

        // 瓦片解码期间周期重绘：后台贴纹理后无重绘消息，需主动刷新看"逐步变清晰"
        // _tileBusy: 队列仍有瓦片待解码时 true；_tileUpdated: 每块贴入后 true，弥补队列清空后 busy=false 的时序漏洞
        // _bgPending: 异步顶层解码中，需周期重绘让 ApplyBgResultIfReady 检测结果就绪并切换显示
        if (imageEngine.TileBusy().load() ||
            imageEngine.TileUpdated().exchange(false, std::memory_order_acq_rel) ||
            imageEngine.IsBgDecodePending()) {
            window.Invalidate();
        }
    });

    window.OnDestroy([this]() {
        WindowManager::Instance().RequestClose(this);
    });

    // 单实例 IPC：另一进程传路径过来，新建窗口显示
    window.OnIpcOpen([](const std::wstring& path) {
        WindowManager::Instance().CreateNewWindow(path, SW_SHOW);
    });
}

// ─── 新增操作功能实现 ───

// OOB 检测：Panning 态每帧调用，鼠标移出客户区 ≥28px 持续 30ms 且过 30ms 冷却 → 触发拖出
void WindowContext::CheckDragOutTrigger() {
    POINT pt;
    if (!GetCursorPos(&pt) || !ScreenToClient(window.Handle(), &pt)) return;
    int cw = window.ClientWidth(), ch = window.ClientHeight();
    constexpr int OOB_MARGIN = 28;
    bool oob = pt.x < -OOB_MARGIN || pt.x > cw + OOB_MARGIN ||
               pt.y < -OOB_MARGIN || pt.y > ch + OOB_MARGIN;
    DWORD now = GetTickCount();
    if (!oob) { _oobSince = 0; return; }
    if (_oobSince == 0) { _oobSince = now; return; }
    if (now - _oobSince < 30) return;           // 持续不足 30ms，等
    if (now - _lastDragOutTick < 30) return;    // 30ms 冷却防抖
    DoDragOut();  // 满足条件，阻塞触发拖出
}

// 触发 OLE 拖出：FitToWindow 恢复标准视图 → DoDragDrop 阻塞 → 按结果切状态
void WindowContext::DoDragOut() {
    const auto& path = imageEngine.FilePath();
    if (path.empty()) { _dragOutState = DragOutState::Idle; return; }

    // 触发即恢复标准视图（用户要求：拖出时图片居中+高度适应，平移偏移归零）
    imageEngine.FitToWindow(window.ClientWidth(), window.ClientHeight());
    window.Invalidate();

    // 仅传文件路径，不读 GPU 像素/PreDecodeCache 做缩略图——DoDragDrop 是模态调用，
    // 模态循环里读 GPU/取缓存锁会死锁，故只用系统标准拖拽光标
    _dragOutState = DragOutState::DragOut;
    bool resumePan = DragOut::DoDragOut(window.Handle(), window.ClientWidth(),
        window.ClientHeight(), path);
    _lastDragOutTick = GetTickCount();
    _oobSince = 0;

    if (resumePan) {
        // 拖回客户区取消且左键仍按住：进入 Returned 态，
        // 图片保持 FitToWindow 的标准视图静止，等 WM_LBUTTONUP 才回 Idle
        _dragOutState = DragOutState::Returned;
        // DoDragDrop 模态循环结束时 OLE 已 ReleaseCapture，重新捕获鼠标，
        // 否则 Returned 态鼠标移出窗口后收不到 WM_MOUSEMOVE，无法再次触发拖出
        SetCapture(window.Handle());
    } else {
        _dragOutState = DragOutState::Idle;
    }
}

// 全屏切换（F11 / 工具栏按钮 / ESC 退出全屏）
void WindowContext::ToggleFullscreen() {
    window.ToggleFullscreen();
    ui.SetFullscreen(window.IsFullscreen());
    // 全屏后客户区尺寸变化，重新适配图片
    imageEngine.OnResize(window.ClientWidth(), window.ClientHeight());
    imageEngine.FitToWindow(window.ClientWidth(), window.ClientHeight());
    if (imageEngine.HasImage()) imageEngine.CheckInteractionTimeout();
    window.Invalidate();
}

// EXIF 面板显示/隐藏（I 键），首次开启时解析当前图片 EXIF
void WindowContext::ToggleExifPanel() {
    if (!ui.IsExifPanelVisible()) {
        const auto& path = imageEngine.FilePath();
        if (!path.empty()) {
            // 传入已知尺寸：WIC 不支持的格式（PSD/HDR）用 ImageEngine 尺寸兜底
            ui.SetExifInfo(ExifParser::Parse(path,
                imageEngine.SrcWidth(), imageEngine.SrcHeight()));
        }
    }
    ui.ToggleExifPanel();
    window.Invalidate();
}

// 删除当前文件到回收站（Del 键）
void WindowContext::DeleteCurrentFile() {
    const auto& path = imageEngine.FilePath();
    if (path.empty()) return;
    // SHFileOperationW 需要双 \0 终止的路径串
    std::wstring buf = path + L'\0';
    SHFILEOPSTRUCTW op = {};
    op.hwnd = window.Handle();
    op.wFunc = FO_DELETE;
    op.pFrom = buf.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
    if (SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted) {
        imageEngine.OnFileDeleted();  // 从列表移除并导航到相邻文件
        const auto& newPath = imageEngine.FilePath();
        if (!newPath.empty()) {
            std::wstring title = L"Ark Viewer 2 - " + newPath.substr(newPath.find_last_of(L"\\/") + 1);
            window.SetTitle(title.c_str());
            fileWatcher.Watch(newPath);
        }
        window.Invalidate();
    } else {
        MessageBoxDlg::Show(window.Handle(), L"删除失败", L"提示", false);
    }
}

// 复制当前文件路径到剪贴板
void WindowContext::CopyPathToClipboard() {
    const auto& path = imageEngine.FilePath();
    if (path.empty()) return;
    size_t bytes = (path.size() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!hMem) return;
    memcpy(GlobalLock(hMem), path.c_str(), bytes);
    GlobalUnlock(hMem);
    OpenClipboard(window.Handle());
    EmptyClipboard();
    SetClipboardData(CF_UNICODETEXT, hMem);
    CloseClipboard();
}

// 复制当前显示图片到剪贴板（CF_DIB 格式）
void WindowContext::CopyImageToClipboard() {
    std::vector<uint8_t> pixels;
    int w, h;
    if (!imageEngine.ReadDisplayPixels(pixels, w, h)) return;
    int stride = w * 4;  // 32-bit BGRA，行步长 4 字节对齐
    int srcStride = (int)(pixels.size() / h);  // D2D mapped pitch（可能有对齐填充）
    size_t dataSize = sizeof(BITMAPINFOHEADER) + (size_t)stride * h;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, dataSize);
    if (!hMem) return;
    auto* p = (uint8_t*)GlobalLock(hMem);
    auto* bih = (BITMAPINFOHEADER*)p;
    bih->biSize = sizeof(BITMAPINFOHEADER);
    bih->biWidth = w;
    bih->biHeight = h;        // 正值 = bottom-up
    bih->biPlanes = 1;
    bih->biBitCount = 32;
    bih->biCompression = BI_RGB;
    bih->biSizeImage = (DWORD)((size_t)stride * h);
    bih->biXPelsPerMeter = 0;
    bih->biYPelsPerMeter = 0;
    bih->biClrUsed = 0;
    bih->biClrImportant = 0;
    // D2D 像素 top-down，CF_DIB 正 biHeight 是 bottom-up，需翻转行序
    uint8_t* dst = p + sizeof(BITMAPINFOHEADER);
    for (int y = 0; y < h; y++) {
        memcpy(dst + y * stride, pixels.data() + (size_t)(h - 1 - y) * srcStride, stride);
    }
    GlobalUnlock(hMem);
    OpenClipboard(window.Handle());
    EmptyClipboard();
    SetClipboardData(CF_DIB, hMem);
    CloseClipboard();
}

// 打开所在文件夹并选中当前文件（O 键）
void WindowContext::OpenContainingFolder() {
    const auto& path = imageEngine.FilePath();
    if (path.empty()) return;
    std::wstring params = L"/select,\"" + path + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
}

// 重命名当前文件：GetSaveFileNameW 输入新名 → MoveFileW → 更新路径/列表/标题栏
// 重命名语义：强制保留在原目录（只取文件名部分），不移动文件
void WindowContext::RenameCurrentFile() {
    const auto& oldPath = imageEngine.FilePath();
    if (oldPath.empty()) return;

    size_t slash = oldPath.find_last_of(L"\\/");
    std::wstring dir = oldPath.substr(0, slash + 1);    // 含分隔符
    std::wstring oldName = oldPath.substr(slash + 1);

    wchar_t file[MAX_PATH] = {};
    StringCchCopyW(file, MAX_PATH, oldName.c_str());
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = window.Handle();
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"重命名 - 输入新文件名";
    ofn.lpstrInitialDir = dir.c_str();
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return;  // 用户取消

    // 只取文件名部分：强制保留原目录，重命名不移动文件
    std::wstring input = ofn.lpstrFile;
    size_t newSlash = input.find_last_of(L"\\/");
    std::wstring newName = (newSlash != std::wstring::npos)
                         ? input.substr(newSlash + 1) : input;
    if (newName.empty() || newName == oldName) return;

    std::wstring targetPath = dir + newName;
    if (!MoveFileW(oldPath.c_str(), targetPath.c_str())) {
        MessageBoxDlg::Show(window.Handle(), L"重命名失败：文件被占用或名称无效", L"提示", false);
        return;
    }

    // 更新路径 + 文件列表（不重新解码，纹理复用）+ 文件监视 + 标题栏 + 缓存失效
    imageEngine.RenameCurrentPath(targetPath);
    fileWatcher.Watch(targetPath);
    _lastSyncPath.clear();  // 强制 OnPaint 重新查文件大小
    window.SetTitle((L"Ark Viewer 2 - " + newName).c_str());
    window.Invalidate();
}

// ─── 另存为辅助 ───

// 输出格式
enum class OutFmt { Jpg, Png, Webp, Bmp };

// 手写 32 位 BGRA BMP（BI_RGB，底向上行序），输出到 out
static bool EncodeBmp(const uint8_t* bgra, int w, int h, int stride, std::vector<uint8_t>& out) {
    if (!bgra || w <= 0 || h <= 0 || stride < w * 4) return false;
    const int rowSize = w * 4;  // 32bpp 行宽恒 4 字节对齐
    BITMAPFILEHEADER fh = {};
    BITMAPINFOHEADER ih = {};
    fh.bfType = 0x4D42;                       // 'BM'
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = h;                          // 正值 = 底向上
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = (DWORD)(rowSize * h);
    fh.bfSize = fh.bfOffBits + ih.biSizeImage;

    out.resize(fh.bfSize);
    memcpy(out.data(), &fh, sizeof(fh));
    memcpy(out.data() + sizeof(fh), &ih, sizeof(ih));
    // 底向上：BMP 第 0 行 = 源图最后一行
    BYTE* dst = out.data() + fh.bfOffBits;
    for (int y = 0; y < h; y++) {
        const uint8_t* srcRow = bgra + (h - 1 - y) * stride;
        memcpy(dst + y * rowSize, srcRow, rowSize);
    }
    return true;
}

// 自动生成不重名的文件名：原名_ark(n).新扩展名（返回仅文件名，不含目录）
// 在 srcPath 所在目录下检查存在性，递增 n 直到不重名
static std::wstring BuildAutoName(const std::wstring& srcPath, const std::wstring& outExt) {
    auto slash = srcPath.find_last_of(L"\\/");
    std::wstring dir = (slash != std::wstring::npos) ? srcPath.substr(0, slash) : L"";
    std::wstring fname = (slash != std::wstring::npos) ? srcPath.substr(slash + 1) : srcPath;
    auto dot = fname.find_last_of(L'.');
    std::wstring base = (dot != std::wstring::npos) ? fname.substr(0, dot) : fname;
    std::wstring ext = outExt;
    if (!ext.empty() && ext[0] == L'.') ext = ext.substr(1);  // 去前导点
    for (int n = 1; n < 10000; n++) {
        std::wstring name = base + L"_ark(" + std::to_wstring(n) + L")." + ext;
        std::wstring full = dir.empty() ? name : (dir + L"\\" + name);
        if (!PathFileExistsW(full.c_str())) return name;
    }
    return base + L"_ark(1)." + ext;  // 兜底
}

// 另存为：支持 JPG/PNG/WebP/BMP 格式转换；初始名自动 _ark(n) 防重名
void WindowContext::SaveAs() {
    const auto& src = imageEngine.FilePath();
    if (src.empty()) return;

    // 初始文件名：原名_ark(1).jpg（默认 JPG），用全路径让对话框定位到源目录
    auto slash = src.find_last_of(L"\\/");
    std::wstring srcDir = (slash != std::wstring::npos) ? src.substr(0, slash) : L"";
    std::wstring initPath = (srcDir.empty() ? L"" : (srcDir + L"\\"))
                          + BuildAutoName(src, L"jpg");
    wchar_t file[MAX_PATH] = {};
    StringCchCopyW(file, MAX_PATH, initPath.c_str());

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = window.Handle();
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"另存为";
    // 过滤列表顺序决定 nFilterIndex：1=JPG 2=PNG 3=WebP 4=BMP 5=所有文件
    ofn.lpstrFilter = L"JPEG 图像\0*.jpg;*.jpeg\0PNG 图像\0*.png\0WebP 图像\0*.webp\0BMP 图像\0*.bmp\0所有文件\0*.*\0";
    ofn.lpstrDefExt = L"jpg";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;

    // 确定输出格式：优先 nFilterIndex，"所有文件"按用户输入扩展名推断
    auto lowerExt = [](const std::wstring& path) -> std::wstring {
        auto d = path.find_last_of(L'.');
        auto s = path.find_last_of(L"\\/");
        if (d == std::wstring::npos || (s != std::wstring::npos && d < s)) return L"";
        std::wstring e = path.substr(d + 1);
        for (auto& c : e) c = (wchar_t)towlower(c);
        return e;
    };
    std::wstring typedExt = lowerExt(ofn.lpstrFile);
    OutFmt fmt = OutFmt::Jpg;
    std::wstring ext = L"jpg";
    if (ofn.nFilterIndex == 2 || typedExt == L"png")       { fmt = OutFmt::Png;  ext = L"png"; }
    else if (ofn.nFilterIndex == 3 || typedExt == L"webp") { fmt = OutFmt::Webp; ext = L"webp"; }
    else if (ofn.nFilterIndex == 4 || typedExt == L"bmp")  { fmt = OutFmt::Bmp;  ext = L"bmp"; }

    // 确保输出路径扩展名与所选格式一致
    std::wstring outPath = ofn.lpstrFile;
    if (typedExt != ext) {
        auto d = outPath.find_last_of(L'.');
        auto s = outPath.find_last_of(L"\\/");
        if (d != std::wstring::npos && (s == std::wstring::npos || d > s))
            outPath = outPath.substr(0, d) + L"." + ext;   // 替换扩展名
        else
            outPath += L"." + ext;                          // 追加
    }

    // 读原图全分辨率像素（瓦片大图不能读显示层，否则丢分辨率）
    std::vector<uint8_t> pixels;
    int w, h, stride;
    if (!imageEngine.ReadSourcePixels(pixels, w, h, stride)) {
        MessageBoxDlg::Show(window.Handle(), L"读取像素失败", L"错误", false);
        return;
    }

    // JPEG：弹质量滑条（不写盘，暂不抑制监视）
    int jpgQuality = -1;
    if (fmt == OutFmt::Jpg) {
        SaveAsDialog dlg;
        jpgQuality = dlg.Show(window.Handle(), pixels.data(), w, h, stride);
        if (jpgQuality < 0) return;  // 取消
    }

    // 即将写盘：抑制目录监视（保存写入触发 FileWatcher → 重载当前图导致闪退）
    // 写盘前设 3s 覆盖写入期间通知，写盘后再刷新覆盖写入后通知延迟
    watchSuppressUntil.store(GetTickCount() + 3000, std::memory_order_release);

    std::vector<uint8_t> buf;  // JPG/WebP/BMP 编码缓冲（PNG 直接写文件）
    bool ok = false;
    switch (fmt) {
        case OutFmt::Jpg:  ok = JpegDecoder::EncodeJpeg(pixels.data(), w, h, stride, jpgQuality, buf); break;
        case OutFmt::Png:  ok = WicDecoder::EncodePng(pixels.data(), w, h, stride, outPath.c_str()); break;
        case OutFmt::Webp: ok = WebpDecoder::EncodeWebp(pixels.data(), w, h, stride, 90, buf); break;
        case OutFmt::Bmp:  ok = EncodeBmp(pixels.data(), w, h, stride, buf); break;
    }
    if (!ok) {
        MessageBoxDlg::Show(window.Handle(), L"编码失败", L"错误", false);
        return;
    }

    // PNG 已由 EncodePng 直接写文件；其余写缓冲到磁盘
    if (fmt != OutFmt::Png) {
        HANDLE hf = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hf == INVALID_HANDLE_VALUE) {
            MessageBoxDlg::Show(window.Handle(), L"写入文件失败", L"错误", false);
            return;
        }
        DWORD written = 0;
        WriteFile(hf, buf.data(), (DWORD)buf.size(), &written, nullptr);
        CloseHandle(hf);
    }
    // 写盘完成：刷新抑制窗口，确保文件系统延迟到达的变更通知也被忽略
    watchSuppressUntil.store(GetTickCount() + 3000, std::memory_order_release);
    ui.SetStatusText(L"已另存为: " + outPath);
}

// 设为系统壁纸：SystemParametersInfoW(SPI_SETDESKWALLPAPER)
void WindowContext::SetAsWallpaper() {
    const auto& path = imageEngine.FilePath();
    if (path.empty()) return;
    wchar_t buf[MAX_PATH] = {};
    StringCchCopyW(buf, MAX_PATH, path.c_str());
    if (SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, buf, SPIF_UPDATEINIFILE)) {
        ui.SetStatusText(L"已设为壁纸: " + path);
    } else {
        MessageBoxDlg::Show(window.Handle(), L"设为壁纸失败", L"错误", false);
    }
}

// ─── 菜单全项自绘基础设施 ───
// 所有菜单项 MF_OWNERDRAW，itemData 指向 MenuItemData（new 分配，DestroyMenu 前由 FreeMenuData 释放）
// 与主界面深色主题统一：背景 #2C2C2C / 选中 #3A3A3A + 左侧 2px accent 绿竖条

// RGB 宏在引入 commctrl.h 后部分场景不可见，用内联函数构造 COLORREF
static COLORREF Cf(int r, int g, int b) {
    return (COLORREF)((BYTE)r | ((WORD)(BYTE)g << 8) | ((DWORD)(BYTE)b << 16));
}

// 自绘项数据（AppendMenuW 第 4 参数作为 itemData 传入）
struct MenuItemData {
    std::wstring label;     // 主文字
    std::wstring shortcut;  // 快捷键提示（如 "(O)"），空则不显示
    bool isSep = false;     // 分隔线（自绘 1px 横线）
    bool isDanger = false;  // 删除项红字
    bool isPopup = false;   // 含子菜单（右侧画 ▸ 箭头）
    bool isChecked = false; // 打勾（设置子菜单当前策略）
};

// 统一菜单字体（懒创建，程序生命周期复用，OS 退出时回收）
static HFONT MenuFont() {
    static HFONT f = nullptr;
    if (!f) {
        NONCLIENTMETRICSW ncm = { sizeof(ncm) };
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        f = CreateFontIndirectW(&ncm.lfMenuFont);
    }
    return f;
}

// 深色背景画刷（静态复用，避免反复创建/销毁）
static HBRUSH MenuBgBrush() {
    static HBRUSH b = CreateSolidBrush(Cf(0x2C, 0x2C, 0x2C));
    return b;
}

static void SetupMenuInfo(HMENU h) {
    MENUINFO mi = { sizeof(mi) };
    mi.fMask = MIM_BACKGROUND;
    mi.hbrBack = MenuBgBrush();
    SetMenuInfo(h, &mi);
}

// 测量文字宽度（用菜单字体，供 WM_MEASUREITEM 自适应菜单宽度）
static int MeasureTextW(const std::wstring& s) {
    if (s.empty()) return 0;
    HDC hdc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(hdc, MenuFont());
    SIZE sz = {};
    GetTextExtentPoint32W(hdc, s.c_str(), (int)s.size(), &sz);
    SelectObject(hdc, old);
    ReleaseDC(nullptr, hdc);
    return sz.cx;
}

// 添加普通菜单项（MF_OWNERDRAW）
static void AddItem(HMENU h, UINT id, const wchar_t* label,
                    const wchar_t* shortcut = nullptr,
                    bool danger = false, bool checked = false) {
    auto* d = new MenuItemData{ label ? label : L"", shortcut ? shortcut : L"",
                                false, danger, false, checked };
    AppendMenuW(h, MF_STRING | MF_OWNERDRAW, id, (LPCWSTR)d);
}

// 添加分隔线（owner-draw 自绘 1px 横线）
static void AddSep(HMENU h) {
    auto* d = new MenuItemData{ L"", L"", true, false, false, false };
    AppendMenuW(h, MF_SEPARATOR | MF_OWNERDRAW, 0, (LPCWSTR)d);
}

// 添加子菜单项（MF_POPUP | MF_OWNERDRAW，右侧画 ▸ 箭头）
static void AddPopup(HMENU h, HMENU sub, const wchar_t* label) {
    auto* d = new MenuItemData{ label ? label : L"", L"", false, false, true, false };
    AppendMenuW(h, MF_POPUP | MF_OWNERDRAW, (UINT_PTR)sub, (LPCWSTR)d);
}

// 递归释放菜单 itemData（DestroyMenu 前调用，避免内存泄漏）
static void FreeMenuData(HMENU h) {
    int n = GetMenuItemCount(h);
    for (int i = 0; i < n; i++) {
        MENUITEMINFOW mii = { sizeof(mii) };
        mii.fMask = MIIM_SUBMENU | MIIM_DATA;
        if (GetMenuItemInfoW(h, i, TRUE, &mii)) {
            if (mii.hSubMenu) FreeMenuData(mii.hSubMenu);
            if (mii.dwItemData) delete (MenuItemData*)mii.dwItemData;
        }
    }
}

// owner-draw 子类化过程：统一处理所有菜单项的 WM_MEASUREITEM/WM_DRAWITEM
// TrackPopupMenu 期间临时子类化窗口，结束后移除（不改 PlatformWindow）
// 子菜单项的 WM_MEASUREITEM/WM_DRAWITEM 也路由到同一 owner 窗口，无需额外处理
static LRESULT CALLBACK OwnerDrawSubclass(HWND hwnd, UINT msg, WPARAM w, LPARAM l,
                                          UINT_PTR /*id*/, DWORD_PTR /*ref*/) {
    if (msg == WM_MEASUREITEM) {
        auto* mis = reinterpret_cast<MEASUREITEMSTRUCT*>(l);
        if (mis->CtlType != ODT_MENU) return DefSubclassProc(hwnd, msg, w, l);
        auto* d = reinterpret_cast<MenuItemData*>(mis->itemData);
        if (!d) return TRUE;
        if (d->isSep) {
            mis->itemHeight = 6;   // 分隔线小高度
            mis->itemWidth = 0;
            return TRUE;
        }
        // 高度统一，宽度按文字自适应（系统取所有项最大值作为菜单宽）
        mis->itemHeight = GetSystemMetrics(SM_CYMENU) + 8;
        int wid = 28;  // 左缩进（与系统菜单文字缩进对齐）
        wid += MeasureTextW(d->label);
        if (!d->shortcut.empty()) wid += 6 + MeasureTextW(d->shortcut);
        wid += d->isPopup ? 24 : 12;  // 右边距（子菜单留箭头空间）
        mis->itemWidth = wid;
        return TRUE;
    }
    if (msg == WM_DRAWITEM) {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(l);
        if (dis->CtlType != ODT_MENU) return DefSubclassProc(hwnd, msg, w, l);
        auto* d = reinterpret_cast<MenuItemData*>(dis->itemData);
        if (!d) return TRUE;
        HDC hdc = dis->hDC;
        RECT rc = dis->rcItem;

        // 分隔线：深色背景 + 1px #444 横线垂直居中
        if (d->isSep) {
            FillRect(hdc, &rc, MenuBgBrush());
            int y = (rc.top + rc.bottom) / 2;
            HPEN pen = CreatePen(PS_SOLID, 1, Cf(0x44, 0x44, 0x44));
            HGDIOBJ old = SelectObject(hdc, pen);
            MoveToEx(hdc, rc.left + 10, y, nullptr);
            LineTo(hdc, rc.right - 10, y);
            SelectObject(hdc, old);
            DeleteObject(pen);
            return TRUE;
        }

        bool sel = (dis->itemState & ODS_SELECTED) != 0;
        bool disabled = (dis->itemState & ODS_DISABLED) != 0;

        // 背景：选中 #3A3A3A，否则 #2C2C2C
        COLORREF bg = sel ? Cf(0x3A, 0x3A, 0x3A) : Cf(0x2C, 0x2C, 0x2C);
        HBRUSH bgBrush = CreateSolidBrush(bg);
        FillRect(hdc, &rc, bgBrush);
        DeleteObject(bgBrush);

        // 选中项左侧 2px accent 绿竖条（与主界面交互一致）
        if (sel) {
            RECT bar = { rc.left, rc.top, rc.left + 2, rc.bottom };
            HBRUSH g = CreateSolidBrush(Cf(0x90, 0xC2, 0x08));
            FillRect(hdc, &bar, g);
            DeleteObject(g);
        }

        // 文字颜色：删除项红 / 禁用项灰 / 普通 #CCC
        COLORREF fg;
        if (d->isDanger)         fg = Cf(0xFF, 0x6B, 0x6B);
        else if (disabled)       fg = Cf(0x66, 0x66, 0x66);
        else                     fg = Cf(0xCC, 0xCC, 0xCC);

        SetTextColor(hdc, fg);
        SetBkMode(hdc, TRANSPARENT);
        HGDIOBJ oldFont = SelectObject(hdc, MenuFont());

        // 打勾（设置子菜单当前策略项）
        if (d->isChecked) {
            RECT rcChk = rc;
            rcChk.left += 8;
            rcChk.right = rcChk.left + 18;
            DrawTextW(hdc, L"✓", -1, &rcChk, DT_VCENTER | DT_SINGLELINE);
        }

        // 主文字（左缩进 28，右侧为子菜单箭头留 24px）
        RECT rcText = rc;
        rcText.left += 28;
        rcText.right -= d->isPopup ? 24 : 10;
        DrawTextW(hdc, d->label.c_str(), -1, &rcText, DT_VCENTER | DT_SINGLELINE);

        // 快捷键提示紧跟主文字右侧，#888 灰色
        if (!d->shortcut.empty()) {
            SIZE sz;
            GetTextExtentPoint32W(hdc, d->label.c_str(), (int)d->label.size(), &sz);
            RECT rcSc = rcText;
            rcSc.left += sz.cx + 6;
            SetTextColor(hdc, Cf(0x88, 0x88, 0x88));
            DrawTextW(hdc, d->shortcut.c_str(), -1, &rcSc, DT_VCENTER | DT_SINGLELINE);
        }

        // 子菜单箭头 ▸（右侧居中）
        if (d->isPopup) {
            RECT rcAr = rc;
            rcAr.left = rc.right - 22;
            rcAr.right = rc.right - 6;
            SetTextColor(hdc, disabled ? Cf(0x66, 0x66, 0x66) : Cf(0xCC, 0xCC, 0xCC));
            DrawTextW(hdc, L"▸", -1, &rcAr, DT_VCENTER | DT_SINGLELINE | DT_CENTER);
        }

        SelectObject(hdc, oldFont);
        return TRUE;
    }
    return DefSubclassProc(hwnd, msg, w, l);
}

// ─── 菜单窗口边框深色化 ───
// 系统在 MF_OWNERDRAW 菜单项外围画一圈默认浅色边框（白/亮灰 1px），与深色主题冲突。
// 方案：TrackPopupMenu 前装 WH_CBT hook，捕获菜单窗口(#32768)创建并子类化，
//       在 WM_NCPAINT 中用深色画刷重绘非客户区边框框（替代系统浅色描边）。
//       先调 DefSubclassProc 保留默认渲染（含 DWM 阴影合成），再覆盖边框框为 #555。
//       子菜单嵌套时每个 #32768 窗口各自子类化，互不影响。
static HHOOK g_menuCbtHook = nullptr;

static LRESULT CALLBACK MenuNcSubclass(HWND hwnd, UINT msg, WPARAM w, LPARAM l,
                                       UINT_PTR /*id*/, DWORD_PTR /*ref*/) {
    if (msg == WM_NCPAINT) {
        LRESULT r = DefSubclassProc(hwnd, msg, w, l);  // 先默认绘制（保留阴影合成）
        RECT rcWin, rcClient;
        GetWindowRect(hwnd, &rcWin);
        GetClientRect(hwnd, &rcClient);
        POINT ptLT = { 0, 0 };
        ClientToScreen(hwnd, &ptLT);
        // 非客户区四边宽度（窗口坐标系）
        int bxL = ptLT.x - rcWin.left;
        int bxT = ptLT.y - rcWin.top;
        int bxR = rcWin.right  - (ptLT.x + rcClient.right);
        int bxBo = rcWin.bottom - (ptLT.y + rcClient.bottom);
        int W = rcWin.right - rcWin.left;
        int H = rcWin.bottom - rcWin.top;
        HDC hdc = GetWindowDC(hwnd);
        if (hdc) {
            HBRUSH br = CreateSolidBrush(Cf(0x55, 0x55, 0x55));
            if (bxT  > 0) { RECT r = {0, 0, W, bxT};        FillRect(hdc, &r, br); }
            if (bxBo > 0) { RECT r = {0, H - bxBo, W, H};   FillRect(hdc, &r, br); }
            if (bxL  > 0) { RECT r = {0, 0, bxL, H};        FillRect(hdc, &r, br); }
            if (bxR  > 0) { RECT r = {W - bxR, 0, W, H};    FillRect(hdc, &r, br); }
            DeleteObject(br);
            ReleaseDC(hwnd, hdc);
        }
        return r;
    }
    return DefSubclassProc(hwnd, msg, w, l);
}

// CBT hook：捕获菜单窗口(#32768)创建，子类化以自绘 NC 边框
static LRESULT CALLBACK MenuCbtHook(int code, WPARAM w, LPARAM l) {
    if (code == HCBT_CREATEWND) {
        HWND hwnd = (HWND)w;
        wchar_t cls[16] = {};
        if (GetClassNameW(hwnd, cls, 16) > 0 && wcscmp(cls, L"#32768") == 0) {
            SetWindowSubclass(hwnd, MenuNcSubclass, 2, 0);
        }
    }
    return CallNextHookEx(g_menuCbtHook, code, w, l);
}

// 右键菜单：全项 MF_OWNERDRAW 自绘，与主界面深色主题统一
// 深色背景 #2C2C2C / 选中 #3A3A3A + 左侧 accent 绿竖条 / 删除项红字 / 快捷键灰色
void WindowContext::ShowContextMenu(int screenX, int screenY) {
    HMENU hMenu = CreatePopupMenu();
    SetupMenuInfo(hMenu);

    AddItem(hMenu, IDM_EDIT_COPY_PATH,   L"复制路径");
    AddItem(hMenu, IDM_EDIT_COPY_IMAGE,  L"复制图片");
    AddItem(hMenu, IDM_FILE_OPEN_FOLDER, L"打开所在文件夹", L"(O)");
    AddSep(hMenu);
    // 旋转/翻转子菜单（含顺时针旋转）——子菜单项同样 MF_OWNERDRAW
    HMENU hSub = CreatePopupMenu();
    SetupMenuInfo(hSub);
    AddItem(hSub, IDM_VIEW_ROTATE_LEFT,  L"逆时针旋转", L"(R)");
    AddItem(hSub, IDM_VIEW_ROTATE_RIGHT, L"顺时针旋转");
    AddItem(hSub, IDM_VIEW_FLIP_H,       L"水平翻转",   L"(H)");
    AddItem(hSub, IDM_VIEW_FLIP_V,       L"垂直翻转",   L"(V)");
    AddPopup(hMenu, hSub, L"旋转/翻转");
    AddItem(hMenu, IDM_VIEW_EXIF, L"EXIF 信息", L"(I)");
    AddSep(hMenu);
    AddItem(hMenu, IDM_FILE_RENAME,    L"重命名");
    AddItem(hMenu, IDM_FILE_SAVEAS,    L"另存为");
    AddItem(hMenu, IDM_FILE_WALLPAPER, L"设为壁纸");
    AddSep(hMenu);
    AddItem(hMenu, IDM_FILE_DELETE, L"删除到回收站", L"(Del)", true);  // danger 红字

    // 临时子类化窗口处理所有项的 WM_MEASUREITEM/WM_DRAWITEM
    HWND hwnd = window.Handle();
    SetWindowSubclass(hwnd, OwnerDrawSubclass, 1, 0);
    // CBT hook 捕获菜单窗口创建，子类化 WM_NCPAINT 绘制深色边框
    g_menuCbtHook = SetWindowsHookExW(WH_CBT, MenuCbtHook, nullptr, GetCurrentThreadId());
    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
        screenX, screenY, 0, hwnd, nullptr);
    if (g_menuCbtHook) { UnhookWindowsHookEx(g_menuCbtHook); g_menuCbtHook = nullptr; }
    RemoveWindowSubclass(hwnd, OwnerDrawSubclass, 1);
    FreeMenuData(hMenu);  // 释放 itemData（含子菜单递归）
    DestroyMenu(hMenu);
    if (cmd > 0) {
        PostMessage(hwnd, WM_COMMAND, cmd, 0);  // 复用 OnCommand 处理
    }
}

// 汉堡菜单：打开文件 / 设置（文件夹穿透策略）/ 退出
// 同右键菜单风格，全项 MF_OWNERDRAW 自绘
void WindowContext::ShowHamburgerMenu() {
    // 汉堡按钮在标题栏右侧第 5 个位置，菜单在其下方弹出
    RECT rc;
    GetWindowRect(window.Handle(), &rc);
    float dpiScale = ui.DpiScale();
    int sx = rc.right - (int)(UI_TITLEBAR_HEIGHT * dpiScale) * 5;  // 汉堡按钮左侧
    int sy = rc.top + (int)(UI_TITLEBAR_HEIGHT * dpiScale);        // 标题栏底部

    HMENU hMenu = CreatePopupMenu();
    SetupMenuInfo(hMenu);
    AddItem(hMenu, IDM_FILE_OPEN, L"打开文件");
    AddSep(hMenu);

    // 设置子菜单：文件夹穿透策略（三选一，当前策略打勾）
    HMENU hSet = CreatePopupMenu();
    SetupMenuInfo(hSet);
    int policy = Config::Instance().Get().folderNavPolicy;
    AddItem(hSet, IDM_FOLDERNAV_LOOP,      L"本文件夹循环",        nullptr, false, policy == 0);
    AddItem(hSet, IDM_FOLDERNAV_PENETRATE, L"进入下个文件夹",      nullptr, false, policy == 1);
    AddItem(hSet, IDM_FOLDERNAV_PROMPT,    L"提示是否进入下个文件夹", nullptr, false, policy == 2);
    AddPopup(hMenu, hSet, L"设置");

    AddSep(hMenu);
    AddItem(hMenu, IDM_FILE_EXIT, L"退出");

    HWND hwnd = window.Handle();
    SetWindowSubclass(hwnd, OwnerDrawSubclass, 1, 0);
    // CBT hook 捕获菜单窗口创建，子类化 WM_NCPAINT 绘制深色边框
    g_menuCbtHook = SetWindowsHookExW(WH_CBT, MenuCbtHook, nullptr, GetCurrentThreadId());
    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
        sx, sy, 0, hwnd, nullptr);
    if (g_menuCbtHook) { UnhookWindowsHookEx(g_menuCbtHook); g_menuCbtHook = nullptr; }
    RemoveWindowSubclass(hwnd, OwnerDrawSubclass, 1);
    FreeMenuData(hMenu);
    DestroyMenu(hMenu);
    if (cmd > 0) PostMessage(hwnd, WM_COMMAND, cmd, 0);
}

// 设置弹窗：D2D 自绘面板（左侧竖向 Tab + 内容区 + 确定按钮）
// 与主界面统一深色主题，完全自绘无系统控件
void WindowContext::ShowSettingsDialog() {
    SettingsPanel panel;
    panel.Show(window.Handle());
}

// 设置弹窗过程：3 Tab 分页（常规/习惯/文件关联），控件动态创建
// 深色化：WM_CTLCOLOR 返回 #2C2C2C 画刷 + #CCC 文字，DWM 属性让标题栏变深
// [已删除] 旧 Win32 原生控件 SettingsDlgProc + 模板构建，改为 D2D 自绘 SettingsPanel
#if 0
static INT_PTR CALLBACK SettingsDlgProc(HWND hDlg, UINT msg, WPARAM w, LPARAM l) {
    static HBRUSH s_darkBrush = nullptr;
    static HFONT s_font = nullptr;
    static HWND s_hTab = nullptr;
    static HWND s_hList = nullptr;                       // 文件关联 ListView
    static std::vector<HWND> s_pageItems[3];             // 各页控件 HWND（切换 Tab 批量 ShowWindow）
    static std::vector<std::wstring> s_exts;             // ListView 对应的扩展名（含点）
    static std::vector<bool> s_origAssoc;                // 打开弹窗时各格式关联状态快照（确定时 diff）
    // Tab 标签文字（自绘时直接索引，避免 TabCtrl_GetItem 取回文字的编码风险）
    static const wchar_t* const s_tabLabels[] = { L"常规", L"习惯", L"文件关联" };
    static DWORD s_prevTheme = 0;                        // 弹窗前的 visual styles 状态（关闭时恢复）

    if (msg == WM_INITDIALOG) {
        SetWindowLongPtrW(hDlg, GWLP_USERDATA, (LONG_PTR)l);
        // 深色画刷 + ClearType 字体（18px 与主程序视觉对齐）
        if (s_darkBrush) DeleteObject(s_darkBrush);
        s_darkBrush = CreateSolidBrush(Cf(0x2C, 0x2C, 0x2C));
        if (s_font) DeleteObject(s_font);
        // GB2312_CHARSET：经典样式下 DEFAULT_CHARSET 可能映射到西文字符集导致中文乱码
        s_font = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            GB2312_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
        SendMessageW(hDlg, WM_SETFONT, (WPARAM)s_font, MAKELPARAM(TRUE, 0));
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hDlg, 20, &dark, sizeof(dark));
        // 全局禁用 visual styles：SetWindowTheme 逐控件禁用可能不彻底，
        // SetThemeAppProperties(0) 确保后续创建的所有控件用经典样式（DrawTextEx + WM_CTLCOLOR）
        s_prevTheme = GetThemeAppProperties();
        SetThemeAppProperties(0);

        // 创建 Tab 控件（底部留 42px 给确定按钮）
        RECT rcDlg; GetClientRect(hDlg, &rcDlg);
        s_hTab = CreateWindowExW(0, WC_TABCONTROLW, L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP,
            6, 6, rcDlg.right - 12, rcDlg.bottom - 48, hDlg,
            (HMENU)(INT_PTR)IDC_SET_TAB, nullptr, nullptr);
        SendMessageW(s_hTab, WM_SETFONT, (WPARAM)s_font, TRUE);
        // 禁用 visual styles：标签按钮才会用经典样式 + WM_CTLCOLOR 深色画刷渲染
        // 否则系统主题绘制浅色标签按钮，与深色背景冲突
        SetWindowTheme(s_hTab, L"", L"");
        // Tab 深色化：背景 #252525（与工具栏一致）
        SendMessageW(s_hTab, 0x1323, 0, (LPARAM)Cf(0x25, 0x25, 0x25));  // TCM_SETBKCOLOR
        // 子类化 Tab：内容区（display area）背景默认是 COLOR_BTNFACE #F0F0F0，
        // TCM_SETBKCOLOR 只影响标签区不影响内容区，须在 WM_ERASEBKGND 自行填充深色
        SetWindowSubclass(s_hTab, [](HWND h, UINT msg, WPARAM w, LPARAM l,
                                     UINT_PTR /*id*/, DWORD_PTR /*ref*/) -> LRESULT {
            if (msg == WM_ERASEBKGND) {
                HDC hdc = (HDC)w;
                RECT rc; GetClientRect(h, &rc);
                TabCtrl_AdjustRect(h, FALSE, &rc);  // 取内容区矩形
                HBRUSH bg = CreateSolidBrush(Cf(0x2C, 0x2C, 0x2C));
                FillRect(hdc, &rc, bg);
                DeleteObject(bg);
                return 1;  // 已擦除
            }
            return DefSubclassProc(h, msg, w, l);
        }, 1, 0);
        TCITEMW ti{}; ti.mask = TCIF_TEXT;
        // 注意：必须用 TCM_INSERTITEMW 消息——项目未定义 UNICODE 宏，无后缀
        // TabCtrl_InsertItem 会解析成 ANSI 版 TCM_INSERTITEMA，把宽字符串
        // 指针当 ANSI 逐字节读取，中文标签显示为乱码（"常规"→"8^醮"）
        ti.pszText = (LPWSTR)L"常规";     SendMessageW(s_hTab, TCM_INSERTITEMW, 0, (LPARAM)&ti);
        ti.pszText = (LPWSTR)L"习惯";     SendMessageW(s_hTab, TCM_INSERTITEMW, 1, (LPARAM)&ti);
        ti.pszText = (LPWSTR)L"文件关联"; SendMessageW(s_hTab, TCM_INSERTITEMW, 2, (LPARAM)&ti);

        // Tab 内容区（hDlg 坐标系）
        RECT rcTab; GetClientRect(s_hTab, &rcTab);
        MapWindowPoints(s_hTab, hDlg, (LPPOINT)&rcTab, 2);
        TabCtrl_AdjustRect(s_hTab, FALSE, &rcTab);
        int px = rcTab.left, py = rcTab.top;
        int pw = rcTab.right - rcTab.left, ph = rcTab.bottom - rcTab.top;

        // addChild：在 hDlg 创建控件（坐标基于内容区），记录到对应页 vector
        // Button 类控件统一禁用 visual styles + BS_OWNERDRAW 自绘
        // BS_GROUPBOX/BS_AUTOCHECKBOX/BS_AUTORADIOBUTTON 全转 BS_OWNERDRAW：
        //   经典样式勾选框白底灰边与深色主题不协调，系统绘制文字会乱码
        //   GWLP_USERDATA 存原始 type，WM_DRAWITEM 按类型分别绘制深色勾选框/圆点
        auto addChild = [&](int page, const wchar_t* cls, DWORD style,
                            int x, int y, int cw, int ch, int id, const wchar_t* text) {
            DWORD origType = style & 0xF;
            if (origType == BS_GROUPBOX || origType == BS_AUTOCHECKBOX || origType == BS_AUTORADIOBUTTON) {
                style = (style & ~0xF) | BS_OWNERDRAW;
            }
            HWND h = CreateWindowExW(0, cls, text, style | WS_CHILD | WS_VISIBLE,
                                     px + x, py + y, cw, ch, hDlg,
                                     (HMENU)(INT_PTR)id, nullptr, nullptr);
            SendMessageW(h, WM_SETFONT, (WPARAM)s_font, TRUE);
            if (wcscmp(cls, L"Button") == 0) SetWindowTheme(h, L"", L"");
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)origType);  // 记录原始 type 供自绘区分
            s_pageItems[page].push_back(h);
            return h;
        };
        // groupbox id 用 0（不参与命令路由）
        constexpr int GB = 0;

        // === 常规页：文件夹穿透 + 视图（控件高 22，y 间距 28，适配 18px 字体）===
        addChild(0, L"Button", BS_GROUPBOX, 8, 4, pw - 16, 110, GB, L"文件夹穿透策略");
        addChild(0, L"Button", BS_AUTORADIOBUTTON | WS_GROUP, 20, 32, pw - 40, 22, IDC_SET_RADIO_LOOP, L"本文件夹循环");
        addChild(0, L"Button", BS_AUTORADIOBUTTON, 20, 60, pw - 40, 22, IDC_SET_RADIO_PENETRATE, L"进入下个文件夹");
        addChild(0, L"Button", BS_AUTORADIOBUTTON, 20, 88, pw - 40, 22, IDC_SET_RADIO_PROMPT, L"提示是否进入下个文件夹");
        addChild(0, L"Button", BS_GROUPBOX, 8, 124, pw - 16, 86, GB, L"视图");
        addChild(0, L"Button", BS_AUTOCHECKBOX | WS_GROUP, 20, 152, pw - 40, 22, IDC_SET_CHK_BIRDSEYE, L"鸟瞰图");
        addChild(0, L"Button", BS_AUTOCHECKBOX, 20, 180, pw - 40, 22, IDC_SET_CHK_THUMBBAR, L"缩略图条");

        // === 习惯页：窗口使用习惯 + 滚轮行为 ===
        addChild(1, L"Button", BS_GROUPBOX, 8, 4, pw - 16, 110, GB, L"看图窗口使用习惯");
        addChild(1, L"Button", BS_AUTOCHECKBOX | WS_GROUP, 20, 32, pw - 40, 22, IDC_SET_CHK_ALWAYSONTOP, L"窗口总是置顶于所有窗口最前面");
        addChild(1, L"Button", BS_AUTOCHECKBOX, 20, 60, pw - 40, 22, IDC_SET_CHK_MULTIPLEWINDOWS, L"允许打开多个看图窗口");
        addChild(1, L"Button", BS_GROUPBOX, 8, 124, pw - 16, 86, GB, L"滚轮行为");
        addChild(1, L"Button", BS_AUTORADIOBUTTON | WS_GROUP, 20, 152, pw - 40, 22, IDC_SET_RADIO_WHEEL_ZOOM, L"滚轮缩放");
        addChild(1, L"Button", BS_AUTORADIOBUTTON, 20, 180, pw - 40, 22, IDC_SET_RADIO_WHEEL_NAV, L"滚轮切换图片");

        // === 文件关联页：全选 + 格式 ListView ===
        addChild(2, L"Button", BS_AUTOCHECKBOX | WS_GROUP, 8, 4, pw - 16, 22, IDC_SET_CHK_ASSOC_ALL, L"全选 / 全不选");
        s_hList = addChild(2, WC_LISTVIEWW,
            LVS_REPORT | LVS_SINGLESEL | LVS_NOCOLUMNHEADER | LVS_SHOWSELALWAYS | WS_BORDER | WS_VSCROLL,
            8, 32, pw - 16, ph - 40, IDC_SET_LST_FORMATS, L"");
        ListView_SetExtendedListViewStyle(s_hList, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
        LVCOLUMNW col{}; col.mask = LVCF_FMT | LVCF_WIDTH; col.fmt = LVCFMT_LEFT; col.cx = pw - 40;
        // 同 Tab：无 UNICODE 宏时必须用 W 消息，否则宽结构被当 ANSI 处理
        SendMessageW(s_hList, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
        // 填充 SupportedExtensions 返回的格式（不含点，展示时补点）
        s_exts.clear(); s_origAssoc.clear();
        int itemIdx = 0;
        for (const auto& e : SupportedExtensions()) {
            std::wstring dot = L"." + std::wstring(e.begin(), e.end());
            LVITEMW it{}; it.mask = LVIF_TEXT; it.iItem = itemIdx;
            it.pszText = const_cast<LPWSTR>(dot.c_str());
            int idx = (int)SendMessageW(s_hList, LVM_INSERTITEMW, 0, (LPARAM)&it);
            bool assoc = FileAssoc::IsAssociated(dot);
            ListView_SetCheckState(s_hList, idx, assoc);
            s_exts.push_back(dot);
            s_origAssoc.push_back(assoc);
            itemIdx++;
        }

        // Config → 控件
        auto& cfg = Config::Instance().Get();
        CheckRadioButton(hDlg, IDC_SET_RADIO_LOOP, IDC_SET_RADIO_PROMPT,
                         IDC_SET_RADIO_LOOP + cfg.folderNavPolicy);
        CheckDlgButton(hDlg, IDC_SET_CHK_BIRDSEYE, cfg.birdsEyeVisible ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_SET_CHK_THUMBBAR, cfg.thumbnailBarVisible ? BST_CHECKED : BST_UNCHECKED);
        CheckRadioButton(hDlg, IDC_SET_RADIO_WHEEL_ZOOM, IDC_SET_RADIO_WHEEL_NAV,
                         IDC_SET_RADIO_WHEEL_ZOOM + cfg.wheelBehavior);
        CheckDlgButton(hDlg, IDC_SET_CHK_ALWAYSONTOP, cfg.alwaysOnTop ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_SET_CHK_MULTIPLEWINDOWS, cfg.allowMultipleWindows ? BST_CHECKED : BST_UNCHECKED);

        // 默认显示第 0 页，隐藏 1/2
        for (HWND h : s_pageItems[1]) ShowWindow(h, SW_HIDE);
        for (HWND h : s_pageItems[2]) ShowWindow(h, SW_HIDE);
        return TRUE;
    }
    // 深色化：对话框/静态/按钮背景统一深色，文字浅灰
    // SetBkMode(OPAQUE) + SetBkColor 匹配背景：ClearType 在透明模式下边缘
    // 与深色背景混合导致发虚，OPAQUE 让 ClearType 按已知背景色正确合成
    if (msg == WM_CTLCOLORDLG || msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLORBTN) {
        HDC hdc = (HDC)w;
        SetTextColor(hdc, Cf(0xCC, 0xCC, 0xCC));
        SetBkMode(hdc, OPAQUE);
        SetBkColor(hdc, Cf(0x2C, 0x2C, 0x2C));
        return (INT_PTR)s_darkBrush;
    }
    if (msg == WM_NOTIFY) {
        auto* nmh = (LPNMHDR)l;
        // Tab 切换：批量 Show/Hide 对应页控件
        if (nmh->hwndFrom == s_hTab && nmh->code == TCN_SELCHANGE) {
            int sel = TabCtrl_GetCurSel(s_hTab);
            for (int i = 0; i < 3; i++)
                for (HWND h : s_pageItems[i])
                    ShowWindow(h, (i == sel) ? SW_SHOW : SW_HIDE);
            return TRUE;
        }
        // Tab 标签自绘：禁用 visual styles 后标签按钮是经典浅色，
        // 须完全自绘深色背景 + 文字（CDRF_SKIPDEFAULT 跳过系统默认绘制）
        // 选中标签 #2C2C2C（与内容区融合）+ 白字，未选中 #252525 + #CCC
        if (nmh->hwndFrom == s_hTab && nmh->code == NM_CUSTOMDRAW) {
            auto* cd = (LPNMCUSTOMDRAW)l;
            if (cd->dwDrawStage == CDDS_PREPAINT) {
                SetWindowLongPtrW(hDlg, DWLP_MSGRESULT, CDRF_NOTIFYITEMDRAW);
                return TRUE;
            }
            if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
                bool sel = (cd->uItemState & CDIS_SELECTED) != 0;
                COLORREF bgCol = sel ? Cf(0x2C, 0x2C, 0x2C) : Cf(0x25, 0x25, 0x25);
                HBRUSH bg = CreateSolidBrush(bgCol);
                FillRect(cd->hdc, &cd->rc, bg);
                DeleteObject(bg);
                // OPAQUE + 匹配背景色：ClearType 锐利，避免透明模式文字发虚
                SetBkMode(cd->hdc, OPAQUE);
                SetBkColor(cd->hdc, bgCol);
                SetTextColor(cd->hdc, sel ? Cf(0xFF, 0xFF, 0xFF) : Cf(0xCC, 0xCC, 0xCC));
                // 直接用 s_font（WM_GETFONT 可能返回 NULL 导致用系统默认非 Unicode 字体画中文乱码）
                HFONT old = (HFONT)SelectObject(cd->hdc, s_font);
                int idx = (int)cd->dwItemSpec;
                if (idx >= 0 && idx <= 2) {
                    DrawTextW(cd->hdc, s_tabLabels[idx], -1, &cd->rc,
                              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }
                SelectObject(cd->hdc, old);
                SetWindowLongPtrW(hDlg, DWLP_MSGRESULT, CDRF_SKIPDEFAULT);
                return TRUE;
            }
        }
        // ListView 深色化（文字 #CCC，背景 #2C2C2C）
        if (nmh->hwndFrom == s_hList && nmh->code == NM_CUSTOMDRAW) {
            auto* cd = (LPNMLVCUSTOMDRAW)l;
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) {
                SetWindowLongPtrW(hDlg, DWLP_MSGRESULT, CDRF_NOTIFYITEMDRAW);
                return TRUE;
            }
            if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                cd->clrText = Cf(0xCC, 0xCC, 0xCC);
                cd->clrTextBk = Cf(0x2C, 0x2C, 0x2C);
                SetWindowLongPtrW(hDlg, DWLP_MSGRESULT, CDRF_DODEFAULT);
                return TRUE;
            }
        }
    }
    if (msg == WM_DRAWITEM) {
        auto* dis = (LPDRAWITEMSTRUCT)l;
        if (dis->CtlID == IDOK) {
            // 确定按钮自绘：深色背景 + 浅色文字，按下=accent 绿
            RECT rc = dis->rcItem;
            bool pressed = (dis->itemState & ODS_SELECTED) != 0;
            HBRUSH bg = CreateSolidBrush(pressed ? Cf(0x90, 0xC2, 0x08) : Cf(0x38, 0x38, 0x38));
            FillRect(dis->hDC, &rc, bg);
            DeleteObject(bg);
            HBRUSH bd = CreateSolidBrush(Cf(0x55, 0x55, 0x55));
            FrameRect(dis->hDC, &rc, bd);
            DeleteObject(bd);
            SetTextColor(dis->hDC, pressed ? Cf(0x1A, 0x1A, 0x1A) : Cf(0xCC, 0xCC, 0xCC));
            SetBkMode(dis->hDC, OPAQUE);
            SetBkColor(dis->hDC, pressed ? Cf(0x90, 0xC2, 0x08) : Cf(0x38, 0x38, 0x38));
            HFONT old = (HFONT)SelectObject(dis->hDC, s_font);
            DrawTextW(dis->hDC, L"确定", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dis->hDC, old);
            return TRUE;
        }
        // GroupBox/复选框/单选框自绘：按 GWLP_USERDATA 存的原始 type 分支
        if (dis->CtlType == ODT_BUTTON) {
            DWORD origType = (DWORD)GetWindowLongPtrW(dis->hwndItem, GWLP_USERDATA);
            wchar_t buf[64] = {0};
            GetWindowTextW(dis->hwndItem, buf, 64);
            RECT rc = dis->rcItem;
            bool pressed = (dis->itemState & ODS_SELECTED) != 0;
            bool checked = (dis->itemState & ODS_CHECKED) != 0;

            if (origType == BS_GROUPBOX) {
                // GroupBox：深色背景 + 扁平边框 + 标题文字
                HBRUSH bg = CreateSolidBrush(Cf(0x2C, 0x2C, 0x2C));
                FillRect(dis->hDC, &rc, bg);
                DeleteObject(bg);
                HPEN pen = CreatePen(PS_SOLID, 1, Cf(0x55, 0x55, 0x55));
                HPEN oldPen = (HPEN)SelectObject(dis->hDC, pen);
                HBRUSH oldBrush = (HBRUSH)SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
                Rectangle(dis->hDC, rc.left, rc.top, rc.right, rc.bottom);
                SelectObject(dis->hDC, oldPen);
                SelectObject(dis->hDC, oldBrush);
                DeleteObject(pen);
                if (buf[0] && s_font) {
                    HFONT old = (HFONT)SelectObject(dis->hDC, s_font);
                    RECT rcCalc = { 0, 0, 0, 0 };
                    DrawTextW(dis->hDC, buf, -1, &rcCalc, DT_CALCRECT | DT_SINGLELINE);
                    RECT rcText = { rc.left + 10, rc.top - 9,
                                    rc.left + 10 + rcCalc.right + 4, rc.top + 9 };
                    SetTextColor(dis->hDC, Cf(0xCC, 0xCC, 0xCC));
                    SetBkMode(dis->hDC, OPAQUE);
                    SetBkColor(dis->hDC, Cf(0x2C, 0x2C, 0x2C));
                    DrawTextW(dis->hDC, buf, -1, &rcText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                    SelectObject(dis->hDC, old);
                }
                return TRUE;
            }

            // 复选框/单选框共用：深色背景 + 左侧勾选图形 + 右侧文字
            HBRUSH bg = CreateSolidBrush(Cf(0x2C, 0x2C, 0x2C));
            FillRect(dis->hDC, &rc, bg);
            DeleteObject(bg);

            // 勾选图形尺寸 14×14，垂直居中于控件
            const int boxSz = 14;
            int boxX = rc.left + 2;
            int boxY = rc.top + (rc.bottom - rc.top - boxSz) / 2;

            if (origType == BS_AUTOCHECKBOX) {
                // 复选框：深色方框 #1A 背景 + #555 边框，选中时 accent 绿勾
                HBRUSH boxBg = CreateSolidBrush(Cf(0x1A, 0x1A, 0x1A));
                RECT boxRc = {boxX, boxY, boxX + boxSz, boxY + boxSz};
                FillRect(dis->hDC, &boxRc, boxBg);
                DeleteObject(boxBg);
                HPEN pen = CreatePen(PS_SOLID, 1, pressed ? Cf(0x90, 0xC2, 0x08) : Cf(0x55, 0x55, 0x55));
                HPEN oldPen = (HPEN)SelectObject(dis->hDC, pen);
                HBRUSH oldBrush = (HBRUSH)SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
                Rectangle(dis->hDC, boxX, boxY, boxX + boxSz, boxY + boxSz);
                SelectObject(dis->hDC, oldPen);
                SelectObject(dis->hDC, oldBrush);
                DeleteObject(pen);
                if (checked) {
                    // accent 绿勾 (√)
                    HPEN cp = CreatePen(PS_SOLID, 2, Cf(0x90, 0xC2, 0x08));
                    HPEN ocp = (HPEN)SelectObject(dis->hDC, cp);
                    MoveToEx(dis->hDC, boxX + 3, boxY + 7, nullptr);
                    LineTo(dis->hDC, boxX + 6, boxY + 10);
                    LineTo(dis->hDC, boxX + 11, boxY + 4);
                    SelectObject(dis->hDC, ocp);
                    DeleteObject(cp);
                }
            } else {
                // 单选框：深色圆圈 #1A 背景 + #555 边框，选中时圆心 accent 绿
                HPEN pen = CreatePen(PS_SOLID, 1, pressed ? Cf(0x90, 0xC2, 0x08) : Cf(0x55, 0x55, 0x55));
                HPEN oldPen = (HPEN)SelectObject(dis->hDC, pen);
                HBRUSH circleBg = CreateSolidBrush(Cf(0x1A, 0x1A, 0x1A));
                HBRUSH oldBrush = (HBRUSH)SelectObject(dis->hDC, circleBg);
                Ellipse(dis->hDC, boxX, boxY, boxX + boxSz, boxY + boxSz);
                SelectObject(dis->hDC, oldPen);
                SelectObject(dis->hDC, oldBrush);
                DeleteObject(pen);
                DeleteObject(circleBg);
                if (checked) {
                    // 圆心实心点 accent 绿
                    HBRUSH dotBg = CreateSolidBrush(Cf(0x90, 0xC2, 0x08));
                    HPEN dotPen = CreatePen(PS_SOLID, 1, Cf(0x90, 0xC2, 0x08));
                    HPEN oldDotPen = (HPEN)SelectObject(dis->hDC, dotPen);
                    HBRUSH oldDotBrush = (HBRUSH)SelectObject(dis->hDC, dotBg);
                    int dotSz = 6;
                    Ellipse(dis->hDC, boxX + (boxSz - dotSz) / 2, boxY + (boxSz - dotSz) / 2,
                            boxX + (boxSz + dotSz) / 2, boxY + (boxSz + dotSz) / 2);
                    SelectObject(dis->hDC, oldDotPen);
                    SelectObject(dis->hDC, oldDotBrush);
                    DeleteObject(dotPen);
                    DeleteObject(dotBg);
                }
            }

            // 文字：勾选图形右侧，垂直居中
            if (buf[0] && s_font) {
                HFONT old = (HFONT)SelectObject(dis->hDC, s_font);
                RECT rcText = { boxX + boxSz + 8, rc.top, rc.right, rc.bottom };
                SetTextColor(dis->hDC, Cf(0xCC, 0xCC, 0xCC));
                SetBkMode(dis->hDC, OPAQUE);
                SetBkColor(dis->hDC, Cf(0x2C, 0x2C, 0x2C));
                DrawTextW(dis->hDC, buf, -1, &rcText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                SelectObject(dis->hDC, old);
            }
            return TRUE;
        }
    }
    if (msg == WM_COMMAND) {
        WORD id = LOWORD(w);
        WORD code = HIWORD(w);
        // BS_OWNERDRAW 不自动切换勾选状态，BN_CLICKED 时手动切换
        if (code == BN_CLICKED) {
            // 单选组1：文件夹穿透策略
            if (id >= IDC_SET_RADIO_LOOP && id <= IDC_SET_RADIO_PROMPT) {
                CheckRadioButton(hDlg, IDC_SET_RADIO_LOOP, IDC_SET_RADIO_PROMPT, id);
                return TRUE;
            }
            // 单选组2：滚轮行为
            if (id >= IDC_SET_RADIO_WHEEL_ZOOM && id <= IDC_SET_RADIO_WHEEL_NAV) {
                CheckRadioButton(hDlg, IDC_SET_RADIO_WHEEL_ZOOM, IDC_SET_RADIO_WHEEL_NAV, id);
                return TRUE;
            }
            // 复选框（非全选）：手动切换
            if (id == IDC_SET_CHK_BIRDSEYE || id == IDC_SET_CHK_THUMBBAR ||
                id == IDC_SET_CHK_ALWAYSONTOP || id == IDC_SET_CHK_MULTIPLEWINDOWS) {
                bool checked = IsDlgButtonChecked(hDlg, id) == BST_CHECKED;
                CheckDlgButton(hDlg, id, checked ? BST_UNCHECKED : BST_CHECKED);
                return TRUE;
            }
        }
        // 全选 checkbox：手动切换 + 同步 ListView 所有项
        if (id == IDC_SET_CHK_ASSOC_ALL && code == BN_CLICKED) {
            bool checked = IsDlgButtonChecked(hDlg, IDC_SET_CHK_ASSOC_ALL) == BST_CHECKED;
            CheckDlgButton(hDlg, IDC_SET_CHK_ASSOC_ALL, checked ? BST_UNCHECKED : BST_CHECKED);
            bool all = !checked;  // 切换后的新状态
            int n = ListView_GetItemCount(s_hList);
            for (int i = 0; i < n; i++) ListView_SetCheckState(s_hList, i, all);
            return TRUE;
        }
        if (id == IDOK) {
            auto cfg = Config::Instance().Get();
            for (int i = 0; i < 3; i++)
                if (IsDlgButtonChecked(hDlg, IDC_SET_RADIO_LOOP + i) == BST_CHECKED)
                    cfg.folderNavPolicy = i;
            cfg.birdsEyeVisible = IsDlgButtonChecked(hDlg, IDC_SET_CHK_BIRDSEYE) == BST_CHECKED;
            cfg.thumbnailBarVisible = IsDlgButtonChecked(hDlg, IDC_SET_CHK_THUMBBAR) == BST_CHECKED;
            for (int i = 0; i < 2; i++)
                if (IsDlgButtonChecked(hDlg, IDC_SET_RADIO_WHEEL_ZOOM + i) == BST_CHECKED)
                    cfg.wheelBehavior = i;
            bool prevTop = cfg.alwaysOnTop;
            cfg.alwaysOnTop = IsDlgButtonChecked(hDlg, IDC_SET_CHK_ALWAYSONTOP) == BST_CHECKED;
            cfg.allowMultipleWindows = IsDlgButtonChecked(hDlg, IDC_SET_CHK_MULTIPLEWINDOWS) == BST_CHECKED;
            Config::Instance().Set(cfg);

            auto* ctx = (WindowContext*)GetWindowLongPtrW(hDlg, GWLP_USERDATA);
            // 置顶变化 → 所有窗口应用
            if (cfg.alwaysOnTop != prevTop)
                WindowManager::Instance().ApplyAlwaysOnTop(cfg.alwaysOnTop);
            // 文件关联 diff：对比打开时快照，增删关联
            int n = ListView_GetItemCount(s_hList);
            for (int i = 0; i < n; i++) {
                bool now = ListView_GetCheckState(s_hList, i) == TRUE;
                if (now == s_origAssoc[i]) continue;
                if (now) FileAssoc::Associate(s_exts[i]);
                else     FileAssoc::Unassociate(s_exts[i]);
            }
            if (ctx) ctx->window.Invalidate();
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
        if (id == IDCANCEL) { EndDialog(hDlg, IDCANCEL); return TRUE; }
    }
    if (msg == WM_NCDESTROY) {
        // 恢复 visual styles（主程序不受影响）
        SetThemeAppProperties(s_prevTheme);
        if (s_darkBrush) { DeleteObject(s_darkBrush); s_darkBrush = nullptr; }
        if (s_font) { DeleteObject(s_font); s_font = nullptr; }
        s_hTab = nullptr; s_hList = nullptr;
        for (auto& v : s_pageItems) v.clear();
        s_exts.clear(); s_origAssoc.clear();
    }
    return FALSE;
}

// 设置弹窗：3 Tab 分页（常规/习惯/文件关联）
// 内存模板只放确定按钮（cdit=1），Tab + 各页控件由 SettingsDlgProc 动态创建
// 字段逐 WORD 推送（不强转结构体），规避 MSVC 默认对齐填充导致的模板错位
void WindowContext::ShowSettingsDialog() {
    constexpr DWORD VIS = WS_CHILD | WS_VISIBLE;
    std::vector<WORD> tpl;
    auto align = [&](){ if (tpl.size() & 1) tpl.push_back(0); };
    auto pushStr = [&](const wchar_t* s) {
        do { tpl.push_back((WORD)*s); } while (*s++);
    };
    auto pushItem = [&](DWORD style, short x, short y, short cx, short cy,
                        WORD id, WORD atom, const wchar_t* text) {
        align();
        tpl.push_back((WORD)(style & 0xFFFF));
        tpl.push_back((WORD)(style >> 16));
        tpl.push_back(0); tpl.push_back(0);          // exStyle = 0
        tpl.push_back((WORD)x); tpl.push_back((WORD)y);
        tpl.push_back((WORD)cx); tpl.push_back((WORD)cy);
        tpl.push_back(id);
        tpl.push_back(0xFFFF); tpl.push_back(atom);  // class by atom（0x80=Button）
        pushStr(text);
        tpl.push_back(0);  // creation data count = 0
    };

    // 对话框头：cdit=1（仅确定按钮），尺寸 340×320 dlu（容纳 18px 字体 + 宽松布局）
    align();
    DWORD dlgStyle = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER;
    tpl.push_back((WORD)(dlgStyle & 0xFFFF));
    tpl.push_back((WORD)(dlgStyle >> 16));
    tpl.push_back(0); tpl.push_back(0);  // exStyle = 0
    tpl.push_back(1);                     // cdit = 1 控件
    tpl.push_back(0); tpl.push_back(0);  // x, y
    tpl.push_back(340); tpl.push_back(320);  // cx, cy
    tpl.push_back(0); tpl.push_back(0);  // menu=none, class=default
    pushStr(L"设置");  // 标题

    // 唯一模板控件：确定按钮（右下角）
    pushItem(VIS | BS_DEFPUSHBUTTON | BS_OWNERDRAW | WS_TABSTOP, 250, 298, 80, 18, IDOK, 0x0080, L"确定");

    DialogBoxIndirectParamW(GetModuleHandleW(nullptr),
        (LPCDLGTEMPLATEW)tpl.data(), window.Handle(), SettingsDlgProc, (LPARAM)this);
}
#endif

// ─── WindowManager ───

WindowManager& WindowManager::Instance() {
    static WindowManager wm;
    return wm;
}

// 置顶切换：对所有窗口设 HWND_TOPMOST / HWND_NOTOPMOST
// SWP_NOMOVE|NOSIZE|NOACTIVATE 仅改 Z 序，不位移/不抢焦点
void WindowManager::ApplyAlwaysOnTop(bool on) {
    HWND flag = on ? HWND_TOPMOST : HWND_NOTOPMOST;
    for (auto& w : _windows) {
        SetWindowPos(w->window.Handle(), flag, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

// 界面缩放变化（设置面板确定后）：重算所有窗口 scale + 重建字体，立即生效
// scale = 系统 DPI 因子 × 用户手动缩放（100~200%），窗口尺寸不变（UI 元素缩放即可）
void WindowManager::ApplyUiScale() {
    auto& cfg = Config::Instance().Get();
    UINT dpi = GetDpiForSystem();
    float scale = (dpi / 96.0f) * (cfg.uiScale / 100.0f);
    for (auto& w : _windows) {
        w->ui.SetDpiScale(scale);
        w->ui.ApplyScale();
        InvalidateRect(w->window.Handle(), nullptr, FALSE);
    }
}

WindowContext* WindowManager::CreateNewWindow(const std::wstring& path, int cmdShow, bool promptOnLimit) {
    // 多窗口开关关闭 + 已有窗口 + 非空路径：替换当前窗口内容，不开新窗口
    // 启动首窗口（_windows 空）和拖入单张（OnDropFiles 已直接 LoadPath）不受影响
    if (!Config::Instance().Get().allowMultipleWindows && !_windows.empty() && !path.empty()) {
        auto* cur = _windows.back().get();
        cur->LoadPath(path);
        if (IsIconic(cur->window.Handle())) ShowWindow(cur->window.Handle(), SW_RESTORE);
        return cur;
    }
    if (promptOnLimit && (int)_windows.size() >= MAX_WINDOWS) {
        if (MessageBoxDlg::Show(nullptr,
                L"已达到 10 个窗口上限，是否继续打开？",
                L"提示", true) == IDNO) {
            return nullptr;
        }
    }
    auto ctx = std::make_unique<WindowContext>();
    if (!ctx->Create(cmdShow, path)) {
        LOG_ERR("WinMgr", "窗口创建失败");
        return nullptr;
    }
    WindowContext* p = ctx.get();
    _windows.push_back(std::move(ctx));
    // 新窗口继承当前置顶配置（启动/拖入/IPC 新建均走此路径）
    if (Config::Instance().Get().alwaysOnTop) {
        SetWindowPos(p->window.Handle(), HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    // 置前激活：IPC 双击场景（资源管理器在前台）下窗口需提到最前并抢焦点。
    // SetWindowPos(HWND_TOP) 不依赖前台权限，保证 Z 序置顶；SetForegroundWindow
    // 在用户双击启动的进程内通常有权限，成功则同时获得键盘焦点。
    {
        HWND h = p->window.Handle();
        SetWindowPos(h, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(h);
        SetActiveWindow(h);
    }
    return p;
}

void WindowManager::OnDropFiles(WindowContext* ctx, const std::vector<std::wstring>& paths) {
    if (paths.empty()) return;
    if (paths.size() == 1) {
        ctx->LoadPath(paths[0]);  // 单张：替换当前窗口
        return;
    }
    // 多张：每张新建窗口；首次超 10 上限时询问一次，选否则停止
    bool allowExceed = false;
    for (const auto& p : paths) {
        if ((int)_windows.size() >= MAX_WINDOWS && !allowExceed) {
            if (MessageBoxDlg::Show(nullptr,
                    L"将打开多个新窗口，已达 10 个上限，是否继续？",
                    L"提示", true) == IDNO) break;
            allowExceed = true;
        }
        CreateNewWindow(p, SW_SHOW, /*promptOnLimit=*/false);
    }
}

void WindowManager::RequestClose(WindowContext* ctx) {
    _pendingClose.push_back(ctx);
}

void WindowManager::ReapClosed() {
    if (_pendingClose.empty()) return;
    for (auto* ctx : _pendingClose) {
        auto it = std::find_if(_windows.begin(), _windows.end(),
            [ctx](const std::unique_ptr<WindowContext>& p) { return p.get() == ctx; });
        if (it != _windows.end()) _windows.erase(it);  // unique_ptr 析构：join 瓦片/预解码/监视线程
    }
    _pendingClose.clear();
    // 最后一个窗口关闭：退出消息循环（进程结束，单实例 Mutex 释放）
    if (_windows.empty()) {
        PostQuitMessage(0);
    }
}

int WindowManager::RunLoop() {
    _running = true;
    // 性能遥测：注册快照采集器（_perfEnabled 时每秒由 ActivityLog::PollTick 调用）
    ActivityLog::Instance().SetSnapshotCollector([this]() { CollectSnapshot(); });
    MSG msg = {};
    // GetMessage(nullptr,...) 服务所有窗口；每条消息派发后清理已关闭窗口
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
        ReapClosed();  // 延迟清理：避免在 WndProc 调用栈内销毁对象
    }
    return (int)msg.wParam;
}

// 性能遥测：每秒聚合各窗口内存/bitmap 估算/瓦片命中率/队列深度
// 由 ActivityLog::PollTick 在主线程 WM_TIMER 中调用（_perfEnabled 时）
void WindowManager::CollectSnapshot() {
    // 进程内存
    PROCESS_MEMORY_COUNTERS pmc = {};
    bool hasMem = GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    double memWs = hasMem ? (double)pmc.WorkingSetSize / (1024.0 * 1024.0) : 0;
    double memPk = hasMem ? (double)pmc.PeakWorkingSetSize / (1024.0 * 1024.0) : 0;

    // 聚合各窗口 bitmap 估算 + 瓦片命中率 + 队列深度
    size_t totalBitmapBytes = 0;
    uint64_t tileHit = 0, tileMiss = 0;
    int totalActiveWorkers = 0;
    for (const auto& ctx : _windows) {
        totalBitmapBytes += ctx->imageEngine.EstimateBitmapBytes();
        tileHit += ctx->imageEngine.TileHitCount();
        tileMiss += ctx->imageEngine.TileMissCount();
        totalActiveWorkers += ctx->imageEngine.TileActiveWorkers();
        // 读取后清零，下次 snapshot 重新统计
        ctx->imageEngine.TileResetHitMiss();
    }

    ActivityLog::Instance().LogSnapshot(L"性能", {
        Perf::N("mem_ws_mb", memWs),
        Perf::N("mem_pk_mb", memPk),
        Perf::N("bitmap_mb", (double)totalBitmapBytes / (1024.0 * 1024.0)),
        Perf::N("tile_hit", (double)tileHit),
        Perf::N("tile_miss", (double)tileMiss),
        Perf::N("tile_hit_rate", (tileHit + tileMiss > 0) ? (double)tileHit / (double)(tileHit + tileMiss) : 0),
        Perf::N("tile_active_workers", (double)totalActiveWorkers),
        Perf::N("window_count", (double)_windows.size())
    });
}
