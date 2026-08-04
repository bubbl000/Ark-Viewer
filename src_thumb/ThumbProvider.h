#pragma once
#include <Windows.h>
#include <thumbcache.h>      // IThumbnailProvider
#include <shobjidl_core.h>   // IInitializeWithItem, IInitializeWithStream, IShellItem, IStream
#include <string>

// 缩略图提供程序 COM 对象
// 实现 IThumbnailProvider（资源管理器调 GetThumbnail 取 HBITMAP）
// + IInitializeWithItem（Shell 通过 IShellItem 注入文件路径）
// + IInitializeWithStream（Shell 打开文件注入 IStream —— dllhost 低完整性环境下
//     provider 自己 CreateFileW 打开 E 盘文件可能失败，Explorer 传流则无权限问题）
// + IUnknown
class ThumbProvider : public IThumbnailProvider,
                      public IInitializeWithItem,
                      public IInitializeWithStream {
public:
    ThumbProvider() = default;

    // ── IUnknown ──
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    IFACEMETHODIMP_(ULONG) AddRef() override;
    IFACEMETHODIMP_(ULONG) Release() override;

    // ── IInitializeWithItem ──
    IFACEMETHODIMP Initialize(IShellItem* pItem, DWORD grfMode) override;

    // ── IInitializeWithStream ──
    IFACEMETHODIMP Initialize(IStream* pStream, DWORD grfMode) override;

    // ── IThumbnailProvider ──
    IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP* phbmp, WTS_ALPHATYPE* pdwAlpha) override;

private:
    LONG  _ref  = 1;
    DWORD _mode = 0;
    std::wstring _path;  // 由 IShellItem 解析出的路径 / 流模式写出的临时文件路径
    bool _hasStream = false;
};
