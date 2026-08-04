#include "SettingsPanel.h"
#include "FileAssoc.h"
#include "DecoderFactory.h"
#include "WindowManager.h"
#include "Logger.h"
#include <dwrite.h>
#include <windowsx.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

// 颜色辅助：#RRGGBB → D2D1_COLOR_F
static D2D1_COLOR_F Cf(uint8_t r, uint8_t g, uint8_t b, float a = 1.0f) {
    return D2D1::ColorF(r / 255.f, g / 255.f, b / 255.f, a);
}

static const wchar_t* const kTabLabels[] = { L"常规", L"习惯", L"文件关联" };

void SettingsPanel::Show(HWND parent) {
    _parent = parent;
    _cfg = Config::Instance().Get();

    // DPI 缩放：取父窗口所在显示器的 DPI（Per-Monitor V2 下各窗口独立）
    UINT dpi = GetDpiForWindow(parent);
    _s = dpi / 96.0f;

    // 加载文件关联状态
    _assocs.clear();
    for (const auto& e : SupportedExtensions()) {
        std::wstring dot = L"." + std::wstring(e.begin(), e.end());
        _assocs.push_back({ dot, FileAssoc::IsAssociated(dot) });
    }

    // 注册窗口类（仅一次）
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = WProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"ArkSettingsPanel";
        wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
        RegisterClassW(&wc);
        registered = true;
    }

    // 居中创建窗口（尺寸随 DPI 放大）
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int winW = S(WIN_W), winH = S(WIN_H);
    int x = (sw - winW) / 2;
    int y = (sh - winH) / 2;
    _hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"ArkSettingsPanel", L"设置",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, winW, winH,
        parent, nullptr, GetModuleHandleW(nullptr), this);

    // D2D 初始化 + 字体（字号随 DPI 放大）
    _r.Initialize();
    _r.Attach(_hwnd);
    if (auto* dw = _r.DWrite()) {
        dw->CreateTextFormat(L"Microsoft YaHei", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, S(14.0f), L"zh-CN", &_font);
        dw->CreateTextFormat(L"Microsoft YaHei", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, S(12.0f), L"zh-CN", &_smallFont);
        if (_font) _font->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        if (_smallFont) _smallFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    // 深色标题栏
    BOOL dark = TRUE;
    DwmSetWindowAttribute(_hwnd, 20, &dark, sizeof(dark));

    // 模态：禁用父窗口 + 自有消息循环
    EnableWindow(parent, FALSE);
    ShowWindow(_hwnd, SW_SHOW);
    UpdateWindow(_hwnd);

    // 模态循环：用 PeekMessage + WaitMessage，IsWindow(_hwnd) 作为退出条件
    // 不用 PostQuitMessage（那是退出整个应用的语义，会污染主窗口 RunLoop）
    MSG msg;
    while (IsWindow(_hwnd)) {
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        } else {
            WaitMessage();  // 无消息时阻塞，避免 CPU 空转
        }
    }

    // 恢复父窗口
    EnableWindow(parent, TRUE);
    if (IsWindow(parent)) { SetForegroundWindow(parent); SetFocus(parent); }
}

LRESULT CALLBACK SettingsPanel::WProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    SettingsPanel* self = (SettingsPanel*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        self = (SettingsPanel*)((CREATESTRUCTW*)l)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    }
    if (!self) return DefWindowProcW(hwnd, msg, w, l);

    switch (msg) {
    case WM_PAINT:
        self->Paint();
        ValidateRect(hwnd, nullptr);
        return 0;
    case WM_ERASEBKGND:
        return 1;  // D2D 自绘，禁止系统擦除
    case WM_LBUTTONDOWN:
        self->LDown(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        return 0;
    case WM_LBUTTONUP:
        self->LUp(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        return 0;
    case WM_MOUSEMOVE:
        self->Move(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        return 0;
    case WM_MOUSEWHEEL:
        self->Wheel(GET_WHEEL_DELTA_WPARAM(w));
        return 0;
    case WM_KEYDOWN:
        self->Key((int)w);
        return 0;
    case WM_CLOSE:
        // 统一关闭入口：保存设置后再销毁（确定按钮/X/ESC/回车均走此路径）
        self->Apply();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        // 不调 PostQuitMessage：那是退出整个应用，会连带关闭主窗口
        return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

// ─── 绘制辅助 ───

void SettingsPanel::DrawTextVCenter(const wchar_t* text, float x, float y,
    float w, float h, D2D1_COLOR_F color, IDWriteTextFormat* fmt) {
    if (!fmt) return;
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    _r.DrawText(text, wcslen(text), fmt, x, y, w, h, color);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
}

void SettingsPanel::DrawCheck(float x, float y, bool on, bool hover) {
    float sz = S(16);
    _r.FillRectangle(x, y, sz, sz, Cf(0x1A, 0x1A, 0x1A));
    _r.DrawRectangle(x, y, sz, sz, hover ? Cf(0x90, 0xC2, 0x08) : Cf(0x55, 0x55, 0x55), 1.0f);
    if (on) {
        _r.FillRectangle(x + S(2), y + S(2), sz - S(4), sz - S(4), Cf(0x90, 0xC2, 0x08));
        // 白色对勾（直接在当前 BeginDraw 上下文中绘制）
        auto* ctx = _r.Context();
        if (ctx) {
            ComPtr<ID2D1SolidColorBrush> wb;
            ctx->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &wb);
            if (wb) {
                ctx->DrawLine(D2D1::Point2F(x + S(3), y + S(8)), D2D1::Point2F(x + S(6), y + S(11)),
                    wb.Get(), 2.0f);
                ctx->DrawLine(D2D1::Point2F(x + S(6), y + S(11)), D2D1::Point2F(x + S(12), y + S(4)),
                    wb.Get(), 2.0f);
            }
        }
    }
}

void SettingsPanel::DrawRadio(float x, float y, bool on, bool hover) {
    float sz = S(16);
    float cx = x + sz / 2, cy = y + sz / 2, r = sz / 2 - 1;
    _r.FillCircle(cx, cy, r, Cf(0x1A, 0x1A, 0x1A));
    auto* ctx = _r.Context();
    if (ctx) {
        ComPtr<ID2D1SolidColorBrush> pb;
        ctx->CreateSolidColorBrush(hover ? Cf(0x90, 0xC2, 0x08) : Cf(0x55, 0x55, 0x55), &pb);
        if (pb) {
            D2D1_ELLIPSE e = D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r);
            ctx->DrawEllipse(&e, pb.Get(), 1.0f);
        }
    }
    if (on) _r.FillCircle(cx, cy, S(4), Cf(0x90, 0xC2, 0x08));
}

void SettingsPanel::DrawSection(float x, float y, float w, const wchar_t* title) {
    if (_smallFont) {
        _smallFont->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        _r.DrawText(title, wcslen(title), _smallFont.Get(), x, y, w, S(24), Cf(0x88, 0x88, 0x88));
        _smallFont->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
    // 分隔线
    _r.FillRectangle(x, y + S(24), w, 1, Cf(0x44, 0x44, 0x44));
}

// ─── 主绘制 ───

void SettingsPanel::Paint() {
    _r.BeginDraw();
    _r.Clear(0x1E / 255.f, 0x1E / 255.f, 0x1E / 255.f);

    int W = _r.Width(), H = _r.Height();
    float fw = (float)W, fh = (float)H;
    int navW = S(NAV_W), tabH = S(TAB_H), rowH = S(ROW_H);

    // ── 左侧导航栏 ──
    _r.FillRectangle(0, 0, navW, fh, Cf(0x25, 0x25, 0x25));
    for (int i = 0; i < 3; i++) {
        float y = i * tabH;
        _navR[i] = { 0, y, (float)navW, (float)tabH };
        bool sel = (_tab == i), hov = (_hoverId == i);
        _r.FillRectangle(0, y, navW, tabH,
            sel ? Cf(0x33, 0x33, 0x33) : (hov ? Cf(0x2C, 0x2C, 0x2C) : Cf(0x25, 0x25, 0x25)));
        if (sel) _r.FillRectangle(0, y, S(3), tabH, Cf(0x90, 0xC2, 0x08));  // 左侧竖条
        DrawTextVCenter(kTabLabels[i], S(16), y, navW - S(16), tabH,
            sel ? Cf(0xFF, 0xFF, 0xFF) : Cf(0xCC, 0xCC, 0xCC), _font.Get());
    }
    _r.FillRectangle(navW, 0, 1, fh, Cf(0x44, 0x44, 0x44));  // 分隔线

    // ── 内容区 ──
    float cx = navW + 1;
    float cw = fw - cx;

    if (_tab == 0) {
        // 常规页
        float y = S(16);
        DrawSection(cx + S(16), y, cw - S(32), L"文件夹穿透策略"); y += S(36);
        const wchar_t* navOpts[] = { L"本文件夹循环", L"进入下个文件夹", L"提示是否进入下个文件夹" };
        for (int i = 0; i < 3; i++) {
            DrawRadio(cx + S(20), y + S(7), _cfg.folderNavPolicy == i, _hoverId == 10 + i);
            DrawTextVCenter(navOpts[i], cx + S(44), y, cw - S(60), rowH, Cf(0xCC, 0xCC, 0xCC), _font.Get());
            y += rowH;
        }
        y += S(10);
        DrawSection(cx + S(16), y, cw - S(32), L"视图"); y += S(36);
        const wchar_t* viewOpts[] = { L"鸟瞰图", L"缩略图条" };
        bool viewVal[] = { _cfg.birdsEyeVisible, _cfg.thumbnailBarVisible };
        for (int i = 0; i < 2; i++) {
            DrawCheck(cx + S(20), y + S(7), viewVal[i], _hoverId == 20 + i);
            DrawTextVCenter(viewOpts[i], cx + S(44), y, cw - S(60), rowH, Cf(0xCC, 0xCC, 0xCC), _font.Get());
            y += rowH;
        }
    } else if (_tab == 1) {
        // 习惯页
        float y = S(16);
        DrawSection(cx + S(16), y, cw - S(32), L"看图窗口使用习惯"); y += S(36);
        const wchar_t* habitOpts[] = { L"窗口总是置顶于所有窗口最前面", L"允许打开多个看图窗口" };
        bool habitVal[] = { _cfg.alwaysOnTop, _cfg.allowMultipleWindows };
        for (int i = 0; i < 2; i++) {
            DrawCheck(cx + S(20), y + S(7), habitVal[i], _hoverId == 30 + i);
            DrawTextVCenter(habitOpts[i], cx + S(44), y, cw - S(60), rowH, Cf(0xCC, 0xCC, 0xCC), _font.Get());
            y += rowH;
        }
        y += S(10);
        DrawSection(cx + S(16), y, cw - S(32), L"滚轮行为"); y += S(36);
        const wchar_t* wheelOpts[] = { L"滚轮缩放", L"滚轮切换图片" };
        for (int i = 0; i < 2; i++) {
            DrawRadio(cx + S(20), y + S(7), _cfg.wheelBehavior == i, _hoverId == 40 + i);
            DrawTextVCenter(wheelOpts[i], cx + S(44), y, cw - S(60), rowH, Cf(0xCC, 0xCC, 0xCC), _font.Get());
            y += rowH;
        }
    } else {
        // 文件关联页
        float y = S(16);
        bool allOn = !_assocs.empty();
        for (auto& a : _assocs) if (!a.on) { allOn = false; break; }
        _allR = { cx + S(16), y, cw - S(32), (float)rowH };
        DrawCheck(cx + S(16), y + S(7), allOn, _hoverId == 50);
        DrawTextVCenter(L"全选 / 全不选", cx + S(40), y, cw - S(60), rowH, Cf(0xCC, 0xCC, 0xCC), _font.Get());
        y += rowH + S(4);

        // 列表区域
        _listR = { cx + S(16), y, cw - S(32), fh - y - S(50) };
        // 裁剪
        _r.Context()->PushAxisAlignedClip(
            D2D1::RectF(_listR.x, _listR.y, _listR.x + _listR.w, _listR.y + _listR.h),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        _r.FillRectangle(_listR.x, _listR.y, _listR.w, _listR.h, Cf(0x25, 0x25, 0x25));

        int lrowH = S(LIST_ROW_H);
        int first = _assocScroll / lrowH;
        if (first < 0) first = 0;
        int visCnt = (int)(_listR.h / lrowH) + 2;
        for (int i = first; i < (int)_assocs.size() && i < first + visCnt; i++) {
            float iy = _listR.y + i * lrowH - _assocScroll;
            if (iy + lrowH < _listR.y || iy > _listR.y + _listR.h) continue;
            if (_hoverId == 100 + i)
                _r.FillRectangle(_listR.x, iy, _listR.w, lrowH, Cf(0x33, 0x33, 0x33));
            DrawCheck(_listR.x + S(8), iy + (lrowH - S(16)) / 2, _assocs[i].on, _hoverId == 100 + i);
            DrawTextVCenter(_assocs[i].ext.c_str(), _listR.x + S(32), iy, _listR.w - S(40), lrowH,
                Cf(0xCC, 0xCC, 0xCC), _smallFont.Get());
        }
        _r.Context()->PopAxisAlignedClip();

        // 滚动条
        int totalH = (int)_assocs.size() * lrowH;
        if (totalH > _listR.h) {
            float sbW = S(6);
            float sbX = _listR.x + _listR.w - sbW;
            _r.FillRectangle(sbX, _listR.y, sbW, _listR.h, Cf(0x33, 0x33, 0x33));
            float thumbH = _listR.h * _listR.h / totalH;
            float maxScroll = totalH - _listR.h;
            float thumbY = _listR.y + (_listR.h - thumbH) * _assocScroll / maxScroll;
            _r.FillRectangle(sbX, thumbY, sbW, thumbH, Cf(0x60, 0x60, 0x60));
        }
    }

    // ── 确定按钮 ──
    int btnW = S(BTN_W), btnH = S(BTN_H);
    float bx = fw - btnW - S(16);
    float by = fh - btnH - S(12);
    _okR = { bx, by, (float)btnW, (float)btnH };
    _r.FillRectangle(bx, by, btnW, btnH,
        _pressedOk ? Cf(0x90, 0xC2, 0x08) : (_hoverId == 1000 ? Cf(0x44, 0x44, 0x44) : Cf(0x38, 0x38, 0x38)));
    _r.DrawRectangle(bx, by, btnW, btnH, Cf(0x55, 0x55, 0x55), 1.0f);
    if (_font) {
        _font->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        _font->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        _r.DrawText(L"确定", 2, _font.Get(), bx, by, btnW, btnH,
            _pressedOk ? Cf(0x1A, 0x1A, 0x1A) : Cf(0xCC, 0xCC, 0xCC));
        _font->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        _font->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    _r.EndDraw();
}

// ─── 命中检测 ───

int SettingsPanel::HitTest(int x, int y) {
    auto inRect = [&](const FRect& r) {
        return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
    };
    // Tab 导航
    for (int i = 0; i < 3; i++) if (inRect(_navR[i])) return i;
    // 确定按钮
    if (inRect(_okR)) return 1000;

    float cx = S(NAV_W) + 1;
    float cw = (float)_r.Width() - cx;
    // 选项命中区域：内容区内 x ∈ [cx+16, cx+cw-16]
    bool inContentX = (x >= cx + S(16) && x < cx + cw - S(16));
    int rowH = S(ROW_H);

    if (_tab == 0) {
        // 常规页：穿透单选 10-12，视图复选 20-21
        if (inContentX) {
            float yBase = S(16) + S(36);  // 穿透单选起始 y
            for (int i = 0; i < 3; i++) {
                float ry = yBase + i * rowH;
                if (y >= ry && y < ry + rowH) return 10 + i;
            }
            yBase = S(16) + S(36) + 3 * rowH + S(10) + S(36);  // 视图复选起始 y
            for (int i = 0; i < 2; i++) {
                float ry = yBase + i * rowH;
                if (y >= ry && y < ry + rowH) return 20 + i;
            }
        }
    } else if (_tab == 1) {
        // 习惯页：复选 30-31，单选 40-41
        if (inContentX) {
            float yBase = S(16) + S(36);
            for (int i = 0; i < 2; i++) {
                float ry = yBase + i * rowH;
                if (y >= ry && y < ry + rowH) return 30 + i;
            }
            yBase = S(16) + S(36) + 2 * rowH + S(10) + S(36);
            for (int i = 0; i < 2; i++) {
                float ry = yBase + i * rowH;
                if (y >= ry && y < ry + rowH) return 40 + i;
            }
        }
    } else {
        // 文件关联页
        if (inRect(_allR)) return 50;
        if (inRect(_listR)) {
            int idx = (int)((y - _listR.y + _assocScroll) / S(LIST_ROW_H));
            if (idx >= 0 && idx < (int)_assocs.size()) return 100 + idx;
        }
    }
    return -1;
}

// ─── 交互 ───

void SettingsPanel::LDown(int x, int y) {
    int id = HitTest(x, y);
    if (id == 1000) { _pressedOk = true; InvalidateRect(_hwnd, nullptr, FALSE); return; }

    if (id >= 0 && id <= 2) { _tab = id; _hoverId = -1; InvalidateRect(_hwnd, nullptr, FALSE); return; }

    if (id >= 10 && id <= 12) { _cfg.folderNavPolicy = id - 10; InvalidateRect(_hwnd, nullptr, FALSE); return; }
    if (id >= 20 && id <= 21) {
        if (id == 20) _cfg.birdsEyeVisible = !_cfg.birdsEyeVisible;
        else           _cfg.thumbnailBarVisible = !_cfg.thumbnailBarVisible;
        InvalidateRect(_hwnd, nullptr, FALSE); return;
    }
    if (id >= 30 && id <= 31) {
        if (id == 30) _cfg.alwaysOnTop = !_cfg.alwaysOnTop;
        else           _cfg.allowMultipleWindows = !_cfg.allowMultipleWindows;
        InvalidateRect(_hwnd, nullptr, FALSE); return;
    }
    if (id >= 40 && id <= 41) { _cfg.wheelBehavior = id - 40; InvalidateRect(_hwnd, nullptr, FALSE); return; }
    if (id == 50) {
        bool allOn = !_assocs.empty();
        for (auto& a : _assocs) if (!a.on) { allOn = false; break; }
        for (auto& a : _assocs) a.on = !allOn;
        InvalidateRect(_hwnd, nullptr, FALSE); return;
    }
    if (id >= 100) {
        int idx = id - 100;
        if (idx < (int)_assocs.size()) { _assocs[idx].on = !_assocs[idx].on; InvalidateRect(_hwnd, nullptr, FALSE); }
        return;
    }
}

void SettingsPanel::LUp(int x, int y) {
    if (_pressedOk) {
        _pressedOk = false;
        int id = HitTest(x, y);
        // 确定按钮：走 WM_CLOSE 统一路径（保存+销毁）
        if (id == 1000) { PostMessageW(_hwnd, WM_CLOSE, 0, 0); return; }
        InvalidateRect(_hwnd, nullptr, FALSE);
    }
}

void SettingsPanel::Move(int x, int y) {
    int id = HitTest(x, y);
    if (id != _hoverId) {
        _hoverId = id;
        InvalidateRect(_hwnd, nullptr, FALSE);
    }
}

void SettingsPanel::Wheel(int delta) {
    if (_tab != 2) return;  // 仅文件关联页滚动
    int totalH = (int)_assocs.size() * S(LIST_ROW_H);
    float maxScroll = totalH - _listR.h;
    if (maxScroll <= 0) return;
    _assocScroll -= delta;
    if (_assocScroll < 0) _assocScroll = 0;
    if (_assocScroll > (int)maxScroll) _assocScroll = (int)maxScroll;
    InvalidateRect(_hwnd, nullptr, FALSE);
}

void SettingsPanel::Key(int vk) {
    // ESC/回车均走 WM_CLOSE 统一路径（保存+销毁）
    if (vk == VK_ESCAPE || vk == VK_RETURN)
        PostMessageW(_hwnd, WM_CLOSE, 0, 0);
}

void SettingsPanel::Apply() {
    bool prevTop = Config::Instance().Get().alwaysOnTop;
    Config::Instance().Set(_cfg);

    // 置顶变化 → 应用到所有窗口
    if (_cfg.alwaysOnTop != prevTop)
        WindowManager::Instance().ApplyAlwaysOnTop(_cfg.alwaysOnTop);

    // 文件关联：对比当前注册状态，增删关联
    for (const auto& a : _assocs) {
        bool was = FileAssoc::IsAssociated(a.ext);
        if (a.on && !was) FileAssoc::Associate(a.ext);
        else if (!a.on && was) FileAssoc::Unassociate(a.ext);
    }

    // 通知父窗口刷新
    if (_parent) InvalidateRect(_parent, nullptr, FALSE);
}
