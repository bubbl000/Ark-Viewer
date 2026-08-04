#include "UIEngine.h"
#include "..\resources\resource.h"
#include "D2DRenderer.h"
#include "PreDecodeCache.h"
#include "Config.h"
#include <d2d1.h>
#include <sstream>
#include <algorithm>
#include <cmath>

UIEngine::UIEngine() = default;
UIEngine::~UIEngine() {
    if (_textFormat)  _textFormat->Release();
    if (_smallFormat) _smallFormat->Release();
}

void UIEngine::Initialize(IDWriteFactory* dwrite) {
    _dwrite = dwrite;
    _dwrite->CreateTextFormat(L"Microsoft YaHei", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"zh-CN", &_textFormat);
    _dwrite->CreateTextFormat(L"Microsoft YaHei", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"zh-CN", &_smallFormat);
    if (_textFormat)  _textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    if (_smallFormat) _smallFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    CreateToolbarButtons();
}

void UIEngine::SetStatusText(const std::wstring& t) { _statusText = t; }
void UIEngine::SetZoomText(const std::wstring& t)   { _zoomText = t; }
void UIEngine::SetImageInfo(const std::wstring& t)  { _infoText = t; }

void UIEngine::CreateToolbarButtons() {
    _toolbarButtons.clear();
    // Prism 底部工具栏：左组ℹ信息，右组↺↻旋转 + ↔适应 + 1:1
    // （打开/翻页/全屏已移至汉堡菜单与边缘按钮，更多菜单并入汉堡）
    struct { int id; const wchar_t* label; } buttons[] = {
        { IDM_VIEW_INFO,         L"\u2139" },  // ℹ 左组：信息
        { IDM_VIEW_ROTATE_LEFT,  L"\u21BA" },  // ↺ 右组：左旋
        { IDM_VIEW_ROTATE_RIGHT, L"\u21BB" },  // ↻ 右组：右旋
        { IDM_ZOOM_FIT,          L"\u2194" },  // ↔ 右组：适应窗口
        { IDM_ZOOM_100,          L"1:1" },     // 右组：原始大小
        { IDM_VIEW_MORE,         L"\u22EE" },  // ⋮ 右组：更多/设置
    };
    for (auto& b : buttons) {
        _toolbarButtons.push_back({ b.id, b.label, {} });
    }
}

void UIEngine::Draw(D2DRenderer& r) {
    if (_fullscreen) {
        // 全屏模式：工具栏在底部，鼠标移近底部显示工具栏
        int bottomZone = (int)_theme.toolbarHeight + 4;
        bool showBottom = _mouseY >= r.Height() - bottomZone;
        if (showBottom) {
            DrawToolbar(r);
        }
        DrawFullscreenButtons(r);  // 右上角始终显示
    } else {
        DrawTitlebar(r);            // 顶部 35px 自绘标题栏
        DrawToolbar(r);             // 底部工具栏（贴底，状态栏已移除）
        DrawInfoOverlay(r);         // 空状态（无图时显示）
    }
    DrawEdgeNavButtons(r);  // 边缘导航按钮（全屏/非全屏都显示）
    DrawThumbBar(r);        // 缩略图条（底部，智能隐藏，alpha=0 时跳过）
    DrawBirdsEye(r);        // 鸟瞰图（右下角，图片超出视口时显示）
    if (_showExifPanel) DrawExifPanel(r);
    if (_showMorePanel) DrawMorePanel(r);
    DrawGifPanel(r);  // GIF 控制面板（_gifPanelVisible=false 时跳过）
}

// 边缘导航按钮（Prism 38×64）：默认 #44000000 半透明，hover 绿 #90C208
// alpha 由 _edgeNavAlphaL/_edgeNavAlphaR 控制（30ms 定时器插值，鼠标靠近边缘渐显）
void UIEngine::DrawEdgeNavButtons(D2DRenderer& r) {
    int winW = r.Width();
    int winH = r.Height();
    int btnW = 38, btnH = 64;
    int y = (winH - btnH) / 2;
    // 垂直居中、贴左右边缘（x=0 / x=winW-btnW），符合 Prism NavBtnStyle
    _edgeNavLeft  = { 0, y, btnW, y + btnH };
    _edgeNavRight = { winW - btnW, y, winW, y + btnH };

    auto inRect = [&](const RECT& rc) {
        return _mouseX >= rc.left && _mouseX < rc.right &&
               _mouseY >= rc.top && _mouseY < rc.bottom;
    };
    auto drawNav = [&](const RECT& rc, float alpha, const wchar_t* glyph) {
        if (alpha < 0.01f) return;  // 完全透明不绘制
        bool hover = inRect(rc) && alpha > 0.3f;
        // 默认 #44000000（alpha≈0.27），hover 用强调绿
        D2D1_COLOR_F bg = hover ? _theme.accent
                                : D2D1::ColorF(0, 0, 0, alpha * 0.27f);
        // 圆角背景（半径 8，与窗口圆角风格一致）
        r.FillRoundedRectangle((float)rc.left, (float)rc.top, (float)btnW, (float)btnH, 8, 8, bg);
        if (_textFormat && alpha > 0.1f) {
            // 水平+垂直双居中：_textFormat 水平已 CENTER，临时切垂直 CENTER，画完恢复 NEAR
            _textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            r.DrawText(glyph, wcslen(glyph), _textFormat,
                (float)rc.left, (float)rc.top, (float)btnW, (float)btnH,
                hover ? D2D1::ColorF(1, 1, 1, alpha) : D2D1::ColorF(1, 1, 1, alpha * 0.8f));
            _textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }
    };
    drawNav(_edgeNavLeft,  _edgeNavAlphaL, L"\u25C0");  // ◀
    drawNav(_edgeNavRight, _edgeNavAlphaR, L"\u25B6");  // ▶
}

// 自绘标题栏（Prism 35px）：左=软件名(#777)，中=文件名|尺寸|大小，右=5按钮(hover#55FFFFFF/关闭红)
void UIEngine::DrawTitlebar(D2DRenderer& r) {
    int winW = r.Width();
    float h = UI_TITLEBAR_HEIGHT;

    // 标题栏背景（无底部分隔线）
    r.FillRectangle(0, 0, (float)winW, h, _theme.bgTitlebar);

    // 标题栏文字垂直居中：临时设 CENTER，画完恢复
    // （_smallFormat 被 zoomText 等复用，对齐状态不固定，须显式设置保证稳定）
    DWRITE_PARAGRAPH_ALIGNMENT prevPa = DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
    if (_smallFormat) {
        prevPa = _smallFormat->GetParagraphAlignment();
        _smallFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    // 左侧：软件名（#777，纯文字无图标避免 D2D/GDI 混用）
    if (_smallFormat) {
        r.DrawText(L"Ark Viewer", 10, _smallFormat, 10, 0, 120, h,
                   _theme.textSecondary);
    }

    // 中间居中：文件名(#CCC) | 尺寸(#888) | 大小(#888)，仅 _centerFileName 非空时显示
    // 窗口窄时按优先级隐藏：先藏文件大小→再藏尺寸；文件名仍超宽则省略号截断（单行不重叠）
    if (!_centerFileName.empty() && _smallFormat && _dwrite) {
        std::wstring s2 = _centerDims.empty()     ? L"" : (L" | " + _centerDims);
        std::wstring s3 = _centerFileSize.empty() ? L"" : (L" | " + _centerFileSize);
        auto measure = [&](const std::wstring& s) -> float {
            if (s.empty()) return 0;
            ComPtr<IDWriteTextLayout> layout;
            // maxWidth=0 会让 DWrite 按 0 宽度逐字换行，文字变成竖排三行
            // 用整窗宽度作上限 + 禁止换行，测得真实单行宽度
            _dwrite->CreateTextLayout(s.c_str(), (UINT32)s.size(), _smallFormat,
                                      (float)winW, 0, &layout);
            if (layout) layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            DWRITE_TEXT_METRICS m{};
            if (layout) layout->GetMetrics(&m);
            return m.widthIncludingTrailingWhitespace;
        };
        // 可用宽度：左侧避让"Ark Viewer"(140)，右侧避让 5 按钮(UI_TITLEBAR_HEIGHT*5)
        float leftReserve = 140.0f;
        float rightReserve = (float)UI_TITLEBAR_HEIGHT * 5;
        float availW = (float)winW - leftReserve - rightReserve;
        if (availW < 40.0f) availW = 40.0f;

        float w1 = measure(_centerFileName);
        float w2 = measure(s2);
        float w3 = measure(s3);
        // 优先级隐藏：总宽超 availW 依次藏大小→藏尺寸
        if (w1 + w2 + w3 > availW) { s3.clear(); w3 = 0; }
        if (w1 + w2      > availW) { s2.clear(); w2 = 0; }

        float cx = ((float)winW - (w1 + w2 + w3)) / 2;
        if (cx < leftReserve) cx = leftReserve;
        // 文件名超宽时省略号截断（此时 s2/s3 已隐藏，无间隙问题）
        if (w1 > availW) {
            r.DrawTextTrimmed(_centerFileName.c_str(), (UINT32)_centerFileName.size(),
                _smallFormat, cx, 0, availW, h, _theme.textPrimary);
        } else {
            r.DrawText(_centerFileName.c_str(), (UINT32)_centerFileName.size(), _smallFormat,
                       cx, 0, w1 + 2, h, _theme.textPrimary);
        }
        if (w2 > 0)
            r.DrawText(s2.c_str(), (UINT32)s2.size(), _smallFormat,
                       cx + w1, 0, w2 + 2, h, _theme.textSecondary);
        if (w3 > 0)
            r.DrawText(s3.c_str(), (UINT32)s3.size(), _smallFormat,
                       cx + w1 + w2, 0, w3 + 2, h, _theme.textSecondary);
    }

    // 恢复 _smallFormat 原对齐，避免影响后续 zoomText/EXIF 面板绘制
    if (_smallFormat) _smallFormat->SetParagraphAlignment(prevPa);

    // 右侧 5 按钮（32×25 垂直居中）：≡设置 ⛶全屏 ─最小化 ▢最大化 ✕关闭
    // 右侧留 8px 边距，关闭按钮不贴窗口边缘
    int btnW = 32, btnH = 25;
    int by = (int)((h - btnH) / 2);
    int rx = winW - 8;
    auto setBtn = [&](RECT& rc) { rc = { rx - btnW, by, rx, by + btnH }; rx -= btnW; };
    setBtn(_tbCloseBtn);
    setBtn(_tbMaxBtn);
    setBtn(_tbMinBtn);
    setBtn(_tbFsBtn);
    setBtn(_tbMenuBtn);

    auto inBtn = [&](const RECT& rc) {
        return _mouseX >= rc.left && _mouseX < rc.right &&
               _mouseY >= rc.top && _mouseY < rc.bottom;
    };
    auto drawBtn = [&](const RECT& rc, const wchar_t* glyph, bool isClose) {
        bool hover = inBtn(rc);
        if (hover) {
            r.FillRectangle((float)rc.left, (float)rc.top,
                (float)btnW, (float)btnH,
                isClose ? _theme.closeRed : _theme.bgBtnHoverTb);
        }
        if (_textFormat) {
            r.DrawText(glyph, wcslen(glyph), _textFormat,
                (float)rc.left, (float)rc.top + 3,
                (float)btnW, (float)(btnH - 6),
                hover ? D2D1::ColorF(1, 1, 1, 1) : _theme.textPrimary);
        }
    };
    drawBtn(_tbMenuBtn,  L"\u2630", false);  // ≡ 设置
    drawBtn(_tbFsBtn,    L"\u26F6", false);  // ⛶ 全屏
    drawBtn(_tbMinBtn,   L"\u2500", false);  // ─ 最小化
    drawBtn(_tbMaxBtn,   L"\u25A2", false);  // ▢ 最大化
    drawBtn(_tbCloseBtn, L"\u2715", true);   // ✕ 关闭（hover 红）
}

void UIEngine::DrawToolbar(D2DRenderer& r) {
    int winW = r.Width();
    float h = _theme.toolbarHeight;
    // 工具栏贴底（状态栏已移除）
    float offsetY = (float)r.Height() - h;

    // 工具栏背景（无底部分隔线）
    r.FillRectangle(0, offsetY, (float)winW, h, _theme.bgToolbar);

    int bs = _theme.buttonSize;   // 34
    int pad = _theme.padding;     // 6
    int btnY = (int)(offsetY + (h - bs) / 2);

    auto inRect = [&](const RECT& rc) {
        return _mouseX >= rc.left && _mouseX < rc.right &&
               _mouseY >= rc.top && _mouseY < rc.bottom;
    };
    // 工具栏按钮：hover 填 #33FFFFFF + 白字，否则透明 + #CCC
    auto drawToolBtn = [&](const RECT& rc, const wchar_t* label) {
        bool hover = inRect(rc);
        if (hover)
            r.FillRectangle((float)rc.left, (float)rc.top, (float)bs, (float)bs, _theme.bgButtonHover);
        if (_textFormat) {
            // 水平+垂直双居中：_textFormat 水平已 CENTER，临时切垂直 CENTER，画完恢复 NEAR
            _textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            r.DrawText(label, wcslen(label), _textFormat,
                (float)rc.left, (float)rc.top, (float)bs, (float)bs,
                hover ? D2D1::ColorF(1, 1, 1, 1) : _theme.textPrimary);
            _textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }
    };
    // 竖分隔线（1×18 #404040，垂直居中）
    auto drawDivider = [&](float x) {
        r.FillRectangle(x, offsetY + (h - 18) / 2, 1, 18, _theme.divider);
    };
    // 按 id 把 rect 写回 _toolbarButtons（供 HitTest）
    auto syncRect = [&](int id, const RECT& rc) {
        for (auto& b : _toolbarButtons) if (b.id == id) b.rect = rc;
    };

    // ── 左组：ℹ信息 | 分隔线 | 缩放%框(#333) | 图片序号(#555) ──
    float lx = (float)pad;
    RECT infoBtn = { (int)lx, btnY, (int)lx + bs, btnY + bs };
    drawToolBtn(infoBtn, L"\u2139");
    syncRect(IDM_VIEW_INFO, infoBtn);
    lx += bs + pad;
    drawDivider(lx); lx += 1 + pad;
    // 缩放%文本（无背景框，水平垂直居中显示）
    float zoomBoxW = 72;
    if (!_zoomText.empty() && _smallFormat) {
        _smallFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        _smallFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        r.DrawText(_zoomText.c_str(), (UINT32)_zoomText.size(), _smallFormat,
                   lx, offsetY, zoomBoxW, h, _theme.textPrimary);
        _smallFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        _smallFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }
    lx += zoomBoxW + pad;
    // 图片序号（#555 11px）
    if (!_indexText.empty() && _smallFormat)
        r.DrawText(_indexText.c_str(), (UINT32)_indexText.size(), _smallFormat,
                   lx, offsetY + (h - 14) / 2, 90, 14, _theme.textWeak);
    lx += 90 + pad;  // 左组真实结束位置（供中间区域计算）

    // ── 右组：↺左旋 ↻右旋 | 分隔线 | ↔适应 | 1:1 | ⋮更多 ──
    float rx = (float)(winW - pad);
    auto placeRight = [&](int id, const wchar_t* label) {
        rx -= bs;
        RECT rc = { (int)rx, btnY, (int)rx + bs, btnY + bs };
        drawToolBtn(rc, label);
        syncRect(id, rc);
        if (id == IDM_VIEW_MORE) _moreBtn = rc;  // 供 DrawMorePanel 锚定
        rx -= pad;
    };
    placeRight(IDM_VIEW_MORE,         L"\u22EE");  // 最右：更多/设置
    placeRight(IDM_ZOOM_100,          L"1:1");
    placeRight(IDM_ZOOM_FIT,          L"\u2194");
    drawDivider(rx - 1); rx -= 1 + pad;
    placeRight(IDM_VIEW_ROTATE_RIGHT, L"\u21BB");
    placeRight(IDM_VIEW_ROTATE_LEFT,  L"\u21BA");

    // ── 中间区域：活动摘要（_statusText）水平居中，超宽省略号截断 ──
    // 来源：ActivityLog::LastActivity()（如"瓦片: 贴入 [3,5] L2 -> disp L2 src=..."）
    if (!_statusText.empty() && _smallFormat && _dwrite && rx > lx) {
        float midW = rx - lx;
        _smallFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        r.DrawTextTrimmed(_statusText.c_str(), (UINT32)_statusText.size(),
            _smallFormat, lx, offsetY + (h - 14) / 2, midW, 14, _theme.textSecondary);
        _smallFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }
}

void UIEngine::DrawStatusBar(D2DRenderer& r) {
    int winW = r.Width();
    int winH = r.Height();
    float h = _theme.statusBarHeight;
    float y = (float)winH - h;

    r.FillRectangle(0, y, (float)winW, h, _theme.bgToolbar);
    r.DrawRectangle(0, y, (float)winW, 1, _theme.border);

    if (!_statusText.empty() && _smallFormat) {
        r.DrawText(_statusText.c_str(), _statusText.size(),
            _smallFormat, 8, y + 2, (float)winW - 200, h - 4,
            _theme.textSecondary);
    }
    if (!_zoomText.empty() && _smallFormat) {
        r.DrawText(_zoomText.c_str(), _zoomText.size(),
            _smallFormat, (float)winW - 100, y + 2, 90, h - 4,
            _theme.textSecondary);
    }
}

void UIEngine::DrawZoomControl(D2DRenderer& r) {
    // 右下角缩放滑块
    int winW = r.Width();
    int winH = r.Height();
    float sx = (float)winW - 160;
    // 工具栏在底部，缩放控件上移避让
    float sy = (float)winH - _theme.statusBarHeight - _theme.toolbarHeight - 30;
    float sw = 140, sh = 20;

    r.FillRectangle(sx, sy, sw, sh, D2D1::ColorF(0.1f, 0.1f, 0.1f, 0.6f));
    // 缩小/放大 按钮指示
    r.DrawText(L"-", 1, _textFormat, sx + 2, sy, 16, sh, _theme.textPrimary);
    r.DrawText(L"+", 1, _textFormat, sx + sw - 18, sy, 16, sh, _theme.textPrimary);
    // 缩放比例
    if (!_zoomText.empty() && _smallFormat) {
        r.DrawText(_zoomText.c_str(), _zoomText.size(),
            _smallFormat, sx + 20, sy + 2, 100, sh - 4,
            _theme.textPrimary);
    }
}

// 空状态界面（Prism）：无图时居中显示 📂 + 提示 + 支持格式 + 绿色"选择图片"按钮
// 触发条件：_centerFileName 为空（WindowManager 无图时不设中心信息）
void UIEngine::DrawInfoOverlay(D2DRenderer& r) {
    if (!_centerFileName.empty()) return;  // 有图不显示空状态
    int winW = r.Width();
    int winH = r.Height();
    float cx = (float)winW / 2;
    float cy = (float)winH / 2;

    // 📂（52px）水平居中
    if (_textFormat) {
        r.DrawText(L"\U0001F4C2", 2, _textFormat, cx - 26, cy - 110, 52, 52,
                   _theme.textSecondary);
    }
    if (_smallFormat) {
        // 主提示 + 支持格式水平居中：_smallFormat 默认 LEADING，临时切 CENTER，画完恢复
        _smallFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        r.DrawText(L"\u62D6\u653E\u56FE\u7247\u6216\u6587\u4EF6\u5939\u5230\u6B64\u5904", 12,
                   _smallFormat, cx - 130, cy - 48, 260, 22, _theme.textWeak);
        r.DrawText(L"\u652F\u6301 JPG / PNG / WebP / BMP / GIF / TIFF / PSD / HDR / HEIF / RAW / SVG / ICO / TGA / DDS",
                   64, _smallFormat, cx - 240, cy - 22, 480, 18, _theme.divider);
        _smallFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }
    // 绿色"选择图片"按钮（#90C208 110×36，hover #7AAD06）
    int btnW = 110, btnH = 36;
    _emptyOpenBtn = { (int)(cx - btnW / 2), (int)(cy + 18),
                      (int)(cx + btnW / 2), (int)(cy + 18 + btnH) };
    bool hover = _mouseX >= _emptyOpenBtn.left && _mouseX < _emptyOpenBtn.right &&
                 _mouseY >= _emptyOpenBtn.top && _mouseY < _emptyOpenBtn.bottom;
    r.FillRectangle((float)_emptyOpenBtn.left, (float)_emptyOpenBtn.top,
                    (float)btnW, (float)btnH,
                    hover ? _theme.accentHover : _theme.accent);
    if (_textFormat) {
        // 水平+垂直双居中：_textFormat 水平已 CENTER，临时切垂直 CENTER，画完恢复 NEAR
        _textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        r.DrawText(L"\u9009\u62E9\u56FE\u7247", 4, _textFormat,
            (float)_emptyOpenBtn.left, (float)_emptyOpenBtn.top,
            (float)btnW, (float)btnH, D2D1::ColorF(1, 1, 1, 1));
        _textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
}

// 全屏右上角按钮：退出全屏(⛶) + 关闭(✕)
void UIEngine::DrawFullscreenButtons(D2DRenderer& r) {
    int winW = r.Width();
    int btnSize = 28;
    int y = 4;
    // 从右到左：关闭、退出全屏（右侧留 8px 边距，与标题栏按钮一致）
    _fsCloseBtn = { winW - btnSize - 8, y, winW - 8, y + btnSize };
    _fsExitBtn  = { winW - btnSize * 2 - 16, y, winW - btnSize - 8, y + btnSize };

    auto drawBtn = [&](RECT rc, const wchar_t* glyph) {
        r.FillRectangle((float)rc.left, (float)rc.top,
            (float)(rc.right - rc.left), (float)(rc.bottom - rc.top),
            D2D1::ColorF(0.2f, 0.2f, 0.2f, 0.8f));
        if (_textFormat) {
            r.DrawText(glyph, wcslen(glyph), _textFormat,
                (float)rc.left, (float)rc.top + 2,
                (float)(rc.right - rc.left), (float)(rc.bottom - rc.top) - 4,
                _theme.textPrimary);
        }
    };
    drawBtn(_fsExitBtn, L"\u26F6");  // ⛶ 退出全屏
    drawBtn(_fsCloseBtn, L"\u2715"); // ✕ 关闭
}

// toggle 开关组件：轨道 40×20 圆角10，圆点半径8 白色
// on=轨道 accent 主题绿 + 圆点右侧；off=轨道 textWeak 灰 + 圆点左侧
void UIEngine::DrawToggleSwitch(D2DRenderer& r, float x, float y, bool on) {
    r.FillRoundedRectangle(x, y, 40, 20, 10, 10,
        on ? _theme.accent : _theme.textWeak);
    r.FillCircle(on ? x + 30 : x + 10, y + 10, 8, D2D1::ColorF(1, 1, 1, 1));
}

// "更多"浮动面板：从 ⋮ 按钮上方弹出，含鸟瞰图/缩略图两行 toggle
// 锚定 _moreBtn（工具栏最右按钮），面板水平居中对齐按钮，垂直在其上方留 4px 间隙
void UIEngine::DrawMorePanel(D2DRenderer& r) {
    constexpr float panelW = 180.0f, panelH = 88.0f;
    constexpr float rowH = 36.0f;
    // 锚定 ⋮ 按钮：面板水平中心 = 按钮中心，底部 = 按钮顶部 - 4
    float btnCx = (float)(_moreBtn.left + _moreBtn.right) / 2;
    float px = btnCx - panelW / 2;
    float py = (float)_moreBtn.top - panelH - 4;
    // 越界钳制：避免面板超出窗口左右边界
    if (px < 4) px = 4;
    if (px + panelW > r.Width() - 4) px = (float)r.Width() - panelW - 4;
    if (py < 0) py = 0;
    _morePanelRect = { (int)px, (int)py, (int)(px + panelW), (int)(py + panelH) };

    // 面板背景 + 边框（参照 DrawExifPanel 配色）
    r.FillRectangle(px, py, panelW, panelH, _theme.panelBg);
    r.DrawRectangle(px, py, panelW, panelH, _theme.border, 1.0f);

    if (!_smallFormat) return;
    // 两行：左标签 + 右 toggle（toggle 距右边缘 12，垂直居中于行）
    const wchar_t* labels[] = { L"\u9E1F\u77B0\u56FE", L"\u7F29\u7565\u56FE" };  // 鸟瞰图 / 缩略图
    bool states[] = { _birdsEyeEnabled, _thumbBarEnabled };
    RECT* toggleRects[] = { &_moreToggleBirdsEye, &_moreToggleThumb };
    for (int i = 0; i < 2; i++) {
        float rowY = py + 8 + i * rowH;
        // 标签（左对齐，垂直居中）
        r.DrawText(labels[i], wcslen(labels[i]), _smallFormat,
            px + 14, rowY + 3, 100, 22, _theme.textPrimary);
        // toggle（右侧，距右边 12）
        float tx = px + panelW - 40 - 12;
        float ty = rowY + 8;
        DrawToggleSwitch(r, tx, ty, states[i]);
        // 记录 toggle 命中区（略大于轨道便于点击）
        *toggleRects[i] = { (int)tx, (int)ty, (int)(tx + 40), (int)(ty + 20) };
    }
}

bool UIEngine::MorePanelHitTest(int x, int y) const {
    if (!_showMorePanel) return false;
    return x >= _morePanelRect.left && x < _morePanelRect.right &&
           y >= _morePanelRect.top && y < _morePanelRect.bottom;
}

// 返回点中的 toggle：0=鸟瞰图 1=缩略图 -1=无（含面板内非 toggle 区）
int UIEngine::MorePanelToggleAt(int x, int y) const {
    auto inRect = [&](const RECT& rc) {
        return x >= rc.left && x < rc.right && y >= rc.top && y < rc.bottom;
    };
    if (inRect(_moreToggleBirdsEye)) return 0;
    if (inRect(_moreToggleThumb))    return 1;
    return -1;
}

// GIF 控制面板：浮动小面板，帧号 + 上一帧/播放暂停/下一帧
// 仅 GIF 且帧数>1 时显示（由 WindowManager 每帧 SetGifPanelState 控制）
void UIEngine::DrawGifPanel(D2DRenderer& r) {
    if (!_gifPanelVisible) return;
    int winW = r.Width();
    int winH = r.Height();

    // 面板尺寸：帧号文本区 + 3 个按钮
    float btnSize = 28.0f;
    float panelH = 40.0f;
    float textW = 64.0f;  // "帧 12/34"
    float panelW = textW + btnSize * 3 + 20.0f;

    // 位置：自定义优先，否则默认右下角（避让工具栏）
    float px, py;
    if (_gifPanelPosX >= 0) {
        px = (float)_gifPanelPosX; py = (float)_gifPanelPosY;
    } else {
        px = (float)winW - panelW - 12;
        py = (float)winH - _theme.toolbarHeight - panelH - 12;
    }
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    if (px + panelW > winW) px = (float)winW - panelW;
    if (py + panelH > winH) py = (float)winH - panelH;
    _gifPanelRect = { (int)px, (int)py, (int)(px + panelW), (int)(py + panelH) };

    // 面板背景 + 边框
    r.FillRectangle(px, py, panelW, panelH, _theme.panelBg);
    r.DrawRectangle(px, py, panelW, panelH, _theme.border, 1.0f);

    // 帧号文本（垂直居中）
    if (_smallFormat) {
        _smallFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        std::wstring text = L"帧 " + std::to_wstring(_gifFrame + 1)
                          + L"/" + std::to_wstring(_gifFrameCount);
        r.DrawText(text.c_str(), (UINT32)text.size(), _smallFormat,
            px + 8, py, textW, panelH, _theme.textPrimary);
        _smallFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }

    // 3 个按钮：⏮ ▶/⏸ ⏭（垂直居中）
    auto inRect = [&](const RECT& rc) {
        return _mouseX >= rc.left && _mouseX < rc.right &&
               _mouseY >= rc.top && _mouseY < rc.bottom;
    };
    auto drawBtn = [&](RECT& rc, const wchar_t* label) {
        bool hover = inRect(rc);
        if (hover)
            r.FillRectangle((float)rc.left, (float)rc.top,
                            (float)btnSize, (float)btnSize, _theme.bgButtonHover);
        if (_textFormat) {
            _textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            r.DrawText(label, wcslen(label), _textFormat,
                (float)rc.left, (float)rc.top, (float)btnSize, (float)btnSize,
                hover ? D2D1::ColorF(1, 1, 1, 1) : _theme.textPrimary);
            _textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }
    };
    float bx = px + textW + 4;
    float btnY = py + (panelH - btnSize) / 2;
    _gifBtnPrev = { (int)bx, (int)btnY, (int)(bx + btnSize), (int)(btnY + btnSize) };
    drawBtn(_gifBtnPrev, L"\u23EE");  // ⏮
    bx += btnSize;
    _gifBtnPlay = { (int)bx, (int)btnY, (int)(bx + btnSize), (int)(btnY + btnSize) };
    drawBtn(_gifBtnPlay, _gifPlaying ? L"\u23F8" : L"\u25B6");  // ⏸ 或 ▶
    bx += btnSize;
    _gifBtnNext = { (int)bx, (int)btnY, (int)(bx + btnSize), (int)(btnY + btnSize) };
    drawBtn(_gifBtnNext, L"\u23ED");  // ⏭
}

void UIEngine::SetGifPanelState(bool visible, int frame, int count, bool playing) {
    _gifPanelVisible = visible;
    _gifFrame = frame;
    _gifFrameCount = count;
    _gifPlaying = playing;
}

int UIEngine::GifPanelHitTest(int x, int y) const {
    if (!_gifPanelVisible) return -1;
    auto inRect = [&](const RECT& rc) {
        return x >= rc.left && x < rc.right && y >= rc.top && y < rc.bottom;
    };
    if (inRect(_gifBtnPrev)) return IDM_GIF_PREV;
    if (inRect(_gifBtnPlay)) return IDM_GIF_PLAYPAUSE;
    if (inRect(_gifBtnNext)) return IDM_GIF_NEXT;
    return -1;
}

bool UIEngine::GifPanelDragHitTest(int x, int y) const {
    if (!_gifPanelVisible) return false;
    // 面板内但不在按钮上 → 可拖动
    if (x < _gifPanelRect.left || x >= _gifPanelRect.right ||
        y < _gifPanelRect.top || y >= _gifPanelRect.bottom) return false;
    auto inRect = [&](const RECT& rc) {
        return x >= rc.left && x < rc.right && y >= rc.top && y < rc.bottom;
    };
    return !(inRect(_gifBtnPrev) || inRect(_gifBtnPlay) || inRect(_gifBtnNext));
}

void UIEngine::StartGifPanelDrag(int x, int y) {
    _gifDragging = true;
    _gifDragOffX = (float)x - (float)_gifPanelRect.left;
    _gifDragOffY = (float)y - (float)_gifPanelRect.top;
}

void UIEngine::UpdateGifPanelDrag(int x, int y, int winW, int winH) {
    if (!_gifDragging) return;
    float pw = (float)(_gifPanelRect.right - _gifPanelRect.left);
    float ph = (float)(_gifPanelRect.bottom - _gifPanelRect.top);
    _gifPanelPosX = (int)((float)x - _gifDragOffX);
    _gifPanelPosY = (int)((float)y - _gifDragOffY);
    if (_gifPanelPosX < 0) _gifPanelPosX = 0;
    if (_gifPanelPosY < 0) _gifPanelPosY = 0;
    if (_gifPanelPosX + pw > winW) _gifPanelPosX = (int)((float)winW - pw);
    if (_gifPanelPosY + ph > winH) _gifPanelPosY = (int)((float)winH - ph);
}

// EXIF 信息面板：右侧半透明覆盖层，自适应字段数高度
// 遍历 _exif.fields 跳过空值，避免与字段定义耦合
// 标题/字段值单行省略号截断，面板宽度钳制到窗口内防挤压；支持鼠标拖动自定义位置
void UIEngine::DrawExifPanel(D2DRenderer& r) {
    if (!_exif.valid) return;
    int winW = r.Width();
    int winH = r.Height();

    int visibleRows = 0;
    for (auto& f : _exif.fields) if (!f.value.empty()) visibleRows++;
    if (visibleRows == 0) return;

    constexpr float rowH = 20.0f;     // 每行高度
    constexpr float titleH = 32.0f;   // 标题区
    float panelW = 280;
    // 窗口过窄时钳制面板宽度，避免标题/字段被挤压溢出
    float maxW = (float)winW - 24;
    if (panelW > maxW) panelW = maxW;
    if (panelW < 160) panelW = 160;
    float panelH = titleH + visibleRows * rowH + 10;
    float bottomReserve = _theme.toolbarHeight + 12;  // 状态栏已移除
    float maxH = (float)winH - bottomReserve - 12;
    if (panelH > maxH) panelH = maxH;

    // 位置：自定义拖动位置优先，否则默认右上角避让标题栏
    float px, py;
    if (_exifPanelPosX >= 0) {
        px = _exifPanelPosX; py = _exifPanelPosY;
    } else {
        px = (float)winW - panelW - 12;
        py = _fullscreen ? 44.0f : (UI_TITLEBAR_HEIGHT + 12);
    }
    // 越界钳制：确保面板完全在可视区内
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    if (px + panelW > winW) px = (float)winW - panelW;
    if (py + panelH > winH) py = (float)winH - panelH;
    _exifPanelRect = { (int)px, (int)py, (int)(px + panelW), (int)(py + panelH) };

    // 面板背景
    r.FillRectangle(px, py, panelW, panelH, _theme.panelBg);
    r.DrawRectangle(px, py, panelW, panelH, _theme.border, 1.0f);
    if (!_smallFormat) return;

    // 标题（省略号截断防挤压）
    r.DrawTextTrimmed(L"EXIF 信息", wcslen(L"EXIF 信息"), _textFormat,
        px + 12, py + 6, panelW - 24, 24, _theme.textPrimary);

    // 各字段（跳过空值，值超宽省略号截断）
    float rowY = py + titleH;
    for (auto& f : _exif.fields) {
        if (f.value.empty()) continue;
        r.DrawTextTrimmed(f.label, wcslen(f.label), _smallFormat,
            px + 12, rowY, 78, rowH, _theme.textSecondary);
        r.DrawTextTrimmed(f.value.c_str(), f.value.size(), _smallFormat,
            px + 92, rowY, panelW - 104, rowH, _theme.textPrimary);
        rowY += rowH;
        if (rowY > py + panelH - 4) break;  // 超出面板高度截断
    }
}

bool UIEngine::ExifPanelHitTest(int x, int y) const {
    if (!_showExifPanel) return false;
    return x >= _exifPanelRect.left && x < _exifPanelRect.right &&
           y >= _exifPanelRect.top && y < _exifPanelRect.bottom;
}

// 记录鼠标相对面板左上角的偏移，拖动时据此保持抓握点
void UIEngine::StartExifDrag(int x, int y) {
    _exifDragging = true;
    _exifDragOffX = (float)x - (float)_exifPanelRect.left;
    _exifDragOffY = (float)y - (float)_exifPanelRect.top;
}

void UIEngine::UpdateExifDrag(int x, int y, int winW, int winH) {
    if (!_exifDragging) return;
    float pw = (float)(_exifPanelRect.right - _exifPanelRect.left);
    float ph = (float)(_exifPanelRect.bottom - _exifPanelRect.top);
    _exifPanelPosX = (float)x - _exifDragOffX;
    _exifPanelPosY = (float)y - _exifDragOffY;
    // 越界钳制：至少留 20px 在窗口内，避免拖飞找不到
    if (_exifPanelPosX < 0) _exifPanelPosX = 0;
    if (_exifPanelPosY < 0) _exifPanelPosY = 0;
    if (_exifPanelPosX + pw > winW) _exifPanelPosX = (float)winW - pw;
    if (_exifPanelPosY + ph > winH) _exifPanelPosY = (float)winH - ph;
}

int UIEngine::HitTest(int x, int y) {
    // 全屏模式：优先检测右上角按钮
    if (_fullscreen) {
        if (x >= _fsCloseBtn.left && x <= _fsCloseBtn.right &&
            y >= _fsCloseBtn.top && y <= _fsCloseBtn.bottom) {
            return IDM_FILE_EXIT;  // 关闭窗口
        }
        if (x >= _fsExitBtn.left && x <= _fsExitBtn.right &&
            y >= _fsExitBtn.top && y <= _fsExitBtn.bottom) {
            return IDM_VIEW_FULLSCREEN;  // 退出全屏
        }
        // 工具栏在底部，由下方公共逻辑检测工具栏按钮
    } else {
        // 非全屏：检测自绘标题栏按钮（顶部 32px）
        auto inRect = [&](const RECT& rc) {
            return x >= rc.left && x <= rc.right && y >= rc.top && y <= rc.bottom;
        };
        if (inRect(_tbCloseBtn)) return IDM_TITLEBAR_CLOSE;
        if (inRect(_tbMaxBtn))   return IDM_TITLEBAR_MAX;
        if (inRect(_tbMinBtn))   return IDM_TITLEBAR_MIN;
        if (inRect(_tbFsBtn))    return IDM_VIEW_FULLSCREEN;
        if (inRect(_tbMenuBtn))  return IDM_TITLEBAR_MENU;
        // 标题栏非按钮区域由 WM_NCHITTEST 处理拖动，HitTest 返回 -1
        if (y < (int)UI_TITLEBAR_HEIGHT) return -1;
    }

    // 工具栏按钮点击：返回命令 ID 供调用方触发 OnCommand
    for (auto& btn : _toolbarButtons) {
        if (x >= btn.rect.left && x <= btn.rect.right &&
            y >= btn.rect.top && y <= btn.rect.bottom) {
            return btn.id;
        }
    }
    // 空状态"选择图片"按钮（无图时居中）
    if (x >= _emptyOpenBtn.left && x <= _emptyOpenBtn.right &&
        y >= _emptyOpenBtn.top && y <= _emptyOpenBtn.bottom) {
        return IDM_FILE_OPEN;
    }
    // 边缘导航按钮（alpha > 0.3 才响应点击，避免误触）
    auto inRect = [&](const RECT& rc) {
        return x >= rc.left && x <= rc.right && y >= rc.top && y <= rc.bottom;
    };
    if (_edgeNavAlphaL > 0.3f && inRect(_edgeNavLeft))  return IDM_VIEW_PREV;
    if (_edgeNavAlphaR > 0.3f && inRect(_edgeNavRight)) return IDM_VIEW_NEXT;
    return -1;
}

// 透明度动画插值：30ms 定时器调用
// 步长 0.1 → 0↔1 约 300ms 过渡，避免硬切换的视觉跳跃
bool UIEngine::UpdateAnimations() {
    _animDirty = false;
    auto step = [&](float& cur, float target) {
        if (cur == target) return;
        if (cur < target) cur = (std::min)(cur + ALPHA_STEP, target);
        else              cur = (std::max)(cur - ALPHA_STEP, target);
        _animDirty = true;
    };
    step(_thumbBarAlpha, _thumbBarTarget);
    step(_edgeNavAlphaL, _edgeNavAlphaLTarget);
    step(_edgeNavAlphaR, _edgeNavAlphaRTarget);
    return _animDirty;
}

// ── 缩略图条 ──
// 底部浮动条，固定框模式（当前图居中，其余按索引滚动），智能隐藏（alpha 动画）
// 缩略图条顶部 y（与 DrawThumbBar 的 barY 计算一致，barH=72）
// 显隐触发区据此对齐，避免硬编码脱钩导致鼠标在条内却渐隐
float UIEngine::ThumbBarTopY(float winH) const {
    constexpr float barH = 72.0f;
    return winH - _theme.toolbarHeight - barH;  // 状态栏已移除，工具栏贴底
}

// 复用 PreDecodeCache 顶层缩略图像素，GPU 纹理按索引懒创建、超可视范围淘汰
void UIEngine::DrawThumbBar(D2DRenderer& r) {
    if (!_thumbBarEnabled || !_thumbFiles || _thumbFiles->empty() || _thumbCurrent < 0) return;
    if (_thumbBarAlpha < 0.01f) return;

    int winW = r.Width(), winH = r.Height();
    constexpr float thumbW = 60.0f;   // 缩略图边长（需求规定 60x60）
    constexpr float gap = 8.0f;       // 间距
    constexpr float barH = 72.0f;     // 条高（含上下内边距）
    // 工具栏在底部，缩略图条再上移一层避让
    float barY = (float)winH - _theme.toolbarHeight - barH;  // 状态栏已移除，工具栏贴底
    float step = thumbW + gap;
    float centerX = winW * 0.5f;
    int cur = _thumbCurrent;
    int total = (int)_thumbFiles->size();
    // 可视索引范围：覆盖半屏宽度所需 + 余量
    int N = (int)(winW * 0.5f / step) + 2;

    // 半透明背景条
    float bgAlpha = _thumbBarAlpha * 0.85f;
    r.FillRectangle(0, barY, (float)winW, barH, D2D1::ColorF(0.1f, 0.1f, 0.1f, bgAlpha));
    _thumbBarRect = { 0, (int)barY, winW, (int)(barY + barH) };

    // 取/建索引 i 的顶层缩略图 GPU 纹理（懒创建，复用 PreDecodeCache）
    auto getTex = [&](int i) -> ID2D1Bitmap1* {
        auto it = _thumbTextures.find(i);
        if (it != _thumbTextures.end()) return it->second.Get();
        if (_thumbCache) {
            auto c = _thumbCache->Get(i);
            if (c && c->pixels && !c->pixels->empty() && c->width > 0 && c->height > 0) {
                auto tex = r.CreateBitmap(c->width, c->height, c->pixels->data(), c->stride);
                if (tex) { _thumbTextures[i] = tex; return tex.Get(); }
            }
        }
        return nullptr;
    };

    // hover 索引：鼠标悬停在某个非当前缩略图上时高亮反馈（点击才跳转）
    int hoverIdx = ThumbBarHitTest(_mouseX, _mouseY);

    for (int i = cur - N; i <= cur + N; i++) {
        if (i < 0 || i >= total) continue;
        float x = centerX + (i - cur) * step - thumbW * 0.5f;
        float y = barY + (barH - thumbW) * 0.5f;
        bool isCur = (i == cur);
        bool isHover = (i == hoverIdx) && !isCur;
        // 边框：当前图 accent 绿 2px，hover 白色半透 2px，其余 border 1px
        D2D1_COLOR_F bc = isCur ? _theme.accent
                        : (isHover ? D2D1::ColorF(1, 1, 1, 0.6f) : _theme.border);
        r.DrawRectangle(x - 1, y - 1, thumbW + 2, thumbW + 2, bc,
                        (isCur || isHover) ? 2.0f : 1.0f);

        if (ID2D1Bitmap1* bmp = getTex(i)) {
            // 等比缩放 contain 到 thumbW×thumbW，居中绘制
            auto sz = bmp->GetPixelSize();
            if (sz.width > 0 && sz.height > 0) {
                float s = (std::min)(thumbW / (float)sz.width, thumbW / (float)sz.height);
                float dw = sz.width * s, dh = sz.height * s;
                r.DrawBitmap(bmp, x + (thumbW - dw) * 0.5f, y + (thumbW - dh) * 0.5f,
                    dw, dh, _thumbBarAlpha, D2D1_INTERPOLATION_MODE_LINEAR);
            }
        } else {
            // 尚未解码：占位灰块
            r.FillRectangle(x, y, thumbW, thumbW, D2D1::ColorF(0.2f, 0.2f, 0.2f, bgAlpha));
        }
    }

    // 淘汰可视范围外的纹理，控制内存（保留 N+4 余量供快速翻页复用）
    for (auto it = _thumbTextures.begin(); it != _thumbTextures.end(); ) {
        if (std::abs(it->first - cur) > N + 4) it = _thumbTextures.erase(it);
        else ++it;
    }
}

// 缩略图条点击命中：反推索引，仅 alpha>0.3 时响应
int UIEngine::ThumbBarHitTest(int x, int y) {
    if (_thumbBarAlpha < 0.3f) return -1;
    if (!_thumbFiles || _thumbFiles->empty()) return -1;
    if (y < _thumbBarRect.top || y > _thumbBarRect.bottom) return -1;
    int cur = _thumbCurrent;
    int total = (int)_thumbFiles->size();
    constexpr float thumbW = 60.0f, gap = 8.0f;
    float step = thumbW + gap;
    float centerX = (_thumbBarRect.left + _thumbBarRect.right) * 0.5f;
    int i = cur + (int)std::lround((x + thumbW * 0.5f - centerX) / step);
    if (i < 0 || i >= total) return -1;
    // 精确判断 x 是否落在该格子内（含 2px 容差）
    float cellX = centerX + (i - cur) * step - thumbW * 0.5f;
    if (x < cellX - 2 || x > cellX + thumbW + 2) return -1;
    return i;
}

void UIEngine::ClearThumbTextures() { _thumbTextures.clear(); }

// ── 鸟瞰图 ──
// 右下角小图，仅当图片显示超出视口（放大/平移后）才显示
// 蓝框表示当前视口在完整图中的位置，拖动蓝框平移主图
void UIEngine::DrawBirdsEye(D2DRenderer& r) {
    if (!_birdsEyeEnabled || !_viewport.hasImage) return;
    int imgW = _viewport.imgW, imgH = _viewport.imgH;
    if (imgW <= 0 || imgH <= 0) return;
    int winW = _viewport.winW, winH = _viewport.winH;
    bool sideways = (_viewport.rotation == 90 || _viewport.rotation == 270);
    // 显示尺寸（90/270 时宽高互换）
    double dispW = (sideways ? imgH : imgW) * _viewport.scaleX;
    double dispH = (sideways ? imgW : imgH) * _viewport.scaleY;
    // 图片完整在视口内：不显示鸟瞰图
    if (dispW <= winW + 1 && dispH <= winH + 1) return;

    // 鸟瞰图尺寸：最大 160，保持显示比例
    constexpr float beMax = 160.0f;
    float beW, beH;
    if (dispW >= dispH) { beW = beMax; beH = (float)(beMax * dispH / dispW); }
    else { beH = beMax; beW = (float)(beMax * dispW / dispH); }
    float beX = (float)r.Width() - beW - 16;
    // 底部避让：工具栏 + 缩略图条(随 alpha 联动显示时+88) + 10px margin（状态栏已移除）
    float bottomReserve = _theme.toolbarHeight
                          + _thumbBarAlpha * 88.0f + 10.0f;
    float beY = (float)r.Height() - bottomReserve - beH;
    _birdsEyeRect = { (int)beX, (int)beY, (int)(beX + beW), (int)(beY + beH) };
    _beImgX = beX; _beImgY = beY; _beImgW = beW; _beImgH = beH;

    // 背景 #CC1A1A1A + 边框 #444（Prism 配色）
    r.FillRectangle(beX, beY, beW, beH, D2D1::ColorF(0x1A/255.f, 0x1A/255.f, 0x1A/255.f, 0.8f));
    r.DrawRectangle(beX, beY, beW, beH, _theme.border, 1.0f);

    // 绘制缩略图（复用当前图顶层缩略图纹理，带旋转/翻转保持与主图方向一致）
    ID2D1Bitmap1* bmp = nullptr;
    auto it = _thumbTextures.find(_thumbCurrent);
    if (it != _thumbTextures.end()) bmp = it->second.Get();
    if (!bmp && _thumbCache) {
        auto c = _thumbCache->Get(_thumbCurrent);
        if (c && c->pixels && !c->pixels->empty() && c->width > 0 && c->height > 0) {
            auto tex = r.CreateBitmap(c->width, c->height, c->pixels->data(), c->stride);
            if (tex) { bmp = tex.Get(); _thumbTextures[_thumbCurrent] = tex; }
        }
    }
    if (bmp) {
        if (_viewport.rotation || _viewport.flipH || _viewport.flipV) {
            r.DrawBitmapRotated(bmp, beX, beY, beW, beH,
                _viewport.rotation, _viewport.flipH, _viewport.flipV);
        } else {
            r.DrawBitmap(bmp, beX, beY, beW, beH, 0.9f);
        }
    }

    // 蓝框：可见部分占显示图的比例 → 映射到鸟瞰图坐标
    // 显示图在窗口中位置 [offsetX, offsetX+dispW]，窗口可见 [0,winW]
    double fX0 = (0 - _viewport.offsetX) / dispW;
    double fX1 = (winW - _viewport.offsetX) / dispW;
    double fY0 = (0 - _viewport.offsetY) / dispH;
    double fY1 = (winH - _viewport.offsetY) / dispH;
    // sideways(90/270) 时 flipH/flipV 在显示图坐标系方向互换：flipH→垂直, flipV→水平
    // 由 DrawBitmapRotated 变换链推导：flip 绕预旋转框中心，旋转后框方向改变
    if (sideways) {
        if (_viewport.flipH) { double t = 1 - fY0; fY0 = 1 - fY1; fY1 = t; }
        if (_viewport.flipV) { double t = 1 - fX0; fX0 = 1 - fX1; fX1 = t; }
    } else {
        if (_viewport.flipH) { double t = 1 - fX0; fX0 = 1 - fX1; fX1 = t; }
        if (_viewport.flipV) { double t = 1 - fY0; fY0 = 1 - fY1; fY1 = t; }
    }
    fX0 = std::clamp(fX0, 0.0, 1.0); fX1 = std::clamp(fX1, 0.0, 1.0);
    fY0 = std::clamp(fY0, 0.0, 1.0); fY1 = std::clamp(fY1, 0.0, 1.0);
    float bx = beX + (float)(fX0 * beW);
    float by = beY + (float)(fY0 * beH);
    float bw = (float)((fX1 - fX0) * beW);
    float bh = (float)((fY1 - fY0) * beH);
    if (bw > 0.5f && bh > 0.5f) {
        // 视口框：填充 #2290C208 + 边框绿 #90C208 1.5px（Prism 配色）
        r.FillRectangle(bx, by, bw, bh,
                        D2D1::ColorF(_theme.accent.r, _theme.accent.g, _theme.accent.b, 0.13f));
        r.DrawRectangle(bx, by, bw, bh, _theme.accent, 1.5f);
    }
}

bool UIEngine::BirdsEyeHitTest(int x, int y) {
    if (!_birdsEyeEnabled) return false;
    return x >= _birdsEyeRect.left && x <= _birdsEyeRect.right &&
           y >= _birdsEyeRect.top && y <= _birdsEyeRect.bottom;
}

// 拖动鸟瞰图：将鼠标位置映射为图像比例，反推目标偏移，返回与当前偏移的差值
// 调用方对主图 Pan 该差值，实现"点哪居中哪"的拖动平移
bool UIEngine::BirdsEyeDrag(int x, int y, double& outPanDx, double& outPanDy) {
    if (!_birdsEyeEnabled || !_viewport.hasImage) return false;
    if (x < _birdsEyeRect.left || x > _birdsEyeRect.right ||
        y < _birdsEyeRect.top || y > _birdsEyeRect.bottom) return false;
    int imgW = _viewport.imgW, imgH = _viewport.imgH;
    bool sideways = (_viewport.rotation == 90 || _viewport.rotation == 270);
    double dispW = (sideways ? imgH : imgW) * _viewport.scaleX;
    double dispH = (sideways ? imgW : imgH) * _viewport.scaleY;
    if (dispW <= 0 || dispH <= 0 || _beImgW <= 0 || _beImgH <= 0) return false;
    double fX = (x - _beImgX) / _beImgW;
    double fY = (y - _beImgY) / _beImgH;
    // sideways 时 flipH/flipV 对应显示图方向互换（见 DrawBirdsEye 注释）
    if (sideways) {
        if (_viewport.flipH) fY = 1.0 - fY;
        if (_viewport.flipV) fX = 1.0 - fX;
    } else {
        if (_viewport.flipH) fX = 1.0 - fX;
        if (_viewport.flipV) fY = 1.0 - fY;
    }
    fX = std::clamp(fX, 0.0, 1.0); fY = std::clamp(fY, 0.0, 1.0);
    // 目标偏移：使图像 fX/fY 处对齐窗口中心
    double wantOffsetX = _viewport.winW * 0.5 - fX * dispW;
    double wantOffsetY = _viewport.winH * 0.5 - fY * dispH;
    outPanDx = wantOffsetX - _viewport.offsetX;
    outPanDy = wantOffsetY - _viewport.offsetY;
    return true;
}
