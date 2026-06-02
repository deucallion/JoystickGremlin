// Mumble Voice Overlay — self-contained Mumble plugin.
//
// On Windows: draws its own transparent, always-on-top overlay window using
// GDI+. The window is an ordinary OS window (never injected into the game
// process), so EAC has nothing to object to.
//   - Ctrl+Shift+V   toggle lock / unlock (unlock to drag into position)
//   - Right-click    context menu when unlocked
//
// On Linux/macOS: streams talking-state JSON over localhost UDP (:27812) for
// the companion Python overlay app (see overlay/ in the repository).

#include "MumblePlugin.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static uint64_t monoMs() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count();
}

#ifdef _WIN32
#   define WIN32_LEAN_AND_MEAN
#   include <windows.h>
#   include <gdiplus.h>
#   include <shlobj.h>
#   pragma comment(lib, "gdiplus.lib")
#   pragma comment(lib, "user32.lib")
#   pragma comment(lib, "gdi32.lib")
#   pragma comment(lib, "shell32.lib")
#else
#   include <arpa/inet.h>
#   include <netinet/in.h>
#   include <sys/socket.h>
#   include <unistd.h>
#endif

namespace {

// ---------------------------------------------------------------------------
// Plugin metadata
// ---------------------------------------------------------------------------

constexpr const char *PLUGIN_NAME   = "Mumble Voice Overlay (Star Citizen)";
constexpr const char *PLUGIN_AUTHOR = "Mumble Voice Overlay";
constexpr const char *PLUGIN_DESC   =
    "EAC-safe voice overlay. On Windows draws its own always-on-top window "
    "(no extra app needed). Use Ctrl+Shift+V to unlock/move.";

// ---------------------------------------------------------------------------
// Shared Mumble state (both platforms)
// ---------------------------------------------------------------------------

mumble_plugin_id_t  g_pluginId    = 0;
mumble_api_t        g_api         = {};
bool                g_apiValid    = false;
mumble_connection_t g_connection  = -1;
mumble_userid_t     g_localUser   = 0;
bool                g_connected   = false;

struct Speaker {
    mumble_userid_t  id            = 0;
    std::string      name;
    std::string      channel;
    std::string      comment;      // HTML stripped
    bool             locallyMuted  = false;
    bool             isSelf        = false;
    std::string      state         = "passive";
    bool             selfMuted     = false;
    bool             selfDeafened  = false;
    uint64_t         passiveSince  = 0; // monoMs() when went passive
};

std::mutex                         g_mutex;
std::map<mumble_userid_t, Speaker> g_speakers;
bool                               g_selfMuted    = false;
bool                               g_selfDeafened = false;

// ---------------------------------------------------------------------------
// Helpers shared by both platforms
// ---------------------------------------------------------------------------

static std::string stripHtml(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    bool inTag = false;
    for (char c : s) {
        if (c == '<') { inTag = true; continue; }
        if (c == '>') { inTag = false; continue; }
        if (!inTag) {
            if (c == '&') { out += ' '; continue; } // crude entity removal
            out.push_back(c);
        }
    }
    // Collapse whitespace
    std::string result;
    bool prevSpace = true;
    for (char c : out) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!prevSpace) { result.push_back(' '); prevSpace = true; }
        } else {
            result.push_back(c);
            prevSpace = false;
        }
    }
    while (!result.empty() && result.back() == ' ') result.pop_back();
    return result;
}

static std::string apiGetString(
    mumble_error_t (MUMBLE_PLUGIN_CALLING_CONVENTION *fn)(
        mumble_plugin_id_t, mumble_connection_t, mumble_userid_t, const char **),
    mumble_connection_t conn, mumble_userid_t uid)
{
    if (!fn || !g_apiValid) return {};
    const char *raw = nullptr;
    if (fn(g_pluginId, conn, uid, &raw) == MUMBLE_EC_OK && raw) {
        std::string s(raw);
        g_api.freeMemory(g_pluginId, raw);
        return s;
    }
    return {};
}

static void populateSpeaker(Speaker &sp, mumble_connection_t conn) {
    sp.name    = apiGetString(g_api.getUserName,    conn, sp.id);
    sp.comment = stripHtml(apiGetString(g_api.getUserComment, conn, sp.id));
    if (sp.name.empty()) sp.name = "User " + std::to_string(sp.id);

    if (g_api.getChannelOfUser && g_api.getChannelName) {
        mumble_channelid_t ch = -1;
        if (g_api.getChannelOfUser(g_pluginId, conn, sp.id, &ch) == MUMBLE_EC_OK) {
            const char *cn = nullptr;
            if (g_api.getChannelName(g_pluginId, conn, ch, &cn) == MUMBLE_EC_OK && cn) {
                sp.channel = cn;
                g_api.freeMemory(g_pluginId, cn);
            }
        }
    }
    if (g_api.isUserLocallyMuted) {
        bool m = false;
        g_api.isUserLocallyMuted(g_pluginId, conn, sp.id, &m);
        sp.locallyMuted = m;
    }
}

// ---------------------------------------------------------------------------
// Active-state predicate (states that show a card)
// ---------------------------------------------------------------------------
static bool isActive(const Speaker &sp) {
    return sp.state == "talking" || sp.state == "whispering"
        || sp.state == "shouting" || sp.state == "muted";
}

// ///////////////////////////////////////////////////////////////////////////
// WINDOWS — native GDI+ overlay window
// ///////////////////////////////////////////////////////////////////////////
#ifdef _WIN32

static HINSTANCE g_hInst = nullptr;     // set in DllMain
static HWND      g_hwnd  = nullptr;
static ULONG_PTR g_gdipToken = 0;
static std::thread g_windowThread;
static int       g_animFrame = 0;       // for pulse bar animation

// --- Settings (INI in %APPDATA%\MumbleVoiceOverlay\config.ini) ---

struct Settings {
    int  x         = 48;
    int  y         = 120;
    int  width     = 320;
    int  lingerMs  = 1200;
    bool showSelf    = true;
    bool showChannel = true;
    bool showComment = true;
    bool locked      = true;
    int  opacity     = 210;   // 0-255 background alpha

    std::wstring iniPath() const {
        wchar_t buf[MAX_PATH] = {};
        SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, buf);
        std::wstring dir = std::wstring(buf) + L"\\MumbleVoiceOverlay";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir + L"\\config.ini";
    }

    void load() {
        auto p = iniPath();
        auto gi = [&](const wchar_t *k, int d) {
            return (int)GetPrivateProfileIntW(L"overlay", k, d, p.c_str());
        };
        x           = gi(L"x",           x);
        y           = gi(L"y",           y);
        width       = gi(L"width",       width);
        lingerMs    = gi(L"linger_ms",   lingerMs);
        showSelf    = gi(L"show_self",   showSelf    ? 1 : 0) != 0;
        showChannel = gi(L"show_channel",showChannel ? 1 : 0) != 0;
        showComment = gi(L"show_comment",showComment ? 1 : 0) != 0;
        locked      = gi(L"locked",      locked      ? 1 : 0) != 0;
        opacity     = gi(L"opacity",     opacity);
    }

    void save() const {
        auto p = iniPath();
        auto ws = [&](const wchar_t *k, int v) {
            WritePrivateProfileStringW(L"overlay", k,
                std::to_wstring(v).c_str(), p.c_str());
        };
        ws(L"x",           x);
        ws(L"y",           y);
        ws(L"width",       width);
        ws(L"linger_ms",   lingerMs);
        ws(L"show_self",   showSelf    ? 1 : 0);
        ws(L"show_channel",showChannel ? 1 : 0);
        ws(L"show_comment",showComment ? 1 : 0);
        ws(L"locked",      locked      ? 1 : 0);
        ws(L"opacity",     opacity);
    }
} g_cfg;

// --- Context menu IDs ---
enum MenuId : UINT {
    IDM_LOCK        = 1,
    IDM_SHOW_SELF   = 2,
    IDM_SHOW_CHANNEL= 3,
    IDM_SHOW_COMMENT= 4,
};
constexpr int  HOTKEY_ID   = 42;
constexpr UINT TIMER_ANIM  = 1;

// --- GDI+ helpers ---

static std::wstring toWide(const std::string &s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(n - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

static void fillRoundRect(Gdiplus::Graphics &g, Gdiplus::Color col,
                           float x, float y, float w, float h, float r)
{
    Gdiplus::GraphicsPath path;
    path.AddArc(x,         y,         r*2, r*2, 180, 90);
    path.AddArc(x+w-r*2,  y,         r*2, r*2, 270, 90);
    path.AddArc(x+w-r*2,  y+h-r*2,   r*2, r*2,   0, 90);
    path.AddArc(x,         y+h-r*2,   r*2, r*2,  90, 90);
    path.CloseFigure();
    Gdiplus::SolidBrush br(col);
    g.FillPath(&br, &path);
}

// Accent color for a talking state (fully opaque).
static Gdiplus::Color stateColor(const std::string &state) {
    if (state == "talking")    return {255,   0, 229, 255}; // #00E5FF cyan
    if (state == "whispering") return {255, 167, 139, 250}; // #A78BFA purple
    if (state == "shouting")   return {255, 251, 191,  36}; // #FBBF24 amber
    if (state == "muted")      return {255, 251, 113, 133}; // #FB7185 red
    return                            {255,   0, 229, 255};
}

static const wchar_t *stateLabel(const std::string &state) {
    if (state == "talking")    return L"TALKING";
    if (state == "whispering") return L"WHISPER";
    if (state == "shouting")   return L"SHOUT";
    if (state == "muted")      return L"MIC MUTED";
    return L"TALKING";
}

// Draw 4 animated equalizer bars.
static void drawPulse(Gdiplus::Graphics &g, Gdiplus::Color accent,
                       float x, float y, float areaH, bool active)
{
    static const float heights[8] = {0.35f, 0.85f, 0.55f, 1.0f,
                                      0.45f, 0.70f, 0.90f, 0.40f};
    constexpr int   N    = 4;
    constexpr float barW = 3.0f;
    constexpr float gap  = 3.5f;
    constexpr float totalW = N * barW + (N - 1) * gap;
    float ox = x + (26.0f - totalW) / 2.0f;

    Gdiplus::SolidBrush br(accent);
    for (int i = 0; i < N; i++) {
        float frac = active ? heights[(g_animFrame + i * 2) % 8] : 0.25f;
        float bh   = std::max(3.0f, frac * (areaH - 6.0f));
        float bx   = ox + i * (barW + gap);
        float by   = y + (areaH - bh) / 2.0f;
        g.FillRectangle(&br, Gdiplus::RectF(bx, by, barW, bh));
    }
}

// --- Card geometry constants ---
constexpr int PAD_H  = 12;   // horizontal inner padding
constexpr int PAD_V  = 10;   // vertical inner padding
constexpr int PULW   = 26;   // width reserved for pulse bars column
constexpr int RADD   = 7;    // rounded corner radius
constexpr int CARD_GAP = 8;  // gap between cards

static int cardHeight(bool hasComment) {
    // top_pad + name(18) + gap(3) + meta(14) + [gap(3)+comment(13)] + bot_pad
    int h = PAD_V + 18 + 3 + 14 + PAD_V;
    if (hasComment) h += 3 + 13;
    return h;
}

static void drawCard(Gdiplus::Graphics &g, const Speaker &sp, int cx, int cy, int cw) {
    bool hasComment = g_cfg.showComment && !sp.comment.empty();
    int ch = cardHeight(hasComment);
    Gdiplus::Color accent = stateColor(sp.state);
    BYTE op = (BYTE)g_cfg.opacity;

    // Background
    fillRoundRect(g, Gdiplus::Color(op, 12, 18, 24),
                  (float)cx, (float)cy, (float)cw, (float)ch, (float)RADD);

    // Left accent bar
    {
        Gdiplus::SolidBrush br(Gdiplus::Color(op, accent.GetR(), accent.GetG(), accent.GetB()));
        g.FillRectangle(&br, Gdiplus::RectF((float)cx, (float)(cy + 4), 3.0f, (float)(ch - 8)));
    }

    // Corner brackets (top-right, bottom-right)
    {
        Gdiplus::Pen pen(Gdiplus::Color(180, accent.GetR(), accent.GetG(), accent.GetB()), 1.5f);
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        int rx = cx + cw - 6, arm = 12;
        g.DrawLine(&pen, rx - arm, cy + 5,        rx,      cy + 5);
        g.DrawLine(&pen, rx,       cy + 5,        rx,      cy + 5 + arm);
        g.DrawLine(&pen, rx,       cy + ch - 6 - arm, rx, cy + ch - 6);
        g.DrawLine(&pen, rx - arm, cy + ch - 6,   rx,      cy + ch - 6);
    }

    // Pulse bars
    float areaH = (float)(ch - 2 * PAD_V);
    drawPulse(g, accent, (float)(cx + PAD_H), (float)(cy + PAD_V), areaH,
              sp.state != "muted");

    // Text origin
    float tx = (float)(cx + PAD_H + PULW + 6);
    float tw = (float)(cw - (cx + PAD_H + PULW + 6) - PAD_H);
    float ty = (float)(cy + PAD_V);

    Gdiplus::StringFormat sfClip;
    sfClip.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    sfClip.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

    // Name
    {
        std::wstring nameW = toWide(sp.name);
        if (sp.isSelf) nameW += L"  (you)";
        Gdiplus::FontFamily ff(L"Segoe UI");
        Gdiplus::Font font(&ff, 13, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush br(Gdiplus::Color(255, 230, 237, 243));
        g.DrawString(nameW.c_str(), -1, &font,
                     Gdiplus::RectF(tx, ty, tw, 20.0f), &sfClip, &br);
        ty += 21.0f;
    }

    // Meta line: STATE · channel · tags
    {
        std::wstring meta = stateLabel(sp.state);
        if (g_cfg.showChannel && !sp.channel.empty()) {
            meta += L"  ·  "; meta += toWide(sp.channel);
        }
        if (sp.locallyMuted) meta += L"  ·  LOCAL MUTED";
        if (sp.isSelf) {
            if (sp.selfDeafened)    meta += L"  ·  DEAFENED";
            else if (sp.selfMuted)  meta += L"  ·  SELF MUTED";
        }
        Gdiplus::FontFamily ff(L"Consolas");
        Gdiplus::Font font(&ff, 9, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush br(accent);
        g.DrawString(meta.c_str(), -1, &font,
                     Gdiplus::RectF(tx, ty + 1.0f, tw, 16.0f), &sfClip, &br);
        ty += 17.0f;
    }

    // Comment
    if (hasComment) {
        std::wstring cmt = L"“" + toWide(sp.comment) + L"”";
        if (cmt.size() > 65) { cmt.resize(63); cmt += L"…”"; }
        Gdiplus::FontFamily ff(L"Segoe UI");
        Gdiplus::Font font(&ff, 9, Gdiplus::FontStyleItalic, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush br(Gdiplus::Color(255, 107, 114, 128));
        g.DrawString(cmt.c_str(), -1, &font,
                     Gdiplus::RectF(tx, ty + 2.0f, tw, 15.0f), &sfClip, &br);
    }
}

static void drawPlaceholder(Gdiplus::Graphics &g, int w, int h) {
    // Dashed cyan border + label — only visible in unlocked mode
    Gdiplus::Color cyan(180, 0, 229, 255);
    fillRoundRect(g, Gdiplus::Color(30, 0, 229, 255), 1, 1, w - 2, h - 2, 7);
    Gdiplus::Pen pen(cyan, 1.0f);
    pen.SetDashStyle(Gdiplus::DashStyleDash);
    Gdiplus::GraphicsPath border;
    border.AddArc(1.0f, 1.0f, 14, 14, 180, 90);
    border.AddArc((float)(w - 15), 1.0f, 14, 14, 270, 90);
    border.AddArc((float)(w - 15), (float)(h - 15), 14, 14, 0, 90);
    border.AddArc(1.0f, (float)(h - 15), 14, 14, 90, 90);
    border.CloseFigure();
    g.DrawPath(&pen, &border);

    Gdiplus::FontFamily ff(L"Consolas");
    Gdiplus::Font font(&ff, 9, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush br(cyan);
    Gdiplus::StringFormat sf;
    sf.SetAlignment(Gdiplus::StringAlignmentCenter);
    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    g.DrawString(L"VOICE OVERLAY  //  DRAG TO POSITION", -1, &font,
                 Gdiplus::RectF(0, 0, (float)w, (float)h), &sf, &br);
}

// --- Core redraw ---------------------------------------------------------
// Called only from the window thread (timer + WM_OVERLAY_UPDATE).

static void redraw() {
    if (!g_hwnd) return;

    // Snapshot speaker state under lock
    std::vector<Speaker> cards;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        uint64_t now = monoMs();
        for (auto &[id, sp] : g_speakers) {
            // Enforce linger: skip passive speakers whose linger has expired
            if (!isActive(sp)) {
                if (sp.passiveSince && (now - sp.passiveSince) > (uint64_t)g_cfg.lingerMs)
                    continue;
                // Include during linger window (non-active state shows card fading)
                if (!sp.passiveSince) continue; // never went active, skip
            }
            if (!g_cfg.showSelf && sp.isSelf) continue;
            Speaker copy = sp;
            copy.selfMuted    = g_selfMuted;
            copy.selfDeafened = g_selfDeafened;
            cards.push_back(copy);
        }
        // Sort: non-self by name, self last
        std::sort(cards.begin(), cards.end(), [](const Speaker &a, const Speaker &b) {
            if (a.isSelf != b.isSelf) return !a.isSelf;
            return a.name < b.name;
        });
    }

    constexpr int PLACEHOLDER_H = 44;
    int cw = g_cfg.width;
    int totalH = 0;

    if (!g_cfg.locked) {
        totalH = PLACEHOLDER_H;
    } else if (cards.empty()) {
        ShowWindow(g_hwnd, SW_HIDE);
        return;
    } else {
        for (const auto &sp : cards) {
            bool hasComment = g_cfg.showComment && !sp.comment.empty();
            totalH += cardHeight(hasComment) + CARD_GAP;
        }
        totalH -= CARD_GAP;
    }

    // Create 32-bit DIB section for GDI+ and UpdateLayeredWindow
    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem    = CreateCompatibleDC(hdcScreen);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = cw;
    bmi.bmiHeader.biHeight      = -totalH; // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void   *pvBits = nullptr;
    HBITMAP hbm    = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &pvBits, nullptr, 0);
    if (!hbm) { DeleteDC(hdcMem); ReleaseDC(nullptr, hdcScreen); return; }
    HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, hbm);
    memset(pvBits, 0, (size_t)cw * totalH * 4);

    // GDI+ drawing
    {
        Gdiplus::Graphics g(hdcMem);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

        if (!g_cfg.locked) {
            drawPlaceholder(g, cw, totalH);
        } else {
            int y = 0;
            for (const auto &sp : cards) {
                bool hasComment = g_cfg.showComment && !sp.comment.empty();
                drawCard(g, sp, 0, y, cw);
                y += cardHeight(hasComment) + CARD_GAP;
            }
        }
    }

    // Pre-multiply alpha (required by UpdateLayeredWindow with AC_SRC_ALPHA)
    {
        BYTE *px = (BYTE *)pvBits;
        int   total = cw * totalH;
        for (int i = 0; i < total; ++i, px += 4) {
            BYTE a = px[3];
            px[0] = (BYTE)((px[0] * (UINT)a + 127) / 255);
            px[1] = (BYTE)((px[1] * (UINT)a + 127) / 255);
            px[2] = (BYTE)((px[2] * (UINT)a + 127) / 255);
        }
    }

    POINT       ptSrc   = {0, 0};
    POINT       ptDst   = {g_cfg.x, g_cfg.y};
    SIZE        sz      = {cw, totalH};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};

    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    UpdateLayeredWindow(g_hwnd, hdcScreen, &ptDst, &sz, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);

    SelectObject(hdcMem, hbmOld);
    DeleteObject(hbm);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
}

// --- Drag state (unlocked mode) ---
static bool  g_dragging = false;
static POINT g_dragOffset = {};

// --- Window procedure ---

static constexpr UINT WM_OVERLAY_UPDATE = WM_USER + 1;

static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_HOTKEY:
        if (wp == HOTKEY_ID) {
            g_cfg.locked = !g_cfg.locked;
            // Toggle click-through
            LONG ex = GetWindowLong(hwnd, GWL_EXSTYLE);
            if (g_cfg.locked) ex |=  WS_EX_TRANSPARENT;
            else               ex &= ~WS_EX_TRANSPARENT;
            SetWindowLong(hwnd, GWL_EXSTYLE, ex);
            g_cfg.save();
            redraw();
        }
        break;

    case WM_TIMER:
        if (wp == TIMER_ANIM) {
            ++g_animFrame;
            // Expire passive-linger speakers and trigger redraw
            bool changed = false;
            {
                std::lock_guard<std::mutex> lk(g_mutex);
                uint64_t now = monoMs();
                for (auto &[id, sp] : g_speakers) {
                    if (!isActive(sp) && sp.passiveSince
                        && (now - sp.passiveSince) > (uint64_t)g_cfg.lingerMs) {
                        sp.passiveSince = 0;
                        changed = true;
                    }
                }
            }
            (void)changed;
            redraw();
        }
        break;

    case WM_OVERLAY_UPDATE:
        redraw();
        break;

    // Drag support when unlocked
    case WM_LBUTTONDOWN:
        if (!g_cfg.locked) {
            g_dragging = true;
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ClientToScreen(hwnd, &pt);
            RECT rc; GetWindowRect(hwnd, &rc);
            g_dragOffset.x = pt.x - rc.left;
            g_dragOffset.y = pt.y - rc.top;
            SetCapture(hwnd);
        }
        break;

    case WM_MOUSEMOVE:
        if (g_dragging) {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ClientToScreen(hwnd, &pt);
            g_cfg.x = pt.x - g_dragOffset.x;
            g_cfg.y = pt.y - g_dragOffset.y;
            redraw();
        }
        break;

    case WM_LBUTTONUP:
        if (g_dragging) {
            g_dragging = false;
            ReleaseCapture();
            g_cfg.save();
        }
        break;

    case WM_RBUTTONUP:
        if (!g_cfg.locked) {
            POINT pt; GetCursorPos(&pt);
            HMENU hm = CreatePopupMenu();
            AppendMenuW(hm, MF_STRING | (g_cfg.showSelf    ? MF_CHECKED : 0), IDM_SHOW_SELF,    L"Show my own card");
            AppendMenuW(hm, MF_STRING | (g_cfg.showChannel ? MF_CHECKED : 0), IDM_SHOW_CHANNEL, L"Show channel");
            AppendMenuW(hm, MF_STRING | (g_cfg.showComment ? MF_CHECKED : 0), IDM_SHOW_COMMENT, L"Show comment");
            AppendMenuW(hm, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(hm, MF_STRING, IDM_LOCK, L"Lock (click-through)  \tCtrl+Shift+V");
            SetForegroundWindow(hwnd);
            TrackPopupMenu(hm, TPM_RIGHTBUTTON | TPM_TOPALIGN | TPM_LEFTALIGN,
                           pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(hm);
        }
        break;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_LOCK:
            PostMessage(hwnd, WM_HOTKEY, HOTKEY_ID, 0); // reuse hotkey handler
            break;
        case IDM_SHOW_SELF:
            g_cfg.showSelf = !g_cfg.showSelf; g_cfg.save(); redraw(); break;
        case IDM_SHOW_CHANNEL:
            g_cfg.showChannel = !g_cfg.showChannel; g_cfg.save(); redraw(); break;
        case IDM_SHOW_COMMENT:
            g_cfg.showComment = !g_cfg.showComment; g_cfg.save(); redraw(); break;
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return 0;
}

// --- Window thread -------------------------------------------------------

static void windowThreadMain() {
    // Register window class
    WNDCLASSEXW wc      = {};
    wc.cbSize           = sizeof(wc);
    wc.lpfnWndProc      = OverlayWndProc;
    wc.hInstance        = g_hInst;
    wc.lpszClassName    = L"MumbleVoiceOverlay";
    wc.hCursor          = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    DWORD exStyle = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    if (g_cfg.locked) exStyle |= WS_EX_TRANSPARENT;

    g_hwnd = CreateWindowExW(
        exStyle, L"MumbleVoiceOverlay", L"MumbleVoiceOverlay",
        WS_POPUP,
        g_cfg.x, g_cfg.y, g_cfg.width, 60,
        nullptr, nullptr, g_hInst, nullptr);

    RegisterHotKey(g_hwnd, HOTKEY_ID, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'V');
    SetTimer(g_hwnd, TIMER_ANIM, 120, nullptr); // ~8 fps animation tick
    redraw();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnregisterHotKey(g_hwnd, HOTKEY_ID);
    KillTimer(g_hwnd, TIMER_ANIM);
    DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
    UnregisterClassW(L"MumbleVoiceOverlay", g_hInst);
}

static void notifyWindow() {
    if (g_hwnd) PostMessage(g_hwnd, WM_OVERLAY_UPDATE, 0, 0);
}

// Windows init / shutdown
static void platformInit() {
    g_cfg.load();
    Gdiplus::GdiplusStartupInput si;
    Gdiplus::GdiplusStartup(&g_gdipToken, &si, nullptr);
    g_windowThread = std::thread(windowThreadMain);
}

static void platformShutdown() {
    if (g_hwnd) PostMessage(g_hwnd, WM_DESTROY, 0, 0);
    if (g_windowThread.joinable()) g_windowThread.join();
    if (g_gdipToken) { Gdiplus::GdiplusShutdown(g_gdipToken); g_gdipToken = 0; }
}

// ///////////////////////////////////////////////////////////////////////////
// NON-WINDOWS — UDP stream to companion Python overlay
// ///////////////////////////////////////////////////////////////////////////
#else

#include <cstdio>
#include <cstdlib>
using socket_t = int;
static const socket_t INVALID_SOCK = -1;
static socket_t g_sock = INVALID_SOCK;
static struct sockaddr_in g_udpDest = {};

static void appendJsonString(std::string &out, const char *v) {
    out.push_back('"');
    if (v) for (const char *p = v; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c == '"')  { out += "\\\""; }
        else if (c == '\\') { out += "\\\\"; }
        else if (c < 0x20) { char b[8]; snprintf(b,8,"\\u%04x",c); out+=b; }
        else out.push_back((char)c);
    }
    out.push_back('"');
}

static void udpEmit(const std::string &json) {
    if (g_sock == INVALID_SOCK) return;
    std::string line = json + "\n";
    sendto(g_sock, line.c_str(), line.size(), 0,
           (struct sockaddr *)&g_udpDest, sizeof(g_udpDest));
}

static void udpEmitTalk(mumble_userid_t uid, const char *state) {
    std::string j = "{";
    appendJsonString(j, "t"); j += ":"; appendJsonString(j, "talk"); j += ",";
    appendJsonString(j, "id"); j += ":" + std::to_string(uid) + ",";
    appendJsonString(j, "state"); j += ":"; appendJsonString(j, state);
    j += "}";
    udpEmit(j);
}

static void platformInit() {
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCK) return;
    const char *host = getenv("MUMBLE_VOICE_OVERLAY_HOST");
    const char *port = getenv("MUMBLE_VOICE_OVERLAY_PORT");
    uint16_t p = port ? (uint16_t)atoi(port) : 27812;
    g_udpDest.sin_family = AF_INET;
    g_udpDest.sin_port   = htons(p);
    inet_pton(AF_INET, host ? host : "127.0.0.1", &g_udpDest.sin_addr);
}

static void platformShutdown() {
    if (g_sock != INVALID_SOCK) { close(g_sock); g_sock = INVALID_SOCK; }
}

static void notifyWindow() {} // no-op; UDP is emitted inline in callbacks

#endif // _WIN32

// ///////////////////////////////////////////////////////////////////////////
// Mumble callback helpers (platform-agnostic)
// ///////////////////////////////////////////////////////////////////////////

static void updateSpeakerFromMumble(mumble_userid_t uid, mumble_connection_t conn) {
    Speaker sp;
    sp.id     = uid;
    sp.isSelf = (uid == g_localUser);
    populateSpeaker(sp, conn);
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        auto it = g_speakers.find(uid);
        if (it != g_speakers.end()) {
            // Preserve live state
            sp.state       = it->second.state;
            sp.passiveSince= it->second.passiveSince;
        }
        g_speakers[uid] = sp;
    }
}

static void onTalkStateChanged(mumble_userid_t uid, const char *stateStr,
                                mumble_connection_t conn)
{
    bool needDesc = false;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        auto &sp = g_speakers[uid];
        sp.id = uid;
        if (sp.name.empty()) needDesc = true;
        sp.isSelf = (uid == g_localUser);
        bool wasActive = isActive(sp);
        sp.state = stateStr;
        bool nowActive = isActive(sp);
        if (!nowActive && wasActive) sp.passiveSince = monoMs();
        if (nowActive)               sp.passiveSince = 0;
    }
    if (needDesc) updateSpeakerFromMumble(uid, conn);

#ifndef _WIN32
    udpEmitTalk(uid, stateStr);
#endif
    notifyWindow();
}

static void emitChannelRoster(mumble_connection_t conn) {
    if (!g_api.getChannelOfUser || !g_api.getUsersInChannel || !g_apiValid) return;
    mumble_channelid_t ch = -1;
    if (g_api.getChannelOfUser(g_pluginId, conn, g_localUser, &ch) != MUMBLE_EC_OK) return;
    mumble_userid_t *users = nullptr;
    size_t count = 0;
    if (g_api.getUsersInChannel(g_pluginId, conn, ch, &users, &count) != MUMBLE_EC_OK) return;
    for (size_t i = 0; i < count; ++i)
        updateSpeakerFromMumble(users[i], conn);
    if (users) g_api.freeMemory(g_pluginId, users);
    notifyWindow();
}

} // namespace

// ---------------------------------------------------------------------------
// DllMain (Windows only) — capture HINSTANCE before anything else runs
// ---------------------------------------------------------------------------
#ifdef _WIN32
BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hInst = hInst;
        DisableThreadLibraryCalls(hInst);
    }
    return TRUE;
}
#endif

// ///////////////////////////////////////////////////////////////////////////
// Mumble plugin API exports
// ///////////////////////////////////////////////////////////////////////////

mumble_error_t mumble_init(mumble_plugin_id_t id) {
    g_pluginId = id;
    platformInit();
    return MUMBLE_STATUS_OK;
}

void mumble_shutdown() {
    platformShutdown();
}

struct MumbleStringWrapper mumble_getName() {
    return { PLUGIN_NAME, strlen(PLUGIN_NAME), false };
}

mumble_version_t mumble_getAPIVersion() {
    return { 1, 0, 0 };
}

void mumble_registerAPIFunctions(void *api) {
    if (api) { g_api = *reinterpret_cast<mumble_api_t *>(api); g_apiValid = true; }
}

void mumble_releaseResource(const void *) {}

void mumble_setMumbleInfo(mumble_version_t, mumble_version_t, mumble_version_t) {}

mumble_version_t mumble_getVersion() { return { 1, 0, 0 }; }

struct MumbleStringWrapper mumble_getAuthor() {
    return { PLUGIN_AUTHOR, strlen(PLUGIN_AUTHOR), false };
}

struct MumbleStringWrapper mumble_getDescription() {
    return { PLUGIN_DESC, strlen(PLUGIN_DESC), false };
}

uint32_t mumble_getFeatures() { return MUMBLE_FEATURE_NONE; }

void mumble_onServerConnected(mumble_connection_t conn) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_connection  = conn;
    g_connected   = false;
    g_speakers.clear();
}

void mumble_onServerDisconnected(mumble_connection_t) {
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_connection = -1;
        g_connected  = false;
        g_speakers.clear();
    }
    notifyWindow();
}

void mumble_onServerSynchronized(mumble_connection_t conn) {
    if (!g_apiValid) return;
    g_connected = true;
    if (g_api.getLocalUserID)
        g_api.getLocalUserID(g_pluginId, conn, &g_localUser);

    bool m = false, d = false;
    if (g_api.isLocalUserMuted)    g_api.isLocalUserMuted(g_pluginId, &m);
    if (g_api.isLocalUserDeafened) g_api.isLocalUserDeafened(g_pluginId, &d);
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_selfMuted    = m;
        g_selfDeafened = d;
    }
    emitChannelRoster(conn);
}

void mumble_onChannelEntered(mumble_connection_t conn, mumble_userid_t uid,
                              mumble_channelid_t, mumble_channelid_t) {
    if (!g_apiValid) return;
    if (uid == g_localUser) emitChannelRoster(conn);
    else { updateSpeakerFromMumble(uid, conn); notifyWindow(); }
}

void mumble_onChannelExited(mumble_connection_t, mumble_userid_t uid, mumble_channelid_t) {
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_speakers.erase(uid);
    }
    notifyWindow();
}

void mumble_onUserTalkingStateChanged(mumble_connection_t conn,
                                       mumble_userid_t uid,
                                       mumble_talking_state_t state)
{
    const char *s = "passive";
    switch (state) {
        case MUMBLE_TS_TALKING:       s = "talking";    break;
        case MUMBLE_TS_WHISPERING:    s = "whispering"; break;
        case MUMBLE_TS_SHOUTING:      s = "shouting";   break;
        case MUMBLE_TS_TALKING_MUTED: s = "muted";      break;
        default: break;
    }
    onTalkStateChanged(uid, s, conn);
}
