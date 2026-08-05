#include "D2DRenderer.h"
#include <d2d1effects.h>
#include "Logger.h"
#include <d3d11.h>
#include <dxgi1_2.h>

D2DRenderer::D2DRenderer() = default;
D2DRenderer::~D2DRenderer() = default;

bool D2DRenderer::Initialize() {
    HRESULT hr;

    // 1. 创建 D2D 工厂
    D2D1_FACTORY_OPTIONS opts = {};
#ifdef _DEBUG
    opts.debugLevel = D2D1_DEBUG_LEVEL_WARNING;
#endif
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
        __uuidof(ID2D1Factory1), &opts, (void**)&_d2dFactory);
    if (FAILED(hr)) return false;

    // 2. 创建 DWrite 工厂
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory), (IUnknown**)&_dwrite);
    if (FAILED(hr)) return false;

    LOG_INFO("D2D", "D2D 工厂 + DWrite 创建成功");
    _initialized = true;
    return true;
}

bool D2DRenderer::Attach(HWND hwnd) {
    if (!_initialized) return false;
    _hwnd = hwnd;

    // 创建 D3D11 设备
    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<ID3D11DeviceContext> d3dContext;
    D3D_FEATURE_LEVEL featureLevel;

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION,
        &d3dDevice, &featureLevel, &d3dContext);
    if (FAILED(hr)) return false;

    // 获取 DXGI 设备
    ComPtr<IDXGIDevice> dxgiDevice;
    hr = d3dDevice.As(&dxgiDevice);
    if (FAILED(hr)) return false;

    // 创建 D2D 设备
    hr = _d2dFactory->CreateDevice(dxgiDevice.Get(), &_d2dDevice);
    if (FAILED(hr)) return false;

    // 创建 D2D 设备上下文
    hr = _d2dDevice->CreateDeviceContext(
        D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &_d2dContext);
    if (FAILED(hr)) return false;

    // 设置 DPI
    _d2dContext->SetDpi(96.0f, 96.0f);

    // 获取 DXGI 适配器
    ComPtr<IDXGIAdapter> dxgiAdapter;
    hr = dxgiDevice->GetAdapter(&dxgiAdapter);
    if (FAILED(hr)) return false;

    // 创建 DXGI 工厂
    ComPtr<IDXGIFactory2> dxgiFactory;
    hr = dxgiAdapter->GetParent(__uuidof(IDXGIFactory2), &dxgiFactory);
    if (FAILED(hr)) return false;

    // 创建 SwapChain
    RECT rc;
    GetClientRect(_hwnd, &rc);
    _width  = rc.right - rc.left;
    _height = rc.bottom - rc.top;

    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.Width       = (UINT)_width;
    scd.Height      = (UINT)_height;
    scd.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    scd.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;

    hr = dxgiFactory->CreateSwapChainForHwnd(
        d3dDevice.Get(), _hwnd, &scd, nullptr, nullptr, &_swapChain);
    if (FAILED(hr)) {
        LOG_ERR("D2D", "CreateSwapChainForHwnd 失败");
        return false;
    }
    LOG_INFO_STREAM("D2D") << "SwapChain 创建成功: " << _width << "x" << _height;
    if (FAILED(hr)) return false;

    // 防止 DXGI 拦截 Alt+Enter 全屏
    dxgiFactory->MakeWindowAssociation(_hwnd, DXGI_MWA_NO_ALT_ENTER);

    // 创建 D2D 渲染目标 (back buffer)
    ComPtr<IDXGISurface> backBuffer;
    hr = _swapChain->GetBuffer(0, __uuidof(IDXGISurface), &backBuffer);
    if (FAILED(hr)) return false;

    D2D1_BITMAP_PROPERTIES1 bmpProps = {};
    bmpProps.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    bmpProps.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    bmpProps.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    bmpProps.dpiX = 96.0f;
    bmpProps.dpiY = 96.0f;

    hr = _d2dContext->CreateBitmapFromDxgiSurface(
        backBuffer.Get(), &bmpProps, &_d2dTarget);
    if (FAILED(hr)) return false;

    _d2dContext->SetTarget(_d2dTarget.Get());

    return true;
}

bool D2DRenderer::Resize(int w, int h) {
    if (!_swapChain || !_d2dContext) return false;

    // 去重：尺寸变化不大时跳过
    if (abs(_width - w) <= 1 && abs(_height - h) <= 1) return true;

    _width  = w;
    _height = h;

    // 释放对 back buffer 的 D2D 引用
    _d2dContext->SetTarget(nullptr);
    _d2dTarget.Reset();

    // ResizeBuffers — 用显式尺寸而非 HWND 尺寸
    HRESULT hr = _swapChain->ResizeBuffers(0, (UINT)w, (UINT)h,
        DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);
    if (FAILED(hr)) return false;

    // 重新创建 D2D 目标
    ComPtr<IDXGISurface> backBuffer;
    hr = _swapChain->GetBuffer(0, __uuidof(IDXGISurface), &backBuffer);
    if (FAILED(hr)) return false;

    D2D1_BITMAP_PROPERTIES1 props = {};
    props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
    props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    props.dpiX = 96.0f; props.dpiY = 96.0f;

    hr = _d2dContext->CreateBitmapFromDxgiSurface(
        backBuffer.Get(), &props, _d2dTarget.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !_d2dTarget) return false;

    _d2dContext->SetTarget(_d2dTarget.Get());
    return true;
}

bool D2DRenderer::BeginDraw() {
    if (!_d2dContext || !_d2dTarget) return false;
    _d2dContext->SetTarget(_d2dTarget.Get());
    _d2dContext->BeginDraw();
    return true;
}

HRESULT D2DRenderer::EndDraw() {
    HRESULT hr = _d2dContext->EndDraw();
    if (SUCCEEDED(hr) && _swapChain) {
        // 交互窗口内（翻页/缩放/拖拽停手 200ms 内）用 Present(0) 不等 VSync，
        // 避免快速连续 Present(1) 等 VSync + GPU 队列积压导致冻结；
        // 停手后恢复 Present(1) 消除画面撕裂
        auto elapsed = std::chrono::steady_clock::now() - _lastInteractTick;
        UINT syncInterval = (elapsed < std::chrono::milliseconds(200)) ? 0 : 1;
        hr = _swapChain->Present(syncInterval, 0);
    }
    return hr;
}

void D2DRenderer::Clear(float r, float g, float b, float a) {
    _d2dContext->Clear(D2D1::ColorF(r, g, b, a));
}

void D2DRenderer::DrawBitmap(ID2D1Bitmap1* bitmap,
    float dx, float dy, float dw, float dh,
    float opacity, D2D1_INTERPOLATION_MODE interp)
{
    if (!bitmap || !_d2dContext) { LOG_DBG("D2D", "DrawBitmap: 跳过（null bitmap 或 context）"); return; }
    D2D1_RECT_F destRect = { dx, dy, dx + dw, dy + dh };
    _d2dContext->DrawBitmap(bitmap, destRect, opacity, interp);
}

void D2DRenderer::DrawBitmapRotated(ID2D1Bitmap1* bitmap,
    float destX, float destY, float destW, float destH,
    int rotation, bool flipH, bool flipV)
{
    if (!bitmap || !_d2dContext || destW <= 0 || destH <= 0) return;
    D2D1_SIZE_F srcSize = bitmap->GetSize();
    if (srcSize.width <= 0 || srcSize.height <= 0) return;

    // 旋转 90/270 时图像宽高互换：预旋转框须用 (destH, destW)，旋转后正好落入 destW×destH
    // 旧实现统一用 (destW, destH) 作预旋转框，旋转后框变为 (destH, destW)，
    // 与目标 (destW, destH) 不重合 → 图像画在错误位置，旋转态平移/鸟瞰蓝框全部错位
    bool sideways = (rotation == 90 || rotation == 270);
    float preW = sideways ? destH : destW;
    float preH = sideways ? destW : destH;
    auto preCenter = D2D1::Point2F(preW / 2, preH / 2);
    // 旋转后框中心仍为 preCenter，需平移到目标矩形中心 (destX+destW/2, destY+destH/2)
    auto finalCenter = D2D1::Point2F(destX + destW / 2, destY + destH / 2);

    // D2D 行向量 p'=p*M（A*B 为先 A 后 B）：Scale → Flip → Rotation → Translation
    // trans 必须最右（最后应用），作为屏幕平移项，使 offsetX/Y 成为纯屏幕偏移——
    // Pan/Zoom/QueueTiles/鸟瞰蓝框均假设 offset 是屏幕平移（见 ImageToScreenRel）
    // 旧实现 trans*rot*flip*scale 把 trans 放最左（源空间平移），经 rot 后 offset 方向被
    // 旋转转换，导致旋转后拖动方向错乱（拖右变上移）且旋转图像位置偏移
    auto scale = D2D1::Matrix3x2F::Scale(preW / srcSize.width, preH / srcSize.height);
    auto flip = D2D1::Matrix3x2F::Scale(flipH ? -1.0f : 1.0f, flipV ? -1.0f : 1.0f, preCenter);
    // D2D Rotation 正角度=顺时针，rotation 为逆时针角度，取负
    auto rot = D2D1::Matrix3x2F::Rotation(-(float)rotation, preCenter);
    auto trans = D2D1::Matrix3x2F::Translation(
        finalCenter.x - preCenter.x, finalCenter.y - preCenter.y);
    _d2dContext->SetTransform(scale * flip * rot * trans);

    // destRect 用 source 原始尺寸（缩放在 transform 中处理）
    D2D1_RECT_F destRect = { 0, 0, srcSize.width, srcSize.height };
    _d2dContext->DrawBitmap(bitmap, destRect, 1.0f, D2D1_INTERPOLATION_MODE_LINEAR);
    _d2dContext->SetTransform(D2D1::Matrix3x2F::Identity());
}

void D2DRenderer::DrawImageWithTransform(ID2D1Effect* effect,
    float sx, float sy, float ox, float oy)
{
    if (!effect) return;
    auto transform = D2D1::Matrix3x2F(sx, 0, 0, sy, ox, oy);
    _d2dContext->SetTransform(transform);
    _d2dContext->DrawImage(effect);
    _d2dContext->SetTransform(D2D1::Matrix3x2F::Identity());
}

void D2DRenderer::DrawBitmapBlurred(ID2D1Bitmap1* bitmap,
    float dx, float dy, float dw, float dh, float blurRadius)
{
    if (!bitmap || !_d2dContext) return;

    // 创建 GaussianBlur effect，bitmap 作为输入
    ComPtr<ID2D1Effect> blur;
    HRESULT hr = _d2dContext->CreateEffect(CLSID_D2D1GaussianBlur, &blur);
    if (FAILED(hr) || !blur) {
        // effect 创建失败时回退普通绘制（保证至少能显示）
        DrawBitmap(bitmap, dx, dy, dw, dh);
        return;
    }
    blur->SetInput(0, bitmap);
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, blurRadius);
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);

    // 拉伸 + 平移：effect 输出尺寸 == bitmap 原尺寸，需 Scale 到目标矩形
    D2D1_SIZE_F bmpSize = bitmap->GetSize();
    if (bmpSize.width <= 0 || bmpSize.height <= 0) return;
    float sx = dw / bmpSize.width;
    float sy = dh / bmpSize.height;
    auto transform = D2D1::Matrix3x2F(sx, 0, 0, sy, dx, dy);
    _d2dContext->SetTransform(transform);
    _d2dContext->DrawImage(blur.Get());
    _d2dContext->SetTransform(D2D1::Matrix3x2F::Identity());
}

void D2DRenderer::FillRectangle(float x, float y, float w, float h, D2D1_COLOR_F c) {
    auto* b = GetCachedBrush(c);
    if (b) _d2dContext->FillRectangle(D2D1::RectF(x, y, x + w, y + h), b);
}

void D2DRenderer::FillRoundedRectangle(float x, float y, float w, float h,
    float rx, float ry, D2D1_COLOR_F c) {
    auto* b = GetCachedBrush(c);
    if (b) _d2dContext->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), rx, ry), b);
}

void D2DRenderer::DrawRectangle(float x, float y, float w, float h, D2D1_COLOR_F c, float sw) {
    auto* b = GetCachedBrush(c);
    if (b) _d2dContext->DrawRectangle(D2D1::RectF(x, y, x + w, y + h), b, sw);
}

void D2DRenderer::FillCircle(float cx, float cy, float r, D2D1_COLOR_F c) {
    auto* b = GetCachedBrush(c);
    if (b) _d2dContext->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), b);
}

void D2DRenderer::DrawText(const wchar_t* text, size_t len,
    IDWriteTextFormat* format, float x, float y, float w, float h, D2D1_COLOR_F c)
{
    auto* b = GetCachedBrush(c);
    if (b && format) {
        _d2dContext->DrawText(text, (UINT32)len, format,
            D2D1::RectF(x, y, x + w, y + h), b);
    }
}

void D2DRenderer::DrawTextTrimmed(const wchar_t* text, size_t len,
    IDWriteTextFormat* format, float x, float y, float w, float h, D2D1_COLOR_F c)
{
    if (!text || !len || !format || !_d2dContext || !_dwrite) return;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(_dwrite->CreateTextLayout(text, (UINT32)len, format, w, h, &layout)) || !layout) return;
    // 不换行 + 字符级省略号：超出 maxWidth 自动截断加 …
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TRIMMING trim{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
    layout->SetTrimming(&trim, nullptr);
    auto* b = GetCachedBrush(c);
    if (b) _d2dContext->DrawTextLayout(D2D1::Point2F(x, y), layout.Get(), b);
}

// straight alpha BGRA8 → premultiplied alpha
// 快速路径：无任何半透明像素（alpha 全 255）时原样返回，避免大图拷贝开销
const uint8_t* D2DRenderer::PremultiplyIfNeeded(const uint8_t* pixels, int w, int h,
                                                int stride, std::vector<uint8_t>& out)
{
    // 检查是否存在 alpha < 255 的像素
    bool hasAlpha = false;
    for (int y = 0; y < h && !hasAlpha; y++) {
        const uint8_t* row = pixels + (size_t)y * stride;
        for (int x = 0; x < w; x++) {
            if (row[x * 4 + 3] != 255) { hasAlpha = true; break; }
        }
    }
    if (!hasAlpha) return pixels;  // 全不透明：零拷贝

    // 有透明像素：逐像素预乘（color = color * alpha / 255）
    out.resize((size_t)h * stride);
    for (int y = 0; y < h; y++) {
        const uint8_t* src = pixels + (size_t)y * stride;
        uint8_t* dst = out.data() + (size_t)y * stride;
        for (int x = 0; x < w; x++) {
            uint8_t a = src[x * 4 + 3];
            dst[x * 4 + 0] = (uint8_t)((uint32_t)src[x * 4 + 0] * a / 255);
            dst[x * 4 + 1] = (uint8_t)((uint32_t)src[x * 4 + 1] * a / 255);
            dst[x * 4 + 2] = (uint8_t)((uint32_t)src[x * 4 + 2] * a / 255);
            dst[x * 4 + 3] = a;
        }
        // 行尾 padding（stride 可能 > w*4）原样复制
        if (stride > w * 4)
            memcpy(dst + w * 4, src + w * 4, (size_t)(stride - w * 4));
    }
    return out.data();
}

ComPtr<ID2D1Bitmap1> D2DRenderer::CreateBitmap(int w, int h,
    const uint8_t* pixels, int stride, bool isTarget)
{
    if (!_d2dContext) return nullptr;

    D2D1_BITMAP_PROPERTIES1 props = {};
    props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    // 位图带 alpha（PNG/GIF 透明图）：必须 PREMULTIPLIED，否则透明信息被忽略
    // （IGNORE 会把半透明当不透明、全透明显示成黑块）
    props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    props.bitmapOptions = isTarget
        ? D2D1_BITMAP_OPTIONS_TARGET
        : D2D1_BITMAP_OPTIONS_NONE;
    props.dpiX = 96.0f; props.dpiY = 96.0f;

    D2D1_SIZE_U size = { (UINT32)w, (UINT32)h };
    ComPtr<ID2D1Bitmap1> bitmap;

    if (pixels && stride > 0) {
        D2D1_RECT_U rect = { 0, 0, (UINT32)w, (UINT32)h };
        HRESULT hr = _d2dContext->CreateBitmap(size, nullptr, 0, &props, &bitmap);
        if (SUCCEEDED(hr)) {
            // straight alpha → premultiplied（D2D 位图要求预乘；解码器输出是 straight）
            std::vector<uint8_t> premul;
            const uint8_t* src = PremultiplyIfNeeded(pixels, w, h, stride, premul);
            bitmap->CopyFromMemory(&rect, src, (UINT32)stride);
        }
    } else {
        _d2dContext->CreateBitmap(size, nullptr, 0, &props, &bitmap);
    }
    return bitmap;
}

void D2DRenderer::UpdateBitmapRegion(ID2D1Bitmap1* bitmap,
    int dx, int dy, int w, int h, const uint8_t* pixels, int stride)
{
    if (!bitmap || !pixels) return;
    // 与 CreateBitmap 一致：瓦片像素 straight → premultiplied
    std::vector<uint8_t> premul;
    const uint8_t* src = PremultiplyIfNeeded(pixels, w, h, stride, premul);
    D2D1_RECT_U rect = { (UINT32)dx, (UINT32)dy, (UINT32)(dx + w), (UINT32)(dy + h) };
    bitmap->CopyFromMemory(&rect, src, (UINT32)stride);
}

ComPtr<ID2D1Bitmap1> D2DRenderer::CreateStretchedBitmap(
    ID2D1Bitmap1* source, int dstW, int dstH)
{
    if (!_d2dContext || !source || dstW <= 0 || dstH <= 0) return nullptr;

    // 创建目标纹理（作为 render target，不含 CANNOT_DRAW 故后续可作为 DrawBitmap 源）
    D2D1_BITMAP_PROPERTIES1 props = {};
    props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    // 拉伸中间纹理同样保留 alpha（源图带透明时拉伸结果也需带透明）
    props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
    props.dpiX = 96.0f; props.dpiY = 96.0f;

    ComPtr<ID2D1Bitmap1> target;
    HRESULT hr = _d2dContext->CreateBitmap(
        D2D1::SizeU((UINT32)dstW, (UINT32)dstH), nullptr, 0, &props, &target);
    if (FAILED(hr) || !target) {
        LOG_WARN_STREAM("D2D") << "CreateStretchedBitmap CreateBitmap FAILED hr=" << (long)hr;
        return nullptr;
    }

    // 切到目标纹理上，用线性插值拉伸 source 填充（GPU 端，无需 CPU 分配大内存）
    ComPtr<ID2D1Image> oldTarget;
    _d2dContext->GetTarget(&oldTarget);
    _d2dContext->SetTarget(target.Get());
    _d2dContext->BeginDraw();
    _d2dContext->Clear(D2D1::ColorF(0.118f, 0.118f, 0.118f));  // 背景色兜底
    D2D1_RECT_F dstRect = D2D1::RectF(0, 0, (float)dstW, (float)dstH);
    _d2dContext->DrawBitmap(source, &dstRect, 1.0f,
        D2D1_INTERPOLATION_MODE_LINEAR);
    HRESULT endHr = _d2dContext->EndDraw();
    _d2dContext->SetTarget(oldTarget.Get());  // 恢复原 target

    if (FAILED(endHr)) {
        LOG_WARN_STREAM("D2D") << "CreateStretchedBitmap EndDraw 失败 hr=" << (long)endHr;
    }

    return target;
}

// CLSID_D2D1ColorManagement — defined manually since d2d1_1.lib is unavailable
// {F09CE6B5-1587-4063-AB07-913A3E6135C5}
EXTERN_C const GUID DECLSPEC_SELECTANY CLSID_D2D1ColorManagement =
    { 0xF09CE6B5, 0x1587, 0x4063, { 0xAB, 0x07, 0x91, 0x3A, 0x3E, 0x61, 0x35, 0xC5 } };

// CLSID_D2D1GaussianBlur — 同样手动定义避免引入 d2d1effects.lib
// {1FEB6D69-2FE3-4AC6-8BAC-7DC5C5E6E8A5}
EXTERN_C const GUID DECLSPEC_SELECTANY CLSID_D2D1GaussianBlur =
    { 0x1FEB6D69, 0x2FE3, 0x4AC6, { 0x8B, 0xAC, 0x7D, 0xC5, 0xC5, 0xE6, 0xE8, 0xA5 } };

ID2D1SolidColorBrush* D2DRenderer::GetCachedBrush(D2D1_COLOR_F color) {
    if (!_brushCache || memcmp(&color, &_lastBrushColor, sizeof(color)) != 0) {
        _brushCache.Reset();
        _d2dContext->CreateSolidColorBrush(color, &_brushCache);
        _lastBrushColor = color;
    }
    return _brushCache.Get();
}

ComPtr<ID2D1Effect> D2DRenderer::CreateIccEffect(const uint8_t* iccData, size_t iccLen) {
    if (!_d2dContext || !iccData || iccLen == 0) return nullptr;

    ComPtr<ID2D1Effect> effect;
    HRESULT hr = _d2dContext->CreateEffect(CLSID_D2D1ColorManagement, &effect);
    if (FAILED(hr)) return nullptr;

    // 创建源 ICC context
    ComPtr<ID2D1ColorContext> srcContext;
    hr = _d2dContext->CreateColorContext(
        D2D1_COLOR_SPACE_CUSTOM, iccData, (UINT32)iccLen, &srcContext);
    if (FAILED(hr)) return nullptr;

    // 创建目标 sRGB context
    ComPtr<ID2D1ColorContext> dstContext;
    // sRGB uses system default profile
    (void)srcContext; // silence unused warning
    hr = _d2dContext->CreateColorContext(
        D2D1_COLOR_SPACE_SRGB, nullptr, 0, &dstContext);
    if (FAILED(hr)) return nullptr;

    effect->SetValue(D2D1_COLORMANAGEMENT_PROP_SOURCE_COLOR_CONTEXT,
        (IUnknown*)srcContext.Get());
    effect->SetValue(D2D1_COLORMANAGEMENT_PROP_DESTINATION_COLOR_CONTEXT,
        (IUnknown*)dstContext.Get());
    effect->SetValue(D2D1_COLORMANAGEMENT_PROP_QUALITY,
        D2D1_COLORMANAGEMENT_QUALITY_BEST);

    return effect;
}















