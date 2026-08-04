#include "ClassFactory.h"
#include <Windows.h>
#include <shlobj.h>   // SHChangeNotify
#include <string>

// ─── 固定 CLSID：{C4B7E2A1-9F3D-4A6E-8B5C-1D7E9F3A2B48} ───
// {C4B7E2A1-9F3D-4A6E-8B5C-1D7E9F3A2B48}
static const CLSID CLSID_ArkThumbProvider = {
    0xC4B7E2A1, 0x9F3D, 0x4A6E, { 0x8B, 0x5C, 0x1D, 0x7E, 0x9F, 0x3A, 0x2B, 0x48 } };

static const wchar_t* kClsidStr    = L"{C4B7E2A1-9F3D-4A6E-8B5C-1D7E9F3A2B48}";
// IThumbnailProvider IID（系统固定，用于 shellex 键名）
static const wchar_t* kThumbIidStr = L"{E357FCCD-A995-4576-B01F-234630154E96}";

// 受支持的扩展名（系统已原生支持的 JPG/PNG/WEBP 等不在此列，避免覆盖 WIC）
static const wchar_t* kExts[] = {
    L".arw", L".cr2", L".cr3", L".nef", L".dng", L".raf", L".x3f", L".pef", L".rw2", L".orf",
    L".psd", L".psb",
    L".heic", L".heif", L".hif",
    L".svg", L".svgz",
    L".hdr", L".pic"
};

static HMODULE g_hModule = nullptr;

// ── 注册表辅助：写入 REG_SZ 值（自动创建键） ──
static LSTATUS RegSetString(HKEY root, const wchar_t* path,
                            const wchar_t* value, const wchar_t* data) {
    HKEY hKey = nullptr;
    LSTATUS s = RegCreateKeyExW(root, path, 0, nullptr, 0,
                                KEY_SET_VALUE, nullptr, &hKey, nullptr);
    if (s != ERROR_SUCCESS) return s;
    s = RegSetValueExW(hKey, value, 0, REG_SZ,
                       (const BYTE*)data,
                       (DWORD)((wcslen(data) + 1) * sizeof(wchar_t)));
    RegCloseKey(hKey);
    return s;
}

// 获取本 DLL 完整路径
static std::wstring GetSelfPath() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(g_hModule, buf, MAX_PATH);
    return std::wstring(buf);
}

// ── DllMain：记录模块句柄 + 把本 DLL 所在目录加入 DLL 搜索路径 ──
// 宿主进程（dllhost.exe / regsvr32 / 测试程序）的 EXE 目录并非本 DLL 目录，
// 默认 LoadLibrary("libraw.dll") 找不到 libraw/heif/libde265 —— 必须显式加入搜索路径
static void EnableSelfDirSearch(HMODULE hModule) {
    wchar_t buf[MAX_PATH] = {};
    if (!GetModuleFileNameW(hModule, buf, MAX_PATH)) return;
    std::wstring path(buf);
    auto pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return;
    // SetDllDirectory 把指定目录插入搜索路径（替代"当前目录"槽位），全进程生效
    SetDllDirectoryW(path.substr(0, pos).c_str());
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
        EnableSelfDirSearch(hModule);
    }
    return TRUE;
}

// ── DllGetClassObject：COM 入口，返回类工厂 ──
STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (!IsEqualCLSID(rclsid, CLSID_ArkThumbProvider))
        return CLASS_E_CLASSNOTAVAILABLE;
    // 静态工厂常驻，COM 服务器生命周期内不销毁
    static ThumbClassFactory s_factory;
    return s_factory.QueryInterface(riid, ppv);
}

// ── DllCanUnloadNow：简化为永不卸载（静态工厂常驻） ──
STDAPI DllCanUnloadNow() { return S_FALSE; }

// ── DllRegisterServer：写 CLSID + 各扩展名 shellex 键 ──
STDAPI DllRegisterServer() {
    std::wstring dllPath = GetSelfPath();
    if (dllPath.empty()) return E_FAIL;

    // 1. CLSID 注册：InprocServer32 = DLL 路径，ThreadingModel = Apartment
    std::wstring clsidKey = std::wstring(L"CLSID\\") + kClsidStr;
    RegSetString(HKEY_CLASSES_ROOT, clsidKey.c_str(), nullptr, L"ArkThumbProvider");
    RegSetString(HKEY_CLASSES_ROOT, (clsidKey + L"\\InprocServer32").c_str(),
                 nullptr, dllPath.c_str());
    RegSetString(HKEY_CLASSES_ROOT, (clsidKey + L"\\InprocServer32").c_str(),
                 L"ThreadingModel", L"Apartment");

    // 2. 各扩展名 → ThumbnailProvider shellex 键
    //    同时写 .ext\shellex 与 SystemFileAssociations\.ext\shellex，
    //    后者兜底：当 .ext 被其他软件的 ProgId 占用时仍生效
    for (const wchar_t* ext : kExts) {
        std::wstring s1 = std::wstring(ext) + L"\\shellex\\" + kThumbIidStr;
        RegSetString(HKEY_CLASSES_ROOT, s1.c_str(), nullptr, kClsidStr);
        std::wstring s2 = std::wstring(L"SystemFileAssociations\\") + ext +
                          L"\\shellex\\" + kThumbIidStr;
        RegSetString(HKEY_CLASSES_ROOT, s2.c_str(), nullptr, kClsidStr);

        // 2a. 默认 ProgId 的 shellex：Windows 缩略图路由优先查扩展名关联的
        //     ProgId（如 .psd → Photoshop.Image.27），若该 ProgId 有自己的
        //     缩略图处理器（常失效）就轮不到我们。把自己的 CLSID 写进 ProgId
        //     shellex，确保无论关联到哪个程序缩略图都生效（参考 PicPreview）。
        HKEY hExt = nullptr;
        wchar_t progId[256] = {};
        DWORD progIdLen = sizeof(progId);
        if (RegOpenKeyExW(HKEY_CLASSES_ROOT, ext, 0, KEY_QUERY_VALUE, &hExt) == ERROR_SUCCESS) {
            LONG qr = RegQueryValueExW(hExt, nullptr, nullptr, nullptr,
                                       (LPBYTE)progId, &progIdLen);
            RegCloseKey(hExt);
            if (qr == ERROR_SUCCESS && progId[0] != L'\0') {
                std::wstring pKey = std::wstring(progId) + L"\\shellex\\" + kThumbIidStr;
                RegSetString(HKEY_CLASSES_ROOT, pKey.c_str(), nullptr, kClsidStr);
            }
        }
    }

    // 2b. Shell Extensions Approved 批准列表（Windows 10/11 加载 shell 扩展的必要条件）
    //     缺这个键时 dllhost 拒绝加载 provider → 缩略图不显示（Win11 24H2 实测必现）
    //     注意：必须用 HKEY_LOCAL_MACHINE + 完整路径，不能用 HKEY_CLASSES_ROOT 前缀
    //     （HKCR 是 HKLM\Software\Classes 的映射，写进去会落到 Classes\Software\... 错误位置）
    RegSetString(HKEY_LOCAL_MACHINE,
                 L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved",
                 kClsidStr, L"ArkThumbProvider Thumbnail Handler");

    // 3. 通知 shell 刷新缩略图缓存
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

// ── DllUnregisterServer：删除注册表项 ──
// 精准删除本 provider 的 IID 子键，保留其他软件在 shellex 下注册的处理器
STDAPI DllUnregisterServer() {
    // 0. 删除 Shell Extensions Approved 条目（HKLM 完整路径，见 DllRegisterServer 注释）
    HKEY hApproved = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved",
                      0, KEY_SET_VALUE, &hApproved) == ERROR_SUCCESS) {
        RegDeleteValueW(hApproved, kClsidStr);
        RegCloseKey(hApproved);
    }

    // 1. 删 CLSID 整树
    std::wstring clsidKey = std::wstring(L"CLSID\\") + kClsidStr;
    RegDeleteTreeW(HKEY_CLASSES_ROOT, clsidKey.c_str());

    // 2. 各扩展名：仅删 shellex\{ThumbIID} 子键，再尝试删空的父键（非空则保留）
    for (const wchar_t* ext : kExts) {
        std::wstring iid1 = std::wstring(ext) + L"\\shellex\\" + kThumbIidStr;
        RegDeleteKeyW(HKEY_CLASSES_ROOT, iid1.c_str());
        RegDeleteKeyW(HKEY_CLASSES_ROOT, (std::wstring(ext) + L"\\shellex").c_str());

        std::wstring saPrefix = std::wstring(L"SystemFileAssociations\\") + ext;
        RegDeleteKeyW(HKEY_CLASSES_ROOT, (saPrefix + L"\\shellex\\" + kThumbIidStr).c_str());
        RegDeleteKeyW(HKEY_CLASSES_ROOT, (saPrefix + L"\\shellex").c_str());
        RegDeleteKeyW(HKEY_CLASSES_ROOT, saPrefix.c_str());
    }

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}
