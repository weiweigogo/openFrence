#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>
#include <thread>
#include <atomic>
#include "render.h"

#define WM_FENCE_DELETE  (WM_APP + 100)
#define WM_FENCE_RESHOW  (WM_APP + 101)   // posted to undo an external hide (Win+D)
#define WM_FENCE_REFRESH (WM_APP + 102)   // source folder changed → re-enumerate

struct FenceData {
    std::wstring id, title;
    int x, y, w, h;
    bool visible = true;
    std::wstring sourceFolder;
    int sortCol = 0;        // 0=Name, 1=DateModified, 2=Size, 3=Type
    bool sortAsc = true;
};

class FenceWindow {
public:
    FenceWindow(const FenceData& data);
    ~FenceWindow();

    HWND Hwnd() const { return m_hwnd; }
    void Show();
    void Hide();
    void Redraw();
    void SetIcons(const std::vector<IconEntry>& entries);
    // atX/atY = desired cell *center* (drop point); the icon snaps to the
    // nearest free cell of the icon grid
    void AddIcon(const std::wstring& name, const std::wstring& path, float atX, float atY);
    /// Re-snap every icon onto the icon grid and resolve overlaps. Call after
    /// anything that moves the grid (resize, title-bar height change).
    void RelayoutIcons();
    /// Follow a desktop icon-size change (polled by the owner window, which
    /// measures the live size once per tick via QueryDesktopIconSizePx and
    /// passes it to every fence): adopt the new glyph/cell metrics, re-seat
    /// the icons, repaint. Skipped while a drag/move/resize gesture is live
    /// so it is never fought mid-gesture; the fence catches up on the next
    /// poll once the gesture ends.
    void SyncDesktopIconSize(int physicalPx);
    /// Debounced frosted-glass backdrop re-capture (~150 ms, on the fence's
    /// own timer-2). The owner window fans this out on WM_DISPLAYCHANGE /
    /// WM_SETTINGCHANGE: those broadcasts only reach top-level windows, and
    /// fences are children of explorer's icon list, so they never see them.
    /// Safe to call repeatedly — SetTimer restarts the countdown.
    void ScheduleBackdropRefresh();
    /// Put the fence back at its saved screen position when something moved
    /// it without a user gesture — the desktop icon list shifts when the
    /// virtual screen origin moves (monitor plugged in left/above), and a
    /// child keeps parent-relative coordinates, so it would drift with the
    /// parent. An anchor that fell off-screen (its monitor unplugged) is
    /// clamped into the nearest monitor's work area. Called from the owner
    /// window's 300ms tick; no-op while a move/resize/drag is live.
    void RestoreAnchor();
    void SetDragOver(bool over);
    FenceData GetData() const;
    RenderContext& GetRender() { return *m_render; }
    const std::vector<IconEntry>& Icons() const { return m_icons; }

    void EnterRename();
    void ExitRename(bool commit);
    /// In-place icon rename (Explorer style): an edit box replaces the icon
    /// label; commit renames the file on disk. Used after 新建 so the fresh
    /// file lands directly in edit mode.
    void EnterIconRename(int idx);
    void ExitIconRename(bool commit);

    void ToggleCollapse();
    bool IsCollapsed() const { return m_collapsed; }
    /// While collapsed, re-apply the window height so a live-edited title bar
    /// height shows immediately (no-op when expanded).
    void SyncCollapsedSize();
    /// Phone-folder zoom (transient, never persisted): a plain click on empty
    /// body ground flies the fence to a large centered rect while a dim
    /// blurred shade (ZoomOverlay) covers the rest of the desktop; a click on
    /// the shade flies it back. Drives its own animation on timer 6.
    void ZoomOpen();
    /// Dismiss request from the shade (a click on the dimmed backdrop).
    void ZoomRequestClose();
    /// Owner-timer watchdog entry point: force-finishes a stalled animation.
    void ZoomStallCheck();
    bool IsZoomed() const { return m_zoomed; }

    /// Suppresses the "restore the siblings" pass while main.cpp tears the
    /// fence list down (the neighbours are being destroyed too).
    static void SetTeardown(bool on) { s_teardown = on; }

    // Display mode save/restore: called by the settings panel before/after
    // switching between grid (0) and list (1) mode.
    void SaveGridLayout();
    void RestoreGridLayout();
    void FitToListHeight();     // resize window to show all icons in list mode

    // Folder mapping: replace fence contents with the files inside a folder,
    // and watch it for live updates.
    const std::wstring& SourceFolder() const { return m_sourceFolder; }
    void MapToFolder(const std::wstring& folderPath);
    void UnmapFolder();
    void RefreshFromSource();

    // Sort (mapped fences only): read/write the current sort column and
    // direction. SetSort applies immediately + relayouts; SetSortPreset
    // stores values for the next enumeration without re-sorting.
    void SetSort(int column, bool ascending);
    void SetSortPreset(int column, bool ascending);

    // Search / filter (mapped fences only)
    bool IsMapped() const { return !m_sourceFolder.empty(); }
    float ContentTop() const;
    int  SortColumn() const { return m_sortColumn; }
    bool SortAscending() const { return m_sortAscending; }
    void ClearSearch();

    static void RegisterClass(HINSTANCE hInst);
    /// The desktop window fences are parented to: Progman, or the WorkerW
    /// that hosts SHELLDLL_DefView. Being a child of the desktop tree is
    /// what keeps fences visible through Win+D (the shell only hides
    /// foreign top-level windows). Null when explorer is not up yet.
    static HWND DesktopHost();
    static void SetOwner(HWND owner) { s_owner = owner; }
    static HWND Owner() { return s_owner; }
    static void SetLanguage(int lang) { s_lang = lang; }
    static int  GetLanguage() { return s_lang; }
    static const wchar_t* Loc(const wchar_t* en, const wchar_t* zh);
    static const wchar_t* ClassName() { return L"OpenFencesFence"; }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void DrawContent(float drawW = -1.0f, float drawH = -1.0f,
                     bool noBackdrop = false);   // WIC target only, no Present
    void CaptureBackdrop();   // re-snapshot the desktop behind the fence
    int  HitTestIcon(int mx, int my) const;   // → icon index or -1
    void ClampIconPos(float& x, float& y) const;

    // ── Icon grid (desktop-style snap-to-grid placement) ──
    // The fence body is tiled with cellW×cellH cells; icons always rest on a
    // cell. Placement is free (any cell), but positions snap on release and
    // cells never hold two icons — same rules as the Windows desktop.
    struct IconGrid { int cols, rows; float x0, y0, cw, ch; };
    IconGrid GetIconGrid() const;                 // grid metrics for this fence
    int  CellOf(const IconEntry& e) const;        // nearest cell index (r*cols+c)
    void PlaceIconAt(IconEntry& e, int cell) const;   // seat on the cell top-left
    int  FindFreeCell(int prefer, const std::vector<int>& occupied) const;
    // Same-row-first, drag-direction-first variant used when pushing a cell's
    // occupant out of the way (desktop icons flow sideways along the row).
    int  FindPushCell(int prefer, const std::vector<int>& occupied, int dirX) const;
    std::vector<int> OccupiedCells(const std::vector<int>& excludes) const;
    void PushOccupantOf(int cell, const std::vector<int>& excludes, int dirX = 0);
    void PushGroupOut();             // clear bystanders from every cell the
                                     // dragged group currently covers
    void SettleDragGroup();          // snap every member + end the drag
    static HWND s_owner;
    static int s_lang;
    static bool s_teardown;

    HWND m_hwnd = nullptr;
    std::unique_ptr<RenderContext> m_render;
    std::wstring m_title, m_id;
    std::vector<IconEntry> m_icons;
    int m_x = 0, m_y = 0;

    bool m_dragOver = false;
    // True while the app itself has hidden the fence (tray "hide all").
    // External hides (Win+D) leave it false and are undone via WM_FENCE_RESHOW.
    bool m_appHidden = false;

    // In-fence icon dragging (mouse capture based). The press picks up the
    // WHOLE selection: one icon when the press collapsed the selection to a
    // single icon, a group when it landed on a member of a marquee
    // multi-selection. The group follows the cursor by one shared delta;
    // bystanders are pushed aside live and every member snaps to its own
    // free cell on release.
    int   m_dragIcon = -1;        // the pressed icon (gesture sentinel)
    int   m_dragHoverCell = -1;   // grid cell currently under the pressed icon
    int   m_dragDirX = 0;         // last horizontal drag direction (-1 / +1);
                                  // pushed occupants flow that way along the row
    struct DragStart { int idx; float x, y; };   // per-member lift-off state;
    std::vector<DragStart> m_dragGroup;          // indexes stay valid for the
                                                 // whole gesture (no reorders
                                                 // happen mid-drag)
    std::vector<int> m_dragExcludes;             // group indexes, for occupancy
    bool  m_dragMoved = false;    // cursor passed the system drag threshold
    int   m_dragGrabX = 0, m_dragGrabY = 0;   // LBUTTONDOWN point (client)

    // Explorer-style icon states. Hover is an index; the selection is a set
    // of paths (keyed, not indexed, so it survives the bring-to-front
    // reorder a drag performs).
    int m_hoverIcon = -1;
    std::vector<std::wstring> m_selectedPaths;
    bool IsPathSelected(const std::wstring& path) const;
    void ClearSelection();
    void SelectOnly(int idx);     // selection = { icon }; idx < 0 clears
    int  SelectedIndex() const;   // first selected icon's index, or -1
    void BringSelectedToFront();  // draw order: selected icons on top

    // Marquee (left-drag rubber band) multi-select, desktop style: the band
    // selects every icon whose cell it touches, live while it sweeps. Ctrl
    // at press time keeps the current selection and adds to it instead of
    // replacing it; a press that never passes the drag threshold is a plain
    // click and only clears the selection.
    bool m_marquee = false;
    bool m_marqueeMoved = false;   // cursor passed the system drag threshold
    bool m_marqueeAdd = false;     // Ctrl held at press → additive
    int  m_marqAX = 0, m_marqAY = 0;   // anchor (client)
    int  m_marqCX = 0, m_marqCY = 0;   // live cursor (client)
    std::vector<std::wstring> m_marqueeBase;   // selection at press (additive)
    RECT MarqueeRect() const;      // normalized, clamped to the fence body
    void UpdateMarqueeSelection();

    // Manual move / resize. A desktop-child window cannot ride DefWindowProc's
    // HTCAPTION drag or edge-size loops (those only work for top-level
    // windows), so both are reimplemented with mouse capture.
    bool m_moving = false;
    int  m_moveOffX = 0, m_moveOffY = 0;    // cursor offset from the origin
    int  m_resizeMask = 0;                  // 1=left 2=right 4=top 8=bottom
    int  m_resizeHT = 0;                    // HT* code → sizing cursor while dragging
    RECT m_resizeStart = {};                // rect at drag start (parent-client)
    int  m_resizePtX = 0, m_resizePtY = 0;  // cursor at drag start (parent-client)

    // Rename focus: while renaming we attach our input queue to the
    // foreground (explorer) thread so SetFocus can reach this desktop child.
    // 0 = not attached; otherwise the thread id we attached to.
    DWORD m_focusAttached = 0;

    bool m_renaming = false;
    std::wstring m_renameBuf;
    int  m_renameIcon = -1;           // icon being renamed (-1 = none)
    std::wstring m_iconRenameBuf;     // base name (extension kept separately)
    std::wstring m_iconRenameExt;
    size_t m_iconRenameCaret = 0;     // insertion point inside m_iconRenameBuf
    bool m_iconRenamePristine = false; // buffer not edited yet → first input
                                       // replaces it (Explorer select-all)
    bool m_cursorVisible = false;
    UINT_PTR m_cursorTimer = 0;

    // ── Display mode save/restore state ──
    int  m_gridH = 0;
    std::vector<std::pair<float, float>> m_gridPositions;

    // List-mode scroll
    float m_listScroll = 0;
    bool  m_scrollbarDrag = false;
    float m_scrollDragStartY = 0;
    float m_scrollDragStartOffset = 0;
    float ListMaxScroll() const;
    void  ClampListScroll();

    // ── Folder mapping (source folder + change notification) ──
    std::wstring m_sourceFolder;
    HANDLE m_changeHandle = INVALID_HANDLE_VALUE;
    HANDLE m_changeThread = nullptr;
    HANDLE m_stopEvent = nullptr;
    void StartSourceWatch();
    void StopSourceWatch();
    void EnumerateSourceFolder(bool keepPositions);
    static DWORD WINAPI ChangeThreadProc(LPVOID param);

    // ── Sort state (mapped fences only) ──
    int  m_sortColumn = 0;        // 0=Name, 1=DateModified, 2=Size, 3=Type
    bool m_sortAscending = true;
    bool m_sortInitialized = false;
    void ApplySort();

    // ── Search / filter (mapped fences only) ──
    bool m_loading = false;   // shown during folder enumeration
    std::wstring m_searchText;
    bool m_searchFocused = false;
    std::vector<int>  m_filterIdx;
    std::vector<BYTE> m_filterBits;  // O(1) visibility for marquee/drag
    bool SearchActive() const { return !m_searchText.empty(); }
    int  VisibleCount() const;
    void RebuildFilter();
    void RelayoutFiltered();
    void FocusSearch();

    // Collapse / expand (arrow in the title bar's top-right corner).
    // Caption-button semantics: the press only ARMS the button (plate shows);
    // the fence toggles on release, and only while the cursor is still over
    // the button — same as real window caption buttons.
    bool m_collapsed = false;
    bool m_chevronHover = false;
    bool m_chevronDown = false;
    int  m_expandedH = 0;       // full height remembered while collapsed
    // Icon metrics changed while collapsed: the grid cannot be rebuilt with
    // the window folded (its height is just the title bar), so the relayout
    // is deferred until the fence expands again.
    bool m_relayoutPending = false;

    // Zoom (transient, never persisted): see ZoomOpen/ZoomRequestClose.
    // While m_zoomed, GetData reports the remembered base rect so config
    // saves fired by unrelated actions never snapshot the fly-out geometry.
    bool m_zoomed      = false;
    bool m_zoomOpening = false;            // animation in flight (either way)
    bool m_zoomClosing = false;            // direction of that animation
    bool m_zoomInStep  = false;            // re-entrancy guard for the timer
    bool m_zoomBaseValid = false;          // m_zoomBase* holds a saveable rect
    int  m_zoomFrame   = 0;                // current anim frame
    int  m_zoomFrames  = 0;                // total frames of the current phase
    int  m_zoomFrom[4] = {0, 0, 0, 0};     // x,y,w,h lerp source (screen px)
    int  m_zoomTo[4]   = {0, 0, 0, 0};     // lerp target   (screen px)
    int  m_zoomBaseX   = 0;                // pre-zoom rect to restore / save
    int  m_zoomBaseY   = 0;
    int  m_zoomBaseW   = 0;
    int  m_zoomBaseH   = 0;
    // Phone-folder layout interpolation. The base metrics/positions are
    // remembered for restoration; the final metrics and centered positions
    // live in the per-icon tracks. Each animation frame sets the icon grid
    // to a lerp of the two, drawn at the current window size.
    bool m_iconScaled  = false;
    float m_zoomCellW  = 0.0f, m_zoomCellH = 0.0f, m_zoomImg = 0.0f, m_zoomIconSize = 0.0f;
    float m_zoomFinalCellW = 0.0f, m_zoomFinalCellH = 0.0f, m_zoomFinalImg = 0.0f;
    int   m_zoomTargetW = 0, m_zoomTargetH = 0;
    struct ZoomTrack {
        std::wstring path;
        float baseX = 0.0f, baseY = 0.0f;   // base card coords
        float zoomX = 0.0f, zoomY = 0.0f;   // target centered-grid coords
    };
    std::vector<ZoomTrack> m_zoomTracks;
    void ZoomAnimStep();
    void ZoomFinishOpen();
    void ZoomFinishClose();
    void ZoomRestoreIconMetrics();
    void ZoomPrepareTracks();                    // final metrics + per-icon tracks
    void ZoomApplyFrame(float e, int curW, int curH);
    void ZoomDrawFrame(int curW, int curH);
    // WM_TIMER is coalesced to ~31ms on current Windows even with 1ms timer
    // resolution. A tiny ticker thread posts exact ~8ms ticks to the UI.
    void ZoomStartTicker();
    void ZoomStopTicker();
    std::thread m_zoomTicker;
    std::atomic<bool> m_zoomTickerStop{ false };
    LONGLONG m_zoomStartQpc = 0;
    DWORD m_zoomStartTick = 0;
    void ZoomHideOthers();                 // other fences vanish while zoomed
    void ZoomRestoreOthers();
    std::vector<HWND> m_zoomHidden;         // only the siblings WE hid
    // A title-bar press that never traveled SM_CXDRAG was a plain click —
    // on a collapsed fence that click expands it (press-and-drag still moves).
    bool m_pressMoved = false;
    int  m_pressX = 0, m_pressY = 0;
};
