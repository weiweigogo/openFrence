#include "config.h"
#include "fence_window.h"
#include <nlohmann/json.hpp>
#include <shlobj.h>     // SHGetFolderPathW (CSIDL_APPDATA)
#include <shellapi.h>   // ShellExecuteW (reveal in Explorer)
#include <vector>
#include <memory>
#include <cwchar>

using json = nlohmann::json;

// App state owned by main.cpp — serialized here on save, rebuilt on load.
extern std::vector<std::unique_ptr<FenceWindow>> g_fences;
extern bool g_allHidden;
extern bool g_deskHidden;   // tray "hide desktop icons" toggle; replayed by
                            // main.cpp's ApplyDesktopIconsHidden on startup

namespace {

// nlohmann stores narrow strings as UTF-8; the app works in UTF-16.
std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                        s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(),
                                nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(),
                        w.data(), n);
    return w;
}

// Copy a JSON array of 4 numbers into an rgba float[4]; untouched when the
// value is missing or malformed (keeps the design default).
void GetColor4(const json& j, const char* key, float out[4]) {
    if (!j.contains(key) || !j[key].is_array() || j[key].size() != 4) return;
    for (int i = 0; i < 4; i++)
        if (j[key][i].is_number()) out[i] = j[key][i].get<float>();
}

json SaveFence(FenceWindow& f) {
    FenceData d = f.GetData();
    const FenceAppearance& app = f.GetRender().Appearance();
    json jf;
    jf["id"]        = ToUtf8(d.id);
    jf["title"]     = ToUtf8(d.title);
    jf["x"]         = d.x;          // screen coords; the ctor converts to
    jf["y"]         = d.y;          // parent-client on creation
    jf["w"]         = d.w;
    jf["h"]         = d.h;
    jf["collapsed"] = f.IsCollapsed();
    jf["bg"]        = { app.bg[0], app.bg[1], app.bg[2], app.bg[3] };
    jf["titleColor"]= { app.title[0], app.title[1], app.title[2], app.title[3] };
    jf["titleAlign"]= app.titleAlign;
    jf["displayMode"]= app.displayMode;
    jf["source"]    = ToUtf8(d.sourceFolder);
    jf["sortCol"]   = d.sortCol;
    jf["sortAsc"]   = d.sortAsc;
    jf["titleH"]    = app.titleH;   // physical px (appearance is pre-scaled)
    jf["fontSize"]  = app.fontSize;
    jf["fontName"]  = ToUtf8(app.fontName);
    json icons = json::array();
    for (const IconEntry& e : f.Icons()) {
        icons.push_back({
            { "name", ToUtf8(e.name) },
            { "path", ToUtf8(e.path) },
            { "x", e.x }, { "y", e.y },
        });
    }
    jf["icons"] = std::move(icons);
    return jf;
}

void LoadFence(const json& jf) {
    if (!jf.is_object()) return;
    FenceData fd;
    fd.id    = FromUtf8(jf.value("id", std::string()));
    fd.title = FromUtf8(jf.value("title", std::string()));
    fd.x     = jf.value("x", 0);
    fd.y     = jf.value("y", 0);
    fd.w     = jf.value("w", 0);
    fd.h     = jf.value("h", 0);
    if (fd.w < 40 || fd.h < 20) return;   // degenerate entry — skip

    auto f = std::make_unique<FenceWindow>(fd);
    if (!f->Hwnd()) return;               // no desktop host right now

    // Appearance: restore exactly the fields the settings panel edits.
    FenceAppearance& app = f->GetRender().Appearance();
    GetColor4(jf, "bg", app.bg);
    GetColor4(jf, "titleColor", app.title);
    app.titleAlign = jf.value("titleAlign", app.titleAlign);
    app.displayMode = jf.value("displayMode", app.displayMode);
    app.titleH     = jf.value("titleH", app.titleH);
    app.fontSize   = jf.value("fontSize", app.fontSize);
    if (jf.contains("fontName") && jf["fontName"].is_string()) {
        std::wstring fn = FromUtf8(jf["fontName"].get<std::string>());
        if (!fn.empty())
            wcscpy_s(app.fontName, _countof(app.fontName), fn.c_str());
    }
    f->GetRender().RebuildStyles();

    fd.sourceFolder = FromUtf8(jf.value("source", std::string()));

    if (jf.contains("sortCol"))
        f->SetSortPreset(jf.value("sortCol", 0), jf.value("sortAsc", true));

    if (!fd.sourceFolder.empty()) {
        f->MapToFolder(fd.sourceFolder);
    } else {
        // Icons: restore positions as saved, then re-snap onto the current grid
        // (a no-op unless the desktop icon size changed between sessions).
        std::vector<IconEntry> icons;
        if (jf.contains("icons") && jf["icons"].is_array()) {
            icons.reserve(jf["icons"].size());
            for (const auto& ji : jf["icons"]) {
                if (!ji.is_object()) continue;
                IconEntry e;
                e.name = FromUtf8(ji.value("name", std::string()));
                e.path = FromUtf8(ji.value("path", std::string()));
                e.x = ji.value("x", 0.0f);
                e.y = ji.value("y", 0.0f);
                if (e.path.empty()) continue;
                icons.push_back(std::move(e));
            }
        }
        f->SetIcons(icons);
        f->RelayoutIcons();
    }

    // Collapse LAST: while expanded the window still has its saved height,
    // so ToggleCollapse remembers the right m_expandedH.
    if (jf.value("collapsed", false)) f->ToggleCollapse();

    g_fences.push_back(std::move(f));
}

} // namespace

namespace Config {

const std::wstring& Path() {
    static const std::wstring p = [] {
        wchar_t appdata[MAX_PATH] = {};
        SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata);
        return std::wstring(appdata) + L"\\openFences\\config.json";
    }();
    return p;
}

bool SaveNow() {
    json j;
    j["version"]    = 1;
    j["language"]   = FenceWindow::GetLanguage();
    j["hideAll"]    = g_allHidden;
    j["deskHidden"] = g_deskHidden;
    json fences = json::array();
    for (const auto& f : g_fences)
        fences.push_back(SaveFence(*f));
    j["fences"] = std::move(fences);
    // Indented on purpose: the tray menu invites users to open this file.
    std::string txt = j.dump(2);

    // Ensure %APPDATA%\openFences exists.
    std::wstring path = Path();
    std::wstring dir = path.substr(0, path.find_last_of(L'\\'));
    CreateDirectoryW(dir.c_str(), nullptr);   // EEXIST is fine

    // Atomic replace: write .tmp, then rename over the real file.
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, txt.data(), (DWORD)txt.size(), &written, nullptr);
    CloseHandle(h);
    if (!ok || written != (DWORD)txt.size()) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

void MarkDirty() {
    HWND owner = FenceWindow::Owner();
    if (!owner) return;
    // Same-ID SetTimer restarts the countdown — a burst of changes (a drag
    // fires dozens) collapses into one write once things go quiet.
    SetTimer(owner, 3, 800, nullptr);
}

bool LoadApp() {
    HANDLE h = CreateFileW(Path().c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(h, nullptr);
    std::vector<char> bytes(size);
    DWORD got = 0;
    BOOL ok = ReadFile(h, bytes.data(), size, &got, nullptr);
    CloseHandle(h);
    if (!ok || got != size) return false;

    // allow_exceptions=false: a corrupt file degrades to "no config", and
    // the caller falls back to the defaults instead of failing to start.
    json j = json::parse(bytes.begin(), bytes.end(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;

    int lang = j.value("language", 1);
    FenceWindow::SetLanguage(lang == 0 ? 0 : 1);

    if (j.contains("fences") && j["fences"].is_array())
        for (const auto& jf : j["fences"])
            LoadFence(jf);

    // An empty array is a legitimate state (the user deleted every fence) —
    // only a MISSING file triggers the defaults, so honor it.
    g_allHidden = j.value("hideAll", false);
    if (g_allHidden)
        for (auto& f : g_fences) f->Hide();
    // The hide itself is replayed by main.cpp (ApplyDesktopIconsHidden) once
    // the desktop tree is up — parking the fences needs its helpers.
    g_deskHidden = j.value("deskHidden", false);
    return true;
}

void RevealFile() {
    SaveNow();   // never reveal a file that does not exist yet
    std::wstring args = L"/select,\"" + Path() + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(),
                  nullptr, SW_SHOWNORMAL);
}

bool AutoStartEnabled() {
    return RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        L"openFences", RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

void SetAutoStart(bool enable) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return;
    if (enable) {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        // Quote the path — it may contain spaces (C:\Program Files\…).
        std::wstring cmd = std::wstring(L"\"") + exe + L"\"";
        RegSetValueExW(key, L"openFences", 0, REG_SZ,
            (const BYTE*)cmd.c_str(),
            (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, L"openFences");   // absent = disabled
    }
    RegCloseKey(key);
}

} // namespace Config
