#pragma once
#include "PlatformWindow.h"
#include "D2DRenderer.h"
#include "UIEngine.h"
#include "ImageEngine.h"
#include "PreDecodeCache.h"
#include "FileWatcherService.h"
#include <memory>
#include <vector>
#include <atomic>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <chrono>

// ─── 单窗口上下文 ───
// 拥有该窗口的全部资源（渲染器/图片引擎/UI/预解码缓存/文件监视）。
// 多窗口下每个窗口独立一份，互不共享，避免资源竞争与状态串扰。
class WindowContext {
public:
    PlatformWindow      window;
    D2DRenderer         renderer;
    UIEngine            ui;
    ImageEngine         imageEngine;
    PreDecodeCache      preDecodeCache;
    FileWatcherService  fileWatcher;
    std::atomic<DWORD>  lastFileChange{0};  // 文件变更时间戳（工作线程写，主线程定时器读）
    // 自身另存为写文件会触发目录监视，用此截止时间戳在主线程抑制短窗内的重载
    std::atomic<DWORD>  watchSuppressUntil{0};

    // 创建窗口并初始化全部资源；path 非空则加载该图片，否则打开空白窗口
    bool Create(int cmdShow, const std::wstring& path);
    // 加载图片到当前窗口（替换内容）
    void LoadPath(const std::wstring& path);
    // 销毁：停止并回收侧边栏缩略图生成线程
    ~WindowContext();

private:
    void SetupHandlers();
    // Navigate 后刷新文件监视（标题栏已在 RequestNavigate 更新，不调 Update 避免同步阻塞）
    void RefreshAfterNavigate();
    // 点击响应与渲染解耦：只更新 _targetIndex + 标题栏 + Invalidate（O(1)，不阻塞）
    // 真正的 Navigate 在 OnPaint 中执行，自动跳过中间帧
    void RequestNavigate(int dir);
    // 同步消费 _targetIndex：缩放/拖拽前确保索引已切换到最新目标
    void FlushPendingNavigate();

    // ── 侧边栏全文件夹缩略图生成 ──
    void UpdateGridGen();                             // 触发/更新生成（主线程）
    void GridGenLoop();                               // 后台线程：顺序生成全部缩略图
    std::shared_ptr<CachedImage> GetGridThumb(int i); // 从网格缓存取缩略图

    // ── 新增操作功能 ──
    void ToggleFullscreen();                // 全屏切换（F11 / 工具栏按钮）
    void ToggleExifPanel();                 // EXIF 面板显示/隐藏（I 键）
    void DeleteCurrentFile();               // 删除到回收站（Del 键）
    void CopyPathToClipboard();             // 复制文件路径到剪贴板
    void CopyImageToClipboard();            // 复制当前图片到剪贴板
    void OpenContainingFolder();            // 打开所在文件夹并选中（O 键）
    void ShowContextMenu(int screenX, int screenY);  // 右键菜单
    void ShowHamburgerMenu();               // 标题栏汉堡菜单
    void ShowSettingsDialog();              // 工具栏 ⋮ 设置弹窗（穿透策略+鸟瞰图/缩略图开关）
    // ── Prism UI 还原：右键菜单扩展功能 ──
    void RenameCurrentFile();               // 重命名（含 InputBox 输入新名）
    void SaveAs();                          // 另存为（复制到新路径）
    void SetAsWallpaper();                  // 设为系统壁纸

    // ── OLE 拖出（可撤销拖出）──
    // 状态机：Idle→Arming→Panning→DragOut→Returned，替代原 _dragging 布尔
    // 左键拖动窗口内平移；移出客户区 ≥28px 持续 100ms 触发 DoDragDrop；
    // 拖回客户区进入 Returned（图片静止等释放）；200ms 冷却防抖
    enum class DragOutState { Idle, Arming, Panning, DragOut, Returned };
    void CheckDragOutTrigger();             // Panning 态每帧检测 OOB+冷却 → 调 DoDragOut
    void DoDragOut();                       // 触发拖出（阻塞），返回后按结果切状态
    bool IsPanning() const { return _dragOutState == DragOutState::Panning; }

    int  _lastLevel = -2, _lastCount = -2;  // UI 层级信息上次显示值，避免每帧构造 wstring
    // 标题栏中间信息同步缓存：路径变化时才重新查文件大小（避免每帧 IO）
    std::wstring _lastSyncPath;
    std::wstring _lastSizeStr;
    DragOutState _dragOutState = DragOutState::Idle;  // 左键拖拽状态机
    int  _dragStartX = 0, _dragStartY = 0;  // Arming 态阈值检测起点
    DWORD _oobSince = 0;                    // 首次出界 tick（0=未出界）
    DWORD _lastDragOutTick = 0;             // 200ms 冷却基点
    bool _draggingBirdsEye = false;         // 鸟瞰图蓝框拖动状态
    int  _lastX = 0, _lastY = 0;
    int  _targetIndex = -1;                 // 导航目标索引（-1=无待处理导航），点击只改此值
    int  _lastHoverKey = -1;                // 上次 hover 命中键（按钮cmd/缩略图idx+偏移），变化时重绘

    // ── 侧边栏全文件夹缩略图生成状态 ──
    std::mutex _gridMutex;                                 // 保护 _gridCache/_gridGenFiles
    std::condition_variable _gridCv;                       // 唤醒生成线程
    std::unordered_map<int, std::shared_ptr<CachedImage>> _gridCache;  // 全文件夹缩略图缓存
    std::vector<std::wstring> _gridGenFiles;               // 已生成目录文件列表（变化则清缓存）
    int  _gridGenCurrent = -1;                             // 生成基准索引（当前页）
    std::thread _gridThread;                               // 后台生成线程
    std::atomic<bool> _gridStop{false};                    // 停止标志
    std::atomic<bool> _gridDirty{false};                   // 有待生成任务
};

// ─── 窗口管理器（单例） ───
// 维护窗口列表、10 上限询问、拖入分流、消息循环。
class WindowManager {
public:
    static constexpr int MAX_WINDOWS = 10;
    static WindowManager& Instance();

    // 创建新窗口；path 为空则空白。promptOnLimit=true 时超 10 上限弹窗询问，用户选否则返回 nullptr
    // 注：方法名避开 CreateWindow（Windows.h 宏，会展开为 CreateWindowW 导致语法错误）
    WindowContext* CreateNewWindow(const std::wstring& path, int cmdShow, bool promptOnLimit = true);

    // 拖入文件分流：单张替换当前窗口，多张每张新建窗口（含 10 上限询问）
    void OnDropFiles(WindowContext* ctx, const std::vector<std::wstring>& paths);

    // 标记窗口待清理（WM_DESTROY 回调中调用，延迟到消息派发后真正销毁对象）
    void RequestClose(WindowContext* ctx);

    // 消息循环：服务所有窗口，最后一个关闭后退出
    int RunLoop();

    // 置顶切换：对所有窗口设 HWND_TOPMOST / HWND_NOTOPMOST
    void ApplyAlwaysOnTop(bool on);

    // 界面缩放变化（设置面板确定后）：重算所有窗口 scale + 重建字体，立即生效
    void ApplyUiScale();

    size_t Count() const { return _windows.size(); }

private:
    std::vector<std::unique_ptr<WindowContext>> _windows;
    std::vector<WindowContext*> _pendingClose;  // 待清理窗口（避免在 WndProc 内销毁对象）
    bool _running = false;

    void ReapClosed();  // 派发后清理已关闭窗口，最后一个关闭则 PostQuitMessage
    void CollectSnapshot();  // 性能遥测：每秒聚合各窗口内存/bitmap/瓦片命中率
};
