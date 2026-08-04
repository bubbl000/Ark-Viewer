#pragma once
#include <Windows.h>
#include <thumbcache.h>      // IThumbnailProvider
#include <shobjidl_core.h>   // IInitializeWithItem, IShellItem
#include <string>

// 缩略图提供程序 COM 对象
// 实现 IThumbnailProvider（资源管理器调 GetThumbnail 取 HBITMAP）
// + IInitializeWithItem（Shell 通过 IShellItem 注入文件路径，避免全量读流）
// + IUnknown
class ThumbProvider : public IThumbnailProvider, public IInitializeWithItem {
public:
    ThumbProvider() = default;

    // ── IUnknown ──
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    IFACEMETHODIMP_(ULONG) AddRef() override;
    IFACEMETHODIMP_(ULONG) Release() override;

    // ── IInitializeWithItem ──
    IFACEMETHODIMP Initialize(IShellItem* pItem, DWORD grfMode) override;

    // ── IThumbnailProvider ──
    IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP* phbmp, WTS_ALPHATYPE* pdwAlpha) override;

private:
    LONG  _ref  = 1;
    DWORD _mode = 0;
    std::wstring _path;  // 由 IShellItem 解析出的文件系统路径
};
