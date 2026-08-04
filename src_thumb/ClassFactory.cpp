#include "ClassFactory.h"
#include "ThumbProvider.h"
#include <new>  // std::nothrow

// ── IUnknown ──
IFACEMETHODIMP ThumbClassFactory::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IClassFactory)) {
        *ppv = static_cast<IClassFactory*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

IFACEMETHODIMP_(ULONG) ThumbClassFactory::AddRef() {
    return InterlockedIncrement(&_ref);
}

IFACEMETHODIMP_(ULONG) ThumbClassFactory::Release() {
    LONG n = InterlockedDecrement(&_ref);
    // 类工厂由 DllGetClassObject 持有静态实例，不主动销毁
    return (ULONG)n;
}

// ── IClassFactory ──
IFACEMETHODIMP ThumbClassFactory::CreateInstance(
    IUnknown* pUnkOuter, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (pUnkOuter) return CLASS_E_NOAGGREGATION;  // 不支持聚合

    auto* p = new (std::nothrow) ThumbProvider();
    if (!p) return E_OUTOFMEMORY;
    // QI 会 AddRef；临时引用释放后由调用方持有
    HRESULT hr = p->QueryInterface(riid, ppv);
    p->Release();
    return hr;
}

IFACEMETHODIMP ThumbClassFactory::LockServer(BOOL fLock) {
    (void)fLock;
    return S_OK;  // 简化：服务器常驻，不实际计数锁
}
