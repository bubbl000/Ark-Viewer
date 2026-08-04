#include "PlatformWindow.h"
#include <windowsx.h>
#include "../resources/resource.h"
#include "Logger.h"
#include <shellapi.h>
#include <dwmapi.h>

PlatformWindow::PlatformWindow() = default;
PlatformWindow::~PlatformWindow() { Close(); }

bool PlatformWindow::Create(const wchar_t* title, int w, int h) {
    const wchar_t* CLASS_NAME = L"ArkViewer2Window";

    // 注册窗口类
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
    wc.cbWndExtra    = sizeof(LONG_PTR);  // 必须分配！否则 SetWindowLongPtrW(GWLP_USERDATA) 失效
    wc.lpfnWndProc   = StaticWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;     // 自绘背景
    wc.lpszClassName = CLASS_NAME;
    // LoadIcon 只取默认 32 尺寸，高 DPI 任务栏模糊；
    // 用 LoadImage 按系统图标尺寸加载大/小图标，ICO 内多尺寸自动选最优
    wc.hIcon   = (HICON)LoadImageW(wc.hInstance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    wc.hIconSm = (HICON)LoadImageW(wc.hInstance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);

    // 窗口类只注册一次：多窗口下第二次调用 RegisterClassExW 会返回 0
    // （ERROR_CLASS_ALREADY_EXISTS），需跳过避免误判失败
    static bool s_classRegistered = false;
    if (!s_classRegistered) {
        if (!RegisterClassExW(&wc)) return false;
        s_classRegistered = true;
    }

    // 创建窗口
    // 真无边框：仅保留 WS_THICKFRAME（提供缩放能力），移除系统标题栏/菜单/最小化最大化按钮
    // 客户区 = 整个窗口（WM_NCCALCSIZE 返回 0），边缘缩放与标题栏拖动由 WM_NCHITTEST 接管
    // 双击最大化由 WM_NCLBUTTONDBLCLK 手动处理（无 WS_MAXIMIZEBOX 系统不再自动最大化）
    constexpr LONG SELF_DRAWN_STYLE = WS_THICKFRAME | WS_CLIPCHILDREN;
    RECT rc = { 0, 0, w, h };
    AdjustWindowRect(&rc, SELF_DRAWN_STYLE, FALSE);

    _hwnd = CreateWindowExW(
        WS_EX_ACCEPTFILES | WS_EX_APPWINDOW,
        CLASS_NAME, title,
        SELF_DRAWN_STYLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, wc.hInstance, this);

    if (!_hwnd) return false;

    // 保存 this 指针到窗口附加数据
    SetWindowLongPtrW(_hwnd, GWLP_USERDATA, (LONG_PTR)this);

    // 强制重算非客户区边框：创建时样式已去掉 WS_CAPTION，但首帧前系统可能仍按旧帧绘制
    // 残留一条白色系统标题栏，需 SWP_FRAMECHANGED 触发 WM_NCCALCSIZE 重算，消除启动闪烁
    SetWindowPos(_hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);

    // 获取初始客户区尺寸
    GetClientRect(_hwnd, &rc);
    _clientW = rc.right;
    _clientH = rc.bottom;

    // Win11 DWM 圆角（CS_DROPSHADOW 已提供阴影，此处只加圆角）
    // DWMWA_WINDOW_CORNER_PREFERENCE=33, DWMWCP_ROUND=2；旧 SDK 无定义时用数值兜底
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
    int cornerPref = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
                          &cornerPref, sizeof(cornerPref));

    return true;
}

void PlatformWindow::Show(int cmdShow) {
    if (_hwnd) {
        ShowWindow(_hwnd, cmdShow);
        UpdateWindow(_hwnd);
    }
}

int PlatformWindow::RunLoop() {
    _running = true;
    MSG msg = {};
    while (_running && GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}

void PlatformWindow::Close() {
    if (_hwnd) {
        DestroyWindow(_hwnd);
        _hwnd = nullptr;
    }
    _running = false;
}

void PlatformWindow::SetTitle(const wchar_t* title) {
    if (!_hwnd) return;
    SetWindowTextW(_hwnd, title);
    // 只同步重绘标题栏（非客户区），不触发客户区 WM_PAINT（保护 OnPaint 跳帧逻辑）
    // 不调 DwmFlush：它阻塞 ~9ms 等 vsync，违背"标题微秒级不阻塞"原则
    // DWM 会在下一个 vsync 自然显示最新标题（60Hz，连点过快时跳帧但停手后正确）
    SendMessageW(_hwnd, WM_NCPAINT, 1, 0);
}

void PlatformWindow::Invalidate() {
    if (_hwnd) InvalidateRect(_hwnd, nullptr, FALSE);
}
void PlatformWindow::Update() {
    if (_hwnd) UpdateWindow(_hwnd);
}

void PlatformWindow::SetTimer(UINT_PTR id, UINT ms) {
    if (_hwnd) ::SetTimer(_hwnd, id, ms, nullptr);
}
void PlatformWindow::KillTimer(UINT_PTR id) {
    if (_hwnd) ::KillTimer(_hwnd, id);
}

void PlatformWindow::EnableDropFiles(bool enable) {
    if (_hwnd) DragAcceptFiles(_hwnd, enable);
}

LRESULT CALLBACK PlatformWindow::StaticWndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    auto* self = (PlatformWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!self) return DefWindowProcW(hwnd, msg, w, l);
    return self->WndProc(msg, w, l);
}

LRESULT PlatformWindow::WndProc(UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_SIZE: {
        int newW = LOWORD(l);
        int newH = HIWORD(l);
        _clientW = newW;
        _clientH = newH;
        LOG_DBG_STREAM("Win32") << "WM_SIZE: " << newW << "x" << newH;
        if (_onResize) _onResize(newW, newH);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(_hwnd, &ps);
        if (_onPaint) _onPaint(hdc, _clientW, _clientH);
        EndPaint(_hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
        if (_onMouseDown) _onMouseDown(GET_X_LPARAM(l), GET_Y_LPARAM(l), (int)w);
        return 0;
    case WM_LBUTTONUP:
        if (_onMouseUp) _onMouseUp(GET_X_LPARAM(l), GET_Y_LPARAM(l), (int)w);
        return 0;
    case WM_MOUSEMOVE:
        if (_onMouseMove) _onMouseMove(GET_X_LPARAM(l), GET_Y_LPARAM(l), (int)w);
        return 0;
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(w);
        // WM_MOUSEWHEEL 的 lParam 是屏幕坐标，需转客户区坐标。
        // 必须用真正的 POINT 结构（8 字节）调用 ScreenToClient——
        // 之前误用 (POINT*)&x 把 int x 强转 POINT*，ScreenToClient 写入 8 字节
        // 越界到相邻的 int y 上（RTC #2 栈损坏根因），且 int 布局与 POINT 不保证一致
        POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        ScreenToClient(_hwnd, &pt);
        if (_onMouseWheel) _onMouseWheel(delta, (int)pt.x, (int)pt.y, (int)LOWORD(w));
        return 0;
    }
    case WM_DROPFILES: {
        // 一次拖入可能含多张：收集全部路径交上层分流（单张替换 / 多张各开新窗口）
        HDROP hDrop = (HDROP)w;
        UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
        std::vector<std::wstring> paths;
        paths.reserve(count);
        for (UINT i = 0; i < count; i++) {
            wchar_t path[MAX_PATH];
            if (DragQueryFileW(hDrop, i, path, MAX_PATH)) paths.emplace_back(path);
        }
        DragFinish(hDrop);
        if (_onDropFile && !paths.empty()) _onDropFile(paths);
        return 0;
    }
    case WM_COPYDATA: {
        // 单实例 IPC：另一进程传文件路径过来，新建窗口显示
        auto* cds = reinterpret_cast<COPYDATASTRUCT*>(l);
        if (cds && cds->dwData == IPC_OPEN_FILE_ID && cds->cbData >= sizeof(wchar_t)) {
            std::wstring path(reinterpret_cast<const wchar_t*>(cds->lpData),
                              cds->cbData / sizeof(wchar_t));
            if (!path.empty() && path.back() == L'\0') path.pop_back();
            if (_onIpcOpen) _onIpcOpen(path);
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (_onKeyDown) _onKeyDown((int)w, true);
        return 0;
    case WM_KEYUP:
        if (_onKeyDown) _onKeyDown((int)w, false);
        return 0;
    case WM_COMMAND:
        if (_onCommand) _onCommand(LOWORD(w));
        return 0;
    case WM_CONTEXTMENU:
        // 右键菜单：lParam 低位=屏幕坐标x，高位=屏幕坐标y
        // -1,-1 表示键盘触发（Shift+F10），用当前鼠标位置兜底
        if (_onContextMenu) {
            int sx = GET_X_LPARAM(l);
            int sy = GET_Y_LPARAM(l);
            if (sx == -1 && sy == -1) {
                POINT pt; GetCursorPos(&pt);
                sx = pt.x; sy = pt.y;
            }
            _onContextMenu(sx, sy);
        }
        return 0;
    case WM_TIMER:
        if (_onTimer) _onTimer((UINT_PTR)w);
        return 0;
    case WM_DESTROY:
        // 多窗口下不在此处退出消息循环：交 WindowManager 在最后一个窗口关闭时 PostQuitMessage
        // 通知 WindowManager 延迟清理本上下文（不能在此 delete，因为仍在 WndProc 调用栈中）
        _running = false;
        if (_onDestroy) _onDestroy();
        return 0;

    // ── 真无边框：客户区 = 整个窗口矩形，移除系统边框/标题栏 ──
    // 返回 0（wParam=TRUE 时）表示客户区覆盖整个窗口，系统不再预留非客户区
    // 边缘缩放热区改由 WM_NCHITTEST 在客户区边缘像素返回 HTLEFT/HT* 接管
    case WM_NCCALCSIZE: {
        return 0;
    }
    case WM_NCHITTEST: {
        LRESULT ht = NcHitTest(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (ht != HTNOWHERE) return ht;
        break;  // 未命中自绘区域，走默认
    }
    case WM_NCLBUTTONDBLCLK: {
        // 无 WS_MAXIMIZEBOX 时系统不再自动双击最大化，此处手动接管
        // wParam 即 WM_NCHITTEST 返回值，仅标题栏(HTCAPTION)双击切换最大化/还原
        if (!_fullscreen && w == HTCAPTION) {
            if (IsZoomed(_hwnd)) ShowWindow(_hwnd, SW_RESTORE);
            else ShowWindow(_hwnd, SW_MAXIMIZE);
            return 0;
        }
        break;
    }
    case WM_GETMINMAXINFO: {
        // 最大化时约束到显示器工作区（不含任务栏），防止无边框窗口溢出屏幕
        auto* mmi = reinterpret_cast<MINMAXINFO*>(l);
        HMONITOR hMon = MonitorFromWindow(_hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) };
        if (GetMonitorInfoW(hMon, &mi)) {
            mmi->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
            mmi->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
            mmi->ptMaxPosition.x = mi.rcWork.left;
            mmi->ptMaxPosition.y = mi.rcWork.top;
        }
        mmi->ptMinTrackSize.x = 320;
        mmi->ptMinTrackSize.y = 240;
        return 0;
    }
    }
    return DefWindowProcW(_hwnd, msg, w, l);
}

// 真无边框命中测试：8 方向边缘缩放 + 标题栏拖动 + 客户区
// 客户区 = 整个窗口，故边缘像素（pt.x/y 近 0 或近 W/H）判定为缩放热区
// 顶部标题栏（排除右侧按钮）返回 HTCAPTION 支持拖动与双击最大化
LRESULT PlatformWindow::NcHitTest(int sx, int sy) {
    if (_fullscreen) return HTNOWHERE;  // 全屏走默认（WS_POPUP，无边缘缩放）

    POINT pt{ sx, sy };
    if (!ScreenToClient(_hwnd, &pt)) return HTNOWHERE;

    int W = _clientW, H = _clientH;
    // 最大化时不允许缩放（边缘返回客户区/标题栏，避免光标变成缩放箭头）
    if (!IsZoomed(_hwnd) && W > 0 && H > 0) {
        constexpr int EDGE = 4;  // 边缘缩放热区宽度（像素）
        bool left   = pt.x < EDGE;
        bool right  = pt.x >= W - EDGE;
        bool top    = pt.y < EDGE;
        bool bottom = pt.y >= H - EDGE;
        // 四角优先（同时命中两条边）
        if (top    && left)  return HTTOPLEFT;
        if (top    && right) return HTTOPRIGHT;
        if (bottom && left)  return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left)   return HTLEFT;
        if (right)  return HTRIGHT;
        if (top)    return HTTOP;
        if (bottom) return HTBOTTOM;
    }

    // 顶部标题栏区域：拖动（HTCAPTION）或按钮（HTCLIENT 交 OnMouseDown）
    if (pt.y >= 0 && pt.y < TITLEBAR_HEIGHT) {
        int btnAreaLeft = W - TITLEBAR_HEIGHT * 5;  // 右侧 5 按钮区
        if (pt.x >= btnAreaLeft && pt.x < W) return HTCLIENT;
        return HTCAPTION;
    }
    return HTCLIENT;  // 其余客户区交 OnMouseDown
}

void PlatformWindow::ToggleFullscreen() {
    if (!_hwnd) return;
    if (!_fullscreen) {
        // 进入全屏：保存当前样式和窗口位置
        _savedStyle = GetWindowLongW(_hwnd, GWL_STYLE);
        _savedExStyle = GetWindowLongW(_hwnd, GWL_EXSTYLE);
        GetWindowRect(_hwnd, &_savedRect);
        // 移除标题栏和边框，设置为全屏
        SetWindowLongW(_hwnd, GWL_STYLE, WS_POPUP | WS_CLIPCHILDREN);
        SetWindowLongW(_hwnd, GWL_EXSTYLE, WS_EX_APPWINDOW);
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);
        SetWindowPos(_hwnd, HWND_TOP, 0, 0, sw, sh, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        _fullscreen = true;
    } else {
        // 退出全屏：恢复原样式和位置
        SetWindowLongW(_hwnd, GWL_STYLE, _savedStyle);
        SetWindowLongW(_hwnd, GWL_EXSTYLE, _savedExStyle);
        SetWindowPos(_hwnd, HWND_NOTOPMOST,
            _savedRect.left, _savedRect.top,
            _savedRect.right - _savedRect.left,
            _savedRect.bottom - _savedRect.top,
            SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        _fullscreen = false;
    }
    // 更新客户区尺寸
    RECT rc;
    GetClientRect(_hwnd, &rc);
    _clientW = rc.right;
    _clientH = rc.bottom;
}






