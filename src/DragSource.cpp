#include "DragSource.h"
#include <shellapi.h>    // DROPFILES / CF_HDROP
#include <shlobj.h>      // SHCreateStdEnumFmtEtc
#include <cstring>

// ═══════════════════════ ArkDropSource ═══════════════════════

IFACEMETHODIMP ArkDropSource::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDropSource)) {
        *ppv = static_cast<IDropSource*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

// 拖拽每帧由 OLE 调用，决定继续/取消
IFACEMETHODIMP ArkDropSource::QueryContinueDrag(BOOL fEscapePressed, DWORD grfKeyState) {
    // Esc → 取消
    if (fEscapePressed) return DRAGDROP_S_CANCEL;

    // 左键释放 → DRAGDROP_S_DROP 交 OLE 执行 drop（在有效 target 上 drop，否则 OLE 自行取消）
    if (!(grfKeyState & MK_LBUTTON)) return DRAGDROP_S_DROP;

    // 左键仍按住：检测鼠标是否回到本窗口客户区（"可撤销拖出"的取消条件）
    // 用 WindowFromPoint 判断鼠标下的顶层窗口是否还是自己——比 ScreenToClient 范围
    // 判断更可靠：目标窗口（如 PS）覆盖本窗口时，坐标虽在客户区内但鼠标并不在本窗口上，
    // 用坐标判断会误取消拖拽
    POINT pt;
    if (!GetCursorPos(&pt)) return S_OK;
    HWND under = WindowFromPoint(pt);
    if (under == _hwnd) {
        // 鼠标在本窗口上：仅当在客户区内才取消（标题栏等非客户区不算拖回）
        POINT cpt = pt;
        if (ScreenToClient(_hwnd, &cpt)) {
            if (cpt.x >= 0 && cpt.x < _clientW && cpt.y >= 0 && cpt.y < _clientH) {
                _canceledByReturn = true;
                return DRAGDROP_S_CANCEL;
            }
        }
    }
    return S_OK;  // 继续拖拽
}

// ═══════════════════════ ArkDataObject ═══════════════════════

ArkDataObject::~ArkDataObject() {
    for (auto& e : _storage) ReleaseStgMedium(&e.med);
}

bool ArkDataObject::FormatEq(const FORMATETC& a, const FORMATETC& b) {
    return a.cfFormat == b.cfFormat &&
           (a.tymed & b.tymed) != 0 &&
           a.dwAspect == b.dwAspect;
}

// 添加文件路径，构造 CF_HDROP 的 HGLOBAL 存入 _storage
void ArkDataObject::AddFilePath(const std::wstring& path) {
    // HDROP 内存布局：DROPFILES 头 + 终止 \0 + 双 \0 结尾
    size_t pathBytes = (path.size() + 1) * sizeof(wchar_t);
    size_t total = sizeof(DROPFILES) + pathBytes + sizeof(wchar_t);  // 末尾多一个 \0

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, total);
    if (!hMem) return;
    auto* base = (uint8_t*)GlobalLock(hMem);
    if (!base) { GlobalFree(hMem); return; }

    auto* df = reinterpret_cast<DROPFILES*>(base);
    df->pFiles = sizeof(DROPFILES);
    df->pt.x = df->pt.y = 0;
    df->fNC = FALSE;
    df->fWide = TRUE;
    wchar_t* dst = reinterpret_cast<wchar_t*>(base + sizeof(DROPFILES));
    memcpy(dst, path.c_str(), pathBytes);
    dst[path.size() + 1] = L'\0';  // 双 \0 结尾
    GlobalUnlock(hMem);

    Entry e;
    e.fe.cfFormat = CF_HDROP;
    e.fe.ptd = nullptr;
    e.fe.dwAspect = DVASPECT_CONTENT;
    e.fe.lindex = -1;
    e.fe.tymed = TYMED_HGLOBAL;
    e.med.tymed = TYMED_HGLOBAL;
    e.med.hGlobal = hMem;
    e.med.pUnkForRelease = nullptr;
    _storage.push_back(e);
}

IFACEMETHODIMP ArkDataObject::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDataObject)) {
        *ppv = static_cast<IDataObject*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

IFACEMETHODIMP ArkDataObject::GetData(FORMATETC* pformatetcIn, STGMEDIUM* pmedium) {
    if (!pformatetcIn || !pmedium) return E_POINTER;
    pmedium->tymed = TYMED_NULL;
    pmedium->pUnkForRelease = nullptr;

    for (auto& e : _storage) {
        if (FormatEq(e.fe, *pformatetcIn)) {
            // 复制 HGLOBAL 给调用方（调用方负责 ReleaseStgMedium）
            SIZE_T sz = GlobalSize(e.med.hGlobal);
            if (!sz) return E_FAIL;
            HGLOBAL hCopy = GlobalAlloc(GMEM_MOVEABLE, sz);
            if (!hCopy) return E_OUTOFMEMORY;
            memcpy(GlobalLock(hCopy), GlobalLock(e.med.hGlobal), sz);
            GlobalUnlock(hCopy); GlobalUnlock(e.med.hGlobal);
            pmedium->tymed = TYMED_HGLOBAL;
            pmedium->hGlobal = hCopy;
            return S_OK;
        }
    }
    return DV_E_FORMATETC;
}

IFACEMETHODIMP ArkDataObject::GetDataHere(FORMATETC* pformatetc, STGMEDIUM* pmedium) {
    // 简化：GetDataHere 与 GetData 行为一致，调用方提供 medium 时复制进其 HGLOBAL
    if (!pformatetc || !pmedium) return E_POINTER;
    for (auto& e : _storage) {
        if (FormatEq(e.fe, *pformatetc) && pmedium->tymed == TYMED_HGLOBAL && pmedium->hGlobal) {
            SIZE_T sz = GlobalSize(e.med.hGlobal);
            SIZE_T dstSz = GlobalSize(pmedium->hGlobal);
            if (!sz || sz > dstSz) return E_FAIL;
            memcpy(GlobalLock(pmedium->hGlobal), GlobalLock(e.med.hGlobal), sz);
            GlobalUnlock(pmedium->hGlobal); GlobalUnlock(e.med.hGlobal);
            return S_OK;
        }
    }
    return DV_E_FORMATETC;
}

IFACEMETHODIMP ArkDataObject::QueryGetData(FORMATETC* pformatetc) {
    if (!pformatetc) return E_POINTER;
    for (auto& e : _storage) {
        if (FormatEq(e.fe, *pformatetc)) return S_OK;
    }
    return DV_E_FORMATETC;
}

IFACEMETHODIMP ArkDataObject::GetCanonicalFormatEtc(FORMATETC*, FORMATETC* pformatetcOut) {
    if (!pformatetcOut) return E_POINTER;
    pformatetcOut->ptd = nullptr;
    return E_NOTIMPL;
}

IFACEMETHODIMP ArkDataObject::SetData(FORMATETC* pformatetc, STGMEDIUM* pmedium, BOOL fRelease) {
    if (!pformatetc || !pmedium) return E_POINTER;
    Entry e;
    e.fe = *pformatetc;
    if (pformatetc->ptd) {
        // 深拷贝 DVTARGETDEVICE，避免持有调用方的指针导致悬挂引用
        e.fe.ptd = (DVTARGETDEVICE*)CoTaskMemAlloc(sizeof(DVTARGETDEVICE));
        if (e.fe.ptd) memcpy(e.fe.ptd, pformatetc->ptd, sizeof(DVTARGETDEVICE));
    }
    if (fRelease) {
        e.med = *pmedium;  // 接管所有权
    } else {
        // 复制 HGLOBAL
        if (pmedium->tymed != TYMED_HGLOBAL || !pmedium->hGlobal) return E_NOTIMPL;
        SIZE_T sz = GlobalSize(pmedium->hGlobal);
        HGLOBAL hCopy = GlobalAlloc(GMEM_MOVEABLE, sz);
        if (!hCopy) return E_OUTOFMEMORY;
        memcpy(GlobalLock(hCopy), GlobalLock(pmedium->hGlobal), sz);
        GlobalUnlock(hCopy); GlobalUnlock(pmedium->hGlobal);
        e.med.tymed = TYMED_HGLOBAL;
        e.med.hGlobal = hCopy;
        e.med.pUnkForRelease = nullptr;
    }
    _storage.push_back(e);
    return S_OK;
}

IFACEMETHODIMP ArkDataObject::EnumFormatEtc(DWORD dwDirection, IEnumFORMATETC** ppenumFormatEtc) {
    if (!ppenumFormatEtc) return E_POINTER;
    *ppenumFormatEtc = nullptr;
    if (dwDirection != DATADIR_GET) return E_NOTIMPL;

    std::vector<FORMATETC> fmts;
    for (auto& e : _storage) fmts.push_back(e.fe);
    if (fmts.empty()) return E_FAIL;
    return SHCreateStdEnumFmtEtc((UINT)fmts.size(), fmts.data(), ppenumFormatEtc);
}

// ═══════════════════════ DragOut ═══════════════════════

bool DragOut::DoDragOut(HWND hwnd, int clientW, int clientH,
                        const std::wstring& filePath) {
    ArkDataObject* dataObj = new (std::nothrow) ArkDataObject();
    if (!dataObj) return false;
    dataObj->AddFilePath(filePath);
    // AddFilePath 后 dataObj 引用计数仍为 1（构造时），DoDragDrop 期间由 dataObj/ptr 持有

    ArkDropSource dropSrc(hwnd, clientW, clientH);

    // 仅用系统标准拖拽光标（GiveFeedback 返回 DRAGDROP_S_USEDEFAULTCURSORS），
    // 不注入缩略图，避免模态循环里读 GPU/锁导致死锁
    DWORD dwEffect = DROPEFFECT_COPY;
    HRESULT hr = DoDragDrop(dataObj, &dropSrc, DROPEFFECT_COPY, &dwEffect);
    dataObj->Release();

    // 拖出"取消且按钮仍按住"= 鼠标拖回客户区触发的取消
    // 此时调用方应恢复平移；其他情况（drop 成功/Esc/按钮释放在外）= false
    if (dropSrc.CanceledByReturn() && (GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
        return true;
    }
    (void)hr;
    return false;
}
