#include "fence_window.h"
#include "settings_panel.h"
#include "context_menu.h"
#include "menu_icons.h"
#include "config.h"
#include "explorer_sort.h"
#include "zoom_overlay.h"
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>     // IShellFolder, PIDL helpers (shell context menu)
#include <shobjidl.h>   // IContextMenu
#include <shlwapi.h>
#include <commctrl.h>   // TrackMouseEvent (hover leave tracking)
#include <timeapi.h>    // timeBeginPeriod: 1ms timer resolution for 60fps
#pragma comment(lib, "winmm.lib")
#include <cmath>
#include <chrono>
#include <cstdarg>
#include <cstdio>

// App state owned by main.cpp — ZoomHideOthers/ZoomRestoreOthers walk the
// list, and g_allHidden decides whether "the others" may come back at all.
extern std::vector<std::unique_ptr<FenceWindow>> g_fences;
extern bool g_allHidden;

// Phone-folder zoom tuning (see ZoomOpen): animation cadence and the icon
// growth factor applied to the fence's glyph/cell metrics while zoomed.
constexpr UINT  kZoomTimerId    = 6;
constexpr int   kZoomFrameMs    = 8;
constexpr UINT  WM_APP_ZOOM_TICK = WM_APP + 0x51;

constexpr float kZoomMaxScale   = 2.4f; // cap on the computed glyph growth
constexpr int   kZoomDurationMs = 320;  // time-based flight, not frame count

static float EaseInOutCubic(float u) {
    if (u <= 0.0f) return 0.0f;
    if (u >= 1.0f) return 1.0f;
    return (u < 0.5f) ? 4.0f * u * u * u
                      : 1.0f - (-2.0f * u + 2.0f) * (-2.0f * u + 2.0f)
                                   * (-2.0f * u + 2.0f) * 0.5f;
}

HWND FenceWindow::s_owner = nullptr;
int  FenceWindow::s_lang = 1; // 0=EN, 1=ZH — default Chinese
bool FenceWindow::s_teardown = false;

const wchar_t* FenceWindow::Loc(const wchar_t* en, const wchar_t* zh) {
    return s_lang == 1 ? zh : en;
}

// Inset between the fence border and the icon grid (also the free-drag clamp)
static const float kGridPad = 4.0f;

// ── TEMP DEBUG: new-menu diagnostics ──
static void NewDbg(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    FILE* f = _wfopen(L"D:\\person\\openFences\\newmenu_dbg.log", L"a, ccs=UTF-8");
    if (f) { fwprintf(f, L"%s\r\n", buf); fclose(f); }
}
static void NewDbgDumpMenu(HMENU m, int depth) {
    int n = GetMenuItemCount(m);
    for (int i = 0; i < n; i++) {
        MENUITEMINFOW mii = { sizeof(mii) };
        mii.fMask = MIIM_ID | MIIM_FTYPE | MIIM_SUBMENU | MIIM_STRING;
        wchar_t text[256] = {};
        mii.dwTypeData = text; mii.cch = 256;
        if (!GetMenuItemInfoW(m, i, TRUE, &mii)) continue;
        NewDbg(L"  %*s[%d] id=%u type=0x%x sub=%d text=\"%s\"",
               depth * 2, L"", i, mii.wID, mii.fType,
               mii.hSubMenu ? 1 : 0, text);
        if (mii.hSubMenu && depth < 2) NewDbgDumpMenu(mii.hSubMenu, depth + 1);
    }
}

// Process is DPI-aware (see main.cpp), so this returns the real scale factor
static float DpiScale() {
    HDC h = GetDC(nullptr);
    int d = GetDeviceCaps(h, LOGPIXELSX);
    ReleaseDC(nullptr, h);
    return d / 96.0f;
}

// The desktop parent runs on explorer's DPI awareness (Per-Monitor v2 on
// modern Windows, matching our manifest). On the off chance the host context
// differs from ours, Windows refuses to create the child — so for the
// creation call we adopt the host's context and restore ours right after.
#ifndef DPI_AWARENESS_CONTEXT_UNAWARE
DECLARE_HANDLE(DPI_AWARENESS_CONTEXT);
#endif
typedef DPI_AWARENESS_CONTEXT (WINAPI *GetWinDpiCtxT)(HWND);
typedef DPI_AWARENESS_CONTEXT (WINAPI *SetThreadDpiCtxT)(DPI_AWARENESS_CONTEXT);

static DPI_AWARENESS_CONTEXT AdoptHostAwareness(HWND host) {
    if (!host) return nullptr;
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    auto getCtx = (GetWinDpiCtxT)(void*)GetProcAddress(u32, "GetWindowDpiAwarenessContext");
    auto setCtx = (SetThreadDpiCtxT)(void*)GetProcAddress(u32, "SetThreadDpiAwarenessContext");
    if (!getCtx || !setCtx) return nullptr;
    DPI_AWARENESS_CONTEXT hostCtx = getCtx(host);
    return hostCtx ? setCtx(hostCtx) : nullptr;  // nullptr = nothing to restore
}

static void RestoreAwareness(DPI_AWARENESS_CONTEXT prev) {
    if (!prev) return;
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    auto setCtx = (SetThreadDpiCtxT)(void*)GetProcAddress(u32, "SetThreadDpiAwarenessContext");
    if (setCtx) setCtx(prev);
}

HWND FenceWindow::DesktopHost() {
    // On Windows 11 the desktop is one layered surface owned by
    // SHELLDLL_DefView (it paints the wallpaper *and* composites the icon
    // list). Arbitrary children of Progman/WorkerW are never painted into
    // that surface and therefore never appear on screen. The one window
    // whose normal child painting the shell does composite is the desktop
    // icon list (SysListView32), so fences parent themselves to it — they
    // render, and they become part of the desktop tree (immune to Win+D).
    auto listViewOf = [](HWND shell) -> HWND {
        if (!shell) return nullptr;
        for (HWND dv = FindWindowExW(shell, nullptr, L"SHELLDLL_DefView", nullptr); dv;
             dv = FindWindowExW(shell, dv, L"SHELLDLL_DefView", nullptr)) {
            HWND lv = FindWindowExW(dv, nullptr, L"SysListView32", nullptr);
            if (lv) return lv;
        }
        return nullptr;
    };
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman) return nullptr;
    if (HWND lv = listViewOf(progman)) return lv;
    // Wallpaper/slideshow layouts: Progman → WorkerW → SHELLDLL_DefView
    for (HWND ww = FindWindowExW(progman, nullptr, L"WorkerW", nullptr); ww;
         ww = FindWindowExW(progman, ww, L"WorkerW", nullptr))
        if (HWND lv = listViewOf(ww)) return lv;
    // Rare shell states put the WorkerW at the top level
    for (HWND ww = FindWindowExW(nullptr, nullptr, L"WorkerW", nullptr); ww;
         ww = FindWindowExW(nullptr, ww, L"WorkerW", nullptr))
        if (HWND lv = listViewOf(ww)) return lv;
    return progman;   // no icon list (icons disabled?) — best effort
}

// ── OLE Drop Target ──
// DragAcceptFiles only surfaces CF_HDROP (filesystem paths). System icons
// (此电脑, 回收站) are shell namespace objects carried as CFSTR_SHELLIDLIST
// PIDLs. RegisterDragDrop + IDropTarget receives both, and
// SHCreateShellItemArrayFromDataObject resolves them uniformly into IShellItem
// objects from which we extract display names and parse paths.

static UINT s_cfShellIdList = 0;

static bool HasShellDropFormat(IDataObject* pDataObj) {
    FORMATETC fmt = {};
    fmt.dwAspect = DVASPECT_CONTENT;
    fmt.lindex = -1;
    fmt.tymed = TYMED_HGLOBAL;
    fmt.cfFormat = CF_HDROP;
    if (pDataObj->QueryGetData(&fmt) == S_OK) return true;
    if (s_cfShellIdList) {
        fmt.cfFormat = s_cfShellIdList;
        if (pDataObj->QueryGetData(&fmt) == S_OK) return true;
    }
    return false;
}

class FenceDropTarget : public IDropTarget {
    LONG m_ref = 1;
    HWND m_hwnd = nullptr;
    bool m_over = false;
public:
    explicit FenceDropTarget(HWND hwnd) : m_hwnd(hwnd) {}

    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)()  { return InterlockedIncrement(&m_ref); }
    STDMETHOD_(ULONG, Release)() {
        LONG c = InterlockedDecrement(&m_ref);
        if (c == 0) delete this;
        return c;
    }

    STDMETHOD(DragEnter)(IDataObject* pDataObj, DWORD, POINTL, DWORD* pdwEffect) {
        m_over = HasShellDropFormat(pDataObj);
        *pdwEffect = m_over ? DROPEFFECT_MOVE : DROPEFFECT_NONE;
        if (m_over) {
            auto* self = (FenceWindow*)GetWindowLongPtr(m_hwnd, GWLP_USERDATA);
            if (self) self->SetDragOver(true);
        }
        return S_OK;
    }
    STDMETHOD(DragOver)(DWORD, POINTL, DWORD* pdwEffect) {
        *pdwEffect = m_over ? DROPEFFECT_MOVE : DROPEFFECT_NONE;
        return S_OK;
    }
    STDMETHOD(DragLeave)() {
        m_over = false;
        auto* self = (FenceWindow*)GetWindowLongPtr(m_hwnd, GWLP_USERDATA);
        if (self) self->SetDragOver(false);
        return S_OK;
    }
    STDMETHOD(Drop)(IDataObject* pDataObj, DWORD, POINTL pt, DWORD* pdwEffect) {
        m_over = false;
        auto* self = (FenceWindow*)GetWindowLongPtr(m_hwnd, GWLP_USERDATA);
        if (!self) { *pdwEffect = DROPEFFECT_NONE; return S_OK; }
        self->SetDragOver(false);

        CComPtr<IShellItemArray> items;
        if (FAILED(SHCreateShellItemArrayFromDataObject(pDataObj, IID_PPV_ARGS(&items))) || !items) {
            *pdwEffect = DROPEFFECT_NONE;
            return S_OK;
        }

        POINT dropPt = { pt.x, pt.y };
        ScreenToClient(m_hwnd, &dropPt);
        float dx = (float)dropPt.x, dy = (float)dropPt.y;

        DWORD count = 0;
        items->GetCount(&count);
        for (DWORD i = 0; i < count; i++) {
            CComPtr<IShellItem> item;
            if (FAILED(items->GetItemAt(i, &item)) || !item) continue;

            LPWSTR pwszPath = nullptr;
            if (FAILED(item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &pwszPath)) || !pwszPath)
                continue;
            std::wstring parsePath(pwszPath);
            CoTaskMemFree(pwszPath);

            LPWSTR pwszName = nullptr;
            std::wstring displayName;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &pwszName)) && pwszName) {
                displayName = pwszName;
                CoTaskMemFree(pwszName);
            }
            if (displayName.empty()) displayName = parsePath;

            self->AddIcon(displayName, parsePath, dx, dy);
        }

        *pdwEffect = DROPEFFECT_MOVE;
        return S_OK;
    }
};

FenceWindow::FenceWindow(const FenceData& data)
    : m_title(data.title), m_id(data.id), m_x(data.x), m_y(data.y)
{
    m_render = std::make_unique<RenderContext>(data.w, data.h);

    // Fences live *inside* the desktop window tree (see DesktopHost). That
    // is what makes Win+D leave them alone: the shell's "show desktop" sweep
    // only minimizes foreign top-level windows, never the desktop's own
    // children. If the desktop is not up yet (no explorer), fall back to a
    // plain popup; the owner window re-creates us when explorer (re)starts.
    HWND host = DesktopHost();
    POINT pos{ data.x, data.y };
    if (host) ScreenToClient(host, &pos);   // child coords are parent-relative

    DPI_AWARENESS_CONTEXT prevCtx = AdoptHostAwareness(host);
    m_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        ClassName(), data.title.c_str(), host ? WS_CHILD : WS_POPUP,
        pos.x, pos.y, data.w, data.h,
        host, nullptr, GetModuleHandle(nullptr), this);
    RestoreAwareness(prevCtx);

    if (!m_hwnd) return;   // no usable desktop / unexpected failure
    SetWindowLongPtr(m_hwnd, GWLP_USERDATA, (LONG_PTR)this);
    if (host)
        SetWindowPos(m_hwnd, HWND_TOP, 0, 0, 0, 0,   // above the desktop icons
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (!s_cfShellIdList)
        s_cfShellIdList = (UINT)RegisterClipboardFormatW(CFSTR_SHELLIDLIST);
    auto* dt = new FenceDropTarget(m_hwnd);
    if (SUCCEEDED(RegisterDragDrop(m_hwnd, dt)))
        dt->Release();
    else {
        delete dt;
        DragAcceptFiles(m_hwnd, TRUE);   // fallback: filesystem paths only
    }
    ShowWindow(m_hwnd, SW_SHOW);
    Redraw();
    CaptureBackdrop();   // initial frosted-glass snapshot
}

FenceWindow::~FenceWindow() {
    ZoomStopTicker();
    if (m_zoomOpening) timeEndPeriod(1);
    // If the settings panel is open for this fence, tear it down first so it
    // never keeps referencing a fence that is going away.
    SettingsPanel::CloseActiveFor(this);
    StopSourceWatch();
    // When explorer restarts, the desktop tree takes our child window down
    // with it — by the time we run, the HWND is already dead.
    if (!m_hwnd || !IsWindow(m_hwnd)) return;
    if (m_cursorTimer) KillTimer(m_hwnd, 1);
    RevokeDragDrop(m_hwnd);       // no-op if RegisterDragDrop failed
    DragAcceptFiles(m_hwnd, FALSE); // no-op if DragAcceptFiles was never called
    DestroyWindow(m_hwnd);
}

void FenceWindow::Show() { m_appHidden = false; ShowWindow(m_hwnd, SW_SHOW); Redraw(); }
void FenceWindow::Hide() { m_appHidden = true;  ShowWindow(m_hwnd, SW_HIDE); }

void FenceWindow::DrawContent(float drawW, float drawH, bool noBackdrop) {
    ClampListScroll();
    std::wstring displayTitle = m_renaming
        ? (m_renameBuf + (m_cursorVisible ? L"|" : L""))
        : m_title;
    // The marquee band only exists once the gesture passes the click
    // threshold — a plain click must not flash it.
    RECT marq = {};
    bool showMarq = m_marquee && m_marqueeMoved;
    if (showMarq) marq = MarqueeRect();
    m_render->BeginDraw();
    // Explorer-style states: hover plate, selection plates, the translucent
    // "lifted" look for the whole dragged selection (only once the drag has
    // actually moved), and the rubber band while one is live.
    FenceViewState view;
    view.filter     = SearchActive() ? &m_filterIdx : nullptr;
    view.searchText = IsMapped() ? &m_searchText : nullptr;
    view.caretOn    = m_searchFocused && m_cursorVisible;
    view.loading    = m_loading;
    std::wstring iconRenameDisplay;
    if (m_renameIcon >= 0) {
        iconRenameDisplay = m_iconRenameBuf + m_iconRenameExt;
        view.renameIcon = m_renameIcon;
        view.iconRenameText = &iconRenameDisplay;
        view.renameCaret = m_cursorVisible ? (int)m_iconRenameCaret : -1;
    }
    m_render->DrawFence(displayTitle, m_icons, m_renaming, m_dragOver, m_collapsed,
                        m_collapsed ? -1 : m_hoverIcon,
                        &m_selectedPaths,
                        m_dragMoved && m_dragIcon >= 0,
                        showMarq ? &marq : nullptr,
                        m_chevronHover, m_chevronDown,
                        m_listScroll, &view, noBackdrop, drawW, drawH);
    m_render->EndDraw();
}

void FenceWindow::Redraw() {
    // Any repaint that arrives mid-flight (hover, backdrop timer, external
    // invalidate) must present at the CURRENT window size. Presenting the
    // target-size canvas would snap the window to its landing rect and the
    // next animation tick would yank it back — a visible mid-flight jump.
    if (m_zoomOpening && m_hwnd && IsWindow(m_hwnd)) {
        RECT wr = {};
        if (GetWindowRect(m_hwnd, &wr)) {
            int w = wr.right - wr.left, h = wr.bottom - wr.top;
            if (w > 0 && h > 0) {
                DrawContent((float)w, (float)h, true);
                m_render->PresentCropped(m_hwnd, wr.left, wr.top, w, h);
                return;
            }
        }
    }
    DrawContent();
    // Present from the LIVE window rect, never from the cached m_x/m_y: the
    // cached pair is only refreshed in WM_WINDOWPOSCHANGED, and any repaint
    // fired between a SetWindowPos and that notification used to drag the
    // window back to its old rect (the zoom appeared to snap home mid-flight).
    RECT wr = {};
    if (m_hwnd && GetWindowRect(m_hwnd, &wr))
        m_render->Present(m_hwnd, wr.left, wr.top);
    else
        m_render->Present(m_hwnd, m_x, m_y);
}

void FenceWindow::SetIcons(const std::vector<IconEntry>& entries) {
    m_icons = entries;
    Redraw();
}

void FenceWindow::AddIcon(const std::wstring& name, const std::wstring& path,
                          float atX, float atY) {
    // Snap onto the icon grid: the drop point picks the nearest cell, and a
    // taken cell defers to the nearest free one. Dropping a whole file
    // selection therefore fills cells around the drop point instead of
    // stacking every icon on top of the others.
    IconGrid g = GetIconGrid();
    int c = (int)floorf((atX - g.x0) / g.cw);
    int r = (int)floorf((atY - g.y0) / g.ch);
    c = (std::min)((std::max)(c, 0), g.cols - 1);
    r = (std::min)((std::max)(r, 0), g.rows - 1);
    int cell = r * g.cols + c;

    // The same file already lives in this fence: move its icon to the drop
    // cell (pushing the occupant aside) instead of adding a duplicate.
    // Paths are case-insensitive on Windows.
    for (int i = 0; i < (int)m_icons.size(); i++) {
        if (lstrcmpiW(m_icons[i].path.c_str(), path.c_str()) == 0) {
            PushOccupantOf(cell, { i });
            PlaceIconAt(m_icons[i], cell);
            Redraw();
            Config::MarkDirty();
            return;
        }
    }

    IconEntry e;
    e.name = name;
    e.path = path;
    PlaceIconAt(e, FindFreeCell(cell, OccupiedCells({})));
    m_icons.push_back(std::move(e));
    Redraw();
    Config::MarkDirty();
}

int FenceWindow::HitTestIcon(int mx, int my) const {
    IconGrid g = GetIconGrid();
    float adjY = (float)my;
    if (m_render->Appearance().displayMode == 1 && my >= (int)ContentTop())
        adjY += m_listScroll;
    // Topmost icon wins → iterate in reverse draw order
    for (int i = (int)m_icons.size() - 1; i >= 0; i--) {
        if (SearchActive() && !m_filterBits[i]) continue;
        if ((float)mx >= m_icons[i].x && (float)mx < m_icons[i].x + g.cw &&
            adjY >= m_icons[i].y && adjY < m_icons[i].y + g.ch)
            return i;
    }
    return -1;
}

// Path-keyed membership: the bring-to-front reorder a drag performs must
// not drop the selection, so no selection state may hang off indexes.
static bool ContainsPath(const std::vector<std::wstring>& paths,
                         const std::wstring& path) {
    return std::find(paths.begin(), paths.end(), path) != paths.end();
}

bool FenceWindow::IsPathSelected(const std::wstring& path) const {
    return ContainsPath(m_selectedPaths, path);
}

void FenceWindow::ClearSelection() { m_selectedPaths.clear(); }

void FenceWindow::SelectOnly(int idx) {
    m_selectedPaths.clear();
    if (idx >= 0 && idx < (int)m_icons.size())
        m_selectedPaths.push_back(m_icons[idx].path);
}

int FenceWindow::SelectedIndex() const {
    for (int i = 0; i < (int)m_icons.size(); i++)
        if (IsPathSelected(m_icons[i].path)) return i;
    return -1;
}

void FenceWindow::BringSelectedToFront() {
    // Selected icons draw above the rest (draw order = vector order). The
    // relative order inside each group is kept, and selection is path-keyed,
    // so callers must re-locate any index they still hold.
    if (m_selectedPaths.empty()) return;
    std::vector<IconEntry> rest, sel;
    rest.reserve(m_icons.size());
    sel.reserve(m_selectedPaths.size());
    for (auto& e : m_icons) {
        if (IsPathSelected(e.path)) sel.push_back(std::move(e));
        else                        rest.push_back(std::move(e));
    }
    m_icons.clear();
    for (auto& e : rest) m_icons.push_back(std::move(e));
    for (auto& e : sel)  m_icons.push_back(std::move(e));
    if (SearchActive()) RebuildFilter();
}

void FenceWindow::ClampIconPos(float& x, float& y) const {
    IconGrid g = GetIconGrid();
    float minX = kGridPad;
    float minY = ContentTop() + kGridPad;
    float maxX = (float)m_render->Width()  - g.cw - kGridPad;
    float maxY = (float)m_render->Height() - g.ch - kGridPad;
    // (max)(…, minX) keeps the clamp sane if the fence is smaller than a cell
    x = (std::min)((std::max)(x, minX), (std::max)(maxX, minX));
    y = (std::min)((std::max)(y, minY), (std::max)(maxY, minY));
}

// ── Icon grid ──

FenceWindow::IconGrid FenceWindow::GetIconGrid() const {
    const auto& app = m_render->Appearance();
    float W = (float)m_render->Width(), H = (float)m_render->Height();
    float ct = ContentTop();
    IconGrid g;
    if (app.displayMode == 1) {   // list mode: single column, compact rows
        g.cw = W - 2 * kGridPad;
        g.ch = app.iconSize * 2.6f;  // one line of text + small icon + padding
        g.x0 = kGridPad;
        g.y0 = ct + kGridPad;
        g.cols = 1;
        int visRows = (std::max)(1, (int)((H - kGridPad - g.y0) / g.ch));
        g.rows = (std::max)(visRows, VisibleCount());
    } else {
        g.cw = app.cellW; g.ch = app.cellH;
        g.x0 = kGridPad;
        g.y0 = ct + kGridPad;
        g.cols = (std::max)(1, (int)((W - 2 * kGridPad) / g.cw));
        g.rows = (std::max)(1, (int)((H - kGridPad - g.y0) / g.ch));
    }
    return g;
}

int FenceWindow::CellOf(const IconEntry& e) const {
    IconGrid g = GetIconGrid();
    // Map the icon's *center* so a half-cell offset still rounds to the cell
    // the icon visually belongs to.
    int c = (int)floorf((e.x + g.cw * 0.5f - g.x0) / g.cw);
    int r = (int)floorf((e.y + g.ch * 0.5f - g.y0) / g.ch);
    c = (std::min)((std::max)(c, 0), g.cols - 1);
    r = (std::min)((std::max)(r, 0), g.rows - 1);
    return r * g.cols + c;
}

void FenceWindow::PlaceIconAt(IconEntry& e, int cell) const {
    IconGrid g = GetIconGrid();
    int c = cell % g.cols, r = cell / g.cols;
    // Whole-pixel positions: fractional offsets force bilinear resampling
    // and make the glyph look blurry.
    e.x = floorf(g.x0 + c * g.cw + 0.5f);
    e.y = floorf(g.y0 + r * g.ch + 0.5f);
}

std::vector<int> FenceWindow::OccupiedCells(const std::vector<int>& excludes) const {
    std::vector<int> occ;
    occ.reserve(m_icons.size());
    for (int i = 0; i < (int)m_icons.size(); i++)
        if (std::find(excludes.begin(), excludes.end(), i) == excludes.end())
            occ.push_back(CellOf(m_icons[i]));
    return occ;
}

int FenceWindow::FindFreeCell(int prefer, const std::vector<int>& occupied) const {
    IconGrid g = GetIconGrid();
    auto taken = [&](int idx) {
        return std::find(occupied.begin(), occupied.end(), idx) != occupied.end();
    };
    if (!taken(prefer)) return prefer;
    // Walk Chebyshev rings around the preferred cell, columns first so a
    // crowded drop fills downward, then to the right — desktop-like.
    int pc = prefer % g.cols, pr = prefer / g.cols;
    auto dist = [](int a, int b) { int d = a - b; return d < 0 ? -d : d; };
    for (int d = 1, maxD = (std::max)(g.cols, g.rows); d <= maxD; d++) {
        for (int c = pc - d; c <= pc + d; c++) {
            for (int r = pr - d; r <= pr + d; r++) {
                if ((std::max)(dist(c, pc), dist(r, pr)) != d) continue;   // ring only
                if (c < 0 || c >= g.cols || r < 0 || r >= g.rows) continue;
                int idx = r * g.cols + c;
                if (!taken(idx)) return idx;
            }
        }
    }
    return prefer;   // fence completely full — overlap is unavoidable
}

int FenceWindow::FindPushCell(int prefer, const std::vector<int>& occupied, int dirX) const {
    IconGrid g = GetIconGrid();
    auto taken = [&](int idx) {
        return std::find(occupied.begin(), occupied.end(), idx) != occupied.end();
    };
    int pc = prefer % g.cols, pr = prefer / g.cols;
    int maxD = (std::max)(g.cols, g.rows);
    // Scan one row outward from pc: the drag-direction side first, then the
    // other side, nearest cells first — pushed icons slide along the row.
    auto scanRow = [&](int r) -> int {
        if (r < 0 || r >= g.rows) return -1;
        for (int dc = 0; dc <= maxD; dc++) {
            int passes = (dc == 0) ? 1 : 2;
            for (int pass = 0; pass < passes; pass++) {
                int side;
                if (dc == 0)       side = 0;
                else if (pass == 0) side = (dirX < 0) ? -1 : 1;
                else                side = (dirX < 0) ? 1 : -1;
                int c = pc + side * dc;
                if (c < 0 || c >= g.cols) continue;
                int idx = r * g.cols + c;
                if (!taken(idx)) return idx;
            }
        }
        return -1;
    };
    // Same row first; only a completely full row spills, downward before up.
    for (int dr = 0; dr <= maxD; dr++) {
        if (dr == 0) {
            int f = scanRow(pr);
            if (f >= 0) return f;
        } else {
            int f = scanRow(pr + dr);
            if (f >= 0) return f;
            f = scanRow(pr - dr);
            if (f >= 0) return f;
        }
    }
    return prefer;   // fence completely full — overlap unavoidable
}

void FenceWindow::SaveGridLayout() {
    m_gridH = (int)m_render->Height();
    m_gridPositions.clear();
    m_gridPositions.reserve(m_icons.size());
    for (const auto& ic : m_icons)
        m_gridPositions.push_back({ic.x, ic.y});
}

void FenceWindow::RestoreGridLayout() {
    if (SearchActive() || m_gridPositions.size() != m_icons.size()) return;
    for (size_t i = 0; i < m_icons.size(); i++) {
        m_icons[i].x = m_gridPositions[i].first;
        m_icons[i].y = m_gridPositions[i].second;
    }
    if (m_gridH > 0) {
        int th = (int)(m_render->Appearance().titleH + 0.5f);
        int minH = th + (int)(40.0f * DpiScale() + 0.5f);
        int h = (std::max)(m_gridH, minH);
        int w = (int)m_render->Width();
        SetWindowPos(m_hwnd, nullptr, 0, 0, w, h,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

float FenceWindow::ListMaxScroll() const {
    const auto& app = m_render->Appearance();
    if (app.displayMode != 1 || m_icons.empty()) return 0;
    float rowH = app.iconSize * 2.6f;
    float totalH = (float)VisibleCount() * rowH;
    float contentH = (float)m_render->Height() - ContentTop();
    return (std::max)(0.0f, totalH - contentH);
}

void FenceWindow::ClampListScroll() {
    float maxS = ListMaxScroll();
    if (m_listScroll > maxS) m_listScroll = maxS;
    if (m_listScroll < 0) m_listScroll = 0;
}

void FenceWindow::FitToListHeight() {
    if (m_icons.empty()) return;
    const auto& app = m_render->Appearance();
    float rowH = app.iconSize * 2.6f;
    int ct = (int)(ContentTop() + 0.5f);
    int neededH = ct + (int)(kGridPad + VisibleCount() * rowH + kGridPad + 0.5f);
    int minH = ct + (int)(40.0f * DpiScale() + 0.5f);
    neededH = (std::max)(neededH, minH);
    int w = (int)m_render->Width();
    SetWindowPos(m_hwnd, nullptr, 0, 0, w, neededH,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void FenceWindow::RelayoutIcons() {
    if (m_collapsed) return;
    if (SearchActive()) { RelayoutFiltered(); return; }
    std::vector<int> taken;
    taken.reserve(m_icons.size());
    for (auto& ic : m_icons) {
        int cell = FindFreeCell(CellOf(ic), taken);
        PlaceIconAt(ic, cell);
        taken.push_back(cell);
    }
    Redraw();
}

void FenceWindow::SyncDesktopIconSize(int physicalPx) {
    if (!m_hwnd || physicalPx <= 0) return;
    // Never fight a live gesture — the icon being dragged (or the whole
    // fence being moved/resized, or a marquee on the move) works in the
    // current metrics; catch up instead.
    if (m_dragIcon >= 0 || m_moving || m_resizeMask || m_marquee) return;
    // While zoomed the metrics are deliberately 1.5× the desktop's. Letting
    // this poll adopt the desktop size would flip them back and forth on
    // every 300ms tick (Adopt compares against the scaled imgSize, never
    // matches, and relayouts the whole grid each time). The next tick after
    // the zoom closes catches up with any desktop size change that happened
    // in the meantime.
    if (m_zoomed || m_zoomOpening) return;
    if (!m_render->AdoptDesktopIconSize(physicalPx)) return;
    if (m_collapsed) {
        // The grid cannot be rebuilt while folded (height = title bar only).
        m_relayoutPending = true;
        return;
    }
    RelayoutIcons();   // re-seats every icon on the resized grid and redraws
}

void FenceWindow::PushOccupantOf(int cell, const std::vector<int>& excludes,
                                 int dirX) {
    for (int j = 0; j < (int)m_icons.size(); j++) {
        if (std::find(excludes.begin(), excludes.end(), j) != excludes.end())
            continue;
        if (CellOf(m_icons[j]) != cell) continue;
        // Occupancy excludes only the floating (dragged) group; the
        // occupant's own claim on `cell` keeps the search from sending it
        // right back. The cells the group lifted from count as free, so
        // pushes naturally flow into the gaps the drag leaves behind. dirX
        // steers the search along the row in the drag's direction.
        PlaceIconAt(m_icons[j], FindPushCell(cell, OccupiedCells(excludes), dirX));
        return;   // one occupant per cell
    }
}

void FenceWindow::PushGroupOut() {
    // Clear bystanders from every cell the dragged group currently covers.
    // Distinct cells only — members folded onto one cell during the drag
    // must not trigger the same sweep twice.
    std::vector<int> covered;
    covered.reserve(m_dragGroup.size());
    for (const auto& d : m_dragGroup) {
        if (d.idx < 0 || d.idx >= (int)m_icons.size()) continue;
        int c = CellOf(m_icons[d.idx]);
        if (std::find(covered.begin(), covered.end(), c) == covered.end())
            covered.push_back(c);
    }
    for (int c : covered) PushOccupantOf(c, m_dragExcludes, m_dragDirX);
}

void FenceWindow::SettleDragGroup() {
    if (m_dragIcon >= 0 && !m_dragGroup.empty()) {
        // Settle the pressed icon first, then the rest of the group in draw
        // order: each snaps to the nearest cell that is free of both the
        // bystanders and the already-settled members. Before the drag
        // threshold every member sits on its own cell, so this settles in
        // place — no visible change for a plain click.
        std::vector<int> order;
        order.reserve(m_dragGroup.size());
        for (const auto& d : m_dragGroup) if (d.idx == m_dragIcon) order.push_back(d.idx);
        for (const auto& d : m_dragGroup) if (d.idx != m_dragIcon) order.push_back(d.idx);
        std::vector<int> taken = OccupiedCells(m_dragExcludes);
        taken.reserve(taken.size() + order.size());
        for (int idx : order) {
            if (idx < 0 || idx >= (int)m_icons.size()) continue;
            int cell = FindFreeCell(CellOf(m_icons[idx]), taken);
            PlaceIconAt(m_icons[idx], cell);
            taken.push_back(cell);
        }
    }
    if (m_dragMoved) Config::MarkDirty();   // icons changed cells
    m_dragIcon = -1;
    m_dragHoverCell = -1;
    m_dragDirX = 0;
    m_dragMoved = false;
    m_dragGroup.clear();
    m_dragExcludes.clear();
    Redraw();
}

// ── Marquee (rubber-band) multi-select ──

RECT FenceWindow::MarqueeRect() const {
    // Anchor→cursor, normalized and clamped to the fence body: the band
    // never paints over the title bar even when the captured cursor sweeps
    // above it, and it cannot leak past the window edges.
    int w = (int)m_render->Width(), h = (int)m_render->Height();
    int top = (int)(ContentTop() + 0.5f);
    int l = (std::min)(m_marqAX, m_marqCX), r = (std::max)(m_marqAX, m_marqCX);
    int t = (std::min)(m_marqAY, m_marqCY), b = (std::max)(m_marqAY, m_marqCY);
    l = (std::min)((std::max)(l, 0), w);
    r = (std::min)((std::max)(r, 0), w);
    t = (std::min)((std::max)(t, top), h);
    b = (std::min)((std::max)(b, top), h);
    return RECT{ l, t, r, b };
}

void FenceWindow::UpdateMarqueeSelection() {
    RECT band = MarqueeRect();
    IconGrid g = GetIconGrid();
    std::vector<std::wstring> next;
    if (m_marqueeAdd) next = m_marqueeBase;   // Ctrl: keep what was selected
    next.reserve(next.size() + m_icons.size());
    float adjY = 0;
    if (m_render->Appearance().displayMode == 1) adjY = -m_listScroll;
    for (int i = 0; i < (int)m_icons.size(); i++) {
        const auto& ic = m_icons[i];
        if (SearchActive() && !m_filterBits[i]) continue;
        // Test against the same cell rect HitTestIcon clicks against, and
        // count mere intersection (not containment) — desktop behavior.
        long visY = (long)(ic.y + adjY);
        RECT cell{ (long)ic.x, visY,
                   (long)(ic.x + g.cw), visY + (long)g.ch };
        RECT inter;
        if (!IntersectRect(&inter, &band, &cell)) continue;
        if (!ContainsPath(next, ic.path)) next.push_back(ic.path);
    }
    m_selectedPaths = std::move(next);
}

void FenceWindow::SetDragOver(bool over) {
    if (m_dragOver != over) {
        m_dragOver = over;
        Redraw();
    }
}

void FenceWindow::ToggleCollapse() {
    // Collapsing mid-zoom makes no sense (the zoom is a modal-ish viewing
    // state); the chevron is also mostly off-screen against the shade.
    if (m_zoomed || m_zoomOpening) return;
    int th = (int)(m_render->Appearance().titleH + 0.5f);
    int w  = (int)m_render->Width();
    if (m_collapsed) {
        // Expand back to the remembered full height
        m_collapsed = false;
        int h = m_expandedH > th ? m_expandedH : th * 4;
        SetWindowPos(m_hwnd, nullptr, 0, 0, w, h,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        // Icon metrics may have changed while folded (desktop icon zoom);
        // the grid is available again now, so apply the deferred relayout.
        if (m_relayoutPending) {
            m_relayoutPending = false;
            RelayoutIcons();
        }
    } else {
        // Collapse down to just the title bar
        m_collapsed = true;
        m_expandedH = (int)m_render->Height();
        SetWindowPos(m_hwnd, nullptr, 0, 0, w, th,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    // SetWindowPos raises WM_SIZE → Resize + Redraw, and Redraw already knows
    // the new m_collapsed state (arrow direction, icons skipped).
    Config::MarkDirty();
}

void FenceWindow::SyncCollapsedSize() {
    if (!m_collapsed) return;
    int th = (int)(m_render->Appearance().titleH + 0.5f);
    int w  = (int)m_render->Width();
    SetWindowPos(m_hwnd, nullptr, 0, 0, w, th,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// Phone-folder zoom. Open: the fence flies from its current rect to a
// centered one (58%/66% of its monitor's work area — modest on purpose, the
// edges stay grabbable to fine-tune the size while zoomed) while the
// ZoomOverlay shade fades in above the rest of the desktop and the OTHER
// fences hide for the duration. The open animation's last step up-scales
// the icon metrics and reflows the grid (phone folders grow their icons).
// Close: metrics and icon positions are restored, the fence flies home, the
// shade fades out and the siblings return. All of it rides timer 6 at
// ~60 fps. Never persisted: while zoomed, GetData reports the base rect so
// unrelated config saves cannot snapshot the fly-out geometry.
void FenceWindow::ZoomHideOthers() {
    m_zoomHidden.clear();
    for (auto& f : g_fences) {
        if (f.get() == this) continue;
        HWND h = f->Hwnd();
        // Only hide what the user can actually see. A sibling the shell
        // already hid (Win+D, explorer restart) is not ours to reveal.
        if (!h || !IsWindow(h) || !IsWindowVisible(h)) continue;
        f->Hide();
        m_zoomHidden.push_back(h);
    }
}

void FenceWindow::ZoomRestoreOthers() {
    if (s_teardown) { m_zoomHidden.clear(); return; }
    // g_allHidden means the user hid every fence themselves — leave those
    // hidden; only the fences WE hid come back.
    if (g_allHidden) { m_zoomHidden.clear(); return; }
    for (HWND h : m_zoomHidden) {
        if (!IsWindow(h)) continue;
        for (auto& f : g_fences) {
            if (f->Hwnd() == h) { f->Show(); break; }
        }
    }
    m_zoomHidden.clear();
}

void FenceWindow::ZoomPrepareTracks() {
    FenceAppearance& app = m_render->Appearance();
    m_zoomCellW = app.cellW;
    m_zoomCellH = app.cellH;
    m_zoomImg = app.imgSize;
    m_zoomIconSize = app.iconSize;
    m_zoomTracks.clear();
    m_zoomTracks.reserve(m_icons.size());
    for (const auto& ic : m_icons)
        m_zoomTracks.push_back({ ic.path, ic.x, ic.y, ic.x, ic.y });

    m_iconScaled = true;
    if (app.displayMode != 0 || m_icons.empty()) {
        // Lists just morph the window; their rows already read as rows.
        m_zoomFinalCellW = m_zoomCellW;
        m_zoomFinalCellH = m_zoomCellH;
        m_zoomFinalImg = m_zoomImg;
        return;
    }

    const int n = (int)m_icons.size();
    int cols = (std::max)(1, (int)std::ceil(std::sqrt((double)n)));
    int rows = (n + cols - 1) / cols;
    const float top = ContentTop() + kGridPad;
    const float availW = (std::max)(1.0f, (float)m_render->Width() - 2.0f * kGridPad);
    const float availH = (std::max)(1.0f, (float)m_render->Height() - top - kGridPad);
    float sw = availW / ((float)cols * app.cellW);
    float sh = availH / ((float)rows * app.cellH);
    float scale = (std::min)((std::min)(sw, sh), kZoomMaxScale);
    if (scale < 1.0f) scale = 1.0f;

    m_zoomFinalCellW = app.cellW * scale;
    m_zoomFinalCellH = app.cellH * scale;
    m_zoomFinalImg = app.imgSize * scale;

    const float gridH = rows * m_zoomFinalCellH;
    float startY = top + (availH - gridH) * 0.5f;
    if (startY < top) startY = top;
    for (int i = 0; i < n; i++) {
        int r = i / cols;
        int c = i % cols;
        int rowCount = (std::min)(cols, n - r * cols);
        float rowW = rowCount * m_zoomFinalCellW;
        float rowX = ((float)m_render->Width() - rowW) * 0.5f;
        m_zoomTracks[i].zoomX = rowX + c * m_zoomFinalCellW;
        m_zoomTracks[i].zoomY = startY + r * m_zoomFinalCellH;
    }

    // Warm the per-path icon cache at the final glyph size. The animation
    // then draws those bitmaps down-scaled instead of re-querying the shell
    // for every frame.
    app.cellW = m_zoomFinalCellW;
    app.cellH = m_zoomFinalCellH;
    app.imgSize = m_zoomFinalImg;
    for (int i = 0; i < n; i++) {
        m_icons[i].x = m_zoomTracks[i].zoomX;
        m_icons[i].y = m_zoomTracks[i].zoomY;
    }
    m_render->WarmIcons(m_icons);
}

void FenceWindow::ZoomApplyFrame(float e, int curW, int curH) {
    FenceAppearance& app = m_render->Appearance();
    const float th = app.titleH;
    const float baseW = (float)(std::max)(1, m_zoomBaseW);
    const float baseH = (float)(std::max)(1, m_zoomBaseH);
    const float targetW = (float)(std::max)(1, m_zoomTargetW);
    const float targetH = (float)(std::max)(1, m_zoomTargetH);
    const float bodyBase = (std::max)(1.0f, baseH - th);
    const float bodyTarget = (std::max)(1.0f, targetH - th);
    const float bodyCur = (std::max)(1.0f, (float)curH - th);
    const float sxB = (float)curW / baseW;
    const float syB = bodyCur / bodyBase;
    const float sxZ = (float)curW / targetW;
    const float syZ = bodyCur / bodyTarget;

    if (app.displayMode == 0 && m_iconScaled && !m_zoomTracks.empty()) {
        app.cellW = m_zoomCellW * sxB * (1.0f - e) + m_zoomFinalCellW * sxZ * e;
        app.cellH = m_zoomCellH * syB * (1.0f - e) + m_zoomFinalCellH * syZ * e;
        app.imgSize = m_zoomImg * sxB * (1.0f - e) + m_zoomFinalImg * sxZ * e;
        if (app.cellW < 1.0f) app.cellW = 1.0f;
        if (app.cellH < 1.0f) app.cellH = 1.0f;
        if (app.imgSize < 1.0f) app.imgSize = 1.0f;

        for (auto& ic : m_icons) {
            const ZoomTrack* tr = nullptr;
            for (const auto& t : m_zoomTracks) {
                if (t.path == ic.path) { tr = &t; break; }
            }
            if (!tr) {
                ic.x *= sxB;
                ic.y = th + (ic.y - th) * syB;
                continue;
            }
            float bx = tr->baseX * sxB;
            float by = th + (tr->baseY - th) * syB;
            float zx = tr->zoomX * sxZ;
            float zy = th + (tr->zoomY - th) * syZ;
            ic.x = bx + (zx - bx) * e;
            ic.y = by + (zy - by) * e;
        }
    }
}

void FenceWindow::ZoomDrawFrame(int curW, int curH) {
    DrawContent((float)curW, (float)curH, true);
}

void FenceWindow::ZoomOpen() {
    if (m_zoomed || m_zoomOpening || m_collapsed) return;
    if (!m_hwnd || !IsWindow(m_hwnd)) return;

    RECT base = {};
    if (!GetWindowRect(m_hwnd, &base)) return;

    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    int ww = mi.rcWork.right - mi.rcWork.left;
    int wh = mi.rcWork.bottom - mi.rcWork.top;
    int tw = ww * 58 / 100;
    int th = wh * 66 / 100;
    int tx = mi.rcWork.left + (ww - tw) / 2;
    int ty = mi.rcWork.top + (wh - th) / 2;

    m_zoomBaseX = base.left;
    m_zoomBaseY = base.top;
    m_zoomBaseW = (int)m_render->Width();
    m_zoomBaseH = (int)m_render->Height();
    m_zoomTargetW = tw;
    m_zoomTargetH = th;
    m_zoomBaseValid = true;

    m_zoomed = true;
    m_zoomClosing = false;
    m_zoomOpening = true;
    m_zoomFrame = 0;
    m_zoomFrames = 1;      // time-based; kept for the watchdog

    ZoomOverlay::OpenFor(this);
    SetWindowPos(m_hwnd, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    // One fixed target-size canvas for the whole flight. Per frame only the
    // top-left current-size region is drawn and presented; the target never
    // grows/shrinks, so D2D/WIC resources stay alive.
    m_render->Resize(tw, th);
    ZoomPrepareTracks();

    m_zoomFrom[0] = base.left;   m_zoomFrom[1] = base.top;
    m_zoomFrom[2] = m_zoomBaseW; m_zoomFrom[3] = m_zoomBaseH;
    m_zoomTo[0] = tx;            m_zoomTo[1] = ty;
    m_zoomTo[2] = tw;            m_zoomTo[3] = th;

    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    timeBeginPeriod(1);   // 1ms Sleep resolution for the ticker thread
    m_zoomStartQpc = now.QuadPart;
    m_zoomStartTick = GetTickCount();
    ZoomStartTicker();
    ZoomAnimStep();
}

void FenceWindow::ZoomFinishOpen() {
    ZoomStopTicker();
    timeEndPeriod(1);
    m_zoomOpening = false;
    m_zoomClosing = false;
    m_zoomFrame = 0;
    FenceAppearance& app = m_render->Appearance();
    app.cellW = m_zoomFinalCellW;
    app.cellH = m_zoomFinalCellH;
    app.imgSize = m_zoomFinalImg;
    for (auto& ic : m_icons) {
        for (const auto& t : m_zoomTracks) {
            if (t.path == ic.path) { ic.x = t.zoomX; ic.y = t.zoomY; break; }
        }
    }
    ZoomHideOthers();
    ZoomOverlay::SetFade(1.0f);
    Redraw();
    ScheduleBackdropRefresh();
}

void FenceWindow::ZoomRestoreIconMetrics() {
    if (!m_iconScaled) return;
    FenceAppearance& app = m_render->Appearance();
    app.cellW = m_zoomCellW;
    app.cellH = m_zoomCellH;
    app.imgSize = m_zoomImg;
    app.iconSize = m_zoomIconSize;
    for (auto& ic : m_icons) {
        for (const auto& t : m_zoomTracks) {
            if (t.path == ic.path) { ic.x = t.baseX; ic.y = t.baseY; break; }
        }
    }
    m_render->RebuildStyles();
    m_zoomTracks.clear();
    m_iconScaled = false;
}

void FenceWindow::ZoomFinishClose() {
    ZoomStopTicker();
    timeEndPeriod(1);
    m_zoomOpening = false;
    m_zoomClosing = false;
    m_zoomed = false;
    m_zoomBaseValid = false;
    m_zoomFrame = 0;
    if ((int)m_render->Width() != m_zoomBaseW || (int)m_render->Height() != m_zoomBaseH)
        m_render->Resize(m_zoomBaseW, m_zoomBaseH);
    ZoomRestoreIconMetrics();
    RelayoutIcons();
    ZoomOverlay::Close();
    ZoomRestoreOthers();
    SetWindowPos(m_hwnd, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    Redraw();
    ScheduleBackdropRefresh();
}

void FenceWindow::ZoomStallCheck() {
    if (!m_zoomOpening) return;
    LARGE_INTEGER now = {}, freq = {};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    double elapsedMs = (double)(now.QuadPart - m_zoomStartQpc) * 1000.0 / (double)freq.QuadPart;
    if (elapsedMs < 800.0) return;
    m_zoomStartQpc = now.QuadPart - (LONGLONG)((double)freq.QuadPart * kZoomDurationMs / 1000.0);
    int guard = 0;
    while (m_zoomOpening && guard++ < 4)
        ZoomAnimStep();
}

void FenceWindow::ZoomRequestClose() {
    if (!m_zoomed) return;
    if (m_zoomOpening) {
        if (m_zoomClosing) return;
        // Land the opening first; then reverse from the target rect.
        LARGE_INTEGER now = {}, freq = {};
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        m_zoomStartQpc = now.QuadPart - (LONGLONG)((double)freq.QuadPart * kZoomDurationMs / 1000.0);
        m_zoomInStep = false;
        ZoomAnimStep();
        if (!m_zoomed) return;
    }
    ZoomRestoreOthers();

    RECT wr = {};
    if (GetWindowRect(m_hwnd, &wr)) { m_x = wr.left; m_y = wr.top; }
    m_zoomFrom[0] = m_x;
    m_zoomFrom[1] = m_y;
    m_zoomFrom[2] = (int)m_render->Width();
    m_zoomFrom[3] = (int)m_render->Height();
    m_zoomTo[0] = m_zoomBaseX;
    m_zoomTo[1] = m_zoomBaseY;
    m_zoomTo[2] = m_zoomBaseW;
    m_zoomTo[3] = m_zoomBaseH;

    m_zoomClosing = true;
    m_zoomOpening = true;
    m_zoomFrame = 0;
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    timeBeginPeriod(1);
    m_zoomStartQpc = now.QuadPart;
    m_zoomStartTick = GetTickCount();
    ZoomStartTicker();
    ZoomAnimStep();
}

void FenceWindow::ZoomStartTicker() {
    ZoomStopTicker();
    m_zoomTickerStop.store(false, std::memory_order_relaxed);
    HWND hwnd = m_hwnd;
    m_zoomTicker = std::thread([this, hwnd]() {
        while (!m_zoomTickerStop.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kZoomFrameMs));
            if (m_zoomTickerStop.load(std::memory_order_relaxed)) break;
            if (!hwnd || !IsWindow(hwnd)) break;
            PostMessageW(hwnd, WM_APP_ZOOM_TICK, 0, 0);
        }
    });
}

void FenceWindow::ZoomStopTicker() {
    m_zoomTickerStop.store(true, std::memory_order_relaxed);
    if (m_zoomTicker.joinable()) m_zoomTicker.join();
}

void FenceWindow::ZoomAnimStep() {
    if (!m_zoomOpening || m_zoomInStep || !m_hwnd) return;
    m_zoomInStep = true;

    LARGE_INTEGER now = {}, freq = {};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    double elapsedMs = (double)(now.QuadPart - m_zoomStartQpc) * 1000.0 / (double)freq.QuadPart;
    float u = (float)(elapsedMs / (double)kZoomDurationMs);
    if (u < 0.0f) u = 0.0f;
    if (u > 1.0f) u = 1.0f;
    float e = EaseInOutCubic(u);
    m_zoomFrame++;

    int x = m_zoomFrom[0] + (int)((m_zoomTo[0] - m_zoomFrom[0]) * e);
    int y = m_zoomFrom[1] + (int)((m_zoomTo[1] - m_zoomFrom[1]) * e);
    int w = m_zoomFrom[2] + (int)((m_zoomTo[2] - m_zoomFrom[2]) * e);
    int h = m_zoomFrom[3] + (int)((m_zoomTo[3] - m_zoomFrom[3]) * e);
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    m_x = x;
    m_y = y;
    ZoomApplyFrame(e, w, h);
    ZoomDrawFrame(w, h);
    m_render->PresentCropped(m_hwnd, x, y, w, h);
    ZoomOverlay::SetFade(m_zoomClosing ? 1.0f - e : e);

    if (u >= 1.0f - 1e-5f) {
        KillTimer(m_hwnd, kZoomTimerId);
        if (m_zoomClosing) ZoomFinishClose();
        else               ZoomFinishOpen();
    }
    m_zoomInStep = false;
}

void FenceWindow::ScheduleBackdropRefresh() {
    if (!m_hwnd || !IsWindow(m_hwnd)) return;
    // Same debounce the move/resize path uses (WM_WINDOWPOSCHANGED); timer-2
    // fires CaptureBackdrop once things go quiet for 150 ms.
    SetTimer(m_hwnd, 2, 150, nullptr);
}

void FenceWindow::RestoreAnchor() {
    if (!m_hwnd || !IsWindow(m_hwnd)) return;
    if (m_moving || m_resizeMask || m_dragIcon >= 0 || m_marquee) return;
    // The zoom deliberately parks the fence away from its saved anchor;
    // restoring it here would drag the animation back mid-flight.
    if (m_zoomed || m_zoomOpening) return;
    RECT wr;
    if (!GetWindowRect(m_hwnd, &wr)) return;

    int w = (int)m_render->Width(), h = (int)m_render->Height();

    // An anchor that touches no monitor at all (its monitor was unplugged,
    // or the config was saved under a different topology) would leave the
    // fence unreachable — park it on the nearest monitor's work area. A
    // fence the user dragged only partially off-screen still intersects its
    // monitor, so deliberate placements are left alone.
    RECT anchor{ m_x, m_y, m_x + w, m_y + h };
    POINT c{ m_x + w / 2, m_y + h / 2 };
    HMONITOR mon = MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    RECT hit = {};
    if (mon && GetMonitorInfoW(mon, &mi) &&
        !IntersectRect(&hit, &anchor, &mi.rcMonitor)) {
        RECT wa = mi.rcWork;
        int nx = (std::min)((std::max)(m_x, (int)wa.left),
                            (std::max)((int)wa.left, (int)wa.right - w));
        int ny = (std::min)((std::max)(m_y, (int)wa.top),
                            (std::max)((int)wa.top, (int)wa.bottom - h));
        if (nx != m_x || ny != m_y) {
            m_x = nx; m_y = ny;
            Config::MarkDirty();   // the clamped spot becomes the new saved position
        }
    }
    if (wr.left == m_x && wr.top == m_y) return;   // in place, nothing to do

    HWND parent = GetParent(m_hwnd);
    POINT p{ m_x, m_y };
    if (parent) ScreenToClient(parent, &p);
    SetWindowPos(m_hwnd, nullptr, p.x, p.y, 0, 0,
        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    ScheduleBackdropRefresh();   // the pixels behind the fence changed
}

void FenceWindow::CaptureBackdrop() {
    // During the flight the renderer is a scaled snapshot, not the live
    // desktop; capture once the animation lands (timer-2 is re-armed).
    if (m_zoomOpening) { ScheduleBackdropRefresh(); return; }
    int w = (int)m_render->Width(), h = (int)m_render->Height();
    if (w <= 0 || h <= 0) return;
    // Snapshot where the window actually is. m_x/m_y are only a cache of
    // the last WM_WINDOWPOSCHANGED, and a zoom frame can repaint before the
    // cached pair catches up; sampling from the cache shifted the frosted
    // backdrop by the animation delta.
    RECT live = {};
    int sx = m_x, sy = m_y;
    if (GetWindowRect(m_hwnd, &live)) { sx = live.left; sy = live.top; }

    // Snapshot the desktop area behind the fence. Windows excludes
    // WS_EX_LAYERED windows (including this one) from GDI screen captures,
    // so the fence does not end up inside its own backdrop and we do not
    // need to hide the window (which would break an in-progress drag).
    HDC screenDC = GetDC(nullptr);
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;   // top-down rows
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib) {
        HDC memDC = CreateCompatibleDC(screenDC);
        HGDIOBJ oldBmp = SelectObject(memDC, dib);
        BitBlt(memDC, 0, 0, w, h, screenDC, sx, sy, SRCCOPY);
        if (bits) m_render->SetBackdrop((const BYTE*)bits, w, h);
        SelectObject(memDC, oldBmp);
        DeleteDC(memDC);
        DeleteObject(dib);
    }
    ReleaseDC(nullptr, screenDC);
    Redraw();
}

FenceData FenceWindow::GetData() const {
    // While zoomed, report the remembered pre-zoom rect: a config save fired
    // by any unrelated action must not snapshot the transient fly-out.
    if (m_zoomed && m_zoomBaseValid) {
        return { m_id, m_title, m_zoomBaseX, m_zoomBaseY, m_zoomBaseW, m_zoomBaseH, true, m_sourceFolder, m_sortColumn, m_sortAscending };
    }
    return { m_id, m_title, m_x, m_y, (int)m_render->Width(), (int)m_render->Height(), true, m_sourceFolder, m_sortColumn, m_sortAscending };
}

// ── Sort ──

static int CompareIcons(const IconEntry& a, const IconEntry& b,
                        int column, bool asc) {
    if (a.isDir != b.isDir) return a.isDir ? -1 : 1;
    int r = 0;
    switch (column) {
    case 1: // Date modified — newest first descending (Explorer default)
        r = CompareFileTime(&a.lastWrite, &b.lastWrite); break;
    case 2: // Size
        r = a.size < b.size ? -1 : (a.size > b.size ? 1 : 0); break;
    case 3: // Type (extension)
        r = StrCmpLogicalW(a.ext.c_str(), b.ext.c_str()); break;
    default: // Name
        r = StrCmpLogicalW(a.name.c_str(), b.name.c_str()); break;
    }
    if (!asc) r = -r;
    if (r == 0) r = StrCmpLogicalW(a.name.c_str(), b.name.c_str());
    return r;
}

void FenceWindow::ApplySort() {
    if (m_sourceFolder.empty()) return;
    if (!m_sortInitialized) {
        m_sortInitialized = true;
        int col; bool asc;
        if (QueryExplorerSortForFolder(m_sourceFolder, col, asc)) {
            m_sortColumn = col; m_sortAscending = asc;
            Config::MarkDirty();
        }
    }
    std::stable_sort(m_icons.begin(), m_icons.end(),
        [this](const IconEntry& a, const IconEntry& b) {
            return CompareIcons(a, b, m_sortColumn, m_sortAscending) < 0;
        });
}

void FenceWindow::SetSort(int column, bool ascending) {
    m_sortColumn = column;
    m_sortAscending = ascending;
    m_sortInitialized = true;
    if (!m_sourceFolder.empty()) {
        ApplySort();
        if (SearchActive()) RebuildFilter();
        else {
            for (auto& ic : m_icons) ic.x = ic.y = 0;
            RelayoutIcons();
        }
        Config::MarkDirty();
    }
}

void FenceWindow::SetSortPreset(int column, bool ascending) {
    m_sortColumn = column;
    m_sortAscending = ascending;
    m_sortInitialized = true;
}

// ── Search / filter ──

float FenceWindow::ContentTop() const {
    const auto& app = m_render->Appearance();
    return app.titleH + (IsMapped() ? app.searchH : 0.0f);
}

int FenceWindow::VisibleCount() const {
    return SearchActive() ? (int)m_filterIdx.size() : (int)m_icons.size();
}

void FenceWindow::RebuildFilter() {
    m_filterIdx.clear();
    m_filterBits.assign(m_icons.size(), 0);
    if (SearchActive()) {
        for (int i = 0; i < (int)m_icons.size(); i++) {
            if (StrStrIW(m_icons[i].name.c_str(), m_searchText.c_str())) {
                m_filterIdx.push_back(i);
                m_filterBits[i] = 1;
            }
        }
    }
    RelayoutFiltered();
    Redraw();
}

void FenceWindow::RelayoutFiltered() {
    if (!SearchActive()) { RelayoutIcons(); return; }
    IconGrid g = GetIconGrid();
    for (int k = 0; k < (int)m_filterIdx.size(); k++)
        PlaceIconAt(m_icons[m_filterIdx[k]], k);
}

void FenceWindow::FocusSearch() {
    if (!IsMapped() || m_renaming) return;
    m_searchFocused = true;
    m_cursorVisible = true;
    if (m_cursorTimer) { KillTimer(m_hwnd, 1); m_cursorTimer = 0; }
    m_cursorTimer = SetTimer(m_hwnd, 1, 530, nullptr);
    // Attach thread input so SetFocus can reach this desktop child window
    if (!m_focusAttached) {
        HWND fg = GetForegroundWindow();
        DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
        DWORD self_ = GetCurrentThreadId();
        if (fgThread && fgThread != self_ && AttachThreadInput(self_, fgThread, TRUE))
            m_focusAttached = fgThread;
    }
    SetFocus(m_hwnd);
    Redraw();
}

void FenceWindow::ClearSearch() {
    m_searchText.clear();
    if (m_searchFocused) {
        m_searchFocused = false;
        m_cursorVisible = false;
        if (m_cursorTimer) { KillTimer(m_hwnd, 1); m_cursorTimer = 0; }
        if (m_focusAttached) {
            AttachThreadInput(GetCurrentThreadId(), m_focusAttached, FALSE);
            m_focusAttached = 0;
        }
    }
    m_filterIdx.clear();
    m_filterBits.clear();
    RelayoutIcons();
    Redraw();
}

// ── Folder mapping ──

DWORD WINAPI FenceWindow::ChangeThreadProc(LPVOID param) {
    auto* self = (FenceWindow*)param;
    HANDLE handles[2] = { self->m_stopEvent, self->m_changeHandle };
    for (;;) {
        DWORD result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (result == WAIT_OBJECT_0) break;
        if (result == WAIT_OBJECT_0 + 1) {
            PostMessage(self->m_hwnd, WM_FENCE_REFRESH, 0, 0);
            FindNextChangeNotification(self->m_changeHandle);
        }
    }
    return 0;
}

void FenceWindow::StartSourceWatch() {
    if (m_sourceFolder.empty() || m_changeHandle != INVALID_HANDLE_VALUE) return;
    m_changeHandle = FindFirstChangeNotificationW(
        m_sourceFolder.c_str(), FALSE,
        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
        FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE |
        FILE_NOTIFY_CHANGE_ATTRIBUTES);
    if (m_changeHandle == INVALID_HANDLE_VALUE) return;
    m_stopEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_stopEvent) {
        FindCloseChangeNotification(m_changeHandle);
        m_changeHandle = INVALID_HANDLE_VALUE;
        return;
    }
    m_changeThread = CreateThread(nullptr, 0, ChangeThreadProc, this, 0, nullptr);
    if (!m_changeThread) {
        CloseHandle(m_stopEvent); m_stopEvent = nullptr;
        FindCloseChangeNotification(m_changeHandle); m_changeHandle = INVALID_HANDLE_VALUE;
    }
}

void FenceWindow::StopSourceWatch() {
    if (m_changeHandle == INVALID_HANDLE_VALUE) return;
    SetEvent(m_stopEvent);
    WaitForSingleObject(m_changeThread, 2000);
    CloseHandle(m_changeThread); m_changeThread = nullptr;
    CloseHandle(m_stopEvent); m_stopEvent = nullptr;
    FindCloseChangeNotification(m_changeHandle); m_changeHandle = INVALID_HANDLE_VALUE;
}

void FenceWindow::MapToFolder(const std::wstring& folderPath) {
    StopSourceWatch();
    ClearSearch();
    m_sourceFolder = folderPath;
    if (folderPath.empty()) { Config::MarkDirty(); return; }
    EnumerateSourceFolder(false);
    StartSourceWatch();
    Config::MarkDirty();
}

void FenceWindow::UnmapFolder() {
    StopSourceWatch();
    ClearSearch();
    m_sourceFolder.clear();
    Config::MarkDirty();
}

void FenceWindow::RefreshFromSource() {
    if (m_sourceFolder.empty()) return;
    if (m_dragIcon >= 0 || m_moving || m_resizeMask) return;
    EnumerateSourceFolder(true);
    Redraw();
    Config::MarkDirty();
}

void FenceWindow::EnumerateSourceFolder(bool keepPositions) {
    if (m_sourceFolder.empty()) return;

    std::unordered_map<std::wstring, std::pair<float, float>> oldPos;
    if (keepPositions) {
        for (const auto& icon : m_icons) {
            std::wstring key = icon.path;
            for (auto& ch : key) ch = (wchar_t)::towlower(ch);
            oldPos[key] = { icon.x, icon.y };
        }
    }

    std::vector<IconEntry> newIcons;
    std::wstring searchPath = m_sourceFolder + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == L'\0' ||
                (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0')))
                continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) continue;

            IconEntry e;
            e.name = fd.cFileName;
            e.path = m_sourceFolder + L"\\" + fd.cFileName;
            e.size = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            e.lastWrite = fd.ftLastWriteTime;
            e.isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            e.ext = PathFindExtensionW(fd.cFileName);

            if (keepPositions) {
                std::wstring key = e.path;
                for (auto& ch : key) ch = (wchar_t)::towlower(ch);
                auto it = oldPos.find(key);
                if (it != oldPos.end()) {
                    e.x = it->second.first;
                    e.y = it->second.second;
                }
            }
            newIcons.push_back(std::move(e));
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }

    m_icons = std::move(newIcons);
    ApplySort();
    if (SearchActive()) RebuildFilter();
    else              RelayoutIcons();
}

void FenceWindow::EnterRename() {
    m_renaming = true;
    m_renameBuf = m_title;
    m_cursorVisible = true;
    m_cursorTimer = SetTimer(m_hwnd, 1, 530, nullptr);
    // A desktop child shares explorer's foreground; SetFocus can only reach
    // it with our input queue attached to the foreground thread. Attach for
    // the whole rename session (detached in ExitRename).
    HWND fg = GetForegroundWindow();
    DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD self_ = GetCurrentThreadId();
    if (fgThread && fgThread != self_ && AttachThreadInput(self_, fgThread, TRUE))
        m_focusAttached = fgThread;
    SetFocus(m_hwnd);
    Redraw();
}

void FenceWindow::ExitRename(bool commit) {
    if (!m_renaming) return;
    m_renaming = false;
    if (commit && m_renameBuf != m_title) {
        m_title = m_renameBuf;
        Config::MarkDirty();
    }
    KillTimer(m_hwnd, 1);
    m_cursorTimer = 0;
    if (m_focusAttached) {
        AttachThreadInput(GetCurrentThreadId(), m_focusAttached, FALSE);
        m_focusAttached = 0;
    }
    Redraw();
}

void FenceWindow::EnterIconRename(int idx) {
    if (idx < 0 || idx >= (int)m_icons.size() || m_renaming || m_collapsed) return;
    m_renameIcon = idx;
    // Explorer semantics: the base name comes up selected (typing replaces
    // it) while the extension stays fixed. Folders have no extension.
    const std::wstring& full = m_icons[idx].name;
    m_iconRenameExt = m_icons[idx].isDir ? L"" : PathFindExtensionW(full.c_str());
    m_iconRenameBuf = full.substr(0, full.size() - m_iconRenameExt.size());
    m_iconRenameCaret = m_iconRenameBuf.size();
    m_iconRenamePristine = true;
    m_cursorVisible = true;
    if (!m_cursorTimer) m_cursorTimer = SetTimer(m_hwnd, 1, 530, nullptr);
    // Same focus trick as the title rename: a desktop child can only hold
    // keyboard focus with our input queue attached to the foreground thread.
    HWND fg = GetForegroundWindow();
    DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD self_ = GetCurrentThreadId();
    if (fgThread && fgThread != self_ && AttachThreadInput(self_, fgThread, TRUE))
        m_focusAttached = fgThread;
    SetFocus(m_hwnd);
    NewDbg(L"EnterIconRename idx=%d fg=0x%p attach=%d focus=0x%p",
           idx, fg, m_focusAttached != 0, GetFocus());
    Redraw();
}

void FenceWindow::ExitIconRename(bool commit) {
    if (m_renameIcon < 0) return;
    int idx = m_renameIcon;
    m_renameIcon = -1;
    NewDbg(L"ExitIconRename commit=%d buf=\"%s\" ext=\"%s\"",
           commit, m_iconRenameBuf.c_str(), m_iconRenameExt.c_str());
    if (commit && idx < (int)m_icons.size()) {
        std::wstring newName = m_iconRenameBuf + m_iconRenameExt;
        if (newName != m_icons[idx].name) {
            std::wstring oldPath = m_icons[idx].path;
            size_t slash = oldPath.find_last_of(L'\\');
            // Invalid names (empty, reserved chars) fail MoveFileW — then the
            // icon simply keeps its old name, like Explorer's error would.
            if (slash != std::wstring::npos && !m_iconRenameBuf.empty()) {
                std::wstring newPath = oldPath.substr(0, slash + 1) + newName;
                NewDbg(L"MoveFileW \"%s\" -> \"%s\"", oldPath.c_str(), newPath.c_str());
                if (MoveFileW(oldPath.c_str(), newPath.c_str())) {
                    m_icons[idx].name = newName;
                    m_icons[idx].path = newPath;
                    m_icons[idx].ext = PathFindExtensionW(newPath.c_str());
                    for (auto& p : m_selectedPaths)
                        if (p == oldPath) p = newPath;
                    // Mapped fences re-enumerate via the change watcher;
                    // updating the entry now keeps the label right until it
                    // fires (the watcher's keep-positions pass sees the new
                    // path).
                    Config::MarkDirty();
                } else {
                    NewDbg(L"MoveFileW FAILED err=%u", GetLastError());
                }
            }
        }
    }
    if (!m_renaming && !m_searchFocused) {
        if (m_cursorTimer) { KillTimer(m_hwnd, 1); m_cursorTimer = 0; }
        if (m_focusAttached) {
            AttachThreadInput(GetCurrentThreadId(), m_focusAttached, FALSE);
            m_focusAttached = 0;
        }
    }
    Redraw();
}

// Edge hit-test shared by WM_NCHITTEST (resize behavior) and cursor picking.
// Returns HTLEFT/HTRIGHT/… near a border, HTCLIENT anywhere in the body.
// `collapsed` fences are a bare title pill — nothing to resize there.
static int EdgeHit(HWND hwnd, bool collapsed, int mx, int my) {
    if (collapsed) return HTCLIENT;
    RECT rc; GetClientRect(hwnd, &rc);
    const int E = (int)(8.0f * DpiScale() + 0.5f);
    bool L = mx < E, R = mx > rc.right - E;
    bool T = my < E, B = my > rc.bottom - E;
    if (T && L) return HTTOPLEFT;
    if (T && R) return HTTOPRIGHT;
    if (B && L) return HTBOTTOMLEFT;
    if (B && R) return HTBOTTOMRIGHT;
    if (L) return HTLEFT;
    if (R) return HTRIGHT;
    if (T) return HTTOP;
    if (B) return HTBOTTOM;
    return HTCLIENT;
}

// Pointer shape matching a WM_NCHITTEST code.
static LPCWSTR CursorForHT(int ht) {
    switch (ht) {
    case HTTOPLEFT: case HTBOTTOMRIGHT: return IDC_SIZENWSE;
    case HTTOPRIGHT: case HTBOTTOMLEFT: return IDC_SIZENESW;
    case HTLEFT:     case HTRIGHT:      return IDC_SIZEWE;
    case HTTOP:      case HTBOTTOM:     return IDC_SIZENS;
    default:                            return IDC_ARROW;
    }
}

// ── Shell context menu (the Explorer right-click menu for a file) ──
//
// Resolving one: parse the full path into an absolute PIDL, bind to the parent
// folder's IShellFolder, and ask it for the child's IContextMenu. Displaying:
// let the shell fill a popup (command ids in [1, 0x7FFF]), append our single
// fence-specific verb above that range, then dispatch the chosen id.

static HRESULT CreateShellContextMenu(const std::wstring& path, IContextMenu** ppCM) {
    *ppCM = nullptr;

    PIDLIST_ABSOLUTE pidlFull = nullptr;
    HRESULT hr = SHParseDisplayName(path.c_str(), nullptr, &pidlFull, 0, nullptr);
    if (FAILED(hr) || !pidlFull) return E_FAIL;

    // Child = the file's own ID (points inside pidlFull); parent = the folder.
    LPCITEMIDLIST pidlChild = ILFindLastID(pidlFull);
    PIDLIST_ABSOLUTE pidlParent = ILCloneFull(pidlFull);
    if (pidlParent) ILRemoveLastID(pidlParent);

    CComPtr<IShellFolder> spParent;
    CComPtr<IShellFolder> spDesktop;
    hr = SHGetDesktopFolder(&spDesktop);
    if (SUCCEEDED(hr)) {
        if (!pidlParent || ILIsEmpty(pidlParent)) {
            spParent = spDesktop;                    // file lives on the desktop
        } else {
            hr = spDesktop->BindToObject(pidlParent, nullptr, IID_PPV_ARGS(&spParent));
        }
    }

    if (SUCCEEDED(hr) && spParent && pidlChild) {
        hr = spParent->GetUIObjectOf(nullptr, 1,
            reinterpret_cast<PCUITEMID_CHILD_ARRAY>(&pidlChild),
            IID_IContextMenu, nullptr, (void**)ppCM);
    }

    if (pidlParent) CoTaskMemFree(pidlParent);
    CoTaskMemFree(pidlFull);
    return hr;
}

// ── "New" submenu (the shell's own NewMenuHandler) ──
//
// Explorer's 新建 submenu is produced by the handler registered under
// HKCR\Directory\Background\shellex\ContextMenuHandlers\New (stable CLSID
// since Win2000). Instantiating it directly yields the exact system menu —
// localized labels, third-party ShellNew templates — and InvokeCommand
// creates the file in the folder the handler was initialized with. Passing
// the desktop PIDL (not a plain folder path) makes the handler recognize the
// desktop, so it adds the desktop-only entries like 快捷方式.
static const CLSID kCLSID_NewMenu = { 0xD969A300, 0xE7FF, 0x11D0,
                                      { 0xA9, 0x3B, 0x00, 0xA0, 0xC9, 0x0F, 0x27, 0x19 } };

// Command-id base the handler owns inside the fence menu (above our own ids).
static const int kCmdNewFirst = 0x200;

// Set while a 新建 menu session is live so the fence wndproc can forward the
// menu messages the handler needs (Explorer does the same forwarding).
static IContextMenu* s_activeNewCM = nullptr;

// The shell handler creates the file but doesn't tell us its final name
// ("新建 文本文档 (2).txt" on collisions), so diff the folder's names
// around the invocation to find it.
static void SnapshotFolderNames(const std::wstring& dir, std::vector<std::wstring>& out) {
    if (dir.empty()) return;
    std::wstring sp = dir + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(sp.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { out.push_back(fd.cFileName); } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

static HRESULT CreateNewMenuHandler(PCIDLIST_ABSOLUTE pidlFolder, IContextMenu** ppCM) {
    *ppCM = nullptr;

    CComPtr<IContextMenu> spCM;
    HRESULT hr = spCM.CoCreateInstance(kCLSID_NewMenu, nullptr, CLSCTX_INPROC_SERVER);
    NewDbg(L"CreateNewMenuHandler: pidl=%p isEmpty=%d CoCreate=0x%08X",
           pidlFolder, pidlFolder ? ILIsEmpty(pidlFolder) : -1, (unsigned)hr);
    if (SUCCEEDED(hr)) {
        CComPtr<IShellExtInit> spInit;
        hr = spCM.QueryInterface(&spInit);
        NewDbg(L"  QI IShellExtInit=0x%08X", (unsigned)hr);
        if (SUCCEEDED(hr)) {
            hr = spInit->Initialize(pidlFolder, nullptr, nullptr);
            NewDbg(L"  Initialize=0x%08X", (unsigned)hr);
        }
    }
    if (SUCCEEDED(hr)) *ppCM = spCM.Detach();
    return hr;
}

// The fence-only verb is placed far above the shell's command-id range.
static const int kCmdRemoveFromFence = 0x8001;
static const int kCmdSortByName  = 0x8010;
static const int kCmdSortByDate  = 0x8011;
static const int kCmdSortByType  = 0x8012;
static const int kCmdSortBySize  = 0x8013;
static const int kCmdSortAsc     = 0x8014;
static const int kCmdSortDesc    = 0x8015;

// Shows the Explorer menu for `path` at `ptScreen` (screen coords) with a
// "remove from fence" entry appended. Returns false only when no system menu
// could be produced (caller falls back to the plain fence menu). Sets
// `invoked` when a shell command ran, `removeChosen` for our own verb, and
// `renameChosen` when the shell's 重命名 verb was picked (the caller starts
// the fence's in-place rename instead of handing it to Explorer).
static bool RunShellContextMenu(HWND hwnd, const std::wstring& path,
                                const std::wstring& displayName, POINT ptScreen,
                                bool& invoked, bool& removeChosen,
                                bool& renameChosen) {
    invoked = removeChosen = renameChosen = false;

    CComPtr<IContextMenu> spCM;
    if (FAILED(CreateShellContextMenu(path, &spCM)) || !spCM) return false;

    HMENU menu = CreatePopupMenu();
    if (!menu) return false;

    if (FAILED(spCM->QueryContextMenu(menu, 0, 1, 0x7FFF,
                                      CMF_EXPLORE | CMF_CANRENAME))) {
        DestroyMenu(menu);
        return false;
    }
    NewDbg(L"--- shell file menu for \"%s\" ---", path.c_str());
    NewDbgDumpMenu(menu, 0);
    NewDbg(L"--- end ---");

    // The shell's rename verb would start Explorer's rename in ITS view
    // (invisible when desktop icons are hidden); intercept its command id so
    // the fence renames in place instead.
    int renameCmdId = 0;
    int itemCount = GetMenuItemCount(menu);
    for (int i = 0; i < itemCount; i++) {
        MENUITEMINFOW mii = { sizeof(mii) };
        mii.fMask = MIIM_ID | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(menu, i, TRUE, &mii)) continue;
        if (mii.hSubMenu || !mii.wID || mii.wID > 0x7FFF) continue;
        char verb[64] = {};
        if (SUCCEEDED(spCM->GetCommandString(mii.wID - 1, GCS_VERBA, nullptr,
                                             verb, sizeof(verb))) &&
            lstrcmpiA(verb, "rename") == 0) {
            renameCmdId = (int)mii.wID;
            break;
        }
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    std::wstring rm = std::wstring(FenceWindow::Loc(L"Remove from fence", L"从围栏移除")) +
                      L"  \"" + displayName + L"\"";
    AppendMenuW(menu, MF_STRING, kCmdRemoveFromFence, rm.c_str());

    // Win11 look first: we repaint the (identical) HMENU contents ourselves,
    // because Explorer's XAML menu cannot be obtained cross-process. If the
    // Fluent layer fails to start, fall back to the classic system menu.
    SetForegroundWindow(hwnd);
    int idCmd = FluentMenu::Run(hwnd, spCM, menu, path, ptScreen);
    if (idCmd < 0)
        idCmd = TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_RETURNCMD | TPM_RIGHTBUTTON,
                               ptScreen.x, ptScreen.y, 0, hwnd, nullptr);
    DestroyMenu(menu);

    if (idCmd <= 0) return true;                 // dismissed — nothing to run

    if (idCmd == kCmdRemoveFromFence) {
        removeChosen = true;
        return true;
    }
    if (renameCmdId && idCmd == renameCmdId) {
        renameChosen = true;
        return true;
    }
    if (idCmd >= 1 && idCmd <= 0x7FFF) {
        CMINVOKECOMMANDINFO ici = {};
        ici.cbSize = sizeof(ici);
        ici.hwnd = hwnd;
        ici.lpVerb = MAKEINTRESOURCEA(idCmd - 1);
        ici.nShow = SW_SHOWNORMAL;
        if (SUCCEEDED(spCM->InvokeCommand(&ici))) invoked = true;
    }
    return true;
}

void FenceWindow::RegisterClass(HINSTANCE hInst) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = ClassName();
    RegisterClassExW(&wc);
}

LRESULT CALLBACK FenceWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = (FenceWindow*)GetWindowLongPtr(hwnd, GWLP_USERDATA);

    // While the 新建 menu session is live, forward the menu messages the
    // shell handler needs (Explorer forwards these to its handlers too):
    // WM_INITMENUPOPUP fills the lazy submenu, DRAW/MEASURE let the classic
    // TrackPopupMenu fallback render the handler's owner-drawn icons.
    if (s_activeNewCM && (msg == WM_INITMENUPOPUP || msg == WM_DRAWITEM ||
                          msg == WM_MEASUREITEM)) {
        CComPtr<IContextMenu3> cm3;
        CComPtr<IContextMenu2> cm2;
        s_activeNewCM->QueryInterface(IID_PPV_ARGS(&cm3));
        s_activeNewCM->QueryInterface(IID_PPV_ARGS(&cm2));
        LRESULT lr = 0;
        if (cm3)      cm3->HandleMenuMsg2(msg, wp, lp, &lr);
        else if (cm2) cm2->HandleMenuMsg(msg, wp, lp);
    }

    switch (msg) {
    case WM_CREATE: {
        auto* cs = (CREATESTRUCT*)lp;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return 0;
    }
    case WM_DROPFILES: {
        HDROP hdrop = (HDROP)wp;
        if (!self) { DragFinish(hdrop); return 0; }
        self->SetDragOver(false);

        POINT dropPt = {};
        DragQueryPoint(hdrop, &dropPt);
        float dx = (float)dropPt.x, dy = (float)dropPt.y;

        UINT count = DragQueryFileW(hdrop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; i++) {
            WCHAR path[MAX_PATH];
            if (DragQueryFileW(hdrop, i, path, MAX_PATH)) {
                WCHAR display[MAX_PATH];
                wcscpy_s(display, PathFindFileNameW(path));
                PathRemoveExtensionW(display);
                if (display[0])
                    self->AddIcon(display, path, dx, dy);
            }
        }
        DragFinish(hdrop);
        return 0;
    }
    case WM_NCHITTEST: {
        if (!self) return HTCLIENT;
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &pt);
        return EdgeHit(hwnd, self->m_collapsed, pt.x, pt.y);
    }
    case WM_SETCURSOR: {
        // A fence is a layered child of *another process's* desktop list, so
        // the usual cursor plumbing (class cursor applied through the parent
        // chain) never reaches it — without this the pointer keeps whatever
        // shape it had before entering. Recompute the hit from the live
        // cursor position: during a resize drag the pointer can leave the
        // edge zone, and we still want the sizing shape in that case.
        if (self) {
            int ht = self->m_resizeMask
                ? self->m_resizeHT
                : [&] { POINT p; GetCursorPos(&p); ScreenToClient(hwnd, &p);
                        return EdgeHit(hwnd, self->m_collapsed, p.x, p.y); }();
            SetCursor(LoadCursorW(nullptr, CursorForHT(ht)));
            return TRUE;
        }
        return DefWindowProc(hwnd, msg, wp, lp);
    }
    case WM_SIZE: {
        UINT w = LOWORD(lp), h = HIWORD(lp);
        // The zoom flight owns a fixed target-size render target; the window
        // rect is set by UpdateLayeredWindow and must never resize it.
        if (self && self->m_zoomOpening) return 0;
        if (self && w > 0 && h > 0 && (w != self->m_render->Width() || h != self->m_render->Height())) {
            self->m_render->Resize(w, h);
            // Re-seat the icons on the resized grid (also resolves overlaps
            // when the fence shrinks). Skip while collapsing: the tiny height
            // would crush every icon's row and the layout would be lost on
            // expand. RelayoutIcons redraws on its own.
            if (!self->m_collapsed) self->RelayoutIcons();
            else                    self->Redraw();
        }
        return 0;
    }
    case WM_WINDOWPOSCHANGED: {
        if (self) {
            RECT wr; GetWindowRect(hwnd, &wr);
            self->m_x = wr.left;
            self->m_y = wr.top;
            // Re-snapshot the frosted-glass backdrop shortly after the
            // window settles. SetTimer resets the countdown, so a continuous
            // move/resize only triggers one capture once it stops.
            SetTimer(hwnd, 2, 150, nullptr);
        }
        return DefWindowProc(hwnd, msg, wp, lp);
    }
    case WM_NCLBUTTONDOWN: {
        // Edge resize. DefWindowProc's size loop only works for top-level
        // windows, so for our desktop-child fences we run it manually:
        // remember the starting rect/cursor and follow WM_MOUSEMOVE.
        if (!self) return DefWindowProc(hwnd, msg, wp, lp);
        int code = (int)wp;
        if (self->m_zoomed) return 0;   // fixed canvas while zoomed
        if (code >= HTLEFT && code <= HTBOTTOMRIGHT) {
            int mask = 0;
            if (code == HTLEFT || code == HTTOPLEFT || code == HTBOTTOMLEFT)      mask |= 1;
            if (code == HTRIGHT || code == HTTOPRIGHT || code == HTBOTTOMRIGHT)   mask |= 2;
            if (code == HTTOP || code == HTTOPLEFT || code == HTTOPRIGHT)         mask |= 4;
            if (code == HTBOTTOM || code == HTBOTTOMLEFT || code == HTBOTTOMRIGHT) mask |= 8;
            // While zoomed an edge drag fine-tunes the zoom rect itself:
            // the zoom stays armed and the shade stays up; closing still
            // flies home to the remembered base rect.
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };   // screen coords
            RECT wr; GetWindowRect(hwnd, &wr);
            HWND parent = GetParent(hwnd);
            if (parent) {                   // screen → parent-client
                ScreenToClient(parent, &pt);
                POINT a{ wr.left, wr.top }, b{ wr.right, wr.bottom };
                ScreenToClient(parent, &a);
                ScreenToClient(parent, &b);
                wr = { a.x, a.y, b.x, b.y };
            }
            self->m_resizeMask = mask;
            self->m_resizeHT = code;      // keep this sizing shape for the drag
            self->m_resizeStart = wr;
            self->m_resizePtX = pt.x;
            self->m_resizePtY = pt.y;
            SetCapture(hwnd);
            SetCursor(LoadCursorW(nullptr, CursorForHT(code)));
            return 0;
        }
        return DefWindowProc(hwnd, msg, wp, lp);
    }
    case WM_LBUTTONDOWN: {
        if (!self) return 0;
        int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);

        // List-mode scrollbar hit test (before icon/chevron/move handling)
        float maxS = self->ListMaxScroll();
        if (maxS > 0) {
            const auto& app = self->m_render->Appearance();
            float bt = self->ContentTop();
            float w = (float)self->m_render->Width();
            float h = (float)self->m_render->Height();
            float rowH = app.iconSize * 2.6f;
            float sbW = 6.0f, sbPad = 2.0f;
            float trackX = w - sbW - sbPad;
            if ((float)mx >= trackX && (float)my >= bt) {
                float trackH = h - bt - sbPad * 2;
                float totalH = (float)self->VisibleCount() * rowH;
                float contentH = h - bt;
                float thumbH = (std::max)(20.0f, trackH * contentH / totalH);
                float thumbY = bt + sbPad + (self->m_listScroll / maxS) * (trackH - thumbH);
                if ((float)my < thumbY) {
                    self->m_listScroll -= contentH;
                    self->ClampListScroll();
                    self->Redraw();
                    return 0;
                } else if ((float)my > thumbY + thumbH) {
                    self->m_listScroll += contentH;
                    self->ClampListScroll();
                    self->Redraw();
                    return 0;
                } else {
                    self->m_scrollbarDrag = true;
                    self->m_scrollDragStartY = (float)my;
                    self->m_scrollDragStartOffset = self->m_listScroll;
                    SetCapture(hwnd);
                    return 0;
                }
            }
        }

        // A click anywhere outside the renaming icon's label commits the
        // rename first (Explorer behavior), then the click acts normally.
        if (self->m_renameIcon >= 0 && self->HitTestIcon(mx, my) != self->m_renameIcon)
            self->ExitIconRename(true);

        if (!self->m_renaming && my < (int)self->m_render->Appearance().titleH) {
            // Top-right corner of the title bar = collapse/expand arrow.
            int arrowL = (int)(self->m_render->Width() - self->m_render->Appearance().titleH);
            if (mx >= arrowL) {
                self->m_chevronDown = true;
                self->m_chevronHover = true;
                SetCapture(hwnd);
                self->Redraw();
                return 0;
            }
            // Rest of the title bar → drag the whole fence. Manual capture
            // drag: DefWindowProc's HTCAPTION loop doesn't move child windows.
            // Remember the press point: on a collapsed fence a release that
            // never traveled SM_CXDRAG is a plain click and expands it.
            self->m_moveOffX = mx;
            self->m_moveOffY = my;
            self->m_pressX = mx;
            self->m_pressY = my;
            self->m_pressMoved = false;
            self->m_moving = true;
            SetCapture(hwnd);
            return 0;
        }
        // Search-box strip (mapped fences): focus the box on click, consume the event
        if (self && !self->m_renaming && self->IsMapped() &&
            my >= (int)self->m_render->Appearance().titleH &&
            my < (int)self->ContentTop()) {
            self->FocusSearch();
            return 0;
        }
        // Icon → select it and arm a drag inside the fence
        int hit = self->HitTestIcon(mx, my);
        if (hit >= 0) {
            std::wstring pressedPath = self->m_icons[hit].path;
            if (self->m_iconScaled) {
                // The zoom view is a centered layout; the base drag/snap math
                // would yank icons back to the left-aligned grid. Keep clicks
                // (select) and double-clicks (open) working, but do not drag.
                if (!self->IsPathSelected(pressedPath)) self->SelectOnly(hit);
                self->m_hoverIcon = hit;
                self->Redraw();
                return 0;
            }
            // Explorer selection rule: pressing an already-selected icon
            // keeps the multi-selection (the whole group will drag
            // together); pressing any other icon collapses it to a single.
            if (!self->IsPathSelected(pressedPath)) self->SelectOnly(hit);
            // Bring the WHOLE selection to the front so it draws above the
            // rest while dragging. That reorders the vector, so re-locate
            // every index by path afterwards.
            self->BringSelectedToFront();
            self->m_dragGroup.clear();
            self->m_dragExcludes.clear();
            for (int i = 0; i < (int)self->m_icons.size(); i++) {
                if (!self->IsPathSelected(self->m_icons[i].path)) continue;
                self->m_dragGroup.push_back({ i, self->m_icons[i].x, self->m_icons[i].y });
                self->m_dragExcludes.push_back(i);
                if (self->m_icons[i].path == pressedPath) hit = i;
            }
            self->m_hoverIcon = hit;
            self->m_dragIcon = hit;         // the pressed icon leads the group
            self->m_dragDirX = 0;
            self->m_dragMoved = false;      // real drag starts past the
            self->m_dragGrabX = mx;         // system drag distance — a plain
            self->m_dragGrabY = my;         // click must not disturb anything
            self->m_dragHoverCell = self->CellOf(self->m_icons[hit]);
            SetCapture(hwnd);
            self->Redraw();
        } else {
            // Empty body → marquee multi-select (desktop style). The press
            // arms the band; the click/drag threshold in WM_MOUSEMOVE
            // decides whether it becomes a plain click (which clears the
            // selection) or a live rubber band. Ctrl keeps the current
            // selection and adds to it instead of replacing it.
            self->m_marquee = true;
            self->m_marqueeMoved = false;
            self->m_marqueeAdd = (wp & MK_CONTROL) != 0;
            if (self->m_marqueeAdd) {
                self->m_marqueeBase = self->m_selectedPaths;
            } else {
                self->m_marqueeBase.clear();
                self->ClearSelection();
            }
            self->m_marqAX = self->m_marqCX = mx;
            self->m_marqAY = self->m_marqCY = my;
            SetCapture(hwnd);
            self->Redraw();
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        // Belt and suspenders alongside WM_SETCURSOR: mouse moves are routed
        // straight to this window, so setting the shape here works even if the
        // desktop list parent swallows WM_SETCURSOR. lParam is client coords.
        if (self) {
            int ht = self->m_resizeMask
                ? self->m_resizeHT
                : EdgeHit(hwnd, self->m_collapsed, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            SetCursor(LoadCursorW(nullptr, CursorForHT(ht)));
        }
        // List-mode scrollbar thumb drag
        if (self && self->m_scrollbarDrag) {
            float dy = (float)GET_Y_LPARAM(lp) - self->m_scrollDragStartY;
            float maxS = self->ListMaxScroll();
            if (maxS > 0) {
                const auto& app = self->m_render->Appearance();
                float rowH = app.iconSize * 2.6f;
                float bt = self->ContentTop();
                float h = (float)self->m_render->Height();
                float sbPad = 2.0f;
                float trackH = h - bt - sbPad * 2;
                float totalH = (float)self->VisibleCount() * rowH;
                float contentH = h - bt;
                float thumbH = (std::max)(20.0f, trackH * contentH / totalH);
                float range = trackH - thumbH;
                if (range > 0) {
                    self->m_listScroll = self->m_scrollDragStartOffset + (dy / range) * maxS;
                    self->ClampListScroll();
                    self->Redraw();
                }
            }
            return 0;
        }

        // Hover plate (Explorer style). No capture is active in this branch,
        // so re-arm leave tracking on every move and hit-test the cursor.
        if (self && !self->m_collapsed && !self->m_moving &&
            !self->m_resizeMask && self->m_dragIcon < 0 && !self->m_marquee) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            int h = self->HitTestIcon(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (h != self->m_hoverIcon) {
                self->m_hoverIcon = h;
                self->Redraw();
            }
        }
        // Chevron hover. Works expanded AND collapsed (the arrow is the only
        // control a collapsed fence has), and also while the button is
        // pressed — capture keeps the moves coming here, so the plate follows
        // the cursor on and off the button (under-capture coordinates may be
        // negative, hence the explicit >= 0 checks).
        if (self && !self->m_renaming && !self->m_moving &&
            !self->m_resizeMask && self->m_dragIcon < 0 && !self->m_marquee) {
            int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
            int th = (int)self->m_render->Appearance().titleH;
            int arrowL = (int)(self->m_render->Width() - self->m_render->Appearance().titleH);
            bool in = my >= 0 && my < th && mx >= arrowL;
            if (in != self->m_chevronHover) {
                self->m_chevronHover = in;
                self->Redraw();
            }
        }
        if (self && self->m_marquee) {
            int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
            if (!self->m_marqueeMoved) {
                // The same click/drag discrimination icon drags use: a press
                // that never travels the system drag distance stays a plain
                // click (settled in WM_LBUTTONUP).
                int t = GetSystemMetrics(SM_CXDRAG);
                if (abs(mx - self->m_marqAX) < t && abs(my - self->m_marqAY) < t)
                    return 0;
                self->m_marqueeMoved = true;
            }
            self->m_marqCX = mx;
            self->m_marqCY = my;
            self->UpdateMarqueeSelection();
            self->Redraw();
            return 0;
        }
        if (self && self->m_moving) {
            // Click/drag discrimination for the title press: a release that
            // never traveled SM_CXDRAG is a plain click (on a collapsed fence
            // that click expands it — settled in WM_LBUTTONUP).
            int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
            if (!self->m_pressMoved) {
                int t = GetSystemMetrics(SM_CXDRAG);
                if (abs(mx - self->m_pressX) >= t || abs(my - self->m_pressY) >= t)
                    self->m_pressMoved = true;
            }
            // Cursor to parent-client coords, keep the grabbed offset
            POINT p{ mx, my };
            ClientToScreen(hwnd, &p);
            HWND parent = GetParent(hwnd);
            if (parent) ScreenToClient(parent, &p);
            SetWindowPos(hwnd, nullptr, p.x - self->m_moveOffX, p.y - self->m_moveOffY,
                0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        if (self && self->m_resizeMask) {
            POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ClientToScreen(hwnd, &p);
            HWND parent = GetParent(hwnd);
            if (parent) ScreenToClient(parent, &p);
            int dx = p.x - self->m_resizePtX;
            int dy = p.y - self->m_resizePtY;
            RECT r = self->m_resizeStart;
            if (self->m_resizeMask & 1) r.left   += dx;
            if (self->m_resizeMask & 2) r.right  += dx;
            if (self->m_resizeMask & 4) r.top    += dy;
            if (self->m_resizeMask & 8) r.bottom += dy;
            // Enforce a sane minimum so the fence can't vanish
            int th  = (int)(self->m_render->Appearance().titleH + 0.5f);
            int minW = (int)(120.0f * DpiScale() + 0.5f);
            int minH = th + (int)(40.0f * DpiScale() + 0.5f);
            if (r.right - r.left < minW) {
                if (self->m_resizeMask & 1) r.left = r.right - minW;
                else                        r.right = r.left + minW;
            }
            if (r.bottom - r.top < minH) {
                if (self->m_resizeMask & 4) r.top = r.bottom - minH;
                else                        r.bottom = r.top + minH;
            }
            SetWindowPos(hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        if (self && self->m_dragIcon >= 0) {
            int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
            if (!self->m_dragMoved) {
                // Explorer's click/drag discrimination: an icon press only
                // becomes a drag once the cursor travels the system drag
                // distance. Until then the icons stay put (click = select).
                int t = GetSystemMetrics(SM_CXDRAG);
                if (abs(mx - self->m_dragGrabX) < t &&
                    abs(my - self->m_dragGrabY) < t)
                    return 0;
                self->m_dragMoved = true;
                // Stale overlaps under the lifted group resolve at drag
                // start — squatters are pushed to the nearest free cells.
                self->PushGroupOut();
            }
            // The whole selection rides ONE shared delta (Explorer group
            // drag). For a single-icon selection this is exactly the old
            // offset-follow behavior.
            float dx = (float)(mx - self->m_dragGrabX);
            float dy = (float)(my - self->m_dragGrabY);
            for (const auto& d : self->m_dragGroup) {
                if (d.idx < 0 || d.idx >= (int)self->m_icons.size()) continue;
                float nx = d.x + dx, ny = d.y + dy;
                self->ClampIconPos(nx, ny);
                self->m_icons[d.idx].x = floorf(nx + 0.5f);   // whole pixels
                self->m_icons[d.idx].y = floorf(ny + 0.5f);
            }
            // Push-aside: whenever the pressed icon enters a new cell, sweep
            // every cell the group now covers — bystanders flow out of the
            // way mid-drag, along the row in the drag's direction.
            int hovered = self->CellOf(self->m_icons[self->m_dragIcon]);
            if (hovered != self->m_dragHoverCell) {
                FenceWindow::IconGrid g = self->GetIconGrid();
                int dc = (hovered % g.cols) - (self->m_dragHoverCell % g.cols);
                if (dc != 0) self->m_dragDirX = dc > 0 ? 1 : -1;
                self->m_dragHoverCell = hovered;
                self->PushGroupOut();
            }
            self->Redraw();
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (!self) return 0;
        if (self->m_scrollbarDrag) {
            self->m_scrollbarDrag = false;
            ReleaseCapture();
            return 0;
        }
        if (self->m_chevronDown) {
            self->m_chevronDown = false;
            ReleaseCapture();
            POINT p; GetCursorPos(&p); ScreenToClient(hwnd, &p);
            int th = (int)self->m_render->Appearance().titleH;
            int arrowL = (int)(self->m_render->Width() - self->m_render->Appearance().titleH);
            bool inside = p.y >= 0 && p.y < th && p.x >= arrowL;
            self->m_chevronHover = inside;
            if (inside) self->ToggleCollapse();
            else        self->Redraw();
            return 0;
        }
        bool wasBusy = self->m_dragIcon >= 0 || self->m_moving ||
                       self->m_resizeMask || self->m_marquee;
        if (self->m_marquee) {
            // A press that never passed the drag threshold was a plain click
            // on empty ground — the selection was already cleared at press
            // time (additive mode left it alone). A real sweep keeps the
            // band's final selection. Either way: drop the band's state.
            bool plainClick = !self->m_marqueeMoved && !self->m_marqueeAdd;
            self->m_marquee = false;
            self->m_marqueeMoved = false;
            self->m_marqueeAdd = false;
            self->m_marqueeBase.clear();
            // Plain blank click (no Ctrl, no sweep): opens the zoom, or —
            // while already zoomed — collapses it again (the dimmed shade
            // is the other dismissal path).
            if (plainClick) {
                if (self->m_zoomed) self->ZoomRequestClose();
                else               self->ZoomOpen();
            } else {
                self->Redraw();   // erase a real rubber band (a plain click has none)
            }
        }
        if (self->m_dragIcon >= 0) {
            // Mouse capture routed every move to the drag path, so the hover
            // plate is stale — re-derive it from the live cursor before the
            // settle redraw paints the final frame.
            POINT p; GetCursorPos(&p); ScreenToClient(hwnd, &p);
            self->m_hoverIcon = self->HitTestIcon(p.x, p.y);
            self->SettleDragGroup();
        }
        bool geomChanged = self->m_moving || self->m_resizeMask;
        // A collapsed fence's title press that never became a drag was a
        // plain click → expand it (the chevron already toggles both ways;
        // this gives the whole pill one-click open).
        bool plainTitleClick = self->m_moving && self->m_collapsed && !self->m_pressMoved;
        self->m_moving = false;
        self->m_pressMoved = false;
        self->m_resizeMask = 0;
        if (wasBusy) ReleaseCapture();
        if (plainTitleClick)
            self->ToggleCollapse();                 // expand; saves on its own
        else if (geomChanged) Config::MarkDirty();   // fence moved / resized
        return 0;
    }
    case WM_CAPTURECHANGED: {
        // Capture stolen mid-gesture: settle the group instead of leaving it
        // overlapping somebody, and drop any half-drawn rubber band. (Our own
        // ReleaseCapture in WM_LBUTTONUP gets here with the drag/marquee
        // state already cleared — no double settle.)
        if (self) {
            if (self->m_chevronDown) {   // press disarmed without a release
                self->m_chevronDown = false;
                self->Redraw();
            }
            if (self->m_dragIcon >= 0) self->SettleDragGroup();
            if (self->m_marquee) {
                self->m_marquee = false;
                self->m_marqueeMoved = false;
                self->m_marqueeAdd = false;
                self->m_marqueeBase.clear();
                self->Redraw();
            }
            if (self->m_moving || self->m_resizeMask) Config::MarkDirty();
            self->m_moving = false;
            self->m_pressMoved = false;
            self->m_resizeMask = 0;
        }
        return 0;
    }
    case WM_MOUSELEAVE: {
        // Cursor left the fence: drop the hover states. (While a capture is
        // active — drag/move/resize/chevron press — leave notifications are
        // suppressed.)
        if (self && self->m_dragIcon < 0 &&
            !self->m_moving && !self->m_resizeMask && !self->m_marquee) {
            bool dirty = false;
            if (self->m_hoverIcon >= 0) { self->m_hoverIcon = -1;       dirty = true; }
            if (self->m_chevronHover)   { self->m_chevronHover = false; dirty = true; }
            if (dirty) self->Redraw();
        }
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        if (!self) return 0;
        int mx = GET_X_LPARAM(lp);
        int my = GET_Y_LPARAM(lp);
        if (my < (int)self->m_render->Appearance().titleH) {
            // Ignore the collapse-arrow box so it doesn't trigger a rename
            int arrowL = (int)(self->m_render->Width() - self->m_render->Appearance().titleH);
            if (mx >= arrowL) return 0;
            self->EnterRename();
            return 0;
        }
        // Search-box strip: no action on double-click
        if (self->IsMapped() && my < (int)self->ContentTop()) return 0;
        // Double-click on an icon → open it
        int hit = self->HitTestIcon(mx, my);
        if (hit >= 0) {
            ShellExecuteW(hwnd, L"open", self->m_icons[hit].path.c_str(),
                          nullptr, nullptr, SW_SHOW);
        }
        return 0;
    }
    case WM_RBUTTONUP: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ClientToScreen(hwnd, &pt);
        POINT cli{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };

        // Detect which icon was clicked (same geometry as rendering)
        int iconHit = -1;
        if (self && !self->m_icons.empty())
            iconHit = self->HitTestIcon(cli.x, cli.y);

        // Right-click selects too, so the menu appears over the selection
        // plate exactly like the Explorer desktop. A press on an icon that is
        // ALREADY selected keeps the whole multi-selection (Explorer keeps the
        // group intact so the menu verbs apply to every member); otherwise
        // the selection collapses to the pressed icon.
        if (self && iconHit >= 0) {
            if (!self->IsPathSelected(self->m_icons[iconHit].path))
                self->SelectOnly(iconHit);
            self->m_hoverIcon = iconHit;
            self->Redraw();
        }

        // Remove icon `idx`, repairing selection/hover bookkeeping.
        auto eraseIcon = [&](int idx) {
            if (!self || idx < 0 || idx >= (int)self->m_icons.size()) return;
            // Drop the removed path from the selection set so no entry
            // dangles pointing at a dead icon.
            auto& sel = self->m_selectedPaths;
            sel.erase(std::remove(sel.begin(), sel.end(), self->m_icons[idx].path),
                      sel.end());
            self->m_icons.erase(self->m_icons.begin() + idx);
            if (self->m_hoverIcon == idx)      self->m_hoverIcon = -1;
            else if (self->m_hoverIcon > idx)  self->m_hoverIcon--;
            if (self->SearchActive()) self->RebuildFilter();
            self->Redraw();
            Config::MarkDirty();
        };

        // Icon → show the real Explorer context menu (Open / Cut / Copy /
        // Delete / Properties …) with our fence-only verb appended.
        if (self && iconHit >= 0) {
            bool invoked = false, removeChosen = false, renameChosen = false;
            if (RunShellContextMenu(hwnd, self->m_icons[iconHit].path,
                                    self->m_icons[iconHit].name, pt,
                                    invoked, removeChosen, renameChosen)) {
                if (renameChosen) {
                    // Rename right here in the fence instead of letting the
                    // shell rename the desktop/Explorer copy.
                    self->SelectOnly(iconHit);
                    self->EnterIconRename(iconHit);
                } else if (removeChosen) {
                    eraseIcon(iconHit);
                } else if (invoked) {
                    // If the shell command got rid of the file (e.g. Delete),
                    // don't leave a dangling icon behind in the fence.
                    if (GetFileAttributesW(self->m_icons[iconHit].path.c_str())
                            == INVALID_FILE_ATTRIBUTES)
                        eraseIcon(iconHit);
                }
                return 0;
            }
            // Shell menu unavailable — fall through to the fence menu below.
        }

        // Fence-level menu (background, or fallback when the shell menu fails)
        HMENU menu = CreatePopupMenu();
        if (iconHit >= 0 && self) {
            std::wstring rm = std::wstring(Loc(L"Remove", L"移除")) + L" \"" +
                              self->m_icons[iconHit].name + L"\"";
            AppendMenuW(menu, MF_STRING, 3, rm.c_str());
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        }

        AppendMenuW(menu, MF_STRING, 1, Loc(L"Rename", L"重命名"));
        AppendMenuW(menu, MF_STRING, 4, Loc(L"Appearance Settings...", L"外观设置..."));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

        // Explorer's own 新建 submenu, queried straight into the fence menu.
        // Mapped fences create into their folder; unmapped ones point the
        // handler at the desktop PIDL itself so the entries match the system
        // desktop menu exactly (e.g. 快捷方式).
        PIDLIST_ABSOLUTE pidlNewFolder = nullptr;
        if (self && self->IsMapped())
            SHParseDisplayName(self->m_sourceFolder.c_str(), nullptr,
                               &pidlNewFolder, 0, nullptr);
        else
            SHGetSpecialFolderLocation(nullptr, CSIDL_DESKTOP, &pidlNewFolder);
        CComPtr<IContextMenu> spNewCM;
        UINT newMenuPos = 0;
        bool newMenuAdded = false;
        if (pidlNewFolder &&
            SUCCEEDED(CreateNewMenuHandler(pidlNewFolder, &spNewCM))) {
            newMenuPos = (UINT)GetMenuItemCount(menu);
            HRESULT hrQ = spNewCM->QueryContextMenu(menu, newMenuPos,
                    (UINT)kCmdNewFirst, 0x7FFF, CMF_NORMAL);
            NewDbg(L"QueryContextMenu=0x%08X newMenuPos=%u", (unsigned)hrQ, newMenuPos);
            if (SUCCEEDED(hrQ)) {
                // The handler fills its submenu lazily on WM_INITMENUPOPUP —
                // the same message Explorer forwards before showing it.
                HMENU hNewSub = GetSubMenu(menu, newMenuPos);
                if (hNewSub) {
                    CComPtr<IContextMenu3> cm3;
                    CComPtr<IContextMenu2> cm2;
                    spNewCM->QueryInterface(IID_PPV_ARGS(&cm3));
                    spNewCM->QueryInterface(IID_PPV_ARGS(&cm2));
                    LRESULT lr = 0;
                    if (cm3)      cm3->HandleMenuMsg2(WM_INITMENUPOPUP, (WPARAM)hNewSub, 0, &lr);
                    else if (cm2) cm2->HandleMenuMsg(WM_INITMENUPOPUP, (WPARAM)hNewSub, 0);
                }
                NewDbg(L"--- menu tree after New handler (unmapped=%d) ---",
                       self && self->IsMapped() ? 0 : 1);
                NewDbgDumpMenu(menu, 0);
                NewDbg(L"--- end menu tree ---");
                newMenuAdded = true;
                s_activeNewCM = spNewCM;
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            } else {
                spNewCM.Release();
            }
        }

        AppendMenuW(menu, MF_STRING, 2, Loc(L"Delete Fence", L"删除围栏"));

        // Sort-by submenu (mapped fences only)
        if (self && !self->m_sourceFolder.empty()) {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            HMENU sortMenu = CreatePopupMenu();
            auto addSortItem = [&](UINT id, int col, const wchar_t* en, const wchar_t* zh) {
                UINT flags = MF_STRING;
                if (self->m_sortColumn == col) flags |= MF_CHECKED;
                AppendMenuW(sortMenu, flags, id, Loc(en, zh));
            };
            addSortItem(10, 0, L"Name", L"名称");
            addSortItem(11, 1, L"Date modified", L"修改日期");
            addSortItem(12, 3, L"Type", L"类型");
            addSortItem(13, 2, L"Size", L"大小");
            AppendMenuW(sortMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(sortMenu, MF_STRING | (self->m_sortAscending ? MF_CHECKED : 0),
                        14, Loc(L"Ascending", L"升序"));
            AppendMenuW(sortMenu, MF_STRING | (!self->m_sortAscending ? MF_CHECKED : 0),
                        15, Loc(L"Descending", L"降序"));
            AppendMenuW(menu, MF_POPUP, (UINT_PTR)sortMenu,
                        Loc(L"Sort by", L"排序方式"));
        }

        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 5, Loc(L"Map Folder...", L"映射文件夹..."));
        if (self && !self->m_sourceFolder.empty())
            AppendMenuW(menu, MF_STRING, 6, Loc(L"Unmap Folder", L"取消映射"));

        // Fluent glyph icons. The set owns the HBITMAPs until the menu
        // interaction below has fully returned (MIIM_BITMAP is by reference).
        GlyphBitmapSet glyphs;
        {
            int gsz = MenuGlyphSizePx();
            auto icon = [&](UINT id, MenuGlyph g) {
                HBITMAP b = glyphs.add(g, gsz);
                if (!b) return;
                MENUITEMINFOW mii = { sizeof(mii) };
                mii.fMask = MIIM_BITMAP;
                mii.hbmpItem = b;
                SetMenuItemInfoW(menu, id, FALSE, &mii);
            };
            if (iconHit >= 0) icon(3, MenuGlyph::Remove);
            icon(1, MenuGlyph::Pencil);    // Rename
            icon(4, MenuGlyph::Sliders);   // Appearance Settings
            // The New popup is addressed by position: the shell inserts it
            // without a command id of its own.
            if (newMenuAdded) {
                HBITMAP b = glyphs.add(MenuGlyph::Plus, gsz);
                if (b) {
                    MENUITEMINFOW mii = { sizeof(mii) };
                    mii.fMask = MIIM_BITMAP;
                    mii.hbmpItem = b;
                    SetMenuItemInfoW(menu, newMenuPos, TRUE, &mii);
                }
            }
            icon(2, MenuGlyph::Trash);     // Delete Fence
            icon(5, MenuGlyph::Folder);    // Map Folder
            if (self && !self->m_sourceFolder.empty())
                icon(6, MenuGlyph::Remove); // Unmap Folder
        }

        SetForegroundWindow(hwnd);
        // Prefer the Win11 Fluent rendering; -1 means it could not be built
        // (empty model / D2D failure) → classic menu, the same fallback the
        // shell icon menu uses. The Fluent loop swallows the dismiss click,
        // which is exactly what a context menu must do over a surface where
        // a stray press would otherwise start a fence drag.
        int cmd = FluentMenu::Run(hwnd, menu, pt);
        if (cmd < 0)
            cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                pt.x, pt.y, 0, hwnd, nullptr);

        if (self) {
            switch (cmd) {
            case 1: self->EnterRename(); break;
            case 2:
                if (s_owner)
                    PostMessage(s_owner, WM_FENCE_DELETE, (WPARAM)hwnd, 0);
                break;
            case 3: eraseIcon(iconHit); break;
            case 4: SettingsPanel::Run(self); break;
            case 5: {
                CComPtr<IFileOpenDialog> pDlg;
                if (SUCCEEDED(pDlg.CoCreateInstance(CLSID_FileOpenDialog))) {
                    pDlg->SetOptions(FOS_PICKFOLDERS | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
                    if (SUCCEEDED(pDlg->Show(hwnd))) {
                        CComPtr<IShellItem> psi;
                        if (SUCCEEDED(pDlg->GetResult(&psi))) {
                            LPWSTR path = nullptr;
                            if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                                int res = MessageBoxW(hwnd,
                                    Loc(L"Mapping a folder will clear the current fence contents. Continue?",
                                        L"映射文件夹将清空当前围栏内的内容，是否继续？"),
                                    Loc(L"Map Folder", L"映射文件夹"),
                                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
                                if (res == IDYES) {
                                    self->GetRender().Appearance().displayMode = 1;
                                    self->GetRender().RebuildStyles();
                                    self->m_loading = true;
                                    self->Redraw();
                                    // Force paint before blocking enumeration
                                    self->GetRender().BeginDraw();
                                    self->GetRender().EndDraw();
                                    self->GetRender().Present(self->m_hwnd, self->m_x, self->m_y);
                                    self->MapToFolder(path);
                                    self->m_loading = false;
                                }
                                CoTaskMemFree(path);
                            }
                        }
                    }
                }
                break;
            }
            case 6: self->UnmapFolder(); break;
            case 10: case 11: case 12: case 13: {
                // Column IDs map: 10=Name(0), 11=Date(1), 12=Type(3), 13=Size(2)
                static const int colMap[] = { 0, 1, 3, 2 };
                int col = colMap[cmd - 10];
                bool natAsc = (col == 0 || col == 3);   // Name/Type default ascending
                self->SetSort(col, natAsc);
                break;
            }
            case 14: self->SetSort(self->m_sortColumn, true); break;
            case 15: self->SetSort(self->m_sortColumn, false); break;
            default:
                // An entry from the shell 新建 submenu: hand it back to the
                // handler, which creates the file in the target folder.
                if (cmd > kCmdNewFirst && cmd <= 0x7FFF && spNewCM && self) {
                    std::wstring targetDir = self->m_sourceFolder;
                    if (targetDir.empty()) {
                        wchar_t buf[MAX_PATH] = {};
                        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY,
                                                       nullptr, 0, buf)))
                            targetDir = buf;
                    }
                    std::vector<std::wstring> before, after;
                    SnapshotFolderNames(targetDir, before);
                    CMINVOKECOMMANDINFO ici = {};
                    ici.cbSize = sizeof(ici);
                    ici.hwnd = hwnd;
                    ici.lpVerb = MAKEINTRESOURCEA(cmd - kCmdNewFirst);
                    ici.nShow = SW_SHOWNORMAL;
                    HRESULT hrInv = spNewCM->InvokeCommand(&ici);
                    NewDbg(L"InvokeCommand=0x%08X cmd=%d", (unsigned)hrInv, cmd);
                    SnapshotFolderNames(targetDir, after);
                    std::wstring newPath;
                    for (const auto& n : after) {
                        if (std::find(before.begin(), before.end(), n) == before.end()) {
                            newPath = targetDir + L"\\" + n;
                            break;
                        }
                    }
                    NewDbg(L"newPath=\"%s\"", newPath.c_str());
                    if (!newPath.empty()) {
                        if (self->IsMapped()) {
                            // The watcher re-enumerates on its own; refresh
                            // synchronously so the new file is visible and
                            // can enter rename mode right away.
                            self->ClearSearch();
                            self->RefreshFromSource();
                        } else {
                            self->AddIcon(newPath.substr(newPath.find_last_of(L'\\') + 1),
                                          newPath, (float)cli.x, (float)cli.y);
                        }
                        for (int i = 0; i < (int)self->m_icons.size(); i++) {
                            if (lstrcmpiW(self->m_icons[i].path.c_str(), newPath.c_str()) == 0) {
                                self->SelectOnly(i);
                                self->EnterIconRename(i);
                                break;
                            }
                        }
                    }
                }
                break;
            }
        }
        // Destroy only after dispatch: the shell 新建 handler keeps its
        // cached submenu alive until InvokeCommand runs — destroying the
        // menu first makes it fail with E_FAIL.
        DestroyMenu(menu);
        s_activeNewCM = nullptr;
        if (pidlNewFolder) CoTaskMemFree(pidlNewFolder);
        return 0;
    }

    case WM_MOUSEWHEEL: {
        if (!self) return 0;
        float maxS = self->ListMaxScroll();
        if (maxS <= 0) return 0;
        float rowH = self->m_render->Appearance().iconSize * 2.6f;
        int steps = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        self->m_listScroll -= steps * rowH * 3;
        self->ClampListScroll();
        self->Redraw();
        return 0;
    }
    case WM_CHAR:
        if (self && self->m_renaming && wp >= 32) {
            self->m_renameBuf += (wchar_t)wp;
            self->Redraw();
        } else if (self && self->m_renameIcon >= 0 && wp >= 32) {
            NewDbg(L"CHAR wp=%d", (int)wp);
            if (self->m_iconRenamePristine) {
                self->m_iconRenameBuf.clear();
                self->m_iconRenameCaret = 0;
                self->m_iconRenamePristine = false;
            }
            self->m_iconRenameBuf.insert(self->m_iconRenameCaret, 1, (wchar_t)wp);
            self->m_iconRenameCaret++;
            self->Redraw();
        } else if (self && self->m_searchFocused) {
            if (wp == 0x08) {   // backspace
                if (!self->m_searchText.empty()) {
                    self->m_searchText.pop_back();
                    self->RebuildFilter();
                }
            } else if (wp >= 32) {
                self->m_searchText += (wchar_t)wp;
                self->RebuildFilter();
            }
        } else if (self && self->IsMapped() && wp >= 32) {
            self->FocusSearch();
            self->m_searchText += (wchar_t)wp;
            self->RebuildFilter();
        }
        return 0;

    case WM_KEYDOWN:
        if (self && self->m_renaming) {
            if (wp == VK_RETURN)  { self->ExitRename(true); }
            if (wp == VK_ESCAPE)  { self->ExitRename(false); }
            if (wp == VK_BACK && !self->m_renameBuf.empty()) {
                self->m_renameBuf.pop_back();
                self->Redraw();
            }
        } else if (self && self->m_renameIcon >= 0) {
            NewDbg(L"KEYDOWN wp=%d", (int)wp);
            auto& buf = self->m_iconRenameBuf;
            auto& caret = self->m_iconRenameCaret;
            // Explorer's rename-box editing: the initial select-all consumes
            // the first typed/backspaced input; Left/Right/Home/End only
            // collapse it to an edge of the selection.
            if (wp == VK_RETURN) {
                self->ExitIconRename(true);
            } else if (wp == VK_ESCAPE) {
                self->ExitIconRename(false);
            } else if (wp == VK_BACK) {
                if (self->m_iconRenamePristine) {
                    buf.clear(); caret = 0; self->m_iconRenamePristine = false;
                } else if (caret > 0) { buf.erase(caret - 1, 1); caret--; }
                self->Redraw();
            } else if (wp == VK_DELETE) {
                if (self->m_iconRenamePristine) {
                    buf.clear(); caret = 0; self->m_iconRenamePristine = false;
                } else if (caret < buf.size()) buf.erase(caret, 1);
                self->Redraw();
            } else if (wp == VK_LEFT) {
                if (self->m_iconRenamePristine) { self->m_iconRenamePristine = false; caret = 0; }
                else if (caret > 0) caret--;
                self->Redraw();
            } else if (wp == VK_RIGHT) {
                if (self->m_iconRenamePristine) { self->m_iconRenamePristine = false; caret = buf.size(); }
                else if (caret < buf.size()) caret++;
                self->Redraw();
            } else if (wp == VK_HOME) {
                self->m_iconRenamePristine = false; caret = 0;
                self->Redraw();
            } else if (wp == VK_END) {
                self->m_iconRenamePristine = false; caret = buf.size();
                self->Redraw();
            }
        } else if (self && self->m_searchFocused) {
            if (wp == VK_ESCAPE) { self->ClearSearch(); }
        } else if (self && self->IsMapped() && wp == 'F' &&
                   (GetKeyState(VK_CONTROL) & 0x8000)) {
            self->FocusSearch();
        }
        return 0;

    case WM_KILLFOCUS:
        if (self) {
            NewDbg(L"KILLFOCUS renaming=%d renameIcon=%d search=%d",
                   self->m_renaming, self->m_renameIcon, self->m_searchFocused);
            if (self->m_renaming)
                self->ExitRename(false);
            else if (self->m_renameIcon >= 0)
                self->ExitIconRename(true);   // Explorer commits on focus loss
            else if (self->m_searchFocused) {
                self->m_searchFocused = false;
                self->m_cursorVisible = false;
                if (self->m_cursorTimer) { KillTimer(hwnd, 1); self->m_cursorTimer = 0; }
                if (self->m_focusAttached) {
                    AttachThreadInput(GetCurrentThreadId(), self->m_focusAttached, FALSE);
                    self->m_focusAttached = 0;
                }
                self->Redraw();
            }
        }
        return 0;

    case WM_TIMER:
        if (!self) return 0;
        if (wp == 1 && (self->m_renaming || self->m_renameIcon >= 0 ||
                        self->m_searchFocused)) {
            self->m_cursorVisible = !self->m_cursorVisible;
            self->Redraw();
        } else if (wp == 2) {
            KillTimer(hwnd, 2);
            self->CaptureBackdrop();
        } else if (wp == 5) {
            KillTimer(hwnd, 5);
            self->RefreshFromSource();
        } else if (wp == kZoomTimerId) {
            self->ZoomAnimStep();
        }
        return 0;

    case WM_APP_ZOOM_TICK:
        if (self) self->ZoomAnimStep();
        return 0;

    case ZoomOverlay::kOutsideClickMessage:
        if (self) self->ZoomRequestClose();
        return 0;

    case WM_FENCE_REFRESH:
        if (self) SetTimer(hwnd, 5, 500, nullptr);
        return 0;

    case WM_SHOWWINDOW:
        // Win+D ("Show desktop") hides every top-level window that is not
        // part of the shell's desktop tree. A fence belongs *on* the desktop,
        // so resist any hide we did not cause ourselves (m_appHidden is set
        // by Hide()). The re-show is posted, not done in-place: by the time
        // the posted message runs, the hide sweep has finished, so the fence
        // cannot be hidden a second time.
        // (lp != 0 means the hide came from a parent state change — as a
        // desktop child we get those legitimately; only fight direct hides.)
        if (self && !wp && !lp && !self->m_appHidden)
            PostMessage(hwnd, WM_FENCE_RESHOW, 0, 0);
        return 0;

    case WM_FENCE_RESHOW:
        if (self && !self->m_appHidden && !IsWindowVisible(hwnd))
            ShowWindow(hwnd, SW_SHOWNA);   // don't steal focus from the desktop
        return 0;

    case WM_SYSCOMMAND:
        // Win+M minimizes every window; a toolwindow fence has no taskbar
        // button to bring it back, so simply refuse to minimize.
        if ((wp & 0xFFF0) == SC_MINIMIZE) return 0;
        return DefWindowProc(hwnd, msg, wp, lp);

    case WM_DESTROY:
        if (self) {
            // The zoom's hidden siblings must not stay hidden when this
            // fence (their reason for hiding) goes away.
            if (!s_teardown) self->ZoomRestoreOthers();
            else             self->m_zoomHidden.clear();
            if (self->m_zoomed || self->m_zoomOpening) ZoomOverlay::Close();
            self->m_zoomed = false;
            self->m_zoomOpening = false;
            self->m_zoomClosing = false;
            self->StopSourceWatch(); SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
        }
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}
