#include "SaveAsDialog.h"
#include "decoders/JpegDecoder.h"
#include <windowsx.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <cstdio>
#include <algorithm>
#pragma comment(lib, "dwmapi.lib")

// 颜色辅助：#RRGGBB → D2D1_COLOR_F
static D2D1_COLOR_F Cf(uint8_t r, uint8_t g, uint8_t b, float a = 1.0f) {
    return D2D1::ColorF(r / 255.f, g / 255.f, b / 255.f, a);
}

// 字节数 → 可读字符串（"约 3.2 MB"）
static std::wstring FormatSize(size_t bytes) {
    if (bytes < 1024) return std::to_wstring(bytes) + L" B";
    if (bytes < 1024ULL * 1024) return std::to_wstring(bytes / 1024) + L" KB";
    wchar_t buf[32];
    swprintf(buf, 32, L"%.1f MB", bytes / (1024.0 * 1024.0));
    return buf;
}

int SaveAsDialog::Show(HWND parent, const uint8_t* pixels, int w, int h, int stride) {
    _pixels = pixels; _w = w; _h = h; _stride = stride;

    // 注册窗口类（仅一次）
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = WProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"ArkSaveAsDialog";
        wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
        RegisterClassW(&wc);
        registered = true;
    }

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int x = (sw - WIN_W) / 2;
    int y = (sh - WIN_H) / 2;
    _hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"ArkSaveAsDialog", L"另存为 - JPEG 质量",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, WIN_W, WIN_H,
        parent, nullptr, GetModuleHandleW(nullptr), this);

    _r.Initialize();
    _r.Attach(_hwnd);
    if (auto* dw = _r.DWrite()) {
        dw->CreateTextFormat(L"Microsoft YaHei", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"zh-CN", &_font);
        dw->CreateTextFormat(L"Microsoft YaHei", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"zh-CN", &_smallFont);
    }

    // 深色标题栏
    BOOL dark = TRUE;
    DwmSetWindowAttribute(_hwnd, 20, &dark, sizeof(dark));

    // 先算默认质量(100)的预估大小
    RecomputeSize();

    // 模态：禁用父窗口
    EnableWindow(parent, FALSE);
    ShowWindow(_hwnd, SW_SHOW);
    UpdateWindow(_hwnd);
    SetTimer(_hwnd, TIMER_ID, 50, nullptr);  // 50ms 检查防抖重算

    // 模态循环（同 SettingsPanel：PeekMessage + WaitMessage，IsWindow 判断退出）
    MSG msg;
    while (IsWindow(_hwnd)) {
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        } else {
            WaitMessage();
        }
    }

    EnableWindow(parent, TRUE);
    if (IsWindow(parent)) { SetForegroundWindow(parent); SetFocus(parent); }
    return _result;
}

LRESULT CALLBACK SaveAsDialog::WProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    SaveAsDialog* self = (SaveAsDialog*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        self = (SaveAsDialog*)((CREATESTRUCTW*)l)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    }
    if (!self) return DefWindowProcW(hwnd, msg, w, l);

    switch (msg) {
    case WM_PAINT:
        self->Paint();
        ValidateRect(hwnd, nullptr);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN:
        self->LDown(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        return 0;
    case WM_LBUTTONUP:
        self->LUp(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        return 0;
    case WM_MOUSEMOVE:
        self->Move(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        return 0;
    case WM_KEYDOWN:
        self->Key((int)w);
        return 0;
    case WM_TIMER:
        // 防抖：拖动停手 200ms 后重算预估大小
        if (w == TIMER_ID && self->_needRecompute &&
            std::chrono::steady_clock::now() >= self->_recomputeDeadline) {
            self->_needRecompute = false;
            self->RecomputeSize();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_CLOSE:
        self->_result = -1;  // 关闭即取消
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_ID);
        return 0;  // 不 PostQuitMessage（避免影响主窗口）
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

// 屏幕 x → 质量 1-100
int SaveAsDialog::XToQuality(int x) const {
    float t = ((float)x - _sliderR.x) / _sliderR.w;
    if (t < 0) t = 0; else if (t > 1) t = 1;
    return (int)(1 + t * 99 + 0.5f);
}

void SaveAsDialog::ScheduleRecompute() {
    _needRecompute = true;
    _recomputeDeadline = std::chrono::steady_clock::now() +
                         std::chrono::milliseconds(RECOMPUTE_DEBOUNCE_MS);
}

// 用当前质量编码一次，更新预估大小
void SaveAsDialog::RecomputeSize() {
    if (!_pixels) { _estimatedSize = 0; return; }
    std::vector<uint8_t> jpeg;
    if (JpegDecoder::EncodeJpeg(_pixels, _w, _h, _stride, _quality, jpeg)) {
        _estimatedSize = jpeg.size();
    }
}

int SaveAsDialog::HitTest(int x, int y) {
    auto in = [&](const FRect& r) {
        return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
    };
    if (in(_saveR)) return 1;
    if (in(_cancelR)) return 2;
    // 滑条热区（上下扩展 8px 便于点击）
    if (x >= _sliderR.x - 6 && x < _sliderR.x + _sliderR.w + 6 &&
        y >= _sliderR.y - 10 && y < _sliderR.y + _sliderR.h + 10) return 3;
    return -1;
}

void SaveAsDialog::LDown(int x, int y) {
    int id = HitTest(x, y);
    if (id == 1) { _pressedSave = true; InvalidateRect(_hwnd, nullptr, FALSE); return; }
    if (id == 3) {  // 滑条按下即跳到该位置
        _dragging = true;
        int q = XToQuality(x);
        if (q != _quality) { _quality = q; ScheduleRecompute(); InvalidateRect(_hwnd, nullptr, FALSE); }
    }
}

void SaveAsDialog::LUp(int x, int y) {
    if (_dragging) { _dragging = false; return; }
    if (_pressedSave) {
        _pressedSave = false;
        if (HitTest(x, y) == 1) { _result = _quality; DestroyWindow(_hwnd); return; }
        InvalidateRect(_hwnd, nullptr, FALSE);
    }
    if (HitTest(x, y) == 2) { _result = -1; DestroyWindow(_hwnd); return; }  // 取消
}

void SaveAsDialog::Move(int x, int y) {
    if (_dragging) {
        int q = XToQuality(x);
        if (q != _quality) { _quality = q; ScheduleRecompute(); InvalidateRect(_hwnd, nullptr, FALSE); }
        return;
    }
    int id = HitTest(x, y);
    if (id != _hoverId) { _hoverId = id; InvalidateRect(_hwnd, nullptr, FALSE); }
}

void SaveAsDialog::Key(int vk) {
    if (vk == VK_ESCAPE) { _result = -1; DestroyWindow(_hwnd); return; }
    if (vk == VK_RETURN) { _result = _quality; DestroyWindow(_hwnd); return; }
}

void SaveAsDialog::Paint() {
    _r.BeginDraw();
    _r.Clear(0x1E / 255.f, 0x1E / 255.f, 0x1E / 255.f);

    float fw = (float)_r.Width(), fh = (float)_r.Height();

    // 布局自上而下垂直分带，互不重叠：标题 / 质量值 / 滑条+刻度 / 预估大小 / 按钮
    // 文本统一 CENTER 对齐并给足宽度，避免 "质量: 100" 三位数被截断
    auto centerText = [&]() {
        _font->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        _font->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    };
    auto resetText = [&]() {
        _font->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        _font->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    };

    // ── 标题 ──
    if (_font) {
        centerText();
        _r.DrawText(L"另存为 - JPEG 质量", 18, _font.Get(), 0, 14, fw, 28, Cf(0xFF, 0xFF, 0xFF));
    }

    // ── 质量数值（整行居中，宽度充足，任何 1-100 值都不截断）──
    if (_font) {
        std::wstring q = L"质量: " + std::to_wstring(_quality);
        centerText();
        _r.DrawText(q.c_str(), (int)q.size(), _font.Get(), 24, 58, fw - 48, 28, Cf(0xCC, 0xCC, 0xCC));
    }

    // ── 滑条 ──
    float sx = 24, sy = 112, sw = fw - 48, sh = 16;
    _sliderR = { sx, sy, sw, sh };
    float trackY = sy + (sh - 4) / 2;  // 轨道垂直居中
    // 轨道（深色圆角矩形）
    _r.FillRoundedRectangle(sx, trackY, sw, 4, 2, 2, Cf(0x44, 0x44, 0x44));
    // 滑块（accent 绿圆，位置由质量决定）
    float knobX = sx + (float)(_quality - 1) / 99.0f * sw;
    float knobY = sy + sh / 2;
    _r.FillCircle(knobX, knobY, 8, Cf(0x90, 0xC2, 0x08));

    // "1" / "100" 刻度
    if (_smallFont) {
        _smallFont->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        _r.DrawText(L"1", 1, _smallFont.Get(), sx - 4, sy + sh + 4, 20, 14, Cf(0x88, 0x88, 0x88));
        _r.DrawText(L"100", 3, _smallFont.Get(), sx + sw - 16, sy + sh + 4, 20, 14, Cf(0x88, 0x88, 0x88));
        _smallFont->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }

    // ── 预估大小（滑条下方独立一行，不与按钮重叠）──
    if (_font) {
        std::wstring sz = L"约 " + FormatSize(_estimatedSize);
        centerText();
        _r.DrawText(sz.c_str(), (int)sz.size(), _font.Get(), 24, 156, fw - 48, 24, Cf(0x88, 0x88, 0x88));
    }

    // ── 保存 / 取消按钮（底部，预估大小下方留足间距）──
    constexpr int BW = 80, BH = 28;
    float saveX = fw - BW - 16;
    float btnCancelX = saveX - BW - 8;
    float btnY = fh - BH - 16;
    _saveR = { saveX, btnY, (float)BW, (float)BH };
    _cancelR = { btnCancelX, btnY, (float)BW, (float)BH };

    // 保存按钮（accent 绿按下态，否则深色 + hover）
    bool saveHover = (_hoverId == 1);
    _r.FillRectangle(saveX, btnY, BW, BH,
        _pressedSave ? Cf(0x90, 0xC2, 0x08) : (saveHover ? Cf(0x44, 0x44, 0x44) : Cf(0x38, 0x38, 0x38)));
    _r.DrawRectangle(saveX, btnY, BW, BH, Cf(0x55, 0x55, 0x55), 1.0f);
    // 取消按钮
    bool cancelHover = (_hoverId == 2);
    _r.FillRectangle(btnCancelX, btnY, BW, BH,
        cancelHover ? Cf(0x44, 0x44, 0x44) : Cf(0x38, 0x38, 0x38));
    _r.DrawRectangle(btnCancelX, btnY, BW, BH, Cf(0x55, 0x55, 0x55), 1.0f);

    if (_font) {
        centerText();
        _r.DrawText(L"保存", 2, _font.Get(), saveX, btnY, BW, BH,
            _pressedSave ? Cf(0x1A, 0x1A, 0x1A) : Cf(0xCC, 0xCC, 0xCC));
        _r.DrawText(L"取消", 2, _font.Get(), btnCancelX, btnY, BW, BH, Cf(0xCC, 0xCC, 0xCC));
        resetText();
    }

    _r.EndDraw();
}
