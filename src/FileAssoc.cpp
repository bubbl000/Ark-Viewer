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
    SetRegSz(HKEY_CURRENT_USER, PROG_ID, nullptr, L"Ark Viewer 2 Image");
    SetRegSz(HKEY_CURRENT_USER,
             (std::wstring(PROG_ID) + L"\\DefaultIcon").c_str(),
             nullptr, exe + L",0");
    SetRegSz(HKEY_CURRENT_USER,
             (std::wstring(PROG_ID) + L"\\shell\\open\\command").c_str(),
             nullptr, L"\"" + exe + L"\" \"%1\"");
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
    // 主路径：系统标准 API，自动处理 UserChoice hash
    IApplicationAssociationRegistration* reg = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ApplicationAssociationRegistration, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&reg));
    if (SUCCEEDED(hr) && reg) {
        // ASSOCIATIONTYPE::AT_FILEEXTENSION 用于文件扩展名关联（如 .jpg）
        hr = reg->SetAppAsDefault(ExePath().c_str(), ext.c_str(), AT_FILEEXTENSION);
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
