// Minimal CComPtr stand-in for toolchains that ship without the ATL
// component (VS BuildTools installs). This is NOT a full atlbase.h — only
// the smart-pointer subset the project uses: default/copy/move construction,
// assignment, implicit raw conversion, operator&, Attach/Detach, Release,
// CoCreateInstance and QueryInterface. Semantics mirror ATL exactly
// (wrap/assign AddRefs, Release also clears the pointer), so call sites are
// unchanged. CMakeLists.txt only adds this directory to the include path
// when the real ATL headers are absent, so a full VS install never sees it.
#pragma once
#include <windows.h>
#include <unknwn.h>

template <class T>
class CComPtr {
public:
    T* p = nullptr;

    CComPtr() = default;
    CComPtr(T* lp) { if (lp) { p = lp; p->AddRef(); } }
    CComPtr(const CComPtr& o) : p(o.p) { if (p) p->AddRef(); }
    CComPtr(CComPtr&& o) noexcept : p(o.p) { o.p = nullptr; }
    ~CComPtr() { if (p) p->Release(); }

    CComPtr& operator=(T* lp) {
        if (p != lp) {
            T* old = p;
            if (lp) lp->AddRef();
            p = lp;
            if (old) old->Release();
        }
        return *this;
    }
    CComPtr& operator=(const CComPtr& o) {
        if (this != &o && p != o.p) {
            T* old = p;
            if (o.p) o.p->AddRef();
            p = o.p;
            if (old) old->Release();
        }
        return *this;
    }
    CComPtr& operator=(CComPtr&& o) noexcept {
        if (this != &o) {
            if (p) p->Release();
            p = o.p;
            o.p = nullptr;
        }
        return *this;
    }

    // ATL semantics: releases the pointee AND clears the pointer, so a
    // reuse of the CComPtr never doubles-releases.
    void Release() {
        T* t = p;
        if (t) { p = nullptr; t->Release(); }
    }

    // Ownership transfer: hand out the raw pointer and forget it.
    T* Detach() { T* t = p; p = nullptr; return t; }
    void Attach(T* t) { if (p) p->Release(); p = t; }

    HRESULT CoCreateInstance(REFCLSID rclsid, IUnknown* outer = nullptr,
                             DWORD ctx = CLSCTX_ALL) {
        Release();
        return ::CoCreateInstance(rclsid, outer, ctx, __uuidof(T), (void**)&p);
    }
    HRESULT QueryInterface(REFIID riid, void** ppv) {
        return p ? p->QueryInterface(riid, ppv) : E_POINTER;
    }
    template <class Q> HRESULT QueryInterface(Q** pp) {
        return pp ? QueryInterface(__uuidof(Q), (void**)pp) : E_POINTER;
    }
    template <class Q> HRESULT QueryInterface(CComPtr<Q>* pp) {
        return pp ? QueryInterface(__uuidof(Q), (void**)&pp->p) : E_POINTER;
    }

    operator T*() const { return p; }
    T& operator*() const { return *p; }
    T* operator->() const { return p; }
    T** operator&() { return &p; }
    bool operator==(T* t) const { return p == t; }
    bool operator!=(T* t) const { return p != t; }
};
