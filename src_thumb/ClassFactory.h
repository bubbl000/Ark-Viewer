#pragma once
#include <Windows.h>
#include <unknwn.h>

// COM 类工厂：CreateInstance 返回 ThumbProvider 实例
class ThumbClassFactory : public IClassFactory {
public:
    // ── IUnknown ──
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    IFACEMETHODIMP_(ULONG) AddRef() override;
    IFACEMETHODIMP_(ULONG) Release() override;

    // ── IClassFactory ──
    IFACEMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) override;
    IFACEMETHODIMP LockServer(BOOL fLock) override;

private:
    LONG _ref = 1;
};
