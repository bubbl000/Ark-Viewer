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
    } else if (riid == __uuidof(IInitializeWithStream)) {
        *ppv = static_cast<IInitializeWithStream*>(this);
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

// ── IInitializeWithStream：Shell 打开文件后把 IStream 注入 ──
// dllhost 低完整性环境下 provider 自己打开 E 盘文件可能失败（GetDisplayName 权限受限），
// 由 Explorer 传流则没有权限问题。流式复制到临时文件（边读边写，不占内存，
// 支持 1GB+ 的 PSD/PSB）后复用 Extract(path) 解码逻辑。

// 根据文件头部魔数推断扩展名（流模式没有文件名，Extract 按扩展名路由解码器）
static std::wstring SniffExtension(const std::string& data) {
    if (data.size() >= 4 && data[0] == '8' && data[1] == 'B' && data[2] == 'P' && data[3] == 'S')
        return L".psd";  // PSD/PSB 共用魔数，PSB 由 PsdDecoder 内部分辨
    if (data.size() >= 12 && data[4] == 'f' && data[5] == 't' && data[6] == 'y' && data[7] == 'p') {
        // ISO-BMFF（HEIC/HEIF/HIF/CR3）：看 brand
        if (data.size() >= 12) {
            std::string brand(data, 8, 4);
            if (brand == "cr3 " || brand == "crx ") return L".cr3";
            if (brand == "heic" || brand == "heix" || brand == "heim") return L".heic";
            if (brand == "heif" || brand == "mif1" || brand == "msf1") return L".heif";
            return L".heic";
        }
    }
    if (data.size() >= 4 && ((data[0] == 'I' && data[1] == 'I' && data[2] == '*' && data[3] == 0) ||
                             (data[0] == 'M' && data[1] == 'M' && data[2] == 0 && data[3] == '*')))
        return L".arw";  // TIFF 结构（ARW/CR2/DNG/NEF 共用，按 TIFF 解析）
    if (data.size() >= 10 && data[0] == '#' && data[1] == '?' && data[2] == 'R' && data[3] == 'A')
        return L".hdr";
    if (data.size() >= 5 && data[0] == '<' && data[1] == '?' && data[2] == 'x' && data[3] == 'm' && data[4] == 'l')
        return L".svg";
    return L".tmp";  // 无法识别 → 保持 .tmp（Extract 会失败返回 false）
}

IFACEMETHODIMP ThumbProvider::Initialize(IStream* pStream, DWORD grfMode) {
    if (!pStream) return E_INVALIDARG;
    _mode = grfMode;

    // 1. 先读头部 64KB 用于嗅探魔数（覆盖所有已知格式的魔数长度）
    char head[65536];
    ULONG headRead = 0;
    HRESULT hr = pStream->Read(head, sizeof(head), &headRead);
    if (FAILED(hr) || headRead == 0) return E_FAIL;
    std::string headData(head, headRead);
    std::wstring ext = SniffExtension(headData);

    // 2. 创建临时文件（%TEMP%\arkthumb_stream_<pid><ext>）
    wchar_t tmpPath[MAX_PATH] = {};
    if (!GetTempPathW(MAX_PATH, tmpPath)) return E_FAIL;
    std::wstring tmp = std::wstring(tmpPath) + L"arkthumb_stream_" +
                       std::to_wstring(GetCurrentProcessId()) + ext;
    HANDLE hFile = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return E_FAIL;

    // 3. 边读边写（64KB 缓冲循环，任意大小文件不占内存）
    BOOL ok = TRUE;
    DWORD written = 0;
    if (!WriteFile(hFile, head, headRead, &written, nullptr) || written != headRead) {
        ok = FALSE;
    }
    char buf[65536];
    ULONG nRead = 0;
    while (ok && SUCCEEDED(hr)) {
        hr = pStream->Read(buf, sizeof(buf), &nRead);
        if (FAILED(hr)) { ok = FALSE; break; }
        if (nRead == 0) break;
        written = 0;
        if (!WriteFile(hFile, buf, nRead, &written, nullptr) || written != nRead) {
            ok = FALSE;
            break;
        }
    }
    CloseHandle(hFile);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return E_FAIL;
    }

    _path = tmp;
    _hasStream = true;
    return S_OK;
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
