#pragma once
#include <Windows.h>
#ifdef RGB
#undef RGB
#endif
#include <d2d1.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>
#include <chrono>

using Microsoft::WRL::ComPtr;

// ─── Direct2D 渲染表面 ───
// 管理 D2D/DWrite 资源，提供绘图接口
// 对应原 C# 的 Direct2DRenderSurface

class D2DRenderer {
public:
    D2DRenderer();
    ~D2DRenderer();

    // 初始化 D2D 工厂 + 设备
    bool Initialize();

    // 绑定到 HWND，创建渲染目标
    bool Attach(HWND hwnd);

    // 调整渲染目标大小（响应 WM_SIZE）
    bool Resize(int w, int h);

    // 开始/结束绘制
    bool BeginDraw();
    HRESULT EndDraw();

    // 交互期间通知：翻页/缩放/拖拽入口调用，EndDraw 据此切换 Present 参数
    // 交互中用 Present(0) 不等 VSync（流畅），停手 200ms 后恢复 Present(1)（无撕裂）
    void NotifyInteraction() { _lastInteractTick = std::chrono::steady_clock::now(); }

    // ── 绘图命令 ──

    // 清空背景
    void Clear(float r, float g, float b, float a = 1.0f);

    // 绘制位图（支持缩放）
    // source: GPU 纹理
    // destRect: 目标位置/尺寸（物理像素）
    void DrawBitmap(ID2D1Bitmap1* bitmap,
        float destX, float destY, float destW, float destH,
        float opacity = 1.0f,
        D2D1_INTERPOLATION_MODE interpolation = D2D1_INTERPOLATION_MODE_LINEAR);

    // 旋转+翻转绘制位图（只改变视图不改变原图）
    // rotation: 逆时针角度 0/90/180/270，destW/destH 需为旋转后的尺寸（90/270 时宽高互换）
    void DrawBitmapRotated(ID2D1Bitmap1* bitmap,
        float destX, float destY, float destW, float destH,
        int rotation, bool flipH, bool flipV);

    // 使用 Transform + DrawImage（ICC 色彩管理路径）
    void DrawImageWithTransform(ID2D1Effect* effect,
        float scaleX, float scaleY, float offsetX, float offsetY);

    // 高斯模糊绘制位图：用 GaussianBlur effect 把 bitmap 拉伸绘制到目标矩形
    // 用于切换图片时的模糊占位（缩略图/旧图模糊铺满视口）
    // blurRadius = 高斯模糊标准差（像素），0 表示不模糊
    void DrawBitmapBlurred(ID2D1Bitmap1* bitmap,
        float destX, float destY, float destW, float destH,
        float blurRadius);

    // ── 直接渲染 UI ──
    void FillRectangle(float x, float y, float w, float h,
        D2D1_COLOR_F color);
    // 棋盘格背景：在 (x,y,w,h) 矩形内平铺 cell 大小的亮/暗棋盘格
    // 用于透明图片背景（图片绘制在其上，透明处露出棋盘格）
    // alpha 0~1：棋盘格透明度（0 全透明不可见，1 不透明）
    void DrawCheckerboard(float x, float y, float w, float h, float cell, float alpha = 1.0f);
    // 圆角矩形填充：rx/ry 为椭圆角半径（边缘导航按钮等贴边控件圆角化）
    void FillRoundedRectangle(float x, float y, float w, float h,
        float rx, float ry, D2D1_COLOR_F color);
    void DrawRectangle(float x, float y, float w, float h,
        D2D1_COLOR_F color, float strokeWidth = 1.0f);
    void FillCircle(float cx, float cy, float r, D2D1_COLOR_F color);
    void DrawText(const wchar_t* text, size_t len,
        IDWriteTextFormat* format, float x, float y, float w, float h,
        D2D1_COLOR_F color);
    // 单行省略号绘制：超出 w 时按字符截断并加 …（用于标题栏/EXIF 面板防挤压）
    void DrawTextTrimmed(const wchar_t* text, size_t len,
        IDWriteTextFormat* format, float x, float y, float w, float h,
        D2D1_COLOR_F color);
    // 轴对齐裁剪：后续绘制限制在 (x,y,w,h) 矩形内（UI 面板防溢出），须成对调用
    void PushClip(float x, float y, float w, float h);
    void PopClip();

    // ── 创建 GPU 位图 ──
    // 从 CPU 像素数据创建 D2D 位图
    ComPtr<ID2D1Bitmap1> CreateBitmap(int w, int h,
        const uint8_t* pixels, int stride, bool isTarget = false);

    // 更新位图部分区域（瓦片上传）
    void UpdateBitmapRegion(ID2D1Bitmap1* bitmap,
        int dstX, int dstY, int w, int h,
        const uint8_t* pixels, int stride);

    // GPU 端拉伸创建大纹理：用 source 线性插值拉伸到 dstW×dstH
    // 用于大层（>4096²）占位：避免 CPU 分配大内存做最近邻放大，未解码区域不再黑色
    ComPtr<ID2D1Bitmap1> CreateStretchedBitmap(ID2D1Bitmap1* source, int dstW, int dstH);

    // ── 创建 ICC ColorManagement effect ──
    ComPtr<ID2D1Effect> CreateIccEffect(const uint8_t* iccData, size_t iccLen);

    // ── 资源访问 ──
    ID2D1DeviceContext* Context() const { return _d2dContext.Get(); }
    IDWriteFactory*     DWrite()  const { return _dwrite.Get(); }
    ID2D1Bitmap1*       Target()  const { return _d2dTarget.Get(); }
    int                 Width()   const { return _width; }
    int                 Height()  const { return _height; }

    // straight alpha BGRA8 → premultiplied：若全不透明（alpha 全 255）则原样返回（零拷贝），
    // 否则写入 out 并返回 out.data()。D2D 位图要求预乘 alpha。
    static const uint8_t* PremultiplyIfNeeded(const uint8_t* pixels, int w, int h, int stride,
                                              std::vector<uint8_t>& out);

    // ── 颜色 ──
    static D2D1_COLOR_F MakeColor(float r, float g, float b, float a = 1.0f) {
        return D2D1::ColorF(r, g, b, a);
    }

private:
    ComPtr<ID2D1Factory1>    _d2dFactory;
    ComPtr<IDWriteFactory>   _dwrite;
    ComPtr<ID2D1Device>      _d2dDevice;
    ComPtr<ID2D1DeviceContext> _d2dContext;
    ComPtr<IDXGISwapChain1>  _swapChain;
    ComPtr<ID2D1Bitmap1>     _d2dTarget;
    HWND    _hwnd = nullptr;
    int     _width  = 0;
    int     _height = 0;
    bool    _initialized = false;
    ComPtr<ID2D1SolidColorBrush> _brushCache;
    D2D1_COLOR_F _lastBrushColor = {};
    // 棋盘格平铺资源（懒创建，跨帧复用）
    ComPtr<ID2D1Bitmap1>    _checkerBitmap;
    ComPtr<ID2D1BitmapBrush> _checkerBrush;
    float _checkerAlpha = -1.0f;  // 当前棋盘格透明度（-1=未初始化），变化时重建资源
    // 最近一次交互时间戳：EndDraw 据此判断是否在交互窗口内（200ms）
    std::chrono::steady_clock::time_point _lastInteractTick;
    ID2D1SolidColorBrush* GetCachedBrush(D2D1_COLOR_F color);
};





