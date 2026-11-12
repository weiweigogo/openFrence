#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>

/// App-wide persistence.
///
/// State lives in ONE JSON file, %APPDATA%\openFences\config.json
/// (nlohmann/json): language, the "hide all fences" flag, the "hide desktop
/// icons" flag, and every fence with its layout, per-fence appearance and
/// icons. The autostart flag is deliberately NOT in this file — Windows
/// reads it from the HKCU Run key, so the registry value is the single
/// source of truth for it.
///
/// Writes are debounced (Config::MarkDirty) because many interactions fire
/// bursts of changes (drag = dozens of moves); a normal exit saves once more
/// synchronously so nothing rides on the debounce.
namespace Config {

/// Full path of the settings file (%APPDATA%\openFences\config.json).
const std::wstring& Path();

/// Serialize the current app state to Path(). Atomic: writes a .tmp file
/// first, then renames it over the target, so a crash mid-write can never
/// corrupt the existing settings. Returns false when nothing could be
/// written (missing %APPDATA%, sharing, …) — callers treat saves as
/// best-effort and never surface the failure to the user.
bool SaveNow();

/// Schedule a debounced SaveNow (~0.8 s after the last change) on the owner
/// window's timer-3. Safe to call from any interaction handler.
void MarkDirty();

/// Restore saved state: creates the persisted fences (layout + appearance +
/// icons) and applies language / hide-all. Returns false when the file is
/// missing or unreadable — the caller then falls back to the defaults, so a
/// corrupt file can never keep the app from starting.
bool LoadApp();

/// Save immediately, then open Explorer with the settings file selected —
/// the tray menu's "config file location" entry.
void RevealFile();

/// Autostart with Windows: presence of the "openFences" value under
/// HKCU\Software\Microsoft\Windows\CurrentVersion\Run (data = quoted exe
/// path). SetAutoStart(false) simply deletes the value.
bool AutoStartEnabled();
void SetAutoStart(bool enable);

} // namespace Config
