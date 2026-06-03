// Mumble Voice Overlay -- self-contained native Mumble plugin.
//
// Observes Mumble talking state and renders an always-on-top transparent
// overlay window using Win32 layered windows + GDI+. Nothing injected into
// the game process, so EAC has nothing to object to.
//
// Settings: %APPDATA%\MumbleVoiceOverlay\config.json
// Right-click the tray icon to lock/unlock, reload config, etc.

#include "MumblePlugin.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)

#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <objidl.h>
#include <gdiplus.h>
#pragma comment(lib, "Gdiplus.lib")
#pragma comment(lib, "Shell32.lib")

#endif

namespace {

// ---- Plugin identity -------------------------------------------------------

constexpr const char *PLUGIN_NAME   = "Mumble Voice Overlay (Star Citizen)";
constexpr const char *PLUGIN_AUTHOR = "Mumble Voice Overlay";
constexpr const char *PLUGIN_DESC   =
    "Self-contained EAC-safe voice overlay. Shows who is talking in an "
    "always-on-top window. Right-click the tray icon to configure.";

// ---- Speaker model ---------------------------------------------------------

struct UserInfo {
    int id = 0;
    std::string name;
    std::string channel;
    int channelId = -1;
    std::string comment;
    std::string hash;
    bool locallyMuted = false;
    bool isSelf = false;
};

struct ActiveSpeaker {
    UserInfo user;
    std::string state;
    bool isSelf = false;
    bool selfMuted = false;
    bool selfDeafened = false;
};

static bool isActiveState(const std::string &s) {
    return s == "talking" || s == "whispering" || s == "shouting" || s == "muted";
}

static int statePriority(const std::string &s) {
    if (s == "shouting")   return 3;
    if (s == "whispering") return 2;
    if (s == "muted")      return 1;
    if (s == "talking")    return 0;
    return -1;
}

// ---- Configuration (persisted to config.json) ------------------------------
//
// Edit %APPDATA%\MumbleVoiceOverlay\config.json while Mumble is running,
// then right-click the tray icon → Reload config.

struct OverlayConfig {
    // Position & size
    int   x           = 48;    // screen X of overlay top-left
    int   y           = 120;   // screen Y of overlay top-left
    int   width       = 380;   // card width in pixels
    int   cardHeight  = 72;    // height of each speaker card in pixels
    int   cardSpacing = 6;     // gap between cards in pixels

    // Text
    float nameFontPt  = 15.0f; // speaker name font size (points)
    float metaFontPt  = 10.0f; // state/channel line font size (points)

    // Appearance
    float opacity     = 0.92f; // 0.0–1.0
    int   lingerMs    = 1200;  // how long a card stays after talking stops (ms)

    // Behaviour
    int   maxCards    = 8;
    bool  showSelf    = true;  // show your own card when you talk
    bool  showChannel = true;  // show channel name on each card
};

// ---- Config file (Windows-only, plain Win32, no extra libs) ----------------

#if defined(_WIN32)

static std::wstring configDir() {
    wchar_t buf[MAX_PATH] = {};
    GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    return std::wstring(buf) + L"\\MumbleVoiceOverlay";
}

static std::wstring configPath() {
    return configDir() + L"\\config.json";
}

static std::string readFileW(const std::wstring &path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    DWORD sz = GetFileSize(h, nullptr);
    if (!sz || sz == INVALID_FILE_SIZE) { CloseHandle(h); return {}; }
    std::string buf(sz, '\0');
    DWORD rd = 0;
    ReadFile(h, &buf[0], sz, &rd, nullptr);
    CloseHandle(h);
    buf.resize(rd);
    return buf;
}

static void writeFileW(const std::wstring &path, const std::string &text) {
    CreateDirectoryW(configDir().c_str(), nullptr);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, text.c_str(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(h);
}

// Minimal JSON key-value reader — handles numbers, booleans.
static bool jsonFind(const std::string &json, const std::string &key, std::string &out) {
    std::string needle = "\"" + key + "\"";
    size_t p = json.find(needle);
    if (p == std::string::npos) return false;
    p += needle.size();
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) ++p;
    if (p >= json.size() || json[p] != ':') return false;
    ++p;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) ++p;
    if (p >= json.size()) return false;
    size_t e = p;
    while (e < json.size() && json[e] != ',' && json[e] != '}' && json[e] != '\n' && json[e] != '\r') ++e;
    out = json.substr(p, e - p);
    while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
    return !out.empty();
}

static int jsonInt(const std::string &j, const std::string &k, int def) {
    std::string v; if (!jsonFind(j, k, v)) return def;
    try { return std::stoi(v); } catch (...) { return def; }
}
static float jsonFloat(const std::string &j, const std::string &k, float def) {
    std::string v; if (!jsonFind(j, k, v)) return def;
    try { return std::stof(v); } catch (...) { return def; }
}
static bool jsonBool(const std::string &j, const std::string &k, bool def) {
    std::string v; if (!jsonFind(j, k, v)) return def;
    return v == "true" ? true : (v == "false" ? false : def);
}

static OverlayConfig loadConfig() {
    OverlayConfig c;
    std::string json = readFileW(configPath());
    if (json.empty()) return c;
    c.x           = jsonInt  (json, "x",           c.x);
    c.y           = jsonInt  (json, "y",           c.y);
    c.width       = jsonInt  (json, "width",       c.width);
    c.cardHeight  = jsonInt  (json, "cardHeight",  c.cardHeight);
    c.cardSpacing = jsonInt  (json, "cardSpacing", c.cardSpacing);
    c.nameFontPt  = jsonFloat(json, "nameFontPt",  c.nameFontPt);
    c.metaFontPt  = jsonFloat(json, "metaFontPt",  c.metaFontPt);
    c.opacity     = jsonFloat(json, "opacity",     c.opacity);
    c.lingerMs    = jsonInt  (json, "lingerMs",    c.lingerMs);
    c.maxCards    = jsonInt  (json, "maxCards",    c.maxCards);
    c.showSelf    = jsonBool (json, "showSelf",    c.showSelf);
    c.showChannel = jsonBool (json, "showChannel", c.showChannel);
    return c;
}

static void saveConfig(const OverlayConfig &c) {
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "{\n"
        "  \"x\":           %d,\n"
        "  \"y\":           %d,\n"
        "  \"width\":       %d,\n"
        "  \"cardHeight\":  %d,\n"
        "  \"cardSpacing\": %d,\n"
        "  \"nameFontPt\":  %.1f,\n"
        "  \"metaFontPt\":  %.1f,\n"
        "  \"opacity\":     %.2f,\n"
        "  \"lingerMs\":    %d,\n"
        "  \"maxCards\":    %d,\n"
        "  \"showSelf\":    %s,\n"
        "  \"showChannel\": %s\n"
        "}\n",
        c.x, c.y, c.width, c.cardHeight, c.cardSpacing,
        c.nameFontPt, c.metaFontPt, c.opacity, c.lingerMs, c.maxCards,
        c.showSelf    ? "true" : "false",
        c.showChannel ? "true" : "false");
    writeFileW(configPath(), buf);
}

#endif // _WIN32

// ---- Color theme per talking state -----------------------------------------

#if defined(_WIN32)

struct StateTheme { DWORD accent; const wchar_t *label; };

static StateTheme themeForState(const std::string &state) {
    if (state == "talking")    return { 0xFF00E5FF, L"TALKING" };
    if (state == "whispering") return { 0xFFA78BFA, L"WHISPER" };
    if (state == "shouting")   return { 0xFFFBBF24, L"SHOUT" };
    if (state == "muted")      return { 0xFFFB7185, L"MIC MUTED" };
    return { 0xFF00E5FF, L"TALKING" };
}

struct LingerEntry { int userId; DWORD expireTickMs; std::string lastState; };

#endif

// ---- Global state ----------------------------------------------------------

struct PluginState {
    std::mutex mutex;
    mumble_plugin_id_t id = 0;
    mumble_api_t api{};
    bool apiValid = false;

    mumble_connection_t connection = -1;
    mumble_userid_t localUser = 0;
    bool synchronized = false;
    std::vector<mumble_userid_t> known;

    // Speaker model
    std::vector<UserInfo> users;
    struct TalkState {
        int id;
        std::string state;
        std::string peakState;  // highest-priority state seen this activation
        uint32_t peakSetTick = 0;
    };
    std::vector<TalkState> talkStates;
    bool selfMuted = false;
    bool selfDeafened = false;

    OverlayConfig config;

#if defined(_WIN32)
    HWND overlayWnd = nullptr;
    std::thread overlayThread;
    bool overlayRunning = false;
    ULONG_PTR gdiplusToken = 0;

    std::vector<LingerEntry> linger;
    UINT_PTR lingerTimerId = 0;

    NOTIFYICONDATAW trayData = {};
    bool trayAdded = false;
    UINT wmTaskbarCreated = 0;
    bool overlayEnabled = true;   // toggled by left-clicking the tray icon
    HWND settingsDlg = nullptr;   // non-null while the settings dialog is open
#endif
};

PluginState g;

// ---- Speaker model helpers -------------------------------------------------

static UserInfo *findUser(int id) {
    for (auto &u : g.users) { if (u.id == id) return &u; }
    return nullptr;
}

static UserInfo &getOrCreateUser(int id) {
    if (auto *u = findUser(id)) return *u;
    g.users.push_back({});
    g.users.back().id   = id;
    g.users.back().name = "User " + std::to_string(id);
    return g.users.back();
}

static std::string getRawTalkState(int id) {
    for (auto &ts : g.talkStates) { if (ts.id == id) return ts.state; }
    return "passive";
}

#if defined(_WIN32)
// While a user is actively talking, show their peak state (e.g. SHOUT stays
// visible even if Mumble briefly reports TALKING during hold-time wind-down).
static std::string getDisplayTalkState(int id) {
    for (auto &ts : g.talkStates) {
        if (ts.id != id) continue;
        if (!ts.peakState.empty() && isActiveState(ts.state)
                && statePriority(ts.peakState) > statePriority(ts.state))
            return ts.peakState;
        return ts.state;
    }
    return "passive";
}
#endif

static void setTalkState(int id, const std::string &state) {
#if defined(_WIN32)
    DWORD now = GetTickCount();
#endif
    for (auto &ts : g.talkStates) {
        if (ts.id != id) continue;
        ts.state = state;
        if (!isActiveState(state)) {
            ts.peakState.clear();
        } else if (statePriority(state) >= statePriority(ts.peakState)) {
            ts.peakState = state;
#if defined(_WIN32)
            ts.peakSetTick = now;
#endif
        }
        return;
    }
    PluginState::TalkState n;
    n.id        = id;
    n.state     = state;
    n.peakState = isActiveState(state) ? state : "";
#if defined(_WIN32)
    n.peakSetTick = now;
#endif
    g.talkStates.push_back(n);
}

static void removeUser(int id) {
    g.users.erase(std::remove_if(g.users.begin(), g.users.end(),
        [id](const UserInfo &u) { return u.id == id; }), g.users.end());
    g.talkStates.erase(std::remove_if(g.talkStates.begin(), g.talkStates.end(),
        [id](const PluginState::TalkState &ts) { return ts.id == id; }), g.talkStates.end());
}

#if defined(_WIN32)
static std::vector<ActiveSpeaker> buildSnapshot() {
    std::vector<ActiveSpeaker> result;
    for (auto &ts : g.talkStates) {
        if (!isActiveState(ts.state)) continue;
        bool isSelf = (ts.id == static_cast<int>(g.localUser));
        if (!g.config.showSelf && isSelf) continue;
        auto *u = findUser(ts.id);
        ActiveSpeaker sp;
        sp.user = u ? *u : UserInfo{ts.id, "User " + std::to_string(ts.id), {}, -1, {}, {}, false, false};
        sp.state        = getDisplayTalkState(ts.id);
        sp.isSelf       = isSelf || sp.user.isSelf;
        sp.selfMuted    = g.selfMuted;
        sp.selfDeafened = g.selfDeafened;
        result.push_back(sp);
    }
    std::sort(result.begin(), result.end(), [](const ActiveSpeaker &a, const ActiveSpeaker &b) {
        if (a.isSelf != b.isSelf) return !a.isSelf;
        return a.user.name < b.user.name;
    });
    if (static_cast<int>(result.size()) > g.config.maxCards)
        result.resize(g.config.maxCards);
    return result;
}
#endif

// ---- Mumble API wrappers ---------------------------------------------------

static bool apiReady() { return g.apiValid && g.id != 0; }

static std::string userName(mumble_connection_t c, mumble_userid_t u) {
    std::string r; if (!g.api.getUserName) return r;
    const char *raw = nullptr;
    if (g.api.getUserName(g.id, c, u, &raw) == MUMBLE_EC_OK && raw) { r = raw; g.api.freeMemory(g.id, raw); }
    return r;
}
static std::string userComment(mumble_connection_t c, mumble_userid_t u) {
    std::string r; if (!g.api.getUserComment) return r;
    const char *raw = nullptr;
    if (g.api.getUserComment(g.id, c, u, &raw) == MUMBLE_EC_OK && raw) { r = raw; g.api.freeMemory(g.id, raw); }
    return r;
}
static std::string userHash(mumble_connection_t c, mumble_userid_t u) {
    std::string r; if (!g.api.getUserHash) return r;
    const char *raw = nullptr;
    if (g.api.getUserHash(g.id, c, u, &raw) == MUMBLE_EC_OK && raw) { r = raw; g.api.freeMemory(g.id, raw); }
    return r;
}
static std::string channelNameOfUser(mumble_connection_t c, mumble_userid_t u, mumble_channelid_t *outId) {
    std::string r;
    if (!g.api.getChannelOfUser || !g.api.getChannelName) return r;
    mumble_channelid_t ch = -1;
    if (g.api.getChannelOfUser(g.id, c, u, &ch) != MUMBLE_EC_OK) return r;
    if (outId) *outId = ch;
    const char *raw = nullptr;
    if (g.api.getChannelName(g.id, c, ch, &raw) == MUMBLE_EC_OK && raw) { r = raw; g.api.freeMemory(g.id, raw); }
    return r;
}
static bool userLocallyMuted(mumble_connection_t c, mumble_userid_t u) {
    if (!g.api.isUserLocallyMuted) return false;
    bool m = false; g.api.isUserLocallyMuted(g.id, c, u, &m); return m;
}
static const char *talkingStateName(mumble_talking_state_t s) {
    switch (s) {
        case MUMBLE_TS_PASSIVE:       return "passive";
        case MUMBLE_TS_TALKING:       return "talking";
        case MUMBLE_TS_WHISPERING:    return "whispering";
        case MUMBLE_TS_SHOUTING:      return "shouting";
        case MUMBLE_TS_TALKING_MUTED: return "muted";
        default:                       return "invalid";
    }
}
static bool isKnown(mumble_userid_t u) {
    std::lock_guard<std::mutex> lk(g.mutex);
    for (auto id : g.known) { if (id == u) return true; }
    return false;
}

// ---- Win32 overlay ---------------------------------------------------------

#if defined(_WIN32)

// Menu command IDs
enum {
    CMD_TOGGLE = 1001, CMD_SHOWSELF, CMD_SHOWCHANNEL,
    CMD_SETTINGS, CMD_CLOSE
};
// Custom window messages
enum { WM_REPAINT = WM_USER + 1, WM_TRAY = WM_USER + 2, WM_RELOAD = WM_USER + 3 };

// Settings dialog control IDs
enum {
    IDC_LBL_X = 2001, IDC_EDIT_X, IDC_LBL_Y, IDC_EDIT_Y,
    IDC_LBL_WIDTH, IDC_EDIT_WIDTH,
    IDC_LBL_CARDH, IDC_EDIT_CARDH,
    IDC_LBL_SPACING, IDC_EDIT_SPACING,
    IDC_LBL_NAMEFONT, IDC_EDIT_NAMEFONT,
    IDC_LBL_METAFONT, IDC_EDIT_METAFONT,
    IDC_LBL_OPACITY, IDC_EDIT_OPACITY,
    IDC_LBL_LINGER, IDC_EDIT_LINGER,
    IDC_LBL_MAXCARDS, IDC_EDIT_MAXCARDS,
    IDC_CHK_SHOWSELF, IDC_CHK_SHOWCHANNEL,
    IDC_BTN_OK, IDC_BTN_CANCEL, IDC_BTN_APPLY
};

static std::wstring toWide(const std::string &s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring ws(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &ws[0], len);
    ws.resize(len - 1);
    return ws;
}

static std::string stripHtml(const std::string &html) {
    std::string out; bool inTag = false;
    for (char c : html) {
        if (c == '<') { inTag = true; continue; }
        if (c == '>') { inTag = false; out.push_back(' '); continue; }
        if (!inTag) out.push_back(c);
    }
    std::string r; bool sp = true;
    for (char c : out) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { if (!sp) { r.push_back(' '); sp = true; } }
        else { r.push_back(c); sp = false; }
    }
    while (!r.empty() && r.back() == ' ') r.pop_back();
    if (!r.empty() && r.front() == ' ') r.erase(r.begin());
    return r;
}

// -- Settings dialog (Win32 native) ------------------------------------------

static HWND createLabel(HWND parent, int id, const wchar_t *text, int x, int y, int w, int h) {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_RIGHT,
                           x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<intptr_t>(id)),
                           GetModuleHandle(nullptr), nullptr);
}

static HWND createEdit(HWND parent, int id, const wchar_t *text, int x, int y, int w, int h) {
    HWND hw = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text,
                              WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                              x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<intptr_t>(id)),
                              GetModuleHandle(nullptr), nullptr);
    return hw;
}

static HWND createCheck(HWND parent, int id, const wchar_t *text, int x, int y, int w, int h, bool checked) {
    HWND hw = CreateWindowExW(0, L"BUTTON", text,
                              WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                              x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<intptr_t>(id)),
                              GetModuleHandle(nullptr), nullptr);
    SendMessage(hw, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    return hw;
}

static HWND createButton(HWND parent, int id, const wchar_t *text, int x, int y, int w, int h) {
    return CreateWindowExW(0, L"BUTTON", text,
                           WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                           x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<intptr_t>(id)),
                           GetModuleHandle(nullptr), nullptr);
}

static int getEditInt(HWND dlg, int id) {
    wchar_t buf[64] = {};
    GetDlgItemTextW(dlg, id, buf, 64);
    return _wtoi(buf);
}

static float getEditFloat(HWND dlg, int id) {
    wchar_t buf[64] = {};
    GetDlgItemTextW(dlg, id, buf, 64);
    return static_cast<float>(_wtof(buf));
}

static void setEditInt(HWND dlg, int id, int val) {
    wchar_t buf[32]; swprintf_s(buf, L"%d", val);
    SetDlgItemTextW(dlg, id, buf);
}

static void setEditFloat(HWND dlg, int id, float val, int decimals = 1) {
    wchar_t buf[32];
    if (decimals == 2) swprintf_s(buf, L"%.2f", val);
    else               swprintf_s(buf, L"%.1f", val);
    SetDlgItemTextW(dlg, id, buf);
}

static void applySettingsFromDialog(HWND dlg) {
    std::lock_guard<std::mutex> lk(g.mutex);
    g.config.x           = getEditInt  (dlg, IDC_EDIT_X);
    g.config.y           = getEditInt  (dlg, IDC_EDIT_Y);
    g.config.width       = getEditInt  (dlg, IDC_EDIT_WIDTH);
    g.config.cardHeight  = getEditInt  (dlg, IDC_EDIT_CARDH);
    g.config.cardSpacing = getEditInt  (dlg, IDC_EDIT_SPACING);
    g.config.nameFontPt  = getEditFloat(dlg, IDC_EDIT_NAMEFONT);
    g.config.metaFontPt  = getEditFloat(dlg, IDC_EDIT_METAFONT);
    g.config.opacity     = getEditFloat(dlg, IDC_EDIT_OPACITY);
    g.config.lingerMs    = getEditInt  (dlg, IDC_EDIT_LINGER);
    g.config.maxCards    = getEditInt  (dlg, IDC_EDIT_MAXCARDS);
    g.config.showSelf    = IsDlgButtonChecked(dlg, IDC_CHK_SHOWSELF) == BST_CHECKED;
    g.config.showChannel = IsDlgButtonChecked(dlg, IDC_CHK_SHOWCHANNEL) == BST_CHECKED;
    saveConfig(g.config);
    if (g.overlayWnd) PostMessage(g.overlayWnd, WM_REPAINT, 0, 0);
}

static void populateDialogFromConfig(HWND dlg) {
    setEditInt  (dlg, IDC_EDIT_X,        g.config.x);
    setEditInt  (dlg, IDC_EDIT_Y,        g.config.y);
    setEditInt  (dlg, IDC_EDIT_WIDTH,    g.config.width);
    setEditInt  (dlg, IDC_EDIT_CARDH,    g.config.cardHeight);
    setEditInt  (dlg, IDC_EDIT_SPACING,  g.config.cardSpacing);
    setEditFloat(dlg, IDC_EDIT_NAMEFONT, g.config.nameFontPt);
    setEditFloat(dlg, IDC_EDIT_METAFONT, g.config.metaFontPt);
    setEditFloat(dlg, IDC_EDIT_OPACITY,  g.config.opacity, 2);
    setEditInt  (dlg, IDC_EDIT_LINGER,   g.config.lingerMs);
    setEditInt  (dlg, IDC_EDIT_MAXCARDS, g.config.maxCards);
    CheckDlgButton(dlg, IDC_CHK_SHOWSELF,    g.config.showSelf    ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_CHK_SHOWCHANNEL, g.config.showChannel ? BST_CHECKED : BST_UNCHECKED);
}

static LRESULT CALLBACK settingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        // Set dark background
        // Build the form: labels on the left, edits on the right
        const int lblW = 120, editW = 80, rowH = 24, pad = 10, startY = 15, startX = 15;
        int y = startY;

        struct Row { int lblId; const wchar_t *label; int editId; };
        Row rows[] = {
            { IDC_LBL_X,        L"Position X:",     IDC_EDIT_X },
            { IDC_LBL_Y,        L"Position Y:",     IDC_EDIT_Y },
            { IDC_LBL_WIDTH,    L"Card width:",     IDC_EDIT_WIDTH },
            { IDC_LBL_CARDH,    L"Card height:",    IDC_EDIT_CARDH },
            { IDC_LBL_SPACING,  L"Card spacing:",   IDC_EDIT_SPACING },
            { IDC_LBL_NAMEFONT, L"Name font (pt):", IDC_EDIT_NAMEFONT },
            { IDC_LBL_METAFONT, L"Meta font (pt):", IDC_EDIT_METAFONT },
            { IDC_LBL_OPACITY,  L"Opacity (0-1):",  IDC_EDIT_OPACITY },
            { IDC_LBL_LINGER,   L"Linger (ms):",    IDC_EDIT_LINGER },
            { IDC_LBL_MAXCARDS, L"Max cards:",      IDC_EDIT_MAXCARDS },
        };

        for (auto &r : rows) {
            createLabel(hwnd, r.lblId, r.label, startX, y + 2, lblW, rowH - 4);
            createEdit(hwnd, r.editId, L"", startX + lblW + pad, y, editW, rowH - 2);
            y += rowH + 4;
        }

        y += 4;
        createCheck(hwnd, IDC_CHK_SHOWSELF,    L"Show my own card",  startX + 6, y, 200, 20, g.config.showSelf);
        y += 26;
        createCheck(hwnd, IDC_CHK_SHOWCHANNEL, L"Show channel name", startX + 6, y, 200, 20, g.config.showChannel);
        y += 36;

        int btnW = 70, btnH = 28;
        int btnArea = startX + lblW + pad + editW;
        createButton(hwnd, IDC_BTN_APPLY,  L"Apply",  btnArea - btnW * 3 - 16, y, btnW, btnH);
        createButton(hwnd, IDC_BTN_OK,     L"OK",     btnArea - btnW * 2 - 8,  y, btnW, btnH);
        createButton(hwnd, IDC_BTN_CANCEL, L"Cancel", btnArea - btnW,          y, btnW, btnH);

        populateDialogFromConfig(hwnd);

        // Set font on all child controls
        HFONT hFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        EnumChildWindows(hwnd, [](HWND child, LPARAM lp) -> BOOL {
            SendMessage(child, WM_SETFONT, static_cast<WPARAM>(lp), TRUE);
            return TRUE;
        }, reinterpret_cast<LPARAM>(hFont));

        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_BTN_APPLY:
            applySettingsFromDialog(hwnd);
            return 0;
        case IDC_BTN_OK:
            applySettingsFromDialog(hwnd);
            DestroyWindow(hwnd);
            return 0;
        case IDC_BTN_CANCEL:
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_DESTROY:
        g.settingsDlg = nullptr;
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static void openSettingsDialog() {
    if (g.settingsDlg) {
        SetForegroundWindow(g.settingsDlg);
        return;
    }

    static bool registered = false;
    const wchar_t *cls = L"MumbleVoiceOverlaySettings";
    HINSTANCE hInst = GetModuleHandle(nullptr);

    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize      = sizeof(wc);
        wc.lpfnWndProc = settingsWndProc;
        wc.hInstance   = hInst;
        wc.lpszClassName = cls;
        wc.hCursor     = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassExW(&wc);
        registered = true;
    }

    int dlgW = 260, dlgH = 440;
    g.settingsDlg = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        cls, L"Voice Overlay Settings",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, dlgW, dlgH,
        nullptr, nullptr, hInst, nullptr);

    ShowWindow(g.settingsDlg, SW_SHOW);
    SetForegroundWindow(g.settingsDlg);
}

// -- Overlay painting --------------------------------------------------------

static void paintOverlay(HWND hwnd) {
    std::lock_guard<std::mutex> lk(g.mutex);

    if (!g.overlayEnabled) {
        ShowWindow(hwnd, SW_HIDE);
        return;
    }

    auto speakers = buildSnapshot();

    // Collect lingering speakers (stopped talking, still within linger window)
    DWORD now = GetTickCount();
    std::vector<ActiveSpeaker> lingerSpk;
    for (auto it = g.linger.begin(); it != g.linger.end(); ) {
        if (static_cast<int>(now - it->expireTickMs) >= 0) { it = g.linger.erase(it); continue; }
        bool active = false;
        for (auto &sp : speakers) { if (sp.user.id == it->userId) { active = true; break; } }
        if (!active) {
            auto *u = findUser(it->userId);
            if (u) {
                ActiveSpeaker sp;
                sp.user = *u; sp.state = it->lastState;
                sp.isSelf = (it->userId == static_cast<int>(g.localUser));
                lingerSpk.push_back(sp);
            }
        }
        ++it;
    }
    for (auto &ls : lingerSpk) speakers.push_back(ls);

    const int cardH   = g.config.cardHeight;
    const int spacing = g.config.cardSpacing;
    const int w       = g.config.width;
    const float op    = g.config.opacity;

    bool hasCards = !speakers.empty();
    int  totalH   = hasCards
        ? static_cast<int>(speakers.size()) * cardH + (static_cast<int>(speakers.size()) - 1) * spacing
        : 0;

    if (!hasCards) {
        ShowWindow(hwnd, SW_HIDE);
        return;
    }

    int h = totalH;
    POINT screenPos = { g.config.x, g.config.y };

    HDC screenDC = GetDC(nullptr);
    HDC memDC    = CreateCompatibleDC(screenDC);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = -h;
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void   *bits = nullptr;
    HBITMAP hBmp = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(memDC, hBmp);
    memset(bits, 0, w * h * 4);

    Gdiplus::Graphics gfx(memDC);
    gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    gfx.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

    // -- Layout constants derived from cardHeight ----------------------------
    // Text is positioned proportionally so both lines fill the card vertically.
    // nameTop is ~17% from top, metaTop is ~57% from top.
    // This keeps the two lines well spaced with balanced padding.
    const float nameTop = static_cast<float>(cardH) * 0.17f;
    const float metaTop = static_cast<float>(cardH) * 0.57f;
    // Height allocated to each text row (stops text from overflowing into next card)
    const float nameH   = static_cast<float>(cardH) * 0.38f;
    const float metaH   = static_cast<float>(cardH) * 0.33f;
    const float textL   = 14.0f;                              // left margin (past accent rail)
    const float textR   = static_cast<float>(w) - 26.0f;     // right margin (past corner brackets)

    // -- Draw each speaker card ----------------------------------------------
    int yOff = 0;
    for (size_t i = 0; i < speakers.size(); ++i) {
        auto &sp = speakers[i];
        auto theme = themeForState(sp.state);
        BYTE accentR = (theme.accent >> 16) & 0xFF;
        BYTE accentG = (theme.accent >>  8) & 0xFF;
        BYTE accentB =  theme.accent        & 0xFF;
        float cop = g.config.opacity;
        if (sp.state == "passive") cop *= 0.45f;  // linger fade

        // Rounded card background
        {
            Gdiplus::GraphicsPath path;
            const int r = 8;
            Gdiplus::Rect rc(0, yOff, w, cardH);
            path.AddArc(rc.X,                    rc.Y,                    r*2, r*2, 180, 90);
            path.AddArc(rc.X + rc.Width - r*2,   rc.Y,                    r*2, r*2, 270, 90);
            path.AddArc(rc.X + rc.Width - r*2,   rc.Y + rc.Height - r*2, r*2, r*2,   0, 90);
            path.AddArc(rc.X,                    rc.Y + rc.Height - r*2, r*2, r*2,  90, 90);
            path.CloseFigure();

            Gdiplus::SolidBrush bgBrush(Gdiplus::Color(static_cast<BYTE>(215 * cop), 10, 15, 20));
            gfx.FillPath(&bgBrush, &path);

            // Full-width accent gradient: ~8% opacity on left, fading to transparent
            Gdiplus::LinearGradientBrush gradBrush(
                Gdiplus::PointF(static_cast<float>(rc.X), 0),
                Gdiplus::PointF(static_cast<float>(rc.X + rc.Width), 0),
                Gdiplus::Color(static_cast<BYTE>(20 * cop), accentR, accentG, accentB),
                Gdiplus::Color(0, accentR, accentG, accentB));
            Gdiplus::REAL positions[] = { 0.0f, 0.6f, 1.0f };
            Gdiplus::Color colors[] = {
                Gdiplus::Color(static_cast<BYTE>(20 * cop), accentR, accentG, accentB),
                Gdiplus::Color(0, accentR, accentG, accentB),
                Gdiplus::Color(0, accentR, accentG, accentB)
            };
            gradBrush.SetInterpolationColors(colors, positions, 3);
            gfx.FillPath(&gradBrush, &path);

            // Accent rail (3px left edge bar)
            Gdiplus::SolidBrush railBrush(Gdiplus::Color(static_cast<BYTE>(235 * cop), accentR, accentG, accentB));
            gfx.FillRectangle(&railBrush, 0, yOff + 4, 3, cardH - 8);

            // Corner brackets (top-right + bottom-right)
            Gdiplus::Pen pen(Gdiplus::Color(static_cast<BYTE>(190 * cop), accentR, accentG, accentB), 1.5f);
            pen.SetStartCap(Gdiplus::LineCapRound);
            pen.SetEndCap(Gdiplus::LineCapRound);
            const int arm = 10, bx = w - 6;
            int byT = yOff + 5, byB = yOff + cardH - 6;
            gfx.DrawLine(&pen, bx - arm, byT, bx, byT);
            gfx.DrawLine(&pen, bx, byT, bx, byT + arm);
            gfx.DrawLine(&pen, bx, byB - arm, bx, byB);
            gfx.DrawLine(&pen, bx - arm, byB, bx, byB);
        }

        // Speaker name (bold, fills top portion of card)
        {
            std::wstring nameW = toWide(sp.user.name);
            if (sp.isSelf) nameW += L"  (you)";

            Gdiplus::FontFamily ff(L"Segoe UI");
            Gdiplus::Font font(&ff, g.config.nameFontPt, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
            Gdiplus::SolidBrush brush(Gdiplus::Color(static_cast<BYTE>(248 * cop), 230, 237, 243));
            Gdiplus::StringFormat fmt;
            fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
            fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
            fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            Gdiplus::RectF rect(textL, static_cast<float>(yOff) + nameTop, textR - textL, nameH);
            gfx.DrawString(nameW.c_str(), -1, &font, rect, &fmt, &brush);
        }

        // Meta line: STATE · channel · status tags (accent colour, monospace)
        {
            std::wstring meta(theme.label);
            if (g.config.showChannel && !sp.user.channel.empty()) {
                meta += L"  \x00B7  ";
                meta += toWide(sp.user.channel);
            }
            if (sp.user.locallyMuted)          meta += L"  \x00B7  LOCAL MUTED";
            if (sp.isSelf && sp.selfDeafened)  meta += L"  \x00B7  DEAFENED";
            else if (sp.isSelf && sp.selfMuted) meta += L"  \x00B7  SELF MUTED";

            Gdiplus::FontFamily ff(L"Consolas");
            Gdiplus::Font font(&ff, g.config.metaFontPt, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
            Gdiplus::SolidBrush brush(Gdiplus::Color(static_cast<BYTE>(215 * cop), accentR, accentG, accentB));
            Gdiplus::StringFormat fmt;
            fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
            fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
            fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            Gdiplus::RectF rect(textL, static_cast<float>(yOff) + metaTop, textR - textL, metaH);
            gfx.DrawString(meta.c_str(), -1, &font, rect, &fmt, &brush);
        }

        yOff += cardH + spacing;
    }

    // Commit to layered window
    SIZE sz = { w, h };
    POINT ptSrc = { 0, 0 };
    BLENDFUNCTION blend = {};
    blend.BlendOp             = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat         = AC_SRC_ALPHA;

    SetWindowPos(hwnd, HWND_TOPMOST, screenPos.x, screenPos.y, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    UpdateLayeredWindow(hwnd, screenDC, &screenPos, &sz, memDC, &ptSrc, 0, &blend, ULW_ALPHA);

    SelectObject(memDC, oldBmp);
    DeleteObject(hBmp);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
}

// -- Tray icon ---------------------------------------------------------------

static HICON makeTrayIcon() {
    // Draw a simple microphone-badge icon in memory (32x32 RGBA)
    int sz = 32;
    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize     = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth    = sz;
    bmi.bmiHeader.biHeight   = -sz;
    bmi.bmiHeader.biPlanes   = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void *bits = nullptr;
    HBITMAP hBmp = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    SelectObject(memDC, hBmp);
    memset(bits, 0, sz * sz * 4);

    Gdiplus::Bitmap bmpGdi(sz, sz, sz * 4, PixelFormat32bppARGB, static_cast<BYTE *>(bits));
    Gdiplus::Graphics gfx(&bmpGdi);
    gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

    Gdiplus::SolidBrush bg(Gdiplus::Color(210, 10, 15, 22));
    gfx.FillEllipse(&bg, 1, 1, sz - 2, sz - 2);

    Gdiplus::Pen ring(Gdiplus::Color(230, 0, 229, 255), 1.5f);
    gfx.DrawEllipse(&ring, 1, 1, sz - 3, sz - 3);

    // Simple mic shape
    Gdiplus::Pen mic(Gdiplus::Color(255, 0, 229, 255), 2.0f);
    mic.SetStartCap(Gdiplus::LineCapRound);
    mic.SetEndCap(Gdiplus::LineCapRound);
    gfx.DrawLine(&mic, 16, 22, 16, 27);
    gfx.DrawLine(&mic, 12, 27, 20, 27);

    Gdiplus::SolidBrush micBody(Gdiplus::Color(240, 0, 229, 255));
    Gdiplus::GraphicsPath p;
    p.AddArc(11, 7, 10, 10, 180, 180);
    p.AddLine(21, 12, 21, 18);
    p.AddArc(11, 13, 10, 10, 0, 180);
    p.CloseFigure();
    gfx.FillPath(&micBody, &p);

    HICON hIcon = nullptr;
    bmpGdi.GetHICON(&hIcon);

    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
    DeleteObject(hBmp);
    return hIcon;
}

static void addTrayIcon(HWND hwnd) {
    HICON hIcon = makeTrayIcon();
    g.trayData = {};
    g.trayData.cbSize           = sizeof(g.trayData);
    g.trayData.hWnd             = hwnd;
    g.trayData.uID              = 1;
    g.trayData.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g.trayData.uCallbackMessage = WM_TRAY;
    g.trayData.hIcon            = hIcon;
    wcscpy_s(g.trayData.szTip, L"Mumble Voice Overlay");
    Shell_NotifyIconW(NIM_ADD, &g.trayData);
    g.trayAdded = true;
}

static void removeTrayIcon() {
    if (g.trayAdded) { Shell_NotifyIconW(NIM_DELETE, &g.trayData); g.trayAdded = false; }
}

static void updateTrayTip() {
    if (!g.trayAdded) return;
    wcscpy_s(g.trayData.szTip,
             g.overlayEnabled ? L"Voice Overlay (ON)" : L"Voice Overlay (OFF)");
    Shell_NotifyIconW(NIM_MODIFY, &g.trayData);
}

static void showTrayMenu(HWND hwnd) {
    HMENU menu = CreatePopupMenu();

    AppendMenuW(menu, MF_STRING | (g.overlayEnabled ? MF_CHECKED : MF_UNCHECKED),
                CMD_TOGGLE, L"Overlay enabled");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (g.config.showSelf    ? MF_CHECKED : MF_UNCHECKED),
                CMD_SHOWSELF, L"Show my own card");
    AppendMenuW(menu, MF_STRING | (g.config.showChannel ? MF_CHECKED : MF_UNCHECKED),
                CMD_SHOWCHANNEL, L"Show channel name");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CMD_SETTINGS, L"Settings...");

    SetForegroundWindow(hwnd);
    POINT pt; GetCursorPos(&pt);
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                             pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);

    switch (cmd) {
    case CMD_TOGGLE: {
        std::lock_guard<std::mutex> lk(g.mutex);
        g.overlayEnabled = !g.overlayEnabled;
        updateTrayTip();
        break;
    }
    case CMD_SHOWSELF: {
        std::lock_guard<std::mutex> lk(g.mutex);
        g.config.showSelf = !g.config.showSelf;
        saveConfig(g.config);
        break;
    }
    case CMD_SHOWCHANNEL: {
        std::lock_guard<std::mutex> lk(g.mutex);
        g.config.showChannel = !g.config.showChannel;
        saveConfig(g.config);
        break;
    }
    case CMD_SETTINGS:
        openSettingsDialog();
        break;
    }

    PostMessage(hwnd, WM_REPAINT, 0, 0);
}

// -- Window procedure --------------------------------------------------------

static LRESULT CALLBACK overlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        g.lingerTimerId = SetTimer(hwnd, 1, 66, [](HWND hw, UINT, UINT_PTR, DWORD) { paintOverlay(hw); });
        g.wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        addTrayIcon(hwnd);
        return 0;

    case WM_REPAINT:
        paintOverlay(hwnd);
        return 0;

    case WM_RELOAD: {
        std::lock_guard<std::mutex> lk(g.mutex);
        g.config = loadConfig();
        PostMessage(hwnd, WM_REPAINT, 0, 0);
        return 0;
    }

    case WM_TRAY:
        if (lParam == WM_RBUTTONUP) {
            showTrayMenu(hwnd);
        } else if (lParam == WM_LBUTTONUP) {
            // Left-click toggles overlay on/off
            { std::lock_guard<std::mutex> lk(g.mutex); g.overlayEnabled = !g.overlayEnabled; }
            updateTrayTip();
            paintOverlay(hwnd);
        }
        return 0;

    case WM_DESTROY:
        removeTrayIcon();
        if (g.lingerTimerId) { KillTimer(hwnd, g.lingerTimerId); g.lingerTimerId = 0; }
        PostQuitMessage(0);
        return 0;

    default:
        if (msg == g.wmTaskbarCreated) {
            // Explorer restarted — re-add tray icon
            addTrayIcon(hwnd);
        }
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

// -- Overlay thread ----------------------------------------------------------

static void overlayThreadFunc() {
    Gdiplus::GdiplusStartupInput gdi;
    Gdiplus::GdiplusStartup(&g.gdiplusToken, &gdi, nullptr);

    const wchar_t *cls = L"MumbleVoiceOverlay";
    HINSTANCE hInst = GetModuleHandle(nullptr);

    WNDCLASSEXW wc = {};
    wc.cbSize      = sizeof(wc);
    wc.lpfnWndProc = overlayWndProc;
    wc.hInstance   = hInst;
    wc.lpszClassName = cls;
    wc.hCursor     = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    // WS_EX_TRANSPARENT makes the window permanently click-through — the overlay
    // never steals input from the game. Position is configured via config.json.
    DWORD exStyle = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT;

    g.overlayWnd = CreateWindowExW(exStyle, cls, L"Mumble Voice Overlay", WS_POPUP,
                                   g.config.x, g.config.y, g.config.width, 1,
                                   nullptr, nullptr, hInst, nullptr);
    if (!g.overlayWnd) {
        Gdiplus::GdiplusShutdown(g.gdiplusToken);
        return;
    }

    ShowWindow(g.overlayWnd, SW_SHOWNOACTIVATE);

    MSG winMsg;
    while (GetMessage(&winMsg, nullptr, 0, 0) > 0) {
        TranslateMessage(&winMsg);
        DispatchMessage(&winMsg);
    }

    g.overlayWnd = nullptr;
    Gdiplus::GdiplusShutdown(g.gdiplusToken);
    UnregisterClassW(cls, hInst);
}

static void scheduleRepaint() {
    if (g.overlayWnd) PostMessage(g.overlayWnd, WM_REPAINT, 0, 0);
}

static void startOverlay() {
    if (g.overlayRunning) return;
    g.config = loadConfig();
    g.overlayRunning = true;
    g.overlayThread = std::thread(overlayThreadFunc);
}

static void stopOverlay() {
    if (!g.overlayRunning) return;
    g.overlayRunning = false;
    if (g.overlayWnd) PostMessage(g.overlayWnd, WM_CLOSE, 0, 0);
    if (g.overlayThread.joinable()) g.overlayThread.join();
}

#else  // Non-Windows stubs

static void scheduleRepaint() {}
static void startOverlay() {}
static void stopOverlay() {}

#endif // _WIN32

// ---- Update speaker model & trigger repaint --------------------------------

static void recordUser(mumble_connection_t conn, mumble_userid_t user, bool markKnown) {
    mumble_channelid_t channelId = -1;
    std::string name    = userName(conn, user);
    std::string channel = channelNameOfUser(conn, user, &channelId);
    std::string comment = userComment(conn, user);
    std::string hash    = userHash(conn, user);
    {
        std::lock_guard<std::mutex> lk(g.mutex);
        auto &u        = getOrCreateUser(static_cast<int>(user));
        u.name         = name.empty() ? ("User " + std::to_string(user)) : name;
        u.channel      = channel;
        u.channelId    = static_cast<int>(channelId);
        u.comment      = comment;
        u.hash         = hash;
        u.locallyMuted = userLocallyMuted(conn, user);
        u.isSelf       = (user == g.localUser);
        if (markKnown) g.known.push_back(user);
    }
    scheduleRepaint();
}

static void recordTalk(mumble_userid_t user, mumble_talking_state_t state) {
    std::string name = talkingStateName(state);
    {
        std::lock_guard<std::mutex> lk(g.mutex);
        std::string prev = getRawTalkState(static_cast<int>(user));
#if defined(_WIN32)
        // Capture the display state (peak-corrected) before setTalkState clears it.
        // This is what the linger card will show — e.g. SHOUT not TALKING.
        std::string prevDisplay = getDisplayTalkState(static_cast<int>(user));
#endif
        setTalkState(static_cast<int>(user), name);

        if (isActiveState(prev) && !isActiveState(name)) {
#if defined(_WIN32)
            g.linger.erase(std::remove_if(g.linger.begin(), g.linger.end(),
                [user](const LingerEntry &e) { return e.userId == static_cast<int>(user); }),
                g.linger.end());
            g.linger.push_back({static_cast<int>(user),
                GetTickCount() + static_cast<DWORD>(g.config.lingerMs), prevDisplay});
#endif
        }
    }
    scheduleRepaint();
}

static void recordSelf(mumble_connection_t conn) {
    bool muted = false, deafened = false;
    if (g.api.isLocalUserMuted)    g.api.isLocalUserMuted(g.id, &muted);
    if (g.api.isLocalUserDeafened) g.api.isLocalUserDeafened(g.id, &deafened);
    { std::lock_guard<std::mutex> lk(g.mutex); g.selfMuted = muted; g.selfDeafened = deafened; }
    (void)conn;
    scheduleRepaint();
}

static void recordChannelRoster(mumble_connection_t conn) {
    if (!g.api.getChannelOfUser || !g.api.getUsersInChannel) return;
    mumble_channelid_t channel = -1;
    if (g.api.getChannelOfUser(g.id, conn, g.localUser, &channel) != MUMBLE_EC_OK) return;
    mumble_userid_t *users = nullptr; size_t count = 0;
    if (g.api.getUsersInChannel(g.id, conn, channel, &users, &count) != MUMBLE_EC_OK) return;
    for (size_t i = 0; i < count; ++i) recordUser(conn, users[i], true);
    if (users) g.api.freeMemory(g.id, users);
}

}  // namespace

// ===========================================================================
//  Mumble plugin entry points
// ===========================================================================

mumble_error_t mumble_init(mumble_plugin_id_t id) {
    g.id = id; startOverlay(); return MUMBLE_STATUS_OK;
}
void mumble_shutdown() { stopOverlay(); }

struct MumbleStringWrapper mumble_getName() {
    return { PLUGIN_NAME, std::strlen(PLUGIN_NAME), false };
}
mumble_version_t mumble_getAPIVersion() {
    return { 1, 0, 0 };
}
void mumble_registerAPIFunctions(void *api) {
    if (api) { g.api = *reinterpret_cast<mumble_api_t *>(api); g.apiValid = true; }
}
void mumble_releaseResource(const void *) {}
void mumble_setMumbleInfo(mumble_version_t, mumble_version_t, mumble_version_t) {}
mumble_version_t mumble_getVersion() { return { 1, 0, 0 }; }
struct MumbleStringWrapper mumble_getAuthor() {
    return { PLUGIN_AUTHOR, std::strlen(PLUGIN_AUTHOR), false };
}
struct MumbleStringWrapper mumble_getDescription() {
    return { PLUGIN_DESC, std::strlen(PLUGIN_DESC), false };
}
uint32_t mumble_getFeatures() { return MUMBLE_FEATURE_NONE; }

void mumble_onServerConnected(mumble_connection_t conn) {
    std::lock_guard<std::mutex> lk(g.mutex);
    g.connection = conn; g.synchronized = false;
    g.known.clear(); g.users.clear(); g.talkStates.clear();
#if defined(_WIN32)
    g.linger.clear();
#endif
}
void mumble_onServerDisconnected(mumble_connection_t) {
    {
        std::lock_guard<std::mutex> lk(g.mutex);
        g.connection = -1; g.synchronized = false;
        g.known.clear(); g.users.clear(); g.talkStates.clear();
#if defined(_WIN32)
        g.linger.clear();
#endif
    }
    scheduleRepaint();
}
void mumble_onServerSynchronized(mumble_connection_t conn) {
    if (!apiReady()) return;
    {
        std::lock_guard<std::mutex> lk(g.mutex);
        g.connection = conn; g.synchronized = true; g.known.clear();
    }
    if (g.api.getLocalUserID) {
        mumble_userid_t local = 0;
        if (g.api.getLocalUserID(g.id, conn, &local) == MUMBLE_EC_OK) {
            std::lock_guard<std::mutex> lk(g.mutex); g.localUser = local;
        }
    }
    recordSelf(conn);
    recordChannelRoster(conn);
}
void mumble_onChannelEntered(mumble_connection_t conn, mumble_userid_t uid,
                              mumble_channelid_t, mumble_channelid_t) {
    if (!apiReady()) return;
    if (uid == g.localUser) recordChannelRoster(conn);
    else recordUser(conn, uid, true);
}
void mumble_onChannelExited(mumble_connection_t, mumble_userid_t uid, mumble_channelid_t) {
    {
        std::lock_guard<std::mutex> lk(g.mutex);
        removeUser(static_cast<int>(uid));
        for (auto it = g.known.begin(); it != g.known.end(); ++it) {
            if (*it == uid) { g.known.erase(it); break; }
        }
#if defined(_WIN32)
        g.linger.erase(std::remove_if(g.linger.begin(), g.linger.end(),
            [uid](const LingerEntry &e) { return e.userId == static_cast<int>(uid); }),
            g.linger.end());
#endif
    }
    scheduleRepaint();
}
void mumble_onUserTalkingStateChanged(mumble_connection_t conn, mumble_userid_t uid,
                                      mumble_talking_state_t state) {
    if (!apiReady()) return;
    if (!isKnown(uid)) recordUser(conn, uid, true);
    recordTalk(uid, state);
}
