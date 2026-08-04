#pragma once
#include <Windows.h>
#include <oleidl.h>      // IDataObject / IDropSource / IEnumFORMATETC
#include <string>
#include <vector>

// OLE 拖出实现：把当前图片以 CF_HDROP（文件路径）形式拖给外部应用（如 Photoshop）
// 设计要点：
//   - ArkDropSource：QueryContinueDrag 检测鼠标回到客户区即取消（"可撤销拖出"）
//   - ArkDataObject：最小 IDataObject，存 CF_HDROP
//   - 拖拽光标：GiveFeedback 返回 DRAGDROP_S_USEDEFAULTCURSORS，系统画标准复制光标（带 + 号）

// IDropSource：控制拖拽继续/取消
class ArkDropSource : public IDropSource {
public:
    ArkDropSource(HWND hwnd, int clientW, int clientH)
        : _hwnd(hwnd), _clientW(clientW), _clientH(clientH) {}

    // 拖拽期间鼠标回到客户区触发的取消（区别于 Esc/按钮释放）
    bool CanceledByReturn() const { return _canceledByReturn; }

    // ── IUnknown ──
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&_ref); }
    IFACEMETHODIMP_(ULONG) Release() override {
        LONG n = InterlockedDecrement(&_ref);
        if (n == 0) delete this;
        return (ULONG)n;
    }

    // ── IDropSource ──
    IFACEMETHODIMP QueryContinueDrag(BOOL fEscapePressed, DWORD grfKeyState) override;
    // 返回 DRAGDROP_S_USEDEFAULTCURSORS：系统绘制标准拖拽光标（带 + 号的复制光标）
    IFACEMETHODIMP GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }

private:
    LONG  _ref = 1;
    HWND  _hwnd = nullptr;
    int   _clientW = 0;
    int   _clientH = 0;
    bool  _canceledByReturn = false;
};

// 最小 IDataObject：CF_HDROP（文件路径）+ SetData 接收 DragImageBits
class ArkDataObject : public IDataObject {
public:
    ArkDataObject() = default;
    ~ArkDataObject();

    // 添加文件路径（CF_HDROP 格式）。可多次调用添加多个文件
    void AddFilePath(const std::wstring& path);

    // ── IUnknown ──
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&_ref); }
    IFACEMETHODIMP_(ULONG) Release() override {
        LONG n = InterlockedDecrement(&_ref);
        if (n == 0) delete this;
        return (ULONG)n;
    }

    // ── IDataObject ──
    IFACEMETHODIMP GetData(FORMATETC* pformatetcIn, STGMEDIUM* pmedium) override;
    IFACEMETHODIMP GetDataHere(FORMATETC* pformatetc, STGMEDIUM* pmedium) override;
    IFACEMETHODIMP QueryGetData(FORMATETC* pformatetc) override;
    IFACEMETHODIMP GetCanonicalFormatEtc(FORMATETC* pformatectIn, FORMATETC* pformatetcOut) override;
    IFACEMETHODIMP SetData(FORMATETC* pformatetc, STGMEDIUM* pmedium, BOOL fRelease) override;
    IFACEMETHODIMP EnumFormatEtc(DWORD dwDirection, IEnumFORMATETC** ppenumFormatEtc) override;
    IFACEMETHODIMP DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return E_NOTIMPL; }
    IFACEMETHODIMP DUnadvise(DWORD) override { return E_NOTIMPL; }
    IFACEMETHODIMP EnumDAdvise(IEnumSTATDATA**) override { return E_NOTIMPL; }

private:
    struct Entry { FORMATETC fe; STGMEDIUM med; };
    std::vector<Entry> _storage;
    LONG _ref = 1;

    static bool FormatEq(const FORMATETC& a, const FORMATETC& b);
};

// 对外入口：发起一次拖出
// hwnd/clientW/clientH：用于 QueryContinueDrag 检测"拖回客户区取消"
// filePath：拖出的文件完整路径（CF_HDROP）
// 返回 true=取消且鼠标左键仍按住（应恢复平移），false=其他（drop 成功/Esc/按钮释放）
namespace DragOut {
bool DoDragOut(HWND hwnd, int clientW, int clientH,
               const std::wstring& filePath);
}
