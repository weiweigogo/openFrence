#include "zoom_overlay.h"
#include "fence_window.h"
#include "render.h"
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>

namespace {

const wchar_t* const kShadeClass = L"OpenFencesZoomShade";
constexpr float kShadeDim = 0.80f;
constexpr UINT WM_APP_SHADE_READY = WM_APP + 0x52;

LRESULT CALLBACK ShadeWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

struct ShadeState {
    HWND hwnd = nullptr;
    HDC memDC = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ oldBmp = nullptr;
    int w = 0, h = 0;
    float fade = 0.0f;
    POINT origin = { 0, 0 };
    bool popup = false;
    bool ready = false;

    // Capture/blur runs off the UI thread so the zoom flight starts without
    // waiting ~40ms for the wallpaper. The shade appears as soon as it lands.
    std::thread worker;
    std::atomic<bool> cancel{ false };
    std::mutex mtx;
    std::vector<BYTE> blurPixels;
    int bw = 0, bh = 0;
};
ShadeState s_shade;
FenceWindow* s_fence = nullptr;
HHOOK s_mouseHook = nullptr;
HWND s_zoomFence = nullptr;

void RemoveMouseHook() {
    if (s_mouseHook) {
        UnhookWindowsHookEx(s_mouseHook);
        s_mouseHook = nullptr;
    }
    s_zoomFence = nullptr;
}

LRESULT CALLBACK ShadeMouseHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && s_zoomFence &&
        (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN || wp == WM_MBUTTONDOWN)) {
        const MSLLHOOKSTRUCT* ms = (const MSLLHOOKSTRUCT*)lp;
        RECT r = {};
        if (ms && GetWindowRect(s_zoomFence, &r)) {
            const POINT p = ms->pt;
            const bool inside = p.x >= r.left && p.x < r.right &&
                                p.y >= r.top && p.y < r.bottom;
            if (!inside) {
                PostMessageW(s_zoomFence, ZoomOverlay::kOutsideClickMessage, 0, 0);
                return 1;   // swallow: the shade is modal while open
            }
        }
    }
    return CallNextHookEx(s_mouseHook, code, wp, lp);
}

bool RegisterShadeClass() {
    static bool done = false;
    if (done) return true;
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = ShadeWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kShadeClass;
    done = RegisterClassExW(&wc) != 0;
    return done;
}

using GetWinDpiCtxT = DPI_AWARENESS_CONTEXT (WINAPI*)(HWND);
using SetThreadDpiCtxT = DPI_AWARENESS_CONTEXT (WINAPI*)(DPI_AWARENESS_CONTEXT);

DPI_AWARENESS_CONTEXT AdoptHostAwareness(HWND host) {
    if (!host) return nullptr;
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    auto getCtx = (GetWinDpiCtxT)(void*)GetProcAddress(u32, "GetWindowDpiAwarenessContext");
    auto setCtx = (SetThreadDpiCtxT)(void*)GetProcAddress(u32, "SetThreadDpiAwarenessContext");
    if (!getCtx || !setCtx) return nullptr;
    DPI_AWARENESS_CONTEXT hostCtx = getCtx(host);
    return hostCtx ? setCtx(hostCtx) : nullptr;
}

void RestoreAwareness(DPI_AWARENESS_CONTEXT prev) {
    if (!prev) return;
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    auto setCtx = (SetThreadDpiCtxT)(void*)GetProcAddress(u32, "SetThreadDpiAwarenessContext");
    if (setCtx) setCtx(prev);
}

void FreeShadeBitmap() {
    if (s_shade.memDC) {
        if (s_shade.oldBmp) SelectObject(s_shade.memDC, s_shade.oldBmp);
        if (s_shade.bmp) DeleteObject(s_shade.bmp);
        DeleteDC(s_shade.memDC);
    }
    s_shade.memDC = nullptr;
    s_shade.bmp = nullptr;
    s_shade.oldBmp = nullptr;
    s_shade.w = s_shade.h = 0;
    s_shade.ready = false;
}

void BuildShadeData(int x, int y, int w, int h) {
    HDC screenDC = GetDC(nullptr);
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* srcBits = nullptr;
    HBITMAP srcDib = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, &srcBits, nullptr, 0);
    if (!srcDib || !srcBits) {
        if (srcDib) DeleteObject(srcDib);
        ReleaseDC(nullptr, screenDC);
        return;
    }
    HDC srcDC = CreateCompatibleDC(screenDC);
    HGDIOBJ srcOld = SelectObject(srcDC, srcDib);
    BitBlt(srcDC, 0, 0, w, h, screenDC, x, y, SRCCOPY);

    std::vector<BYTE> blurred;
    int bw = 0, bh = 0;
    bool ok = BuildBlurredBackdrop((const BYTE*)srcBits, w, h, blurred, bw, bh);

    SelectObject(srcDC, srcOld);
    DeleteDC(srcDC);
    DeleteObject(srcDib);
    ReleaseDC(nullptr, screenDC);
    if (!ok || bw <= 0 || bh <= 0 || s_shade.cancel.load()) return;

    for (size_t i = 0; i < blurred.size(); i += 4) {
        blurred[i + 0] = (BYTE)(blurred[i + 0] * kShadeDim);
        blurred[i + 1] = (BYTE)(blurred[i + 1] * kShadeDim);
        blurred[i + 2] = (BYTE)(blurred[i + 2] * kShadeDim);
        blurred[i + 3] = 255;
    }

    {
        std::lock_guard<std::mutex> lk(s_shade.mtx);
        s_shade.blurPixels = std::move(blurred);
        s_shade.bw = bw;
        s_shade.bh = bh;
    }
}

void MakeFullBitmap() {
    std::vector<BYTE> pixels;
    int bw = 0, bh = 0;
    {
        std::lock_guard<std::mutex> lk(s_shade.mtx);
        if (s_shade.blurPixels.empty() || s_shade.bw <= 0 || s_shade.bh <= 0) return;
        pixels = s_shade.blurPixels;
        bw = s_shade.bw;
        bh = s_shade.bh;
    }
    const int w = s_shade.w, h = s_shade.h;
    FreeShadeBitmap();
    if (w <= 0 || h <= 0) return;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* dstBits = nullptr;
    HBITMAP dib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &dstBits, nullptr, 0);
    if (!dib || !dstBits) {
        if (dib) DeleteObject(dib);
        return;
    }
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) { DeleteObject(dib); return; }
    HGDIOBJ old = SelectObject(dc, dib);
    BITMAPINFO si = {};
    si.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    si.bmiHeader.biWidth = bw;
    si.bmiHeader.biHeight = -bh;
    si.bmiHeader.biPlanes = 1;
    si.bmiHeader.biBitCount = 32;
    si.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, nullptr);
    StretchDIBits(dc, 0, 0, w, h, 0, 0, bw, bh,
                  pixels.data(), &si, DIB_RGB_COLORS, SRCCOPY);
    s_shade.memDC = dc;
    s_shade.bmp = dib;
    s_shade.oldBmp = old;
    s_shade.w = w; s_shade.h = h;
    s_shade.ready = true;
}

void PresentShade() {
    if (!s_shade.hwnd || !IsWindow(s_shade.hwnd) || !s_shade.ready || !s_shade.memDC) return;
    HDC screen = GetDC(nullptr);
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, (BYTE)(s_shade.fade * 255.0f + 0.5f), AC_SRC_ALPHA };
    POINT ptDst = { 0, 0 };
    if (s_shade.popup) { ptDst.x = s_shade.origin.x; ptDst.y = s_shade.origin.y; }
    SIZE size = { s_shade.w, s_shade.h };
    POINT ptSrc = { 0, 0 };
    UpdateLayeredWindow(s_shade.hwnd, screen, &ptDst, &size,
                        s_shade.memDC, &ptSrc, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

LRESULT CALLBACK ShadeWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_APP_SHADE_READY:
        MakeFullBitmap();
        PresentShade();
        return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        if (s_fence) s_fence->ZoomRequestClose();
        return 0;
    case WM_DESTROY:
        RemoveMouseHook();
        s_shade.hwnd = nullptr;
        s_fence = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

namespace ZoomOverlay {

void OpenFor(FenceWindow* f) {
    Close();
    if (!f || !f->Hwnd() || !IsWindow(f->Hwnd())) return;
    HWND parent = GetParent(f->Hwnd());
    if (!RegisterShadeClass()) return;

    bool popup = (parent == nullptr);
    POINT org = { 0, 0 };
    int w = 0, h = 0;
    if (popup) {
        parent = GetDesktopWindow();
        w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        org.x = GetSystemMetrics(SM_XVIRTUALSCREEN);
        org.y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    } else {
        RECT cr = {};
        GetClientRect(parent, &cr);
        w = cr.right - cr.left;
        h = cr.bottom - cr.top;
        ClientToScreen(parent, &org);
    }
    if (w <= 0 || h <= 0) return;

    DPI_AWARENESS_CONTEXT prev = AdoptHostAwareness(parent);
    s_shade.hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW, kShadeClass, nullptr,
        popup ? WS_POPUP : WS_CHILD,
        0, 0, w, h, popup ? nullptr : parent, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    RestoreAwareness(prev);
    if (!s_shade.hwnd) return;
    s_fence = f;
    s_zoomFence = f->Hwnd();
    if (!s_mouseHook)
        s_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, ShadeMouseHook,
                                        GetModuleHandleW(nullptr), 0);
    s_shade.origin = org;
    s_shade.popup = popup;
    s_shade.w = w; s_shade.h = h;
    s_shade.fade = 0.0f;
    s_shade.ready = false;
    {
        std::lock_guard<std::mutex> lk(s_shade.mtx);
        s_shade.blurPixels.clear();
        s_shade.bw = s_shade.bh = 0;
    }
    s_shade.cancel.store(false);

    LONG_PTR ex = GetWindowLongPtrW(s_shade.hwnd, GWL_EXSTYLE);
    SetWindowLongPtrW(s_shade.hwnd, GWL_EXSTYLE, ex | WS_EX_TRANSPARENT);
    ShowWindow(s_shade.hwnd, SW_SHOWNA);
    if (popup)
        SetWindowPos(s_shade.hwnd, HWND_TOP, org.x, org.y, w, h, SWP_NOACTIVATE);
    else
        SetWindowPos(s_shade.hwnd, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    // Capture AFTER the window is created but before any content exists; the
    // worker then blurs off-thread while the fence flight already runs.
    s_shade.worker = std::thread([x = org.x, y = org.y, w, h]() {
        BuildShadeData(x, y, w, h);
        if (!s_shade.cancel.load()) {
            HWND hwnd = s_shade.hwnd;
            if (hwnd && IsWindow(hwnd)) PostMessageW(hwnd, WM_APP_SHADE_READY, 0, 0);
        }
    });
}

void SetFade(float t) {
    if (!s_shade.hwnd || !IsWindow(s_shade.hwnd)) { s_shade.hwnd = nullptr; return; }
    s_shade.fade = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    PresentShade();
    LONG_PTR ex = GetWindowLongPtrW(s_shade.hwnd, GWL_EXSTYLE);
    if (s_shade.fade < 0.15f) ex |= WS_EX_TRANSPARENT;
    else                      ex &= ~WS_EX_TRANSPARENT;
    SetWindowLongPtrW(s_shade.hwnd, GWL_EXSTYLE, ex);
}

void Close() {
    RemoveMouseHook();
    s_shade.cancel.store(true);
    if (s_shade.worker.joinable()) s_shade.worker.join();
    s_fence = nullptr;
    s_shade.fade = 0.0f;
    FreeShadeBitmap();
    {
        std::lock_guard<std::mutex> lk(s_shade.mtx);
        s_shade.blurPixels.clear();
        s_shade.bw = s_shade.bh = 0;
    }
    if (s_shade.hwnd && IsWindow(s_shade.hwnd)) DestroyWindow(s_shade.hwnd);
    s_shade.hwnd = nullptr;
    s_shade.ready = false;
}

bool Active() { return s_shade.hwnd != nullptr; }

HWND Shade() { return s_shade.hwnd; }

} // namespace ZoomOverlay
