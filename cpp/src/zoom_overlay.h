#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

class FenceWindow;

/// Fullscreen dim/blur shade behind a zoomed fence — the phone-folder
/// treatment: the rest of the desktop fades under a darkened blurred
/// snapshot of the wallpaper, and a click anywhere on the shade flies the
/// fence back. The shade is a WS_CHILD sibling of the fences in explorer's
/// desktop tree, so the taskbar stays above it and other fences sit below.
///
/// Lifecycle: FenceWindow::ZoomOpen calls OpenFor, drives SetFade along its
/// animation, and must call Close when the zoom ends (or when the fence or
/// the desktop tree dies).
namespace ZoomOverlay {

/// The zooming fence posts this to itself when a click lands outside its rect.
constexpr UINT kOutsideClickMessage = WM_APP + 0x53;

/// Create (or replace) the shade under fence `f`. Captures the desktop once
/// for the blurred backdrop.
void OpenFor(FenceWindow* f);

/// Tint ramp driven by the fence's open/close animation. t = 0 invisible,
/// 1 = fully dimmed. Redraws the shade (cheap: one bitmap fill + one tint).
void SetFade(float t);

/// Destroy the shade. Safe to call when it does not exist or its window
/// already died with explorer's desktop tree.
void Close();

/// True while a shade window exists.
bool Active();

/// The shade window (null when inactive). The zooming fence re-inserts
/// itself directly above this handle so the pair flies together.
HWND Shade();

} // namespace ZoomOverlay
