#pragma once
#include <Windows.h>
#include <string>
#include <vector>
#include <functional>

// ─── Win32 窗口封装 ───
// 处理：窗口创建、消息循环、D2D 渲染表面管理
// 全自绘窗口，无标准子控件

// WM_COPYDATA 数据标识：单实例 IPC 传递"打开文件路径"（dwData 字段）
constexpr ULONG_PTR IPC_OPEN_FILE_ID = 1;

class PlatformWindow {
public:
    using PaintCallback  = std::function<void(HDC hdc, int w, int h)>;
    using ResizeCallback = std::function<void(int w, int h)>;
    using MouseEvent     = std::function<void(int x, int y, int modKeys)>;
    using WheelEvent     = std::function<void(int delta, int x, int y, int modKeys)>;
    // 拖入文件：一次拖入可能含多张，由上层决定单张替换 / 多张各开新窗口
    using DropFilesEvent = std::function<void(const std::vector<std::wstring>& paths)>;
    using KeyEvent       = std::function<void(int vk, bool down)>;
    using CommandEvent   = std::function<void(int cmdId)>;
    using ContextMenuEvent = std::function<void(int x, int y)>;
    // 窗口销毁回调（多窗口下由 WindowManager 延迟清理本上下文）
    using DestroyEvent   = std::function<void()>;
    // 单实例 IPC：另一进程通过 WM_COPYDATA 传文件路径过来，新建窗口显示
    using IpcOpenEvent   = std::function<void(const std::wstring& path)>;

    PlatformWindow();
    ~PlatformWindow();

    // 创建窗口（不显示）
    bool Create(const wchar_t* title, int w, int h);

    // 显示窗口
    void Show(int cmdShow = SW_SHOW);

    // 消息循环（阻塞，直到窗口关闭）
    int RunLoop();

    // 关闭窗口
    void Close();

    // ── 事件回调 ──
    void OnPaint(PaintCallback  cb)    { _onPaint = std::move(cb); }
    void OnResize(ResizeCallback cb)   { _onResize = std::move(cb); }
    void OnMouseDown(MouseEvent cb)    { _onMouseDown = std::move(cb); }
    void OnMouseUp(MouseEvent cb)      { _onMouseUp = std::move(cb); }
    void OnMouseMove(MouseEvent cb)    { _onMouseMove = std::move(cb); }
    void OnMouseWheel(WheelEvent cb)   { _onMouseWheel = std::move(cb); }
    void OnDropFile(DropFilesEvent cb) { _onDropFile = std::move(cb); }
    void OnKeyDown(KeyEvent cb)        { _onKeyDown = std::move(cb); }
    void OnCommand(CommandEvent cb)    { _onCommand = std::move(cb); }
    void OnContextMenu(ContextMenuEvent cb) { _onContextMenu = std::move(cb); }
    void OnDestroy(DestroyEvent cb)    { _onDestroy = std::move(cb); }
    void OnIpcOpen(IpcOpenEvent cb)    { _onIpcOpen = std::move(cb); }

    // 窗口句柄
    HWND Handle() const { return _hwnd; }

    // 全屏切换：保存/恢复窗口样式和位置
    void ToggleFullscreen();
    bool IsFullscreen() const { return _fullscreen; }

    // 客户区尺寸
    int ClientWidth()  const { return _clientW; }
    int ClientHeight() const { return _clientH; }

    // 设置窗口标题
    void SetTitle(const wchar_t* title);

    // 使窗口无效（触发重绘）
    void Invalidate();
    // 强制立即重绘（UpdateWindow 同步发送 WM_PAINT，绕过消息队列）
    // 快速按键时 WM_PAINT 优先级低于输入消息会被饿死，需此方法保证画面实时更新
    void Update();

    // 定时器
    void SetTimer(UINT_PTR id, UINT ms);
    void KillTimer(UINT_PTR id);
    void OnTimer(std::function<void(UINT_PTR)> cb) { _onTimer = std::move(cb); }

    // 拖入文件
    void EnableDropFiles(bool enable);

private:
    HWND   _hwnd = nullptr;
    int    _clientW = 0;
    int    _clientH = 0;
    bool   _running = false;

    PaintCallback  _onPaint;
    ResizeCallback _onResize;
    MouseEvent     _onMouseDown;
    MouseEvent     _onMouseUp;
    MouseEvent     _onMouseMove;
    WheelEvent     _onMouseWheel;
    DropFilesEvent _onDropFile;
    KeyEvent       _onKeyDown;
    CommandEvent   _onCommand;
    ContextMenuEvent _onContextMenu;
    DestroyEvent   _onDestroy;
    IpcOpenEvent   _onIpcOpen;
    std::function<void(UINT_PTR)> _onTimer;

    // 全屏状态
    bool   _fullscreen = false;
    LONG   _savedStyle = 0;
    LONG   _savedExStyle = 0;
    RECT   _savedRect = {};

    // 自绘标题栏高度（像素）：归入客户区由 D2D 绘制（Prism 35px）
    static constexpr int TITLEBAR_HEIGHT = 46;  // 与 UI_TITLEBAR_HEIGHT/toolbarHeight 一致

    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l);
    LRESULT WndProc(UINT msg, WPARAM w, LPARAM l);
    // 自绘标题栏命中测试：返回 HT* 值
    LRESULT NcHitTest(int x, int y);

    // 禁止拷贝
    PlatformWindow(const PlatformWindow&) = delete;
    PlatformWindow& operator=(const PlatformWindow&) = delete;
};
