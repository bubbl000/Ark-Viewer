#include "MessageBoxDlg.h"
#include "D2DRenderer.h"
#include <dwmapi.h>
#include <dwrite.h>
#include <windowsx.h>  // GET_X_LPARAM

#pragma comment(lib, "dwmapi.lib")

// ─── 自绘深色消息弹窗 ───
// 样式与主界面统一：背景 #2C2C2C、边框 #444、标题 accent 绿、正文 #CCC、
// 主按钮 accent 绿 #90C208、次按钮 #3A3A3A、微软雅黑
namespace {

constexpr const wchar_t* DLG_CLASS = L"ArkViewer2_MessageBoxDlg";
constexpr int W = 420, H = 180;               // 窗口尺寸
constexpr int BTN_W = 96, BTN_H = 34;         // 按钮尺寸
constexpr int PAD = 20;                       // 边距

// 按钮区域
RECT g_btnMain = { W - BTN_W * 2 - PAD * 2, H - BTN_H - PAD, W - BTN_W - PAD * 2, H - PAD };  // 主（确定/是）
RECT g_btnAlt  = { W - BTN_W - PAD,          H - BTN_H - PAD, W - PAD,             H - PAD };  // 次（否）

bool g_yesNo = false;
bool g_resultSet = false;
int  g_result = IDOK;
std::wstring g_text, g_title;
D2DRenderer* g_d2d = nullptr;
ComPtr<IDWriteTextFormat> g_titleFmt, g_bodyFmt, g_btnFmt;
bool g_hoverMain = false, g_hoverAlt = false;

D2D1_COLOR_F Cf(float r, float g, float b) { return D2D1::ColorF(r / 255.f, g / 255.f, b / 255.f); }

// 创建文本格式（雅黑 + 字号 + 水平居中）
void InitTextFormats() {
    if (!g_d2d) return;
    auto* dw = g_d2d->DWrite();
    if (!dw) return;
    dw->CreateTextFormat(L"Microsoft YaHei", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 15.f,
                         L"zh-cn", &g_titleFmt);
    dw->CreateTextFormat(L"Microsoft YaHei", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.f,
                         L"zh-cn", &g_bodyFmt);
    // 按钮格式：水平+垂直双居中（标题/正文左对齐，按钮文本必须居中于按钮内部）
    dw->CreateTextFormat(L"Microsoft YaHei", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.f,
                         L"zh-cn", &g_btnFmt);
    if (g_titleFmt) g_titleFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    if (g_bodyFmt)  g_bodyFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    if (g_btnFmt) {
        g_btnFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        g_btnFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
}

void EndDialog(int result) {
    g_result = result;
    g_resultSet = true;
}

LRESULT CALLBACK DlgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        int cornerPref = 2;
        DwmSetWindowAttribute(hwnd, 33, &cornerPref, sizeof(cornerPref));
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        if (g_d2d && g_d2d->BeginDraw()) {
            g_d2d->Clear(0x2C / 255.f, 0x2C / 255.f, 0x2C / 255.f);
            // 边框
            g_d2d->DrawRectangle(0, 0, (float)W, (float)H, Cf(0x44, 0x44, 0x44), 1.0f);
            // 标题（accent 绿）
            if (g_titleFmt)
                g_d2d->DrawText(g_title.c_str(), g_title.size(), g_titleFmt.Get(),
                                (float)PAD, 14, (float)(W - PAD * 2), 26, Cf(0x90, 0xC2, 0x08));
            // 正文（#CCC，自动换行区域）
            if (g_bodyFmt)
                g_d2d->DrawText(g_text.c_str(), g_text.size(), g_bodyFmt.Get(),
                                (float)PAD, 52, (float)(W - PAD * 2), 80, Cf(0xCC, 0xCC, 0xCC));
            // 主按钮
            g_d2d->FillRoundedRectangle((float)g_btnMain.left, (float)g_btnMain.top,
                                        (float)BTN_W, (float)BTN_H, 6, 6,
                                        g_hoverMain ? Cf(0xA8, 0xD6, 0x1E) : Cf(0x90, 0xC2, 0x08));
            if (g_btnFmt)
                g_d2d->DrawText(g_yesNo ? L"是" : L"确定", g_yesNo ? 1 : 2, g_btnFmt.Get(),
                                (float)g_btnMain.left, (float)g_btnMain.top,
                                (float)BTN_W, (float)BTN_H, Cf(0x1E, 0x1E, 0x1E));
            // 次按钮（仅确认模式）
            if (g_yesNo) {
                g_d2d->FillRoundedRectangle((float)g_btnAlt.left, (float)g_btnAlt.top,
                                            (float)BTN_W, (float)BTN_H, 6, 6,
                                            g_hoverAlt ? Cf(0x48, 0x48, 0x48) : Cf(0x3A, 0x3A, 0x3A));
                if (g_btnFmt)
                    g_d2d->DrawText(L"否", 1, g_btnFmt.Get(),
                                    (float)g_btnAlt.left, (float)g_btnAlt.top,
                                    (float)BTN_W, (float)BTN_H, Cf(0xCC, 0xCC, 0xCC));
            }
            g_d2d->EndDraw();
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        bool hm = PtInRect(&g_btnMain, pt);
        bool ha = g_yesNo && PtInRect(&g_btnAlt, pt);
        if (hm != g_hoverMain || ha != g_hoverAlt) {
            g_hoverMain = hm; g_hoverAlt = ha;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (PtInRect(&g_btnMain, pt)) {
            EndDialog(g_yesNo ? IDYES : IDOK);
            DestroyWindow(hwnd);
        } else if (g_yesNo && PtInRect(&g_btnAlt, pt)) {
            EndDialog(IDNO);
            DestroyWindow(hwnd);
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wParam == VK_RETURN || wParam == VK_ESCAPE) {
            EndDialog((wParam == VK_RETURN) ? (g_yesNo ? IDYES : IDOK) : (g_yesNo ? IDNO : IDCANCEL));
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_CLOSE:
        EndDialog(IDCANCEL);
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

namespace MessageBoxDlg {

int Show(HWND hwndOwner, const std::wstring& text, const std::wstring& title, bool yesNo) {
    g_yesNo = yesNo;
    g_text = text;
    g_title = title.empty() ? L"提示" : title;
    g_resultSet = false;
    g_result = IDCANCEL;
    g_hoverMain = g_hoverAlt = false;
    g_titleFmt.Reset();
    g_bodyFmt.Reset();
    g_btnFmt.Reset();

    static bool s_registered = false;
    if (!s_registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DlgWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
        wc.lpszClassName = DLG_CLASS;
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        if (!RegisterClassExW(&wc)) return IDCANCEL;
        s_registered = true;
    }

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, DLG_CLASS, title.c_str(),
                                WS_POPUP, 0, 0, W, H, hwndOwner, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) return IDCANCEL;

    // 居中于 owner
    RECT rcOwner = { 0, 0, 800, 600 };
    if (hwndOwner && IsWindow(hwndOwner)) GetWindowRect(hwndOwner, &rcOwner);
    int x = rcOwner.left + ((rcOwner.right - rcOwner.left) - W) / 2;
    int y = rcOwner.top + ((rcOwner.bottom - rcOwner.top) - H) / 2;
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE);

    D2DRenderer d2d;
    g_d2d = &d2d;
    bool ok = d2d.Initialize() && d2d.Attach(hwnd);
    InitTextFormats();
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // 模态：禁用 owner
    if (hwndOwner && IsWindow(hwndOwner)) EnableWindow(hwndOwner, FALSE);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (g_resultSet) {
            PostQuitMessage(0);
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (hwndOwner && IsWindow(hwndOwner)) EnableWindow(hwndOwner, TRUE);
    g_d2d = nullptr;
    g_titleFmt.Reset();
    g_bodyFmt.Reset();
    g_btnFmt.Reset();
    return g_result;
}

}  // namespace MessageBoxDlg
