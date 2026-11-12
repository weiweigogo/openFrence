#include "context_menu.h"
#include "render.h"
#include "icon_extract.h"
#include <windowsx.h>
#include <shlobj.h>
#include <shellapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <atlbase.h>
#include <vector>
#include <memory>
#include <algorithm>
#include <cmath>
#include <functional>

// ─────────────────────────────────────────────────────────────────────────────
// FluentMenu — a self-drawn Windows 11 style context menu.
//
// We cannot show Explorer's real XAML menu from another process, so we read
// the fully-populated legacy HMENU (text, icons, state, submenus) and repaint
// it ourselves in the Win11 dark Fluent style. See context_menu.h for the
// rationale. This file contains:
//   • a menu model built by walking the HMENU,
//   • icon extraction (HBITMAP -> premultiplied BGRA pixel buffer),
//   • a layered popup window per open menu level (root + flyout submenus),
//   • Direct2D rendering and a nested blocking message loop for interaction.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// ── Small utilities ──
static float DpiScale() {
    HDC h = GetDC(nullptr);
    int d = GetDeviceCaps(h, LOGPIXELSX);
    ReleaseDC(nullptr, h);
    return d / 96.0f;
}
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Design metrics at 96 DPI; every value is multiplied by the DPI scale at use.
struct Metrics {
    float rowH   = 30;    // normal item row height
    float sepH   = 9;     // separator band height
    float padV   = 5;     // vertical padding inside the surface
    float padH   = 5;     // horizontal padding inside the surface
    float iconCol= 26;    // reserved icon column width
    float iconSz = 16;    // drawn icon size
    float gap    = 10;    // gap between icon column and text
    float chevr  = 18;    // width reserved for the submenu chevron
    float radius = 8;     // surface corner radius
    float rowRad = 4;     // hover highlight corner radius
    float minW   = 170;
    float maxW   = 340;
    float maxTextW = 240; // longest label before clipping
    float fontPx = 13;
};

// One menu entry (a row) plus its optional children (a flyout submenu).
struct CtxItem {
    std::wstring text;
    UINT id = 0;
    bool sep = false;
    bool disabled = false;
    bool checked = false;
    bool isDefault = false;   // the bold "Open" verb
    bool hasSub = false;
    int iconW = 0, iconH = 0;
    std::vector<BYTE> iconPx; // premultiplied BGRA (iconW*iconH*4), empty = none
    std::vector<CtxItem> children;
};

// Extract pixels for a file's icon by drawing the HICON over black and white
// and recovering alpha from the difference (robust for all icon formats).
static bool ExtractFileIconPixels(const std::wstring& path, int size,
                                  std::vector<BYTE>& out, int& w, int& h) {
    SHFILEINFOW sfi = {};
    if (!SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi),
                        SHGFI_ICON | SHGFI_SMALLICON))
        return false;
    HICON icon = sfi.hIcon;
    if (!icon) return false;

    HDC screen = GetDC(nullptr);
    auto grab = [&](BYTE fill, std::vector<BYTE>& px) {
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = size;
        bi.bmiHeader.biHeight = -size;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        px.assign((size_t)size * size * 4, 0);
        void* bits = nullptr;
        HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dib) return false;
        HDC mem = CreateCompatibleDC(screen);
        HGDIOBJ old = SelectObject(mem, dib);
        RECT rc = { 0, 0, size, size };
        HBRUSH br = CreateSolidBrush(RGB(fill, fill, fill));
        FillRect(mem, &rc, br);
        DeleteObject(br);
        DrawIconEx(mem, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL);
        memcpy(px.data(), bits, px.size());
        SelectObject(mem, old);
        DeleteDC(mem);
        DeleteObject(dib);
        return true;
    };

    std::vector<BYTE> onBlack, onWhite;
    bool ok = grab(0x00, onBlack) && grab(0xFF, onWhite);
    ReleaseDC(nullptr, screen);
    DestroyIcon(icon);
    if (!ok) return false;

    out.resize(onBlack.size());
    for (size_t i = 0; i < out.size(); i += 4) {
        // alpha = white - black (per channel); average for stability
        int a = ((onWhite[i] - onBlack[i]) + (onWhite[i+1] - onBlack[i+1]) +
                 (onWhite[i+2] - onBlack[i+2])) / 3;
        a = clampi(a, 0, 255);
        // color is what we drew on black (already = C*a); premultiply is C*a.
        out[i + 0] = onBlack[i + 0];
        out[i + 1] = onBlack[i + 1];
        out[i + 2] = onBlack[i + 2];
        out[i + 3] = (BYTE)a;
    }
    w = h = size;
    return true;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// The session: owns the model, every open popup level, and the nested loop.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

struct MenuPopup;   // fwd

struct MenuSession {
    HWND owner = nullptr;
    IContextMenu* cm = nullptr;
    std::wstring filePath;
    float s = 1.0f;
    Metrics mt;

    std::vector<CtxItem> model;          // root items
    std::vector<MenuPopup*> popups;      // open levels, index 0 = root
    bool closed = false;
    int result = 0;

    CComPtr<IDWriteFactory> dw;
    CComPtr<IDWriteTextFormat> fmt;
    CComPtr<IDWriteTextFormat> fmtSemi;   // semibold, for the default verb

    bool Build(HMENU menu);
    void CloseAll() { closed = true; }
    void Select(UINT id) { result = (int)id; closed = true; }
    MenuPopup* PopupAt(HWND h) const;
    void CloseDeeperThan(int depth);
    int RunLoop();
};

struct MenuPopup {
    MenuSession* sess = nullptr;
    HWND hwnd = nullptr;
    const std::vector<CtxItem>* items = nullptr;
    int depth = 0;
    MenuPopup* parent = nullptr;
    int parentItem = -1;

    int x = 0, y = 0, w = 0, h = 0;
    int shadow = 0;                    // soft drop-shadow margin around the
                                       // surface; the layered window spans
                                       // surface + this margin on every side
    int hover = -1;
    int scroll = 0;
    int contentH = 0;
    std::vector<float> rowY;             // top y of each item (content space)
    std::vector<float> rowH;             // height of each item
    UINT_PTR openTimer = 0;              // delayed flyout-open
    int pendingSub = -1;
    bool anySub = false;                 // some row carries a chevron
    bool pressed = false;                // left button held (drag-select)
    bool tracking = false;               // TME_LEAVE tracking active

    std::unique_ptr<RenderContext> render;
    std::vector<CComPtr<ID2D1Bitmap>> icons;
    std::vector<CComPtr<IDWriteTextLayout>> texts;   // one per row (null = separator)
    CComPtr<ID2D1SolidColorBrush> brTint, brBorder, brHi, brText, brDim,
                                  brHover, brPressed, brSep, brChev, brCheck,
                                  brBadge, brShadow;
    CComPtr<ID2D1BitmapBrush> backBrush;   // fallback if no backdrop

    bool Create(MenuSession* se, const std::vector<CtxItem>* it, int dep,
                MenuPopup* par, int parItem, POINT at);
    void Layout();
    void BuildIcons();
    void MakeBrushes();
    void CaptureBackdrop();
    void Repaint();
    void DrawMenu();
    int  HitTest(int mx, int my) const;
    void SetHover(int idx);
    void TryOpenSub(int idx, bool immediate);
    void OpenSub(int idx);
    void CancelPendingSub();
    MenuPopup* RoutePoint(int& mx, int& my);
    void OnMouse(int mx, int my);
    void OnLeave();
    void OnClick(int mx, int my);
    void OnWheel(int delta);
    void OnKey(UINT vk);
    void EnsureVisible(int idx);

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
};

// ── Walk the HMENU into the model ──

static void ForwardInitPopup(IContextMenu* cm, HMENU m) {
    // Handlers populate lazy text/icons on WM_INITMENUPOPUP (Explorer does the
    // same). Forward for this popup and all nested submenus.
    CComPtr<IContextMenu3> cm3;
    CComPtr<IContextMenu2> cm2;
    cm->QueryInterface(IID_PPV_ARGS(&cm3));
    cm->QueryInterface(IID_PPV_ARGS(&cm2));
    std::function<void(HMENU)> walk = [&](HMENU pop) {
        LRESULT lr = 0;
        if (cm3)      cm3->HandleMenuMsg2(WM_INITMENUPOPUP, (WPARAM)pop, 0, &lr);
        else if (cm2) cm2->HandleMenuMsg(WM_INITMENUPOPUP, (WPARAM)pop, 0);
        int n = GetMenuItemCount(pop);
        for (int i = 0; i < n; i++) {
            MENUITEMINFOW mii = { sizeof(mii) };
            mii.fMask = MIIM_SUBMENU;
            if (GetMenuItemInfoW(pop, i, TRUE, &mii) && mii.hSubMenu)
                walk(mii.hSubMenu);
        }
    };
    walk(m);
}

static void WalkMenu(HMENU m, std::vector<CtxItem>& out,
                     const std::wstring& filePath, int depth) {
    int n = GetMenuItemCount(m);
    for (int i = 0; i < n; i++) {
        MENUITEMINFOW mii = { sizeof(mii) };
        mii.fMask = MIIM_ID | MIIM_FTYPE | MIIM_STATE | MIIM_BITMAP |
                    MIIM_SUBMENU | MIIM_STRING;
        wchar_t text[256] = {};
        mii.dwTypeData = text; mii.cch = 256;
        if (!GetMenuItemInfoW(m, i, TRUE, &mii)) continue;

        CtxItem it;
        if (mii.fType & MFT_SEPARATOR) { it.sep = true; out.push_back(std::move(it)); continue; }

        it.text = text;
        it.id = mii.wID;
        it.disabled = (mii.fState & (MFS_DISABLED | MFS_GRAYED)) != 0;
        it.checked = (mii.fState & MFS_CHECKED) != 0;
        it.isDefault = (mii.fState & MFS_DEFAULT) != 0;

        // Icon: prefer the item bitmap; fall back to the file-type icon for
        // the default (Open) verb so the top item looks like Win11.
        if (mii.hbmpItem && mii.hbmpItem != HBMMENU_CALLBACK) {
            ExtractBitmapPixels(mii.hbmpItem, it.iconPx, it.iconW, it.iconH);
        }
        if (it.iconPx.empty() && it.isDefault && !filePath.empty()) {
            int isz = GetSystemMetrics(SM_CXSMICON);
            ExtractFileIconPixels(filePath, isz ? isz : 16, it.iconPx, it.iconW, it.iconH);
        }

        if (mii.hSubMenu) {
            it.hasSub = true;
            if (depth < 4) WalkMenu(mii.hSubMenu, it.children, filePath, depth + 1);
            if (it.children.empty()) it.hasSub = false;
        }

        out.push_back(std::move(it));
    }
}

bool MenuSession::Build(HMENU menu) {
    // Only shell verb menus answer WM_INITMENUPOPUP; plain HMENUs (tray,
    // fence title bar) arrive fully populated.
    if (cm) ForwardInitPopup(cm, menu);
    WalkMenu(menu, model, filePath, 0);
    // Collapse duplicate separators and trim leading/trailing ones.
    std::vector<CtxItem> clean;
    clean.reserve(model.size());
    for (auto& it : model) {
        if (it.sep && (clean.empty() || clean.back().sep)) continue;
        clean.push_back(std::move(it));
    }
    while (!clean.empty() && clean.back().sep) clean.pop_back();
    model = std::move(clean);
    return !model.empty();
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Popup window: one layered Direct2D window per open menu level.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

constexpr UINT_PTR kSubTimerId      = 1;
constexpr UINT     kSubOpenDelayMs  = 220;   // hover delay before a flyout opens
constexpr UINT     WM_MENUDISMISS   = WM_USER + 1;

// One WH_MOUSE_LL hook shared by every menu session; installed while any menu
// can be open, removed when the last one closes.  The instance count handles
// re-entrant sessions too (though a guard in Run now prevents stacking via
// FluentMenu — the counter still protects against the classic TrackPopupMenu
// fallback that nests).
static HHOOK  s_llHook = nullptr;
static LONG   s_hookInstances = 0;

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Messages that start a click somewhere (client + non-client area).
static bool IsButtonDownMsg(UINT m) {
    switch (m) {
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDBLCLK: case WM_XBUTTONDBLCLK:
    case WM_NCLBUTTONDOWN: case WM_NCRBUTTONDOWN: case WM_NCMBUTTONDOWN:
    case WM_NCXBUTTONDOWN: case WM_NCLBUTTONDBLCLK: case WM_NCRBUTTONDBLCLK:
    case WM_NCMBUTTONDBLCLK: case WM_NCXBUTTONDBLCLK:
        return true;
    }
    return false;
}

// Low-level mouse hook: any button-down that lands OUTSIDE every open
// FluentMenu window dismisses the session(s) and swallows the click so it
// cannot start a fence drag / marquee selection / app activation shift.
// This works regardless of foreground status — unlike WM_ACTIVATE, which
// only fires when the focus actually changes (a desktop click often does
// not change it, and tray icon right-clicks don't reliably make us
// foreground in the first place).
static LRESULT CALLBACK LLMouseProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && IsButtonDownMsg((UINT)wp)) {
        auto* ms = (const MSLLHOOKSTRUCT*)lp;
        POINT pt = ms->pt;
        bool inside = false;
        HWND m = nullptr;
        while ((m = FindWindowExW(nullptr, m, L"OpenFencesFluentMenu", nullptr)) != nullptr) {
            RECT r{}; GetWindowRect(m, &r);
            if (PtInRect(&r, pt)) { inside = true; break; }
        }
        if (!inside) {
            m = nullptr;
            while ((m = FindWindowExW(nullptr, m, L"OpenFencesFluentMenu", nullptr)) != nullptr)
                PostMessageW(m, WM_MENUDISMISS, 0, 0);
            return 1;   // swallow the dismissal click
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

static void AcquireMenuHook() {
    if (InterlockedIncrement(&s_hookInstances) == 1)
        s_llHook = SetWindowsHookExW(WH_MOUSE_LL, LLMouseProc,
                                     GetModuleHandleW(nullptr), 0);
}
static void ReleaseMenuHook() {
    if (InterlockedDecrement(&s_hookInstances) == 0) {
        if (s_llHook) { UnhookWindowsHookEx(s_llHook); s_llHook = nullptr; }
    }
}

} // namespace

// ── Popup creation & layout ──

bool MenuPopup::Create(MenuSession* se, const std::vector<CtxItem>* it, int dep,
                       MenuPopup* par, int parItemIdx, POINT at) {
    sess = se;
    items = it;
    depth = dep;
    parent = par;
    parentItem = parItemIdx;
    if (!items || items->empty()) return false;

    static bool clsReg = false;
    if (!clsReg) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"OpenFencesFluentMenu";
        if (!RegisterClassExW(&wc)) return false;
        clsReg = true;
    }

    Layout();                        // → w, rowY/rowH, contentH, text layouts

    // Soft drop shadow: the surface floats on a translucent margin drawn as
    // fading concentric strokes (see DrawMenu). The layered window is
    // enlarged by this margin on every side, but ALL placement math below
    // stays in surface coordinates — clamping, flipping and flyout
    // alignment are unchanged; the window simply hangs `shadow` pixels
    // past the surface (legal for ULW; Win11's own menus do the same).
    shadow = (int)(12 * sess->s + 0.5f);

    float s = sess->s;
    HMONITOR mon = MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);

    // Taller than the work area → clamp the height; the wheel scrolls it.
    int maxH = (mi.rcWork.bottom - mi.rcWork.top) - (int)(16 * s);
    if (maxH < (int)(60 * s)) return false;
    h = (std::min)(contentH, maxH);

    // The layered window hangs `shadow` px past the surface on every side
    // (see DrawMenu); keep the entire window inside the work area so the
    // soft drop-shadow is not clipped by the taskbar or display edge.
    const int S = shadow;
    int winW = w + 2 * S, winH = h + 2 * S;

    x = at.x;
    y = at.y;
    if (depth == 0) {
        // Root: clamp to the work area, flipping above the click when needed.
        if (y + winH > mi.rcWork.bottom) y = at.y - h;
        if (x + winW > mi.rcWork.right)  x = mi.rcWork.right - w - S;
        x = clampi(x, mi.rcWork.left + S,
                   (std::max)(mi.rcWork.left + S, mi.rcWork.right - winW + S));
        y = clampi(y, mi.rcWork.top + S,
                   (std::max)(mi.rcWork.top + S,  mi.rcWork.bottom - winH + S));
    } else {
        // Flyout: `at` sits on the parent's right edge; flip left if clipped.
        if (x + winW > mi.rcWork.right && parent)
            x = parent->x - w + (int)(4 * s);
        x = clampi(x, mi.rcWork.left + S,
                   (std::max)(mi.rcWork.left + S, mi.rcWork.right - winW + S));
        y = clampi(y, mi.rcWork.top + S,
                   (std::max)(mi.rcWork.top + S,  mi.rcWork.bottom - winH + S));
    }

    render = std::make_unique<RenderContext>((UINT)(w + 2 * shadow),
                                             (UINT)(h + 2 * shadow));
    if (!render->Target()) return false;
    MakeBrushes();
    BuildIcons();

    hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        L"OpenFencesFluentMenu", L"", WS_POPUP,
        x - shadow, y - shadow, w + 2 * shadow, h + 2 * shadow,
        nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd) return false;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)this);

    CaptureBackdrop();

    // Draw + Present before ShowWindow so the first visible frame is clean.
    Repaint();
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    // Stay above every normal window (including the taskbar — the geometry
    // clamp already prevents overlap) so no part of the menu is ever
    // obscured.  The LL hook dismisses on the first outside click, so the
    // topmost flag does not outstay its welcome.
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (depth == 0) SetForegroundWindow(hwnd);   // flyouts never steal activation
    SetFocus(hwnd);
    return true;
}

void MenuPopup::Layout() {
    const Metrics& mt = sess->mt;
    float s = sess->s;

    anySub = false;
    for (const auto& it : *items)
        if (it.hasSub) { anySub = true; break; }

    // Measure every label; the widest one sets the text column.
    texts.assign(items->size(), nullptr);
    float maxText = 0.0f;
    for (size_t i = 0; i < items->size(); i++) {
        const CtxItem& it = (*items)[i];
        if (it.sep || it.text.empty()) continue;
        CComPtr<IDWriteTextLayout> tl;
        if (FAILED(sess->dw->CreateTextLayout(it.text.c_str(), (UINT32)it.text.size(),
                it.isDefault ? sess->fmtSemi.p : sess->fmt.p,
                mt.maxTextW * s, mt.rowH * s * 2.0f, &tl)) || !tl)
            continue;
        tl->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TEXT_METRICS m = {};
        tl->GetMetrics(&m);
        if (m.width > maxText) maxText = m.width;
        texts[i] = tl;
    }

    float need = mt.padH * s * 2.0f + mt.iconCol * s + mt.gap * s + maxText
               + (anySub ? mt.chevr * s : 0.0f);
    w = (int)std::ceil(clampf(need, mt.minW * s, mt.maxW * s));

    // Re-clamp the layouts to the final text column so over-long labels are
    // trimmed at the column edge instead of overflowing the surface.
    float textCol = (float)w - mt.padH * s * 2.0f - mt.iconCol * s - mt.gap * s
                    - (anySub ? mt.chevr * s : 0.0f);
    DWRITE_TRIMMING trim = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
    for (auto& tl : texts) {
        if (!tl) continue;
        tl->SetMaxWidth((std::max)(1.0f, textCol));
        tl->SetTrimming(&trim, nullptr);
    }

    // Row geometry in content space (the popup scrolls when height-clamped).
    rowY.clear(); rowH.clear();
    rowY.reserve(items->size()); rowH.reserve(items->size());
    float y = mt.padV * s;
    for (const auto& it : *items) {
        rowY.push_back(y);
        float rh = (it.sep ? mt.sepH : mt.rowH) * s;
        rowH.push_back(rh);
        y += rh;
    }
    contentH = (int)std::ceil(y + mt.padV * s);
    scroll = 0;
}

void MenuPopup::BuildIcons() {
    icons.assign(items->size(), nullptr);
    auto* t = render ? render->Target() : nullptr;
    if (!t) return;
    D2D1_BITMAP_PROPERTIES bp = {};
    bp.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    bp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    bp.dpiX = bp.dpiY = 96.0f;
    for (size_t i = 0; i < items->size(); i++) {
        const CtxItem& it = (*items)[i];
        if (it.iconPx.empty() || it.iconW <= 0 || it.iconH <= 0) continue;
        t->CreateBitmap(D2D1::SizeU((UINT32)it.iconW, (UINT32)it.iconH),
                        it.iconPx.data(), (UINT32)it.iconW * 4, bp, &icons[i]);
    }
}

void MenuPopup::MakeBrushes() {
    auto* t = render->Target();
    if (!t) return;
    auto mk = [&](float r, float g, float b, float a, ID2D1SolidColorBrush** out) {
        t->CreateSolidColorBrush(D2D1::ColorF(r, g, b, a), out);
    };
    mk(0.10f, 0.10f, 0.12f, 0.85f, &brTint);    // surface tint over the backdrop
    mk(1, 1, 1, 0.08f, &brBorder);
    mk(1, 1, 1, 0.05f, &brHi);                  // top edge highlight
    mk(1, 1, 1, 0.93f, &brText);
    mk(1, 1, 1, 0.36f, &brDim);                 // disabled entries
    mk(1, 1, 1, 0.06f, &brHover);               // hover plate
    mk(0, 0, 0, 0.10f, &brPressed);             // pressed plate: the hover
                                                // darkens while the button is
                                                // held, so the click has
                                                // visible feedback before the
                                                // item fires
    mk(1, 1, 1, 0.08f, &brSep);
    mk(1, 1, 1, 0.60f, &brChev);
    mk(1, 1, 1, 0.90f, &brCheck);
    // Checked-state badge over a glyph bitmap: Windows accent blue, the same
    // hue the system uses for on-toggles.
    mk(0.0f, 0.47f, 0.83f, 1.0f, &brBadge);
    // Shadow bands share one brush; DrawMenu retunes its alpha per band
    // (the redraw path must not allocate).
    mk(0, 0, 0, 1.0f, &brShadow);

    // Fallback surface when no backdrop could be captured: an opaque dark
    // color expressed as a 1×1 bitmap brush (the only "solid" brush we have).
    static const BYTE px[4] = { 24, 24, 28, 255 };   // BGRA, premultiplied
    D2D1_BITMAP_PROPERTIES bp = {};
    bp.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    bp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    bp.dpiX = bp.dpiY = 96.0f;
    CComPtr<ID2D1Bitmap> b1;
    if (SUCCEEDED(t->CreateBitmap(D2D1::SizeU(1, 1), px, 4, bp, &b1)))
        t->CreateBitmapBrush(b1, D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP),
                             &backBrush);
}

void MenuPopup::CaptureBackdrop() {
    if (!render || w <= 0 || h <= 0) return;
    // Same trick as FenceWindow/SettingsPanel: layered windows are excluded
    // from GDI screen captures, so the popup never lands in its own backdrop.
    // The capture spans surface + shadow margin so the backdrop brush is
    // anchored at the window's top-left (DrawMenu relies on that).
    const int S = shadow;
    const int bw = w + 2 * S, bh = h + 2 * S;
    HDC screenDC = GetDC(nullptr);
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = bw;
    bmi.bmiHeader.biHeight = -bh;                // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib) {
        HDC memDC = CreateCompatibleDC(screenDC);
        HGDIOBJ oldBmp = SelectObject(memDC, dib);
        BitBlt(memDC, 0, 0, bw, bh, screenDC, x - S, y - S, SRCCOPY);
        if (bits) render->SetBackdrop((const BYTE*)bits, bw, bh);
        SelectObject(memDC, oldBmp);
        DeleteDC(memDC);
        DeleteObject(dib);
    }
    ReleaseDC(nullptr, screenDC);
}

// ── Drawing ──

void MenuPopup::Repaint() {
    if (!render || !hwnd) return;
    DrawMenu();
    render->Present(hwnd, x - shadow, y - shadow);
}

void MenuPopup::DrawMenu() {
    auto* t = render ? render->Target() : nullptr;
    if (!t) return;
    render->BeginDraw();

    const Metrics& mt = sess->mt;
    float s = sess->s;
    float W = (float)w, H = (float)h;
    const float S = (float)shadow;

    t->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    // Soft drop shadow: concentric rounded strokes fading outward, drawn
    // first at identity transform; the surface fill below covers their
    // inner overlap. brShadow is cached — only its alpha changes per band.
    if (brShadow && shadow > 0) {
        const int kBandCount = 10;
        const float band = S / kBandCount;
        for (int k = 0; k < kBandCount; k++) {
            float f = 1.0f - (float)k / kBandCount;
            brShadow->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, f * f * 0.12f));
            float off = (float)(k + 1) * band;
            D2D1_ROUNDED_RECT ring = { {S - off, S - off, S + W + off, S + H + off},
                                       mt.radius * s + off, mt.radius * s + off };
            t->DrawRoundedRectangle(ring, brShadow.p, band * 2.0f);
        }
    }

    // Surface: frosted backdrop + translucent tint, rounded, bordered. The
    // surface sits at +S inside the enlarged window, and the backdrop brush
    // is anchored to the capture origin (the window's top-left) — so the
    // fill MUST run under the identity transform; shifting the transform
    // would shift the sampling by S pixels too.
    D2D1_ROUNDED_RECT surface = { {S, S, S + W, S + H}, mt.radius * s, mt.radius * s };
    if (render->HasBackdrop())     t->FillRoundedRectangle(surface, render->BackdropBrush());
    else if (backBrush)            t->FillRoundedRectangle(surface, backBrush.p);

    // Everything below draws in surface coordinates.
    t->SetTransform(D2D1::Matrix3x2F::Translation(S, S));

    t->FillRoundedRectangle({ {0.0f, 0.0f, W, H}, mt.radius * s, mt.radius * s }, brTint.p);
    t->FillRectangle(D2D1_RECT_F{ 1.0f, 1.0f, W - 1.0f, 1.0f + 1.0f * s }, brHi.p);
    t->DrawRoundedRectangle({ {0.0f, 0.0f, W, H}, mt.radius * s, mt.radius * s }, brBorder.p, 1.0f);

    // Rows (clipped to the surface; scrolled rows slide under the padding).
    t->PushAxisAlignedClip(D2D1_RECT_F{ 1.0f, 1.0f, W - 1.0f, H - 1.0f },
                           D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    float padH  = mt.padH * s;
    float textX = padH + mt.iconCol * s + mt.gap * s;
    float iconSz = mt.iconSz * s;

    for (size_t i = 0; i < items->size(); i++) {
        float top = rowY[i] - (float)scroll;
        float rh  = rowH[i];
        if (top + rh <= 0.0f || top >= H) continue;
        const CtxItem& it = (*items)[i];

        if (it.sep) {
            float cy = top + rh * 0.5f;
            t->FillRectangle(D2D1_RECT_F{ padH + 2.0f * s, cy - 0.5f,
                                          W - padH - 2.0f * s, cy + 0.5f }, brSep.p);
            continue;
        }

        // Hover plate; while the button is held it deepens to the pressed
        // plate so the click is acknowledged before the item fires.
        if (hover == (int)i && !it.disabled) {
            t->FillRoundedRectangle(
                D2D1_ROUNDED_RECT{ {padH, top + 1.5f * s, W - padH, top + rh - 1.5f * s},
                                   mt.rowRad * s, mt.rowRad * s },
                pressed ? brPressed.p : brHover.p);
        }

        // Icon gutter: real bitmap when present, check mark for check verbs.
        float ix = padH + (mt.iconCol * s - iconSz) * 0.5f;
        float iy = top + (rh - iconSz) * 0.5f;
        if (i < icons.size() && icons[i]) {
            t->DrawBitmap(icons[i].p, D2D1_RECT_F{ ix, iy, ix + iconSz, iy + iconSz },
                          it.disabled ? 0.4f : 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            // A glyphed toggle (tray menu's Start with Windows) cannot use the
            // gutter check mark — the bitmap already fills the column, so on
            // and off would render identically. Pin a small accent badge to
            // the glyph corner while the item is checked instead.
            if (it.checked && !it.disabled) {
                float r  = 4.2f * s;
                float cx = ix + iconSz - r * 0.55f, cy = iy + iconSz - r * 0.55f;
                t->FillEllipse({ {cx, cy}, r, r }, brBadge.p);
                t->DrawLine({ cx - r * 0.42f, cy + r * 0.02f },
                            { cx - r * 0.08f, cy + r * 0.34f },
                            brCheck.p, 1.6f * s);
                t->DrawLine({ cx - r * 0.08f, cy + r * 0.34f },
                            { cx + r * 0.46f, cy - r * 0.30f },
                            brCheck.p, 1.6f * s);
            }
        } else if (it.checked) {
            float cx = padH + mt.iconCol * s * 0.5f, cy = top + rh * 0.5f;
            t->DrawLine({ cx - 4.0f * s, cy },        { cx - 1.2f * s, cy + 2.8f * s },
                        brCheck.p, 1.5f * s);
            t->DrawLine({ cx - 1.2f * s, cy + 2.8f * s }, { cx + 4.2f * s, cy - 2.8f * s },
                        brCheck.p, 1.5f * s);
        }

        // Label (pre-measured layout; over-long text trims at the column edge)
        if (texts[i]) {
            DWRITE_TEXT_METRICS m = {};
            texts[i]->GetMetrics(&m);
            float ty = top + (rh - m.height) * 0.5f;
            t->DrawTextLayout(D2D1_POINT_2F{ textX, ty }, texts[i].p,
                              it.disabled ? brDim.p : brText.p);
        }

        // Submenu chevron
        if (it.hasSub) {
            float cy  = top + rh * 0.5f;
            float cx0 = W - padH - mt.chevr * s * 0.5f - 2.0f * s;
            ID2D1Brush* cb = it.disabled ? brDim.p : brChev.p;
            t->DrawLine({ cx0, cy - 3.5f * s }, { cx0 + 3.5f * s, cy }, cb, 1.3f * s);
            t->DrawLine({ cx0 + 3.5f * s, cy }, { cx0, cy + 3.5f * s }, cb, 1.3f * s);
        }
    }
    t->PopAxisAlignedClip();

    t->SetTransform(D2D1::Matrix3x2F::Identity());
    render->EndDraw();
}

// ── Interaction ──

// Maps a point (given in this popup's surface coordinates) to the popup
// that should handle it. Points inside the surface are handled here.
// Points in the shadow margin that overlap the parent's surface are
// translated into the parent's space and handed to it: a flyout hugs the
// parent's right edge, so its left shadow margin covers the parent's
// chevron column — without this forwarding the parent never sees the
// mouse there and can neither un-highlight its row nor close the flyout.
// Margin points over neither surface stay here (a harmless no-op hover).
MenuPopup* MenuPopup::RoutePoint(int& mx, int& my) {
    if (mx >= 0 && my >= 0 && mx < w && my < h) return this;
    if (parent) {
        int px = mx + (x - parent->x);
        int py = my + (y - parent->y);
        if (px >= 0 && py >= 0 && px < parent->w && py < parent->h) {
            mx = px; my = py;
            return parent;
        }
    }
    return this;
}

int MenuPopup::HitTest(int mx, int my) const {
    float padH = sess->mt.padH * sess->s;
    if (mx < (int)padH || mx > w - (int)padH) return -1;
    float cy = (float)my + (float)scroll;
    for (size_t i = 0; i < rowY.size(); i++)
        if (cy >= rowY[i] && cy < rowY[i] + rowH[i])
            return (*items)[i].sep ? -1 : (int)i;
    return -1;
}

void MenuPopup::SetHover(int idx) {
    if (idx == hover) return;
    hover = idx;
    Repaint();
}

void MenuPopup::TryOpenSub(int idx, bool immediate) {
    if (idx < 0 || idx >= (int)items->size()) return;
    const CtxItem& it = (*items)[idx];
    if (!it.hasSub || it.disabled || it.children.empty()) return;
    if ((int)sess->popups.size() > depth + 1 &&
        sess->popups[depth + 1]->parentItem == idx)
        return;                                  // this row's flyout is already open
    CancelPendingSub();
    if (!immediate) {
        pendingSub = idx;
        openTimer = SetTimer(hwnd, kSubTimerId, kSubOpenDelayMs, nullptr);
        return;
    }
    OpenSub(idx);
}

void MenuPopup::OpenSub(int idx) {
    CancelPendingSub();
    if (idx < 0 || idx >= (int)items->size()) return;
    if ((int)sess->popups.size() > depth + 1) sess->CloseDeeperThan(depth);

    // Align the flyout's first row with this row, hugging the parent edge.
    POINT at;
    at.x = x + w - (int)(4 * sess->s);
    at.y = y + (int)(rowY[idx] - scroll - sess->mt.padV * sess->s);

    auto* child = new MenuPopup();
    if (child->Create(sess, &(*items)[idx].children, depth + 1, this, idx, at))
        sess->popups.push_back(child);
    else
        delete child;
}

void MenuPopup::CancelPendingSub() {
    if (openTimer && hwnd) KillTimer(hwnd, openTimer);
    openTimer = 0;
    pendingSub = -1;
}

void MenuPopup::OnMouse(int mx, int my) {
    if (!tracking) {
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        tracking = true;
    }
    int idx = HitTest(mx, my);
    SetHover(idx);

    // Moving off the row that owns an open flyout closes that flyout — except
    // while the button is held: drag-select stays on this level.
    if (!pressed && (int)sess->popups.size() > depth + 1 &&
        sess->popups[depth + 1]->parentItem != idx) {
        sess->CloseDeeperThan(depth);
    }

    if (pressed) return;                         // no delayed opens mid-drag
    if (idx >= 0 && (*items)[idx].hasSub && !(*items)[idx].disabled) {
        if (pendingSub != idx) { CancelPendingSub(); TryOpenSub(idx, false); }
    } else if (pendingSub >= 0) {
        CancelPendingSub();
    }
}

void MenuPopup::OnLeave() {
    tracking = false;
    CancelPendingSub();
    // Keep the parent row of an open flyout highlighted; otherwise un-hover.
    int keep = -1;
    if ((int)sess->popups.size() > depth + 1 && sess->popups[depth + 1]->parent == this)
        keep = sess->popups[depth + 1]->parentItem;
    SetHover(keep);
}

void MenuPopup::OnClick(int mx, int my) {
    int idx = HitTest(mx, my);
    if (idx < 0) return;
    const CtxItem& it = (*items)[idx];
    if (it.disabled) return;
    SetHover(idx);
    if (it.hasSub) { TryOpenSub(idx, true); return; }
    sess->Select(it.id);
}

void MenuPopup::OnWheel(int delta) {
    if (contentH <= h) return;
    if ((int)sess->popups.size() > depth + 1) sess->CloseDeeperThan(depth);
    float step = sess->mt.rowH * sess->s * 3.0f;
    int ns = clampi(scroll - (int)((delta / (float)WHEEL_DELTA) * step),
                    0, contentH - h);
    if (ns == scroll) return;
    scroll = ns;
    hover = -1;
    Repaint();
}

void MenuPopup::OnKey(UINT vk) {
    int n = (int)items->size();
    auto usable = [&](int i) { return i >= 0 && i < n && !(*items)[i].sep; };

    switch (vk) {
    case VK_DOWN:
    case VK_UP: {
        int dir = (vk == VK_DOWN) ? 1 : -1;
        int i = hover;
        for (int k = 0; k < n; k++) {
            i += dir;
            if (i >= n) i = 0;
            if (i < 0)  i = n - 1;
            if (usable(i)) break;
        }
        if (usable(i)) { SetHover(i); EnsureVisible(i); }
        return;
    }
    case VK_RETURN:
    case VK_RIGHT: {
        if (!usable(hover) || (*items)[hover].disabled) return;
        if (!(*items)[hover].hasSub) {
            if (vk == VK_RETURN) sess->Select((*items)[hover].id);
            return;
        }
        TryOpenSub(hover, true);
        if ((int)sess->popups.size() > depth + 1 &&
            sess->popups[depth + 1]->parentItem == hover) {
            MenuPopup* child = sess->popups[depth + 1];
            SetFocus(child->hwnd);
            for (int c = 0; c < (int)child->items->size(); c++)
                if (!(*child->items)[c].sep) { child->SetHover(c); break; }
        }
        return;
    }
    case VK_ESCAPE:
    case VK_LEFT: {
        if (depth == 0) { sess->CloseAll(); return; }
        // Copy what we need first: CloseDeeperThan destroys `this`.
        MenuPopup* par = parent;
        int parIdx = parentItem;
        sess->CloseDeeperThan(depth - 1);
        if (par && par->hwnd) {
            SetFocus(par->hwnd);
            par->SetHover(parIdx);
        }
        return;
    }
    }
}

void MenuPopup::EnsureVisible(int idx) {
    if (idx < 0 || idx >= (int)rowY.size() || contentH <= h) return;
    int ns = scroll;
    float top = rowY[idx], bot = top + rowH[idx];
    if (top < (float)ns)            ns = (int)top;
    else if (bot > (float)(ns + h)) ns = (int)std::ceil(bot - h);
    ns = clampi(ns, 0, contentH - h);
    if (ns == scroll) return;
    scroll = ns;
    Repaint();
}

LRESULT CALLBACK MenuPopup::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = (MenuPopup*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_MENUDISMISS:
        if (self && self->sess) self->sess->CloseAll();
        return 0;
    case WM_CREATE: {
        auto* cs = (CREATESTRUCTW*)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (self) {
            // Window origin sits `shadow` px outside the surface.
            int mx = GET_X_LPARAM(lp) - self->shadow;
            int my = GET_Y_LPARAM(lp) - self->shadow;
            MenuPopup* t = self->RoutePoint(mx, my);
            if (t != self) self->SetHover(-1);   // cursor left the surface
            t->OnMouse(mx, my);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (self) self->OnLeave();
        return 0;
    case WM_LBUTTONDOWN: {
        if (self) {
            int mx = GET_X_LPARAM(lp) - self->shadow;
            int my = GET_Y_LPARAM(lp) - self->shadow;
            MenuPopup* t = self->RoutePoint(mx, my);
            // A press in the shadow margin that no surface owns is
            // visually a press OUTSIDE the menu → dismiss, like the
            // classic menus. (The session loop only sees presses that
            // miss every popup window; this one hit ours.)
            if (t == self && (mx < 0 || my < 0 || mx >= self->w || my >= self->h)) {
                self->sess->CloseAll();
                return 0;
            }
            t->pressed = true;
            SetCapture(hwnd);
            t->OnMouse(mx, my);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (self) {
            int mx = GET_X_LPARAM(lp) - self->shadow;
            int my = GET_Y_LPARAM(lp) - self->shadow;
            MenuPopup* t = self->RoutePoint(mx, my);
            t->pressed = false;
            if (GetCapture() == hwnd) ReleaseCapture();
            // A press that started on a row and was released out in the
            // margin = released outside the menu → cancel (no selection).
            if (t == self && (mx < 0 || my < 0 || mx >= self->w || my >= self->h)) {
                self->sess->CloseAll();
                return 0;
            }
            t->OnClick(mx, my);
        }
        return 0;
    }
    case WM_RBUTTONUP: {
        // TPM_RIGHTBUTTON parity: a right-release on a row also picks the
        // item (the legacy call sites both track with TPM_RIGHTBUTTON); a
        // release out in the unowned margin dismisses like a right-click
        // outside any system menu.
        if (self) {
            int mx = GET_X_LPARAM(lp) - self->shadow;
            int my = GET_Y_LPARAM(lp) - self->shadow;
            MenuPopup* t = self->RoutePoint(mx, my);
            if (t == self && (mx < 0 || my < 0 || mx >= self->w || my >= self->h)) {
                self->sess->CloseAll();
                return 0;
            }
            t->OnClick(mx, my);
        }
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (self) self->OnWheel(GET_WHEEL_DELTA_WPARAM(wp));
        return 0;
    case WM_KEYDOWN:
        if (self) self->OnKey((UINT)wp);   // may destroy `self` (Esc on flyout)
        return 0;
    case WM_TIMER:
        if (self && wp == self->openTimer && self->pendingSub >= 0)
            self->OpenSub(self->pendingSub);
        return 0;
    case WM_DESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        if (self) { self->hwnd = nullptr; self->openTimer = 0; }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── Session ──

MenuPopup* MenuSession::PopupAt(HWND h) const {
    if (!h) return nullptr;
    for (auto* p : popups)
        if (p && p->hwnd == h) return p;
    return nullptr;
}

void MenuSession::CloseDeeperThan(int depth) {
    while ((int)popups.size() > depth + 1) {
        MenuPopup* p = popups.back();
        popups.pop_back();
        if (p->hwnd) DestroyWindow(p->hwnd);
        delete p;
    }
}

int MenuSession::RunLoop() {
    AcquireMenuHook();
    MSG msg;
    while (!closed) {
        BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got == 0) {                        // WM_QUIT: re-post and bail out
            PostQuitMessage((int)msg.wParam);
            break;
        }
        if (got == -1) continue;

        MenuPopup* p = PopupAt(msg.hwnd);

        // A click starting outside every menu level dismisses the session and
        // is swallowed so it cannot leak through to a fence (drag / menu).
        if (IsButtonDownMsg(msg.message) && !p) { closed = true; break; }

        // Another application taking activation dismisses the menu too.
        if (msg.message == WM_ACTIVATE && LOWORD(msg.wParam) == WA_INACTIVE && p) {
            closed = true; break;
        }

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    ReleaseMenuHook();
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Entry point
// ─────────────────────────────────────────────────────────────────────────────

int FluentMenu::Run(HWND owner, HMENU menu, POINT ptScreen) {
    // Plain-HMENU entry point (no shell verb list): delegates to the full
    // pipeline with a null IContextMenu.
    return Run(owner, nullptr, menu, std::wstring(), ptScreen);
}

// Guard against re-entrant FluentMenu::Run calls: a right-click on a fence or
// tray icon while a menu is already open would nest a second session on top,
// creating a persistent stack of unclosable popups.  Fall back to the classic
// menu (TrackPopupMenu) instead.
static LONG s_flRunDepth = 0;

int FluentMenu::Run(HWND owner, IContextMenu* cm, HMENU menu,
                    const std::wstring& filePath, POINT ptScreen) {
    if (!menu) return -1;
    if (InterlockedIncrement(&s_flRunDepth) > 1) {
        InterlockedDecrement(&s_flRunDepth);
        return -1;   // already showing a Fluent menu on this thread
    }

    MenuSession sess;
    sess.owner = owner;
    sess.cm = cm;
    sess.filePath = filePath;
    sess.s = DpiScale();

    // Text formats shared by every popup level.
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory), (IUnknown**)&sess.dw)) || !sess.dw)
        return -1;
    float fontPx = sess.mt.fontPx * sess.s;
    sess.dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fontPx, L"en-US",
        &sess.fmt);
    sess.dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fontPx, L"en-US",
        &sess.fmtSemi);
    if (!sess.fmt) return -1;
    if (!sess.fmtSemi) sess.fmtSemi = sess.fmt;

    if (!sess.Build(menu)) return -1;          // nothing to show → classic fallback

    bool shown = false;
    auto* root = new MenuPopup();
    if (root->Create(&sess, &sess.model, 0, nullptr, -1, ptScreen)) {
        sess.popups.push_back(root);
        shown = true;
        sess.RunLoop();
    }

    // Tear down every level (deepest first), then the objects.
    while (!sess.popups.empty()) {
        MenuPopup* p = sess.popups.back();
        sess.popups.pop_back();
        if (p->hwnd) DestroyWindow(p->hwnd);
        delete p;
    }

    int ret = shown ? sess.result : -1;
    InterlockedDecrement(&s_flRunDepth);
    return ret;
}
