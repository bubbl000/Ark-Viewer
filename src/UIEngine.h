#include <d2d1.h>
#include <d2d1_1.h>   // ID2D1Bitmap1/D2D1_INTERPOLATION_MODE 等需 1.1+ 头文件（缩略图条/鸟瞰图纹理用）
#pragma once
#include <dwrite.h>
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include <wrl/client.h>
#include "ExifParser.h"

// ─── DirectUI 自绘引擎 ───
//  DirectUI 方案
// 所有 UI 元素直接用 D2D 绘制，无标准 HWND 子控件

class D2DRenderer;
class PreDecodeCache;

using Microsoft::WRL::ComPtr;

// 鸟瞰图视口信息（WindowManager 每帧从 ImageEngine 同步）
struct ViewportInfo {
    bool hasImage = false;
    int imgW = 0, imgH = 0;          // 原图尺寸（旋转前）
    int winW = 0, winH = 0;          // 窗口客户区
    double scaleX = 1, scaleY = 1;   // 当前显示缩放
    double offsetX = 0, offsetY = 0; // 显示偏移（像素）
    int rotation = 0;                // 0/90/180/270
    bool flipH = false, flipV = false;
};

// 自绘标题栏高度（须与 PlatformWindow::TITLEBAR_HEIGHT 一致；Prism 35px）
constexpr float UI_TITLEBAR_HEIGHT = 46.0f;  // 与工具栏 toolbarHeight 一致

// Prism 配色体系（深色系，参考 MainWindow.xaml）
struct UITheme {
    D2D1_COLOR_F bgTitlebar    = D2D1::ColorF(0x2A/255.f, 0x2A/255.f, 0x2A/255.f);  // #2A2A2A 标题栏
    D2D1_COLOR_F bgToolbar     = D2D1::ColorF(0x25/255.f, 0x25/255.f, 0x25/255.f);  // #252525 工具栏/状态栏
    D2D1_COLOR_F bgButton      = D2D1::ColorF(0, 0, 0, 0);            // 透明（hover 才填色）
    D2D1_COLOR_F bgButtonHover = D2D1::ColorF(1, 1, 1, 0.2f);         // 工具栏 hover #33FFFFFF
    D2D1_COLOR_F bgBtnHoverTb  = D2D1::ColorF(1, 1, 1, 0.33f);        // 标题栏 hover #55FFFFFF
    D2D1_COLOR_F textPrimary   = D2D1::ColorF(0xCC/255.f, 0xCC/255.f, 0xCC/255.f);  // #CCC
    D2D1_COLOR_F textSecondary = D2D1::ColorF(0x88/255.f, 0x88/255.f, 0x88/255.f);  // #888
    D2D1_COLOR_F textWeak      = D2D1::ColorF(0x55/255.f, 0x55/255.f, 0x55/255.f);  // #555
    D2D1_COLOR_F accent        = D2D1::ColorF(0x90/255.f, 0xC2/255.f, 0x08/255.f);  // #90C208 强调绿
    D2D1_COLOR_F accentHover   = D2D1::ColorF(0x7A/255.f, 0xAD/255.f, 0x06/255.f);  // #7AAD06
    D2D1_COLOR_F danger        = D2D1::ColorF(0xFF/255.f, 0x6B/255.f, 0x6B/255.f);  // #FF6B6B 删除
    D2D1_COLOR_F closeRed      = D2D1::ColorF(0xE8/255.f, 0x11/255.f, 0x23/255.f);  // #FFE81123 关闭 hover
    D2D1_COLOR_F divider       = D2D1::ColorF(0x40/255.f, 0x40/255.f, 0x40/255.f);  // #404040 分隔线
    D2D1_COLOR_F border        = D2D1::ColorF(0x44/255.f, 0x44/255.f, 0x44/255.f);  // #444
    D2D1_COLOR_F panelBg       = D2D1::ColorF(0x1A/255.f, 0x1A/255.f, 0x1A/255.f, 0.85f);  // #1A1A1A
    D2D1_COLOR_F menuBg        = D2D1::ColorF(0x2C/255.f, 0x2C/255.f, 0x2C/255.f);  // #2C2C2C 菜单
    float toolbarHeight        = 46.0f;   // Prism 工具栏 46px
    float statusBarHeight      = 24.0f;
    int   buttonSize           = 34;      // ToolBtnStyle 34×34
    int   iconSize             = 16;
    int   padding              = 6;
};

class UIEngine {
public:
    UIEngine();
    ~UIEngine();

    void Initialize(IDWriteFactory* dwrite);

    // 每一帧在 D2D DeviceContext 上绘制 UI
    void Draw(D2DRenderer& renderer);

    // ── UI 状态 ──
    void SetStatusText(const std::wstring& text);
    void SetZoomText(const std::wstring& text);
    void SetImageInfo(const std::wstring& text);
    // 标题栏文本（文件名，自绘标题栏显示）
    void SetTitleText(const std::wstring& t) { _titleText = t; }
    // 标题栏中间居中信息（文件名|尺寸|大小，有图才显示）
    void SetCenterInfo(const std::wstring& name, const std::wstring& dims, const std::wstring& size) {
        _centerFileName = name; _centerDims = dims; _centerFileSize = size;
    }
    // 工具栏图片序号（如 "3/57"）
    void SetIndexText(const std::wstring& t) { _indexText = t; }

    // ── 全屏模式 ──
    void SetFullscreen(bool f) { _fullscreen = f; }
    bool IsFullscreen() const { return _fullscreen; }
    // 鼠标位置：全屏时据此判断是否显示工具栏/状态栏（渐隐）
    void SetMousePos(int x, int y) { _mouseX = x; _mouseY = y; }

    // ── EXIF 面板 ──
    void SetExifInfo(const ExifInfo& info) { _exif = info; }
    void ToggleExifPanel() { _showExifPanel = !_showExifPanel; }
    bool IsExifPanelVisible() const { return _showExifPanel; }
    // 面板拖动：点击面板内任意位置可拖动移动（点击外部仍操作图片，不关闭面板）
    bool ExifPanelHitTest(int x, int y) const;
    void StartExifDrag(int x, int y);                       // 记录拖动起点偏移
    void UpdateExifDrag(int x, int y, int winW, int winH);  // 更新自定义位置（含越界钳制）
    void EndExifDrag() { _exifDragging = false; }
    bool IsExifDragging() const { return _exifDragging; }

    // ── "更多"浮动面板（⋮ 按钮触发，含鸟瞰图/缩略图 toggle） ──
    void ToggleMorePanel() { _showMorePanel = !_showMorePanel; }
    bool IsMorePanelVisible() const { return _showMorePanel; }
    bool MorePanelHitTest(int x, int y) const;  // 是否在面板区域内
    // 返回点中的 toggle：0=鸟瞰图 1=缩略图 -1=无（含面板内非 toggle 区）
    int  MorePanelToggleAt(int x, int y) const;

    // ── GIF 控制面板（浮动小面板，可拖动） ──
    void SetGifPanelState(bool visible, int frame, int count, bool playing);
    void SetGifPanelPos(int x, int y) { _gifPanelPosX = x; _gifPanelPosY = y; }
    int  GifPanelHitTest(int x, int y) const;       // 返回 IDM_GIF_* 或 -1
    bool GifPanelDragHitTest(int x, int y) const;   // 面板内非按钮区域可拖动
    void StartGifPanelDrag(int x, int y);
    void UpdateGifPanelDrag(int x, int y, int winW, int winH);
    void EndGifPanelDrag() { _gifDragging = false; }
    bool IsGifDragging() const { return _gifDragging; }
    bool IsGifPanelVisible() const { return _gifPanelVisible; }
    int  GifPanelPosX() const { return _gifPanelPosX; }
    int  GifPanelPosY() const { return _gifPanelPosY; }

    // ── 缩略图条 ──
    // 主开关（来自 Config.thumbnailBarVisible，由"更多"菜单切换）
    void SetThumbBarEnabled(bool e) { _thumbBarEnabled = e; if (!e) _thumbBarTarget = 0; }
    // 缩略图条顶部 y 坐标（与 DrawThumbBar 计算一致，供显隐触发对齐用）
    float ThumbBarTopY(float winH) const;
    // 数据源：每帧由 WindowManager 同步（PreDecodeCache 提供顶层缩略图像素）
    void SetThumbSource(PreDecodeCache* cache, int current, const std::vector<std::wstring>* files) {
        _thumbCache = cache; _thumbCurrent = current; _thumbFiles = files;
    }
    // 点击命中：返回索引，-1=未命中（仅 alpha>0.3 时响应）
    int ThumbBarHitTest(int x, int y);
    // 目录变化时清空纹理缓存（索引失效）
    void ClearThumbTextures();

    // ── 鸟瞰图 ──
    void SetBirdsEyeEnabled(bool e) { _birdsEyeEnabled = e; }
    // 每帧同步视口信息（鸟瞰图蓝框计算用）
    void SetViewportInfo(const ViewportInfo& v) { _viewport = v; }
    // 鸟瞰图命中：返回是否在鸟瞰图区域内（含蓝框拖动热区）
    bool BirdsEyeHitTest(int x, int y);
    // 拖动鸟瞰图蓝框 → 主图平移：返回相对偏移量（dx,dy 像素），非鸟瞰区返回 false
    bool BirdsEyeDrag(int x, int y, double& outPanDx, double& outPanDy);

    // ── 交互命中测试 ──
    // 返回命令 ID，未命中返回 -1
    int HitTest(int x, int y);

private:
    IDWriteFactory* _dwrite = nullptr;
    IDWriteTextFormat* _textFormat  = nullptr;
    IDWriteTextFormat* _smallFormat = nullptr;
    UITheme _theme;

    std::wstring _statusText;
    std::wstring _zoomText;
    std::wstring _infoText;

    // 全屏状态
    bool _fullscreen = false;
    int  _mouseX = -1, _mouseY = -1;
    // 全屏右上角按钮区域（退出全屏 / 关闭）
    RECT _fsExitBtn = {};
    RECT _fsCloseBtn = {};
    // 自绘标题栏按钮区域（非全屏时显示）
    RECT _tbMinBtn = {}, _tbMaxBtn = {}, _tbCloseBtn = {};  // 系统三按钮
    RECT _tbFsBtn = {}, _tbMenuBtn = {};                     // 全屏 + 汉堡
    std::wstring _titleText;  // 标题栏文本（文件名）
    // 标题栏中间居中信息（Prism：文件名|尺寸|大小）
    std::wstring _centerFileName, _centerDims, _centerFileSize;
    std::wstring _indexText;  // 工具栏图片序号
    // 边缘导航按钮区域（左右翻页，鼠标靠近边缘渐显）
    RECT _edgeNavLeft = {}, _edgeNavRight = {};
    // 底部工具栏"更多"按钮区域（最右，弹出鸟瞰图/缩略图条开关）
    RECT _moreBtn = {};
    // 空状态"选择图片"按钮区域（无图时居中显示）
    RECT _emptyOpenBtn = {};
    // 缩略图条命中区域（智能隐藏时点击仍可跳转）
    RECT _thumbBarRect = {};
    // 鸟瞰图区域（右下角小图 + 蓝框）
    RECT _birdsEyeRect = {};

    // ── 缩略图条状态 ──
    bool _thumbBarEnabled = false;          // 主开关（Config.thumbnailBarVisible）
    PreDecodeCache* _thumbCache = nullptr;  // 顶层缩略图数据源
    int _thumbCurrent = -1;                  // 当前图片索引
    const std::vector<std::wstring>* _thumbFiles = nullptr;
    // GPU 纹理缓存：索引→已上传纹理（懒创建，超出可视范围淘汰）
    std::unordered_map<int, ComPtr<ID2D1Bitmap1>> _thumbTextures;

    // ── 鸟瞰图状态 ──
    bool _birdsEyeEnabled = false;
    ViewportInfo _viewport;
    // 鸟瞰图内"图片完整显示"矩形（缩略图绘制区，蓝框相对此计算）
    float _beImgX = 0, _beImgY = 0, _beImgW = 0, _beImgH = 0;

    // EXIF 面板
    bool _showExifPanel = false;
    ExifInfo _exif;
    // 面板自定义位置（-1=默认右上角自动定位）+ 拖动状态
    float _exifPanelPosX = -1.0f, _exifPanelPosY = -1.0f;
    bool  _exifDragging = false;
    float _exifDragOffX = 0.0f, _exifDragOffY = 0.0f;
    RECT  _exifPanelRect = {};  // 绘制时计算的当前面板矩形（拖动命中用）

    // "更多"浮动面板（⋮ 按钮触发，含鸟瞰图/缩略图 toggle 开关）
    bool  _showMorePanel = false;
    RECT  _morePanelRect = {};        // 面板区域（点击命中用）
    RECT  _moreToggleBirdsEye = {};   // 鸟瞰图 toggle 命中区
    RECT  _moreToggleThumb = {};      // 缩略图 toggle 命中区

    // ── GIF 控制面板 ──
    bool  _gifPanelVisible = false;   // 仅 GIF 且帧数>1 时显示
    int   _gifFrame = 0, _gifFrameCount = 0;
    bool  _gifPlaying = false;
    int   _gifPanelPosX = -1, _gifPanelPosY = -1;  // -1=默认右下角
    bool  _gifDragging = false;
    float _gifDragOffX = 0.0f, _gifDragOffY = 0.0f;
    RECT  _gifPanelRect = {};
    RECT  _gifBtnPrev = {}, _gifBtnPlay = {}, _gifBtnNext = {};

    // ── 透明度动画（缩略图条 / 边缘按钮 / 鸟瞰图共用） ──
    // 30ms 定时器插值到 target，步长 0.1 → 约 300ms 完成 0→1 过渡
    static constexpr float ALPHA_STEP = 0.1f;
    float _thumbBarAlpha = 0.0f, _thumbBarTarget = 0.0f;
    float _edgeNavAlphaL = 0.0f, _edgeNavAlphaLTarget = 0.0f;
    float _edgeNavAlphaR = 0.0f, _edgeNavAlphaRTarget = 0.0f;
    bool _animDirty = false;  // 本帧 alpha 是否变化，需 Invalidate

    // 按钮区域
    struct ButtonRect {
        int id;          // 命令 ID
        std::wstring label;    // 文字标签
        RECT rect;       // 像素坐标
        bool hover = false;
    };
    std::vector<ButtonRect> _toolbarButtons;

    void CreateToolbarButtons();
    void DrawToolbar(D2DRenderer& r);
    void DrawStatusBar(D2DRenderer& r);
    void DrawInfoOverlay(D2DRenderer& r);
    void DrawZoomControl(D2DRenderer& r);
    // 全屏右上角按钮（退出全屏 + 关闭）
    void DrawFullscreenButtons(D2DRenderer& r);
    // 自绘标题栏（非全屏时顶部 32px）
    void DrawTitlebar(D2DRenderer& r);
    // 边缘导航按钮（左右翻页，鼠标靠近渐显）
    void DrawEdgeNavButtons(D2DRenderer& r);
    // EXIF 信息面板（右侧半透明覆盖层）
    void DrawExifPanel(D2DRenderer& r);
    // 缩略图条（底部，固定框模式 + 智能隐藏，复用 PreDecodeCache 顶层缩略图）
    void DrawThumbBar(D2DRenderer& r);
    // 鸟瞰图（右下角，图片超出视口时显示，蓝框拖动平移主图）
    void DrawBirdsEye(D2DRenderer& r);
    // "更多"浮动面板（⋮ 上方弹出，含鸟瞰图/缩略图 toggle 开关）
    void DrawMorePanel(D2DRenderer& r);
    // GIF 控制面板（浮动小面板，帧号+上一帧/播放暂停/下一帧）
    void DrawGifPanel(D2DRenderer& r);
    // toggle 开关组件：轨道 40×20 圆角10 + 圆点半径8（on=accent 绿/off=灰）
    void DrawToggleSwitch(D2DRenderer& r, float x, float y, bool on);

public:
    // ── 透明度动画 ──
    // 由 WindowManager 30ms 定时器调用：插值各 alpha 到目标值
    // 返回 true 表示本帧 alpha 有变化，调用方需 Invalidate
    bool UpdateAnimations();
    // 设置缩略图条/边缘按钮目标透明度（由鼠标位置触发）
    void SetThumbBarTarget(float t) { _thumbBarTarget = t; }
    void SetEdgeNavTargetL(float t) { _edgeNavAlphaLTarget = t; }
    void SetEdgeNavTargetR(float t) { _edgeNavAlphaRTarget = t; }
    float ThumbBarAlpha() const { return _thumbBarAlpha; }
    float EdgeNavAlphaL() const { return _edgeNavAlphaL; }
    float EdgeNavAlphaR() const { return _edgeNavAlphaR; }
};
