#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlguid.h>
#include <commctrl.h>
#include "render.h"
#include "fence_window.h"
#include "context_menu.h"
#include "menu_icons.h"
#include "config.h"
#include "zoom_overlay.h"
#include "resource.h"
#include <vector>
#include <memory>
#include <algorithm>

struct FenceRect { int x, y, w, h; };
// Not file-static: config.cpp serializes / rebuilds them (see Config::SaveNow
// / Config::LoadApp).
std::vector<std::unique_ptr<FenceWindow>> g_fences;
static HWND g_owner = nullptr;
static UINT g_taskbarCreated = 0;   // broadcast when explorer (re)starts

// ── DPI 感知 ──
// Without this, Windows virtualizes the process at 96 DPI on scaled displays
// and bitmap-upscales every window we present — the classic cause of blurry
// icons/text at 125%/150% scaling. Must run before any window is created.
//
// Prefer Per-Monitor V2: that is the awareness context explorer's Progman
// runs with, and CreateWindowExW refuses to create a WS_CHILD whose context
// differs from the parent's — fence windows are Progman children, so we must
// match it. (DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4.)
static void EnableDpiAwareness() {
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
        typedef BOOL (WINAPI* SetCtxT)(HANDLE);
        auto setCtx = (SetCtxT)(void*)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
        if (setCtx && setCtx((HANDLE)-4 /* PER_MONITOR_AWARE_V2 */)) return;
    }
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        typedef HRESULT (WINAPI* SetDpiAwarenessT)(int);
        auto setAwareness = (SetDpiAwarenessT)(void*)GetProcAddress(shcore, "SetProcessDpiAwareness");
        HRESULT hr = setAwareness ? setAwareness(2 /* PROCESS_SYSTEM_DPI_AWARE */) : E_FAIL;
        FreeLibrary(shcore);
        if (SUCCEEDED(hr)) return;
    }
    SetProcessDPIAware();
}

// ── 桌面图标约束 ──
// LVM_* message constants come from <commctrl.h>

HWND FindDesktopLV() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman) return nullptr;
    HWND defview = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
    if (!defview) return nullptr;
    return FindWindowExW(defview, nullptr, L"SysListView32", nullptr);
}

// Move a fence to a new parent without changing its screen position. Used to
// park fences on SHELLDLL_DefView while their usual parent (SysListView32) is
// hidden — see ToggleDesktopIcons. SetParent across the process boundary to
// explorer-owned parents is supported (the fences already cross it at birth),
// but only for windows that are children; a popup-fallback fence is left alone.
// HWND_TOP keeps the fence above its new siblings, matching the way fences
// sit above the desktop icons under the listview.
static void ReparentFence(HWND fence, HWND newParent) {
    if (!fence || !IsWindow(fence) || !newParent) return;
    if (!(GetWindowLongW(fence, GWL_STYLE) & WS_CHILD)) return;
    if (GetParent(fence) == newParent) return;
    RECT rc;
    if (!GetWindowRect(fence, &rc)) return;
    if (!SetParent(fence, newParent)) return;
    POINT p{ rc.left, rc.top };
    ScreenToClient(newParent, &p);
    SetWindowPos(fence, HWND_TOP, p.x, p.y, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOACTIVATE);
}

void ScanAndConstrainIcons() {
    HWND lv = FindDesktopLV();
    if (!lv) return;
    int count = (int)SendMessageW(lv, LVM_GETITEMCOUNT, 0, 0);
    if (count <= 0 || count > 2000) return;

    std::vector<FenceRect> rects;
    for (auto& f : g_fences) {
        FenceData d = f->GetData();
        rects.push_back({ d.x, d.y, d.w, d.h });
    }
    if (rects.empty()) return;

    for (int i = 0; i < count; i++) {
        POINT pt = {};
        SendMessageW(lv, LVM_GETITEMPOSITION, i, (LPARAM)&pt);
        int x = pt.x, y = pt.y;

        for (auto& f : rects) {
            if (x < f.x - 20 || x > f.x + f.w + 20) continue;
            if (y < f.y - 20 || y > f.y + f.h + 20) continue;

            int cx = x, cy = y;
            if      (cx < f.x)           cx = f.x;
            else if (cx > f.x + f.w - 40) cx = f.x + f.w - 40;
            if      (cy < f.y + 24)      cy = f.y + 24;
            else if (cy > f.y + f.h - 20) cy = f.y + f.h - 20;

            if (cx != x || cy != y) {
                SendMessageW(lv, LVM_SETITEMPOSITION, i, (LPARAM)MAKELPARAM(cx, cy));
            }
            break;
        }
    }
}

// ── 托盘 ──
#define WM_TRAYICON (WM_APP + 1)

void AddTrayIcon(HWND hwnd) {
    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd = hwnd; nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP));
    if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"openFences");
    Shell_NotifyIconW(NIM_ADD, &nid);
    // Bust the tray icon cache so a rebuilt exe with a new icon is picked up
    // immediately (Windows keys the cache by executable path).
    SHChangeNotify(SHCNE_UPDATEIMAGE, SHCNF_DWORD,
                   (LPVOID)(DWORD_PTR)GetCurrentProcessId(), nullptr);
}

bool g_allHidden = false;        // extern'd by config.cpp (persisted)
static bool g_deskHidden = false;
// Set by WatchDesktopIconVisibility when EXPLORER hid the icon list (desktop
// right-click → View → Show desktop icons) and the fences were parked on
// SHELLDLL_DefView to survive it. Distinct from g_deskHidden, which means WE
// hid the list via ToggleDesktopIcons.
static bool g_extParked = false;

void ToggleDesktopIcons() {
    HWND lv = FindDesktopLV();
    HWND defview = lv ? GetParent(lv) : nullptr;
    if (!lv || !defview) return;
    // The fences are children of the icon list, so plainly hiding the list
    // would hide them too. Instead park them one level up on SHELLDLL_DefView
    // for as long as the list stays hidden: same desktop tree, same
    // visibility, same hit-testing — only the icon list itself is gone.
    if (!g_deskHidden) {
        for (auto& f : g_fences) ReparentFence(f->Hwnd(), defview);
        ShowWindow(lv, SW_HIDE);
        g_deskHidden = true;
    } else {
        ShowWindow(lv, SW_SHOW);
        for (auto& f : g_fences) ReparentFence(f->Hwnd(), lv);
        g_deskHidden = false;
    }
}

// Explorer's own "Show desktop icons" switch (desktop context menu → View)
// hides SysListView32 from outside our control. The fences are its WS_CHILD
// windows, so they vanish with it; Win32 gives a child no way to opt out of
// an ancestor's visibility. Poll for the external hide and park the fences
// on SHELLDLL_DefView for as long as the list is gone — the same parking
// ToggleDesktopIcons performs when WE hide the icons — then move them back
// when the list returns. (If explorer hides DefView as well there is no
// visible desktop surface left to sit on; the fences simply reappear with
// the tree, so that case needs no special handling.) Called from the 300ms
// owner timer-2, which already watches the icon list; IsWindowVisible is a
// cheap check so the poll costs nothing.
static void WatchDesktopIconVisibility() {
    if (g_deskHidden) return;              // our own toggle owns the parking
    HWND lv = FindDesktopLV();
    if (!lv) return;
    bool shown = IsWindowVisible(lv) != FALSE;
    if (!shown && !g_extParked) {
        HWND defview = GetParent(lv);
        if (!defview) return;
        for (auto& f : g_fences) ReparentFence(f->Hwnd(), defview);
        g_extParked = true;
    } else if (shown && g_extParked) {
        for (auto& f : g_fences) ReparentFence(f->Hwnd(), lv);
        g_extParked = false;
    }
}

static void CreateDesktopShortcut() {
    wchar_t desktop[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desktop)))
        return;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring lnk = std::wstring(desktop) + L"\\openFences.lnk";

    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW, (void**)&sl)))
        return;
    sl->SetPath(exe);
    sl->SetDescription(L"openFences");
    IPersistFile* pf = nullptr;
    if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf))) {
        pf->Save(lnk.c_str(), TRUE);
        pf->Release();
    }
    sl->Release();
}

void ShowTrayMenu(HWND hwnd) {
    POINT pt; GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 100, FenceWindow::Loc(L"New Fence", L"新建围栏"));
    AppendMenuW(menu, MF_STRING, 101, g_allHidden
        ? FenceWindow::Loc(L"Show All Fences", L"显示全部围栏")
        : FenceWindow::Loc(L"Hide All Fences", L"隐藏全部围栏"));
    AppendMenuW(menu, MF_STRING, 103, g_deskHidden
        ? FenceWindow::Loc(L"Show Desktop Icons", L"显示桌面图标")
        : FenceWindow::Loc(L"Hide Desktop Icons", L"隐藏桌面图标"));

    // Language is a global setting, so it lives here (tray) rather than in the
    // per-fence appearance panel.
    HMENU langMenu = CreatePopupMenu();
    AppendMenuW(langMenu, MF_STRING | (FenceWindow::GetLanguage() == 0 ? MF_CHECKED : 0),
        110, L"English");
    AppendMenuW(langMenu, MF_STRING | (FenceWindow::GetLanguage() == 1 ? MF_CHECKED : 0),
        111, L"中文");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)langMenu, FenceWindow::Loc(L"Language", L"语言"));
    AppendMenuW(menu, MF_STRING | (Config::AutoStartEnabled() ? MF_CHECKED : 0),
        112, FenceWindow::Loc(L"Start with Windows", L"开机自启动"));
    AppendMenuW(menu, MF_STRING, 113,
        FenceWindow::Loc(L"Config File Location", L"配置文件位置"));
    AppendMenuW(menu, MF_STRING, 114,
        FenceWindow::Loc(L"Create Desktop Shortcut", L"创建桌面快捷方式"));

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 102, FenceWindow::Loc(L"Exit", L"退出"));

    // Fluent glyph icons, by menu position (0 New Fence, 1 Hide/Show All
    // Fences, 2 Hide/Show Desktop Icons, 3 separator, 4 Language ▸,
    // 5 Start with Windows, 6 Config File Location, 7 separator, 8 Exit).
    // The set owns the HBITMAPs until the menu interaction below has fully
    // returned (MIIM_BITMAP is by reference).
    GlyphBitmapSet glyphs;
    {
        int gsz = MenuGlyphSizePx();
        auto icon = [&](UINT pos, MenuGlyph g) {
            HBITMAP b = glyphs.add(g, gsz);
            if (!b) return;
            MENUITEMINFOW mii = { sizeof(mii) };
            mii.fMask = MIIM_BITMAP;
            mii.hbmpItem = b;
            SetMenuItemInfoW(menu, pos, TRUE, &mii);
        };
        icon(0, MenuGlyph::Plus);
        icon(1, g_allHidden ? MenuGlyph::Eye : MenuGlyph::EyeOff);
        icon(2, MenuGlyph::Grid);
        icon(4, MenuGlyph::Globe);
        icon(5, MenuGlyph::Rocket);
        icon(6, MenuGlyph::Folder);
        icon(8, MenuGlyph::Power);
    }

    SetForegroundWindow(hwnd);
    // Prefer the Win11 Fluent rendering; fall back to the classic menu only
    // if it could not be built (-1). The Fluent loop swallows the click that
    // dismisses the menu — exactly right over the desktop, where a leaked
    // press would start a selection band or icon drag.
    int cmd = FluentMenu::Run(hwnd, menu, pt);
    if (cmd < 0)
        cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);   // also destroys the attached langMenu
    if (cmd == 100) {
        HDC hdc = GetDC(nullptr);
        float s = GetDeviceCaps(hdc, LOGPIXELSX) / 96.0f;
        ReleaseDC(nullptr, hdc);
        FenceData fd{ L"f" + std::to_wstring(g_fences.size()+1),
                      FenceWindow::Loc(L"New Fence", L"新建围栏"),
                      (int)(100*s), (int)(100*s), (int)(300*s), (int)(220*s) };
        g_fences.push_back(std::make_unique<FenceWindow>(fd));
        if (g_deskHidden || g_extParked) {
            // It was created under the hidden icon list; park it with the rest.
            HWND lv = FindDesktopLV();
            if (lv) ReparentFence(g_fences.back()->Hwnd(), GetParent(lv));
        }
        Config::MarkDirty();
    } else if (cmd == 101) {
        g_allHidden = !g_allHidden;
        for (auto& f : g_fences) {
            if (g_allHidden) f->Hide(); else f->Show();
        }
        Config::MarkDirty();
    } else if (cmd == 103) {
        ToggleDesktopIcons();
    } else if (cmd == 110) {
        FenceWindow::SetLanguage(0);   // English
        Config::MarkDirty();
    } else if (cmd == 111) {
        FenceWindow::SetLanguage(1);   // 中文
        Config::MarkDirty();
    } else if (cmd == 112) {
        Config::SetAutoStart(!Config::AutoStartEnabled());
    } else if (cmd == 113) {
        Config::RevealFile();
    } else if (cmd == 114) {
        CreateDesktopShortcut();
    } else if (cmd == 102) {
        Config::SaveNow();   // do not ride on the debounce for a real exit
        NOTIFYICONDATAW nid = { sizeof(NOTIFYICONDATAW), hwnd, 1 };
        Shell_NotifyIconW(NIM_DELETE, &nid);
        PostQuitMessage(0);
    }
}

// An explorer restart tears down the desktop tree, taking our child fences
// with it. Salvage their state from the (now dead) objects and recreate
// them; the constructor re-discovers the fresh desktop host on its own.
static void RebuildFences() {
    // The fences are recreated under the fresh icon list; drop the parked
    // state (the watcher re-parks on the next tick if explorer still hides
    // the list).
    g_extParked = false;
    // The old desktop tree is gone with the shade; drop it before clearing
    // fences so no stale shade/hook survives the rebuild.
    ZoomOverlay::Close();
    FenceWindow::SetTeardown(true);
    struct Saved { FenceData data; std::vector<IconEntry> icons; bool collapsed; };
    std::vector<Saved> saved;
    saved.reserve(g_fences.size());
    for (auto& f : g_fences)
        saved.push_back({ f->GetData(), f->Icons(), f->IsCollapsed() });
    g_fences.clear();   // dtors see the dead HWNDs and skip DestroyWindow
    FenceWindow::SetTeardown(false);
    for (auto& s : saved) {
        auto f = std::make_unique<FenceWindow>(s.data);
        if (!s.data.sourceFolder.empty()) {
            f->SetSortPreset(s.data.sortCol, s.data.sortAsc);
            f->MapToFolder(s.data.sourceFolder);
        } else
            f->SetIcons(s.icons);
        if (s.collapsed) f->ToggleCollapse();
        if (g_allHidden) f->Hide();
        g_fences.push_back(std::move(f));
    }
    // The fresh desktop came back with its icons visible; if the user had
    // them hidden, re-hide them (and park the fences on SHELLDLL_DefView
    // again, exactly like ToggleDesktopIcons does).
    if (g_deskHidden) {
        HWND lv = FindDesktopLV();
        HWND defview = lv ? GetParent(lv) : nullptr;
        if (lv && defview) {
            for (auto& f : g_fences) ReparentFence(f->Hwnd(), defview);
            ShowWindow(lv, SW_HIDE);
        } else {
            g_deskHidden = false;   // no icon list right now; drop stale state
        }
    }
}

LRESULT CALLBACK OwnerWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreated && g_taskbarCreated) { RebuildFences(); return 0; }
    switch (msg) {
    case WM_CREATE: AddTrayIcon(hwnd); return 0;
    case WM_TRAYICON: if (LOWORD(lp) == WM_RBUTTONUP) ShowTrayMenu(hwnd); return 0;
    case WM_HOTKEY:
        if (wp == 1) ToggleDesktopIcons();
        return 0;
    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
        // Resolution / personalization broadcasts only reach top-level
        // windows; the fences are children of explorer's desktop tree, so
        // fan out here. Each fence debounces the actual re-capture on its
        // own timer-2 — without this the frosted glass keeps showing stale
        // wallpaper pixels after a resolution switch.
        for (auto& f : g_fences) f->ScheduleBackdropRefresh();
        return 0;
    case WM_TIMER:
        if (wp == 2) {   // desktop icon-size watcher
            // Keep fences alive across explorer's own icon hide/show.
            WatchDesktopIconVisibility();
            // One live measurement per tick, shared by all fences (the query
            // opens explorer's process — not worth repeating per fence).
            int px = QueryDesktopIconSizePx();
            for (auto& f : g_fences) {
                // A zoom flight that lost its ticker must not freeze the
                // desktop half-open.
                f->ZoomStallCheck();
                // Display topology changes move the desktop icon list; keep
                // every fence at its saved screen position.
                f->RestoreAnchor();
                if (px > 0) f->SyncDesktopIconSize(px);
            }
            return 0;
        }
        if (wp == 3) {   // config save debounce (Config::MarkDirty)
            Config::SaveNow();
            return 0;
        }
        ScanAndConstrainIcons();
        return 0;
    case WM_FENCE_DELETE: {
        HWND target = (HWND)wp;
        auto it = std::find_if(g_fences.begin(), g_fences.end(),
            [target](auto& f) { return f->Hwnd() == target; });
        if (it != g_fences.end()) {
            g_fences.erase(it);
            Config::MarkDirty();
        }
        return 0;
    }
    case WM_DESTROY:
        Config::SaveNow();   // safety net; the tray Exit path saves too
        NOTIFYICONDATAW nid = { sizeof(NOTIFYICONDATAW), hwnd, 1 };
        Shell_NotifyIconW(NIM_DELETE, &nid);
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── Entry ──
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    // 第二个实例会把围栏重复绘制在同一桌面上；命名互斥体保证只有一个实例
    // （进程崩溃时系统自动释放，不会留下死锁）。
    HANDLE singleInstance = CreateMutexW(nullptr, TRUE, L"openFences_SingleInstance");
    if (!singleInstance || GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    EnableDpiAwareness();
    ComInit com;
    InitCommonControls();
    FenceWindow::RegisterClass(hInst);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = OwnerWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"OpenFencesOwner";
    RegisterClassExW(&wc);

    g_owner = CreateWindowExW(0, L"OpenFencesOwner", nullptr, WS_POPUP,
        0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);

    FenceWindow::SetOwner(g_owner);
    RegisterHotKey(g_owner, 1, MOD_CONTROL | MOD_SHIFT, 'H');
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    // Scale default layout to physical pixels (design values are at 96 DPI)
    HDC hdc = GetDC(nullptr);
    float s = GetDeviceCaps(hdc, LOGPIXELSX) / 96.0f;
    ReleaseDC(nullptr, hdc);

    // Restore the persisted layout (language, fences, icons, appearance).
    // Only a missing/unreadable config falls back to the three starter
    // fences — an empty list is a valid saved state (everything deleted).
    if (!Config::LoadApp()) {
        // Default titles follow the startup language (they are fence names,
        // set once at birth — a later language switch does not rename
        // existing fences).
        FenceData fences[] = {
            { L"f1", FenceWindow::Loc(L"Applications", L"应用"),
              (int)(50*s),  (int)(50*s),  (int)(280*s), (int)(280*s) },
            { L"f2", FenceWindow::Loc(L"Documents", L"文档"),
              (int)(370*s), (int)(50*s),  (int)(280*s), (int)(380*s) },
            { L"f3", FenceWindow::Loc(L"Projects", L"项目"),
              (int)(50*s),  (int)(360*s), (int)(280*s), (int)(220*s) },
        };
        for (auto& fd : fences)
            g_fences.push_back(std::make_unique<FenceWindow>(fd));
    }

    // If explorer's icons are hidden right now (we restarted, or the user
    // hid them before launching us), park immediately instead of sitting
    // invisible under the hidden list until the first timer-2 tick.
    WatchDesktopIconVisibility();

    // ScanAndConstrainIcons disabled for now — cross-process SendMessage
    // to explorer's ListView can cause deadlocks.
    // SetTimer(g_owner, 1, 500, nullptr);

    // Follow desktop icon zoom live (Ctrl+wheel / View menu): each tick
    // measures the icon list's live glyph size out of explorer's address
    // space (the registry value is persisted lazily and lags wheel zoom)
    // and every fence re-adopts it. Each fence early-outs unless the size
    // actually changed.
    SetTimer(g_owner, 2, 300, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    ZoomOverlay::Close();
    FenceWindow::SetTeardown(true);
    g_fences.clear();
    return 0;
}
