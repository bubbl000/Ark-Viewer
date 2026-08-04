#include "ThumbProvider.h"
#include "ThumbExtractor.h"
#include <cstring>
#include <vector>

// BGRA8 像素 → 32bpp HBITMAP（top-down DIB section，alpha 已预乘）
// 各解码器输出均为预乘 alpha（SVG 经 D2D PREMULTIPLIED；其余 A=0xFF 即不透明）
static HBITMAP BgraToHBitmap(const uint8_t* px, int w, int h) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;   // 负值=自顶向下，与 BGRA 行序一致
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP hb = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hb || !bits) {
        if (hb) DeleteObject(hb);
        return nullptr;
    }
    // BGRA 字节序与 32bpp DIB 完全匹配，逐行拷贝（DIB 行间距即 w*4）
    std::memcpy(bits, px, (size_t)w * 4 * h);
    return hb;
}

// SEH 安全调用包装：本函数无 C++ 析构对象，可使用 __try/__except
// 解码器内部含第三方 DLL（libraw/heif），AV 时不能让异常传播拖垮 dllhost
static bool SehSafeExtract(const wchar_t* path, uint32_t maxDim,
                           std::vector<uint8_t>* outBgra,
                           uint32_t* outW, uint32_t* outH) {
    bool ok = false;
    __try {
        ok = ThumbExtractor::Extract(path, maxDim, *outBgra, *outW, *outH);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return ok;
}

// ── IUnknown ──
// 多继承下 QI 必须返回对应子对象指针：vtable 不同，错返会导致调用方走错槽位
// （曾经误把 IInitializeWithItem 当 IThumbnailProvider 返回，Initialize 实际调用到
//  GetThumbnail，解引用 phbmp=NULL → AV/NRE）
IFACEMETHODIMP ThumbProvider::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) ||
        riid == __uuidof(IThumbnailProvider)) {
        *ppv = static_cast<IThumbnailProvider*>(this);
    } else if (riid == __uuidof(IInitializeWithItem)) {
        *ppv = static_cast<IInitializeWithItem*>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

IFACEMETHODIMP_(ULONG) ThumbProvider::AddRef() {
    return InterlockedIncrement(&_ref);
}

IFACEMETHODIMP_(ULONG) ThumbProvider::Release() {
    LONG n = InterlockedDecrement(&_ref);
    if (n == 0) delete this;
    return (ULONG)n;
}

// ── IInitializeWithItem：Shell 注入 IShellItem，从中取出文件系统路径 ──
IFACEMETHODIMP ThumbProvider::Initialize(IShellItem* pItem, DWORD grfMode) {
    if (!pItem) return E_INVALIDARG;
    _mode = grfMode;

    PWSTR pszPath = nullptr;
    HRESULT hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszPath);
    if (FAILED(hr) || !pszPath) return hr;
    _path = pszPath;
    CoTaskMemFree(pszPath);
    return _path.empty() ? E_FAIL : S_OK;
}

// ── IThumbnailProvider：生成 cx×cx 缩略图 HBITMAP ──
IFACEMETHODIMP ThumbProvider::GetThumbnail(UINT cx, HBITMAP* phbmp, WTS_ALPHATYPE* pdwAlpha) {
    if (!phbmp) return E_POINTER;
    *phbmp = nullptr;
    if (pdwAlpha) *pdwAlpha = WTSAT_UNKNOWN;  // 让 shell 自行判断 alpha
    if (_path.empty()) return E_FAIL;

    // 上限 1024，避免异常大的请求触发巨型解码
    uint32_t maxDim = (cx == 0 || cx > 1024) ? 256 : cx;

    std::vector<uint8_t> bgra;
    uint32_t w = 0, h = 0;
    if (!SehSafeExtract(_path.c_str(), maxDim, &bgra, &w, &h) || bgra.empty()) {
        return E_FAIL;  // 失败 → 系统显示默认图标
    }

    HBITMAP hb = BgraToHBitmap(bgra.data(), (int)w, (int)h);
    if (!hb) return E_FAIL;
    *phbmp = hb;
    if (pdwAlpha) *pdwAlpha = WTSAT_ARGB;  // 32bpp BGRA 含 alpha 通道
    return S_OK;
}
