#include "FileAssoc.h"
#include "Logger.h"
#include <Windows.h>
#include <shlobj.h>    // IApplicationAssociationRegistration / SetAppAsDefault
#include <shlguid.h>   // CLSID_ApplicationAssociationRegistration

namespace FileAssoc {

// 取本 exe 完整路径
static std::wstring ExePath() {
    wchar_t p[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return p;
}

// 写一个 REG_SZ 注册表值（HKCU 下），封装 RegCreateKeyExW + RegSetValueExW
static void SetRegSz(HKEY root, const wchar_t* subkey, const wchar_t* value,
                     const std::wstring& data) {
    HKEY hKey;
    if (RegCreateKeyExW(root, subkey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &hKey, nullptr) != ERROR_SUCCESS) return;
    RegSetValueExW(hKey, value, 0, REG_SZ,
                   (const BYTE*)data.c_str(),
                   (DWORD)((data.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(hKey);
}

void EnsureProgId() {
    std::wstring exe = ExePath();
    // 注意：HKCU 的类注册根是 Software\Classes，直接写 PROG_ID 会落到 HKCU\ArkViewer2.Image（错误位置）
    std::wstring base = std::wstring(L"Software\\Classes\\") + PROG_ID;
    SetRegSz(HKEY_CURRENT_USER, base.c_str(), nullptr, L"Ark Viewer 2 Image");
    SetRegSz(HKEY_CURRENT_USER,
             (base + L"\\DefaultIcon").c_str(),
             nullptr, exe + L",0");
    SetRegSz(HKEY_CURRENT_USER,
             (base + L"\\shell\\open\\command").c_str(),
             nullptr, L"\"" + exe + L"\" \"%1\"");

    // Capabilities 注册（SetAppAsDefault 的硬性前提）：
    // HKCU\Software\RegisteredApplications 值 "Ark Viewer 2" → Software\Classes\ArkViewer2.Image\Capabilities
    // Capabilities\FileAssociations 下每个扩展名 → PROG_ID（由 Associate 逐个添加）
    SetRegSz(HKEY_CURRENT_USER, L"Software\\RegisteredApplications",
             L"Ark Viewer 2", base + L"\\Capabilities");
    SetRegSz(HKEY_CURRENT_USER, (base + L"\\Capabilities").c_str(),
             L"ApplicationName", L"Ark Viewer 2");
    SetRegSz(HKEY_CURRENT_USER, (base + L"\\Capabilities\\FileAssociations").c_str(),
             nullptr, L"");  // 创建子键，扩展名映射由 Associate 写入
}

// 注册单个扩展名到 Capabilities\FileAssociations（SetAppAsDefault 需要）
static void RegisterCapability(const std::wstring& ext) {
    std::wstring key = L"Software\\Classes\\ArkViewer2.Image\\Capabilities\\FileAssociations";
    SetRegSz(HKEY_CURRENT_USER, key.c_str(), ext.c_str(), PROG_ID);
}

bool IsAssociated(const std::wstring& ext) {
    std::wstring key = L"Software\\Classes\\" + ext + L"\\OpenWithProgids";
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return false;
    // 枚举值名，找含 PROG_ID 的
    bool found = false;
    wchar_t valName[64];
    DWORD nameLen = _countof(valName);
    DWORD idx = 0;
    while (RegEnumValueW(hKey, idx++, valName, &nameLen, nullptr,
                         nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
        if (wcscmp(valName, PROG_ID) == 0) { found = true; break; }
        nameLen = _countof(valName);
    }
    RegCloseKey(hKey);
    return found;
}

bool Associate(const std::wstring& ext) {
    EnsureProgId();
    RegisterCapability(ext);  // Capabilities\FileAssociations 注册（SetAppAsDefault 前提）
    // 主路径：系统标准 API，自动处理 UserChoice hash
    // 注意：SetAppAsDefault 第一个参数是 ProgId 字符串（如 "ArkViewer2.Image"），
    // 不是 exe 路径——传错会导致 API 失败走兜底，只加"打开方式"不设默认
    IApplicationAssociationRegistration* reg = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ApplicationAssociationRegistration, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&reg));
    if (SUCCEEDED(hr) && reg) {
        // ASSOCIATIONTYPE::AT_FILEEXTENSION 用于文件扩展名关联（如 .jpg）
        hr = reg->SetAppAsDefault(PROG_ID, ext.c_str(), AT_FILEEXTENSION);
        reg->Release();
        if (SUCCEEDED(hr)) return true;
    }
    // 兜底：SetAppAsDefault 被 UserChoice hash 拦截时，写 OpenWithProgids 加入"打开方式"列表
    std::wstring key = L"Software\\Classes\\" + ext + L"\\OpenWithProgids";
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        DWORD zero = 0;
        RegSetValueExW(hKey, PROG_ID, 0, REG_NONE, (const BYTE*)&zero, 0);
        RegCloseKey(hKey);
        return true;
    }
    LOG_WARN("FileAssoc", "associate failed");
    return false;
}

bool Unassociate(const std::wstring& ext) {
    std::wstring key = L"Software\\Classes\\" + ext + L"\\OpenWithProgids";
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS)
        return true;  // 键不存在视为已取消
    LSTATUS res = RegDeleteValueW(hKey, PROG_ID);
    RegCloseKey(hKey);
    return res == ERROR_SUCCESS || res == ERROR_FILE_NOT_FOUND;
}

}  // namespace FileAssoc
