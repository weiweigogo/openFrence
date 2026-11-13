#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d2d1.h>
#include <wincodec.h>
#include <dwrite.h>
#include <atlbase.h>
#include <string>
#include <vector>
#include <unordered_map>

// Simple COM/OLE init RAII. OleInitialize is required for RegisterDragDrop
// and other OLE features (drag-and-drop, clipboard); it internally calls
// CoInitializeEx so COM interfaces work as well.
struct ComInit {
    ComInit()  { OleInitialize(nullptr); }
    ~ComInit() { OleUninitialize(); }
};

/// Live desktop icon glyph size, in physical pixels — the size the desktop
/// is *currently* drawing its icons at, which follows Ctrl+wheel zoom.
/// Measured out of explorer's address space via LVM_GETITEMRECT (the icon
/// rect height minus its fixed 4px vertical padding; calibrated against the
/// known anchors: medium icons = 48px glyph, jumbo plateau = 256px). The
/// registry IconSize is NOT consulted for the live value because explorer
/// persists it lazily — it stays stale throughout a wheel-zoom session and
/// only serves as fallback when the live read fails. Returns 0 when no size
/// could be determined at all.
int QueryDesktopIconSizePx();

/// Downsample + acrylic blur + saturation/noise, shared by the fence's
/// frosted backdrop and the zoom shade's pre-rendered dimmed wallpaper.
/// Returns opaque 32bpp BGRA pixels at ~1/4 resolution (outW/outH).
bool BuildBlurredBackdrop(const BYTE* bgrx, int w, int h,
                          std::vector<BYTE>& out, int& outW, int& outH);

struct FenceAppearance {
    // Alpha values are tuned for the frosted-glass backdrop: a lower alpha
    // lets the blurred desktop snapshot show through the panel tint.
    float bg[4]      = {0.08f, 0.08f, 0.10f, 0.55f};
    float title[4]   = {0.05f, 0.05f, 0.07f, 0.80f};
    float border[4]  = {1.0f, 1.0f, 1.0f, 0.10f};
    float sep[4]     = {1.0f, 1.0f, 1.0f, 0.06f};
    float text[4]    = {1.0f, 1.0f, 1.0f, 0.93f};
    float iconText[4]= {1.0f, 1.0f, 1.0f, 0.65f};
    float accent[4]  = {0.35f, 0.55f, 0.95f, 1.0f};
    float r          = 12.0f;
    float titleH     = 34.0f;
    float searchH    = 28.0f;   // search box height (mapped fences only)
    float fontSize   = 12.0f;
    float iconSize   = 11.0f;   // icon *label* font size
    int   titleAlign = 0;       // title text: 0 = left, 1 = center, 2 = right
    int   displayMode = 0;      // 0 = grid (icons in a grid), 1 = list (one per row)
    WCHAR fontName[64] = L"Segoe UI";

    // Icon cell metrics. Shared by rendering AND mouse hit-testing so the
    // icon you see is exactly the icon you click. All appearance values are
    // designed at 96 DPI and scaled to physical pixels by RenderContext, and
    // AdoptDesktopIconSize then snaps imgSize/cellW/cellH to the size the
    // desktop itself uses for its icons, so fence glyphs read the same size
    // as their desktop counterparts.
    float cellW      = 96.0f;   // icon cell width
    float cellH      = 74.0f;   // icon cell height
    float imgSize    = 48.0f;   // drawn glyph size (design fallback)
};

struct IconEntry {
    std::wstring name;
    std::wstring path;  // full file path for icon extraction
    std::wstring ext;   // extension incl. '.' (PathFindExtensionW), for Type sort
    ULONGLONG size = 0;
    FILETIME lastWrite = {};
    bool isDir = false;
    float x = 0.0f;     // cell top-left inside the fence client area; icons
    float y = 0.0f;     // rest on the icon grid (snapped on drop/release)
};

struct FenceViewState {
    const std::vector<int>* filter = nullptr;   // null = show all
    const std::wstring* searchText = nullptr;   // null = no search box
    bool caretOn = false;
    bool loading = false;
    // In-place icon rename: the index of the icon being renamed and its live
    // text. Its label is replaced by an Explorer-style edit box; renameCaret
    // is the insertion offset inside iconRenameText (-1 = caret hidden).
    int renameIcon = -1;
    const std::wstring* iconRenameText = nullptr;
    int renameCaret = -1;
};

class RenderContext {
public:
    RenderContext(UINT w, UINT h);
    ~RenderContext();

    bool BeginDraw();
    bool EndDraw();
    // hoverIcon is an index into `icons` (-1 = none) and renders the
    // Explorer-style hover plate. `selectedPaths` is the set of selected
    // icon paths (multi-select): each gets the selection plate, and
    // `dragActive` gives every member the translucent "picked up" look while
    // a group drag is live. `marquee` is the rubber-band rect (client px)
    // drawn while a left-drag selection is in progress; null = no band.
    // `chevronHover`/`chevronDown` drive the collapse button's states:
    // it brightens on hover and shows a caption-button-style plate while
    // hovered/pressed.
    bool DrawFence(const std::wstring& title, const std::vector<IconEntry>& icons,
                   bool renaming, bool dragOver = false, bool collapsed = false,
                   int hoverIcon = -1,
                   const std::vector<std::wstring>* selectedPaths = nullptr,
                   bool dragActive = false,
                   const RECT* marquee = nullptr,
                   bool chevronHover = false, bool chevronDown = false,
                   float scrollY = 0.0f,
                   const FenceViewState* view = nullptr,
                   // Zoom animation: draw at an explicit logical size (the
                   // fixed target canvas stays m_width/m_height), optionally
                   // skipping the (target-sized) backdrop texture.
                   bool noBackdrop = false,
                   float drawW = -1.0f, float drawH = -1.0f);
    bool Resize(UINT w, UINT h);
    bool Present(HWND hwnd, int x, int y);
    // Preload every icon at the CURRENT imgSize without drawing the card.
    void WarmIcons(const std::vector<IconEntry>& icons);
    // Present the top-left w×h of the fixed target at screen (x,y), without
    // resizing the render target. Used by the zoom flight.
    bool PresentCropped(HWND hwnd, int x, int y, int w, int h);

    /// Cheap style refresh: release and recreate only the brushes and text
    /// formats from the current FenceAppearance. The render target, DIB,
    /// WIC bitmap, icon cache and backdrop are left untouched, so this is
    /// safe to call at slider-drag frequency for live preview.
    void RebuildStyles();

    /// Adopt a new desktop icon glyph size (physical px, as measured by
    /// QueryDesktopIconSizePx) when it differs from the current one. The
    /// cell metrics scale with the glyph so spacing stays proportional, and
    /// the icon cache is dropped on a change because list selection depends
    /// on the glyph size. Early-outs unless the value actually moved, so it
    /// is safe to call on every poll tick. Returns true when the metrics
    /// changed (caller should relayout and repaint).
    bool AdoptDesktopIconSize(int physicalPx);

    /// Use captured desktop pixels as the frosted-glass backdrop.
    /// `bgrx` is top-down, w*h*4 bytes in B,G,R,X order; the pixels are
    /// blurred internally before being stored as a D2D bitmap brush.
    void SetBackdrop(const BYTE* bgrx, int w, int h);

    UINT Width()  const { return m_width; }
    UINT Height() const { return m_height; }
    FenceAppearance& Appearance() { return m_app; }

    // Raw access for custom drawing (the settings panel paints its own UI).
    ID2D1RenderTarget* Target() const { return m_d2dTarget.p; }
    IDWriteFactory* WriteFactory() const { return m_dwFactory.p; }
    bool HasBackdrop() const { return m_backdropBrush.p != nullptr; }
    ID2D1BitmapBrush* BackdropBrush() const { return m_backdropBrush.p; }

private:
    bool CreateBrushes();
    bool CreateTextFormats();
    void BuildChevronPath();
    void BuildTitleBarPaths(float w, float h);
    bool CopyWicToDib();
    bool CopyWicRegionToPresent(int w, int h);
    bool CreateDibDC();
    void ScaleAppearance();   // grow 96-DPI design metrics to physical pixels
    ID2D1Bitmap* LoadFileIcon(const std::wstring& filePath);
    // Extract one glyph from a shell image list. Returns null when the list
    // has nothing usable for the index. When cropPadded is set the glyph is
    // cropped to its opaque bounds if the entry pads small artwork inside a
    // much larger canvas (seen in the jumbo list for a few file types).
    CComPtr<ID2D1Bitmap> TryLoadIcon(int iconIndex, int imgList, bool cropPadded);
    // Wrap premultiplied BGRA pixels (96 DPI) into a D2D bitmap.
    CComPtr<ID2D1Bitmap> MakeBitmapFromPixels(const BYTE* bgra, int w, int h);

    float m_dpiX = 96.0f;
    float m_dpiY = 96.0f;

    CComPtr<ID2D1Factory>      m_d2dFactory;
    CComPtr<IWICImagingFactory> m_wicFactory;
    CComPtr<IDWriteFactory>     m_dwFactory;
    CComPtr<IWICBitmap>         m_wicBitmap;
    CComPtr<ID2D1RenderTarget>  m_d2dTarget;
    CComPtr<ID2D1SolidColorBrush> m_bgBrush, m_titleBrush, m_sepBrush;
    // Border: vertical gradient (brighter top edge reads as key light,
    // dimmer bottom) instead of a uniform 1px ring.
    CComPtr<ID2D1LinearGradientBrush> m_borderGradBrush;
    // Cached top-edge highlight strip (was a per-frame brush allocation in
    // DrawFence — redraws run at drag frequency).
    CComPtr<ID2D1SolidColorBrush> m_topHiBrush;
    CComPtr<ID2D1SolidColorBrush> m_textBrush, m_iconTextBrush, m_accentBrush;
    // Rubber-band marquee (accent color, light fill / stronger edge). Cached
    // because a live band repaints on every mouse move — no per-frame brushes.
    CComPtr<ID2D1SolidColorBrush> m_marqueeFillBrush, m_marqueeEdgeBrush;
    CComPtr<IDWriteTextFormat>  m_titleFormat, m_iconFormat, m_listFormat;
    CComPtr<ID2D1StrokeStyle>   m_chevronStroke;   // round caps/joins for the collapse arrow
    // Collapse arrow: ONE open path (its apex is a true round join — two
    // separate lines would double a round cap there and bulge). Points up;
    // DrawFence flips it for the collapsed state. Rebuilt with the styles
    // because its geometry follows the title-bar height.
    CComPtr<ID2D1PathGeometry>  m_chevronPath;
    // Title bar fill + top highlight strip as cached paths carrying the
    // window's top corner arcs — a flat rect would paint square corners over
    // the rounded body shape. Rebuilt with the styles and on Resize.
    CComPtr<ID2D1PathGeometry>  m_titleBarPath;
    CComPtr<ID2D1PathGeometry>  m_topHiPath;
    CComPtr<ID2D1SolidColorBrush> m_chevronBrush;  // dim idle, brightens on hover
    CComPtr<ID2D1SolidColorBrush> m_chevronPlate;  // caption-button hover/press plate

    HDC     m_memDC   = nullptr;
    HBITMAP m_dib     = nullptr;
    HGDIOBJ m_oldBmp  = nullptr;
    // Reusable current-size presentation DIB for the zoom flight. Allocated
    // at the target size once; each frame copies the top-left current region.
    HDC     m_presentDC   = nullptr;
    HBITMAP m_presentDib  = nullptr;
    void*   m_presentBits = nullptr;
    HGDIOBJ m_presentOld  = nullptr;
    UINT    m_presentW    = 0, m_presentH = 0;
    UINT    m_width   = 0, m_height = 0;
    float   m_pathW   = 0.0f, m_pathH = 0.0f;   // title-bar paths' size
    FenceAppearance m_app;

    std::unordered_map<std::wstring, CComPtr<ID2D1Bitmap>> m_iconCache;

    // Frosted-glass backdrop: blurred snapshot of the desktop behind the fence
    CComPtr<ID2D1Bitmap>      m_backdropBmp;
    CComPtr<ID2D1BitmapBrush> m_backdropBrush;

    // Measured size of the selected icon's label, cached so the accent label
    // box does not re-run IDWriteTextLayout on every frame (redraws fire at
    // drag frequency). Invalidated when the text format is rebuilt.
    std::wstring m_selLabelPath;
    float m_selLabelW = 0.0f, m_selLabelH = 0.0f;

    // Search box text width (cached per text value to avoid per-frame layout).
    std::wstring m_searchTextCache;
    float m_searchTextW = 0.0f;
};
