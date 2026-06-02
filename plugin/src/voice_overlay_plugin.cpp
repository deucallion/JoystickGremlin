// Mumble Voice Overlay -- self-contained native Mumble plugin.
//
// This plugin observes Mumble (who is talking, who is in your channel, what
// Mumble knows about them) and renders an always-on-top transparent overlay
// window directly, using the Win32 API and GDI+. Nothing is injected into the
// game process -- the overlay is an ordinary OS window the desktop compositor
// draws over the game -- so anti-cheat systems like EAC have nothing to object
// to.
//
// No separate overlay application is needed. Install this single DLL into
// Mumble and you're done.

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

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#pragma comment(lib, "Gdiplus.lib")

#else
// On non-Windows platforms the overlay rendering is a no-op stub. The plugin
// still compiles for CI / link-checking but won't display anything. A future
// version could add X11/Wayland rendering here.
#endif

namespace {

// ---- Plugin identity ----------------------------------------------------

constexpr const char *PLUGIN_NAME    = "Mumble Voice Overlay (Star Citizen)";
constexpr const char *PLUGIN_AUTHOR  = "Mumble Voice Overlay";
constexpr const char *PLUGIN_DESC    = "Self-contained EAC-safe voice overlay. Shows who is talking in an always-on-top window over your game.";
constexpr int PROTOCOL_VERSION       = 1;

// ---- Speaker model (replaces the Python protocol.py) --------------------

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
    std::string state;        // "talking", "whispering", "shouting", "muted"
    bool isSelf = false;
    bool selfMuted = false;
    bool selfDeafened = false;
};

static bool isActiveState(const std::string &state) {
    return state == "talking" || state == "whispering" || state == "shouting" || state == "muted";
}

// ---- Overlay configuration ----------------------------------------------

struct OverlayConfig {
    int x = 48;
    int y = 120;
    int width = 360;
    int cardHeight = 64;
    int cardSpacing = 8;
    int maxCards = 8;
    int lingerMs = 1200;
    float opacity = 0.92f;
    bool showSelf = true;
    bool showChannel = true;
    bool locked = true;
};

// ---- Color theme per talking state (Windows only) -----------------------

#if defined(_WIN32)

struct StateTheme {
    DWORD accent;     // ARGB
    const wchar_t *label;
};

static StateTheme themeForState(const std::string &state) {
    if (state == "talking")    return { 0xFF00E5FF, L"TALKING" };
    if (state == "whispering") return { 0xFFA78BFA, L"WHISPER" };
    if (state == "shouting")   return { 0xFFFBBF24, L"SHOUT" };
    if (state == "muted")      return { 0xFFFB7185, L"MIC MUTED" };
    return { 0xFF00E5FF, L"TALKING" };
}

// ---- Linger tracking (keep card visible briefly after silence) -----------

struct LingerEntry {
    int userId;
    DWORD expireTickMs;
};

#endif // _WIN32

// ---- Global state -------------------------------------------------------

struct PluginState {
    std::mutex mutex;
    mumble_plugin_id_t id = 0;
    mumble_api_t api{};
    bool apiValid = false;

    mumble_connection_t connection = -1;
    mumble_userid_t localUser = 0;
    bool synchronized = false;

    std::vector<mumble_userid_t> known;

    // Speaker model state
    std::vector<UserInfo> users;
    struct TalkState { int id; std::string state; };
    std::vector<TalkState> talkStates;
    bool selfMuted = false;
    bool selfDeafened = false;

    // Overlay
    OverlayConfig config;

#if defined(_WIN32)
    HWND overlayWnd = nullptr;
    std::thread overlayThread;
    bool overlayRunning = false;
    ULONG_PTR gdiplusToken = 0;

    // Linger
    std::vector<LingerEntry> linger;
    UINT_PTR lingerTimerId = 0;

    // Drag state
    bool dragging = false;
    POINT dragOffset = {};
#endif
};

PluginState g;

// ---- UserInfo helpers ---------------------------------------------------

static UserInfo *findUser(int id) {
    for (auto &u : g.users) {
        if (u.id == id) return &u;
    }
    return nullptr;
}

static UserInfo &getOrCreateUser(int id) {
    if (auto *u = findUser(id)) return *u;
    g.users.push_back({});
    g.users.back().id = id;
    g.users.back().name = "User " + std::to_string(id);
    return g.users.back();
}

static std::string getTalkState(int id) {
    for (auto &ts : g.talkStates) {
        if (ts.id == id) return ts.state;
    }
    return "passive";
}

static void setTalkState(int id, const std::string &state) {
    for (auto &ts : g.talkStates) {
        if (ts.id == id) { ts.state = state; return; }
    }
    g.talkStates.push_back({id, state});
}

static void removeUser(int id) {
    g.users.erase(std::remove_if(g.users.begin(), g.users.end(),
        [id](const UserInfo &u) { return u.id == id; }), g.users.end());
    g.talkStates.erase(std::remove_if(g.talkStates.begin(), g.talkStates.end(),
        [id](const PluginState::TalkState &ts) { return ts.id == id; }), g.talkStates.end());
}

#if defined(_WIN32)
// Build the current list of active speakers (sorted: self last, then by name).
static std::vector<ActiveSpeaker> buildSnapshot() {
    std::vector<ActiveSpeaker> result;
    for (auto &ts : g.talkStates) {
        if (!isActiveState(ts.state)) continue;
        bool isSelf = (ts.id == static_cast<int>(g.localUser));
        if (!g.config.showSelf && isSelf) continue;

        auto *u = findUser(ts.id);
        ActiveSpeaker sp;
        sp.user = u ? *u : UserInfo{ts.id, "User " + std::to_string(ts.id), {}, -1, {}, {}, false, false};
        sp.state = ts.state;
        sp.isSelf = isSelf || sp.user.isSelf;
        sp.selfMuted = g.selfMuted;
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
#endif // _WIN32

// ---- Mumble API convenience wrappers -----------------------------------

bool apiReady() {
    return g.apiValid && g.id != 0;
}

std::string userName(mumble_connection_t connection, mumble_userid_t user) {
    std::string result;
    if (!g.api.getUserName) return result;
    const char *raw = nullptr;
    if (g.api.getUserName(g.id, connection, user, &raw) == MUMBLE_EC_OK && raw) {
        result = raw;
        g.api.freeMemory(g.id, raw);
    }
    return result;
}

std::string userComment(mumble_connection_t connection, mumble_userid_t user) {
    std::string result;
    if (!g.api.getUserComment) return result;
    const char *raw = nullptr;
    if (g.api.getUserComment(g.id, connection, user, &raw) == MUMBLE_EC_OK && raw) {
        result = raw;
        g.api.freeMemory(g.id, raw);
    }
    return result;
}

std::string userHash(mumble_connection_t connection, mumble_userid_t user) {
    std::string result;
    if (!g.api.getUserHash) return result;
    const char *raw = nullptr;
    if (g.api.getUserHash(g.id, connection, user, &raw) == MUMBLE_EC_OK && raw) {
        result = raw;
        g.api.freeMemory(g.id, raw);
    }
    return result;
}

std::string channelNameOfUser(mumble_connection_t connection, mumble_userid_t user, mumble_channelid_t *outId) {
    std::string result;
    if (!g.api.getChannelOfUser || !g.api.getChannelName) return result;
    mumble_channelid_t channel = -1;
    if (g.api.getChannelOfUser(g.id, connection, user, &channel) != MUMBLE_EC_OK) return result;
    if (outId) *outId = channel;
    const char *raw = nullptr;
    if (g.api.getChannelName(g.id, connection, channel, &raw) == MUMBLE_EC_OK && raw) {
        result = raw;
        g.api.freeMemory(g.id, raw);
    }
    return result;
}

bool userLocallyMuted(mumble_connection_t connection, mumble_userid_t user) {
    if (!g.api.isUserLocallyMuted) return false;
    bool muted = false;
    g.api.isUserLocallyMuted(g.id, connection, user, &muted);
    return muted;
}

const char *talkingStateName(mumble_talking_state_t state) {
    switch (state) {
        case MUMBLE_TS_PASSIVE: return "passive";
        case MUMBLE_TS_TALKING: return "talking";
        case MUMBLE_TS_WHISPERING: return "whispering";
        case MUMBLE_TS_SHOUTING: return "shouting";
        case MUMBLE_TS_TALKING_MUTED: return "muted";
        default: return "invalid";
    }
}

bool isKnown(mumble_userid_t user) {
    std::lock_guard<std::mutex> lock(g.mutex);
    for (mumble_userid_t u : g.known) {
        if (u == user) return true;
    }
    return false;
}

// ---- Overlay rendering (Win32 + GDI+) -----------------------------------

#if defined(_WIN32)

static std::wstring toWide(const std::string &s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring ws(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &ws[0], len);
    ws.resize(len - 1);
    return ws;
}

// Strip HTML tags from Mumble comments (simplified version of Python strip_html).
static std::string stripHtml(const std::string &html) {
    std::string out;
    out.reserve(html.size());
    bool inTag = false;
    for (char c : html) {
        if (c == '<') { inTag = true; continue; }
        if (c == '>') { inTag = false; out.push_back(' '); continue; }
        if (!inTag) out.push_back(c);
    }
    // Collapse whitespace
    std::string result;
    bool lastSpace = true;
    for (char c : out) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!lastSpace) { result.push_back(' '); lastSpace = true; }
        } else {
            result.push_back(c);
            lastSpace = false;
        }
    }
    while (!result.empty() && result.back() == ' ') result.pop_back();
    if (!result.empty() && result.front() == ' ') result.erase(result.begin());
    return result;
}

// Render the overlay to a layered window using GDI+.
static void paintOverlay(HWND hwnd) {
    std::lock_guard<std::mutex> lock(g.mutex);

    auto speakers = buildSnapshot();

    // Also include lingering speakers that just stopped talking
    std::vector<ActiveSpeaker> lingerSpeakers;
    DWORD now = GetTickCount();
    for (auto it = g.linger.begin(); it != g.linger.end(); ) {
        if (static_cast<int>(now - it->expireTickMs) >= 0) {
            it = g.linger.erase(it);
            continue;
        }
        // Check this user isn't already in the active list
        bool alreadyActive = false;
        for (auto &sp : speakers) {
            if (sp.user.id == it->userId) { alreadyActive = true; break; }
        }
        if (!alreadyActive) {
            auto *u = findUser(it->userId);
            if (u) {
                ActiveSpeaker sp;
                sp.user = *u;
                sp.state = "passive";
                sp.isSelf = (it->userId == static_cast<int>(g.localUser));
                sp.selfMuted = g.selfMuted;
                sp.selfDeafened = g.selfDeafened;
                lingerSpeakers.push_back(sp);
            }
        }
        ++it;
    }

    // Merge: active speakers first, then lingering ones (faded)
    for (auto &ls : lingerSpeakers)
        speakers.push_back(ls);

    int cardH = g.config.cardHeight;
    int spacing = g.config.cardSpacing;
    int totalH = speakers.empty() ? 0 :
        static_cast<int>(speakers.size()) * cardH + (static_cast<int>(speakers.size()) - 1) * spacing;

    if (!g.config.locked) {
        // Show placeholder when unlocked
        totalH = std::max(totalH, 48);
    }

    if (speakers.empty() && g.config.locked) {
        // Nothing to show: make the window tiny and invisible
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOACTIVATE);
        ShowWindow(hwnd, SW_HIDE);
        return;
    }

    int w = g.config.width;
    int h = totalH + (g.config.locked ? 0 : 48);

    // Position the window
    POINT screenPos = { g.config.x, g.config.y };

    // Create a compatible DC for layered window update
    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void *bits = nullptr;
    HBITMAP hBmp = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(memDC, hBmp);

    // Clear to fully transparent
    memset(bits, 0, w * h * 4);

    Gdiplus::Graphics graphics(memDC);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

    // Draw each speaker card
    int yOff = 0;
    for (size_t i = 0; i < speakers.size(); ++i) {
        auto &sp = speakers[i];
        auto theme = themeForState(sp.state);

        BYTE accentR = (theme.accent >> 16) & 0xFF;
        BYTE accentG = (theme.accent >> 8) & 0xFF;
        BYTE accentB = theme.accent & 0xFF;

        float op = g.config.opacity;
        bool isLingering = (sp.state == "passive");
        if (isLingering) op *= 0.5f;

        // Card background: dark glass
        {
            Gdiplus::GraphicsPath path;
            int r = 8;
            Gdiplus::Rect rc(0, yOff, w, cardH);
            path.AddArc(rc.X, rc.Y, r * 2, r * 2, 180, 90);
            path.AddArc(rc.X + rc.Width - r * 2, rc.Y, r * 2, r * 2, 270, 90);
            path.AddArc(rc.X + rc.Width - r * 2, rc.Y + rc.Height - r * 2, r * 2, r * 2, 0, 90);
            path.AddArc(rc.X, rc.Y + rc.Height - r * 2, r * 2, r * 2, 90, 90);
            path.CloseFigure();

            Gdiplus::SolidBrush bgBrush(Gdiplus::Color(static_cast<BYTE>(220 * op), 12, 18, 24));
            graphics.FillPath(&bgBrush, &path);

            // Accent bloom (subtle left glow)
            Gdiplus::SolidBrush bloomBrush(Gdiplus::Color(static_cast<BYTE>(40 * op), accentR, accentG, accentB));
            Gdiplus::GraphicsPath bloomPath;
            bloomPath.AddArc(rc.X, rc.Y, r * 2, r * 2, 180, 90);
            bloomPath.AddLine(rc.X + r, rc.Y, rc.X + 100, rc.Y);
            bloomPath.AddLine(rc.X + 100, rc.Y, rc.X + 100, rc.Y + rc.Height);
            bloomPath.AddArc(rc.X, rc.Y + rc.Height - r * 2, r * 2, r * 2, 90, 90);
            bloomPath.CloseFigure();
            graphics.FillPath(&bloomBrush, &bloomPath);

            // Accent rail (left edge)
            Gdiplus::SolidBrush railBrush(Gdiplus::Color(static_cast<BYTE>(230 * op), accentR, accentG, accentB));
            graphics.FillRectangle(&railBrush, 0, yOff + 4, 3, cardH - 8);

            // Corner brackets (top-right and bottom-right)
            Gdiplus::Pen bracketPen(Gdiplus::Color(static_cast<BYTE>(200 * op), accentR, accentG, accentB), 1.5f);
            bracketPen.SetStartCap(Gdiplus::LineCapRound);
            bracketPen.SetEndCap(Gdiplus::LineCapRound);
            int arm = 12;
            int bx = w - 6, by = yOff + 5;
            int byb = yOff + cardH - 6;
            graphics.DrawLine(&bracketPen, bx - arm, by, bx, by);
            graphics.DrawLine(&bracketPen, bx, by, bx, by + arm);
            graphics.DrawLine(&bracketPen, bx, byb - arm, bx, byb);
            graphics.DrawLine(&bracketPen, bx - arm, byb, bx, byb);
        }

        // Speaker name
        {
            std::wstring nameW = toWide(sp.user.name);
            if (sp.isSelf) nameW += L"  (you)";

            Gdiplus::FontFamily fontFamily(L"Segoe UI");
            Gdiplus::Font nameFont(&fontFamily, 13.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::SolidBrush nameBrush(Gdiplus::Color(static_cast<BYTE>(240 * op), 230, 237, 243));
            Gdiplus::PointF namePos(14.0f, static_cast<float>(yOff) + 8.0f);
            graphics.DrawString(nameW.c_str(), -1, &nameFont, namePos, &nameBrush);
        }

        // Meta line: STATE · channel · status tags
        {
            std::wstring meta(theme.label);
            if (g.config.showChannel && !sp.user.channel.empty()) {
                meta += L"  ·  ";
                meta += toWide(sp.user.channel);
            }
            if (sp.user.locallyMuted) {
                meta += L"  ·  LOCAL MUTED";
            }
            if (sp.isSelf) {
                if (sp.selfDeafened) meta += L"  ·  DEAFENED";
                else if (sp.selfMuted) meta += L"  ·  SELF MUTED";
            }

            Gdiplus::FontFamily monoFamily(L"Consolas");
            Gdiplus::Font metaFont(&monoFamily, 10.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::SolidBrush metaBrush(Gdiplus::Color(static_cast<BYTE>(220 * op), accentR, accentG, accentB));
            Gdiplus::PointF metaPos(14.0f, static_cast<float>(yOff) + 28.0f);
            graphics.DrawString(meta.c_str(), -1, &metaFont, metaPos, &metaBrush);
        }

        // Comment (if present, truncated)
        {
            std::string comment = stripHtml(sp.user.comment);
            if (!comment.empty()) {
                if (comment.size() > 90) comment = comment.substr(0, 90) + "...";
                std::wstring commentW = L"“" + toWide(comment) + L"”";
                Gdiplus::FontFamily fontFamily(L"Segoe UI");
                Gdiplus::Font commentFont(&fontFamily, 10.0f, Gdiplus::FontStyleItalic, Gdiplus::UnitPixel);
                Gdiplus::SolidBrush commentBrush(Gdiplus::Color(static_cast<BYTE>(180 * op), 107, 114, 128));
                Gdiplus::PointF commentPos(14.0f, static_cast<float>(yOff) + 44.0f);
                graphics.DrawString(commentW.c_str(), -1, &commentFont, commentPos, &commentBrush);
            }
        }

        yOff += cardH + spacing;
    }

    // Placeholder when unlocked and no speakers
    if (!g.config.locked) {
        int placeholderY = yOff;
        Gdiplus::Pen dashPen(Gdiplus::Color(120, 0, 229, 255), 1.0f);
        dashPen.SetDashStyle(Gdiplus::DashStyleDash);
        Gdiplus::Rect placeholderRect(0, placeholderY, w, 40);
        graphics.DrawRectangle(&dashPen, placeholderRect);

        Gdiplus::SolidBrush placeholderBg(Gdiplus::Color(16, 0, 229, 255));
        graphics.FillRectangle(&placeholderBg, placeholderRect);

        Gdiplus::FontFamily monoFamily(L"Consolas");
        Gdiplus::Font placeholderFont(&monoFamily, 9.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush placeholderBrush(Gdiplus::Color(200, 0, 229, 255));
        Gdiplus::StringFormat sf;
        sf.SetAlignment(Gdiplus::StringAlignmentCenter);
        sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        Gdiplus::RectF prf(0.0f, static_cast<float>(placeholderY), static_cast<float>(w), 40.0f);
        graphics.DrawString(L"VOICE OVERLAY  //  DRAG TO POSITION", -1,
                            &placeholderFont, prf, &sf, &placeholderBrush);
        h = placeholderY + 40;
    }

    // Update layered window
    SIZE size = { w, h };
    POINT ptSrc = { 0, 0 };
    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    SetWindowPos(hwnd, HWND_TOPMOST, screenPos.x, screenPos.y, w, h,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    UpdateLayeredWindow(hwnd, screenDC, &screenPos, &size, memDC, &ptSrc, 0, &blend, ULW_ALPHA);

    SelectObject(memDC, oldBmp);
    DeleteObject(hBmp);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
}

// Repaint timer callback (fires ~30fps while there are active speakers).
static void CALLBACK lingerTimerProc(HWND hwnd, UINT, UINT_PTR, DWORD) {
    paintOverlay(hwnd);
}

static void scheduleRepaint() {
#if defined(_WIN32)
    if (g.overlayWnd) {
        PostMessage(g.overlayWnd, WM_USER + 1, 0, 0);
    }
#endif
}

static LRESULT CALLBACK overlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        // Start a timer for linger expiry and repaint (~15fps is plenty)
        g.lingerTimerId = SetTimer(hwnd, 1, 66, lingerTimerProc);
        return 0;

    case WM_USER + 1:
        paintOverlay(hwnd);
        return 0;

    case WM_LBUTTONDOWN:
        if (!g.config.locked) {
            g.dragging = true;
            SetCapture(hwnd);
            POINT cursor;
            GetCursorPos(&cursor);
            RECT wr;
            GetWindowRect(hwnd, &wr);
            g.dragOffset.x = cursor.x - wr.left;
            g.dragOffset.y = cursor.y - wr.top;
        }
        return 0;

    case WM_MOUSEMOVE:
        if (g.dragging) {
            POINT cursor;
            GetCursorPos(&cursor);
            int nx = cursor.x - g.dragOffset.x;
            int ny = cursor.y - g.dragOffset.y;
            g.config.x = nx;
            g.config.y = ny;
            paintOverlay(hwnd);
        }
        return 0;

    case WM_LBUTTONUP:
        if (g.dragging) {
            g.dragging = false;
            ReleaseCapture();
        }
        return 0;

    case WM_DESTROY:
        if (g.lingerTimerId) {
            KillTimer(hwnd, g.lingerTimerId);
            g.lingerTimerId = 0;
        }
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

static void overlayThreadFunc() {
    // Initialize GDI+
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    Gdiplus::GdiplusStartup(&g.gdiplusToken, &gdiplusStartupInput, nullptr);

    const wchar_t *className = L"MumbleVoiceOverlay";
    HINSTANCE hInst = GetModuleHandle(nullptr);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = overlayWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = className;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    g.overlayWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
        className,
        L"Mumble Voice Overlay",
        WS_POPUP,
        g.config.x, g.config.y,
        g.config.width, 1,
        nullptr, nullptr, hInst, nullptr
    );

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
    UnregisterClassW(className, hInst);
}

static void startOverlay() {
    if (g.overlayRunning) return;
    g.overlayRunning = true;
    g.overlayThread = std::thread(overlayThreadFunc);
}

static void stopOverlay() {
    if (!g.overlayRunning) return;
    g.overlayRunning = false;
    if (g.overlayWnd) {
        PostMessage(g.overlayWnd, WM_CLOSE, 0, 0);
    }
    if (g.overlayThread.joinable()) {
        g.overlayThread.join();
    }
}

#else // Non-Windows stubs

static void scheduleRepaint() {}
static void startOverlay() {}
static void stopOverlay() {}

#endif // _WIN32

// ---- Update speaker state and trigger repaint ----------------------------

static void recordUser(mumble_connection_t connection, mumble_userid_t user, bool markKnown) {
    mumble_channelid_t channelId = -1;
    std::string name    = userName(connection, user);
    std::string channel = channelNameOfUser(connection, user, &channelId);
    std::string comment = userComment(connection, user);
    std::string hash    = userHash(connection, user);

    {
        std::lock_guard<std::mutex> lock(g.mutex);
        auto &u = getOrCreateUser(static_cast<int>(user));
        u.name = name.empty() ? ("User " + std::to_string(user)) : name;
        u.channel = channel;
        u.channelId = static_cast<int>(channelId);
        u.comment = comment;
        u.hash = hash;
        u.locallyMuted = userLocallyMuted(connection, user);
        u.isSelf = (user == g.localUser);

        if (markKnown) {
            g.known.push_back(user);
        }
    }
    scheduleRepaint();
}

static void recordTalk(mumble_userid_t user, mumble_talking_state_t state) {
    std::string stateName = talkingStateName(state);
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        std::string prev = getTalkState(static_cast<int>(user));
        setTalkState(static_cast<int>(user), stateName);

        // If transitioning from active to passive, start linger
        if (isActiveState(prev) && !isActiveState(stateName)) {
#if defined(_WIN32)
            // Remove existing linger for this user
            g.linger.erase(std::remove_if(g.linger.begin(), g.linger.end(),
                [user](const LingerEntry &e) { return e.userId == static_cast<int>(user); }),
                g.linger.end());
            g.linger.push_back({static_cast<int>(user), GetTickCount() + static_cast<DWORD>(g.config.lingerMs)});
#endif
        }
    }
    scheduleRepaint();
}

static void recordSelf(mumble_connection_t connection) {
    bool muted = false, deafened = false;
    if (g.api.isLocalUserMuted) g.api.isLocalUserMuted(g.id, &muted);
    if (g.api.isLocalUserDeafened) g.api.isLocalUserDeafened(g.id, &deafened);
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        g.selfMuted = muted;
        g.selfDeafened = deafened;
    }
    (void)connection;
    scheduleRepaint();
}

static void recordChannelRoster(mumble_connection_t connection) {
    if (!g.api.getChannelOfUser || !g.api.getUsersInChannel) return;
    mumble_channelid_t channel = -1;
    if (g.api.getChannelOfUser(g.id, connection, g.localUser, &channel) != MUMBLE_EC_OK) return;
    mumble_userid_t *users = nullptr;
    size_t count = 0;
    if (g.api.getUsersInChannel(g.id, connection, channel, &users, &count) != MUMBLE_EC_OK) return;
    for (size_t i = 0; i < count; ++i) {
        recordUser(connection, users[i], true);
    }
    if (users) g.api.freeMemory(g.id, users);
}

}  // namespace

// =========================================================================
//  Mandatory plugin functions
// =========================================================================

mumble_error_t mumble_init(mumble_plugin_id_t id) {
    g.id = id;
    startOverlay();
    return MUMBLE_STATUS_OK;
}

void mumble_shutdown() {
    stopOverlay();
}

struct MumbleStringWrapper mumble_getName() {
    MumbleStringWrapper wrapper;
    wrapper.data           = PLUGIN_NAME;
    wrapper.size           = std::strlen(PLUGIN_NAME);
    wrapper.needsReleasing = false;
    return wrapper;
}

mumble_version_t mumble_getAPIVersion() {
    mumble_version_t version;
    version.major = 1;
    version.minor = 0;
    version.patch = 0;
    return version;
}

void mumble_registerAPIFunctions(void *apiStruct) {
    if (apiStruct) {
        g.api      = *reinterpret_cast<mumble_api_t *>(apiStruct);
        g.apiValid = true;
    }
}

void mumble_releaseResource(const void *) {}

// =========================================================================
//  General information functions
// =========================================================================

void mumble_setMumbleInfo(mumble_version_t, mumble_version_t, mumble_version_t) {}

mumble_version_t mumble_getVersion() {
    mumble_version_t version;
    version.major = 1;
    version.minor = 0;
    version.patch = 0;
    return version;
}

struct MumbleStringWrapper mumble_getAuthor() {
    MumbleStringWrapper wrapper;
    wrapper.data           = PLUGIN_AUTHOR;
    wrapper.size           = std::strlen(PLUGIN_AUTHOR);
    wrapper.needsReleasing = false;
    return wrapper;
}

struct MumbleStringWrapper mumble_getDescription() {
    MumbleStringWrapper wrapper;
    wrapper.data           = PLUGIN_DESC;
    wrapper.size           = std::strlen(PLUGIN_DESC);
    wrapper.needsReleasing = false;
    return wrapper;
}

uint32_t mumble_getFeatures() {
    return MUMBLE_FEATURE_NONE;
}

// =========================================================================
//  Event callbacks
// =========================================================================

void mumble_onServerConnected(mumble_connection_t connection) {
    std::lock_guard<std::mutex> lock(g.mutex);
    g.connection   = connection;
    g.synchronized = false;
    g.known.clear();
    g.users.clear();
    g.talkStates.clear();
#if defined(_WIN32)
    g.linger.clear();
#endif
}

void mumble_onServerDisconnected(mumble_connection_t) {
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        g.connection   = -1;
        g.synchronized = false;
        g.known.clear();
        g.users.clear();
        g.talkStates.clear();
#if defined(_WIN32)
        g.linger.clear();
#endif
    }
    scheduleRepaint();
}

void mumble_onServerSynchronized(mumble_connection_t connection) {
    if (!apiReady()) return;
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        g.connection   = connection;
        g.synchronized = true;
        g.known.clear();
    }
    if (g.api.getLocalUserID) {
        mumble_userid_t local = 0;
        if (g.api.getLocalUserID(g.id, connection, &local) == MUMBLE_EC_OK) {
            std::lock_guard<std::mutex> lock(g.mutex);
            g.localUser = local;
        }
    }
    recordSelf(connection);
    recordChannelRoster(connection);
}

void mumble_onChannelEntered(mumble_connection_t connection, mumble_userid_t userID,
                              mumble_channelid_t, mumble_channelid_t) {
    if (!apiReady()) return;
    if (userID == g.localUser) {
        recordChannelRoster(connection);
    } else {
        recordUser(connection, userID, true);
    }
}

void mumble_onChannelExited(mumble_connection_t, mumble_userid_t userID, mumble_channelid_t) {
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        removeUser(static_cast<int>(userID));
        for (auto it = g.known.begin(); it != g.known.end(); ++it) {
            if (*it == userID) { g.known.erase(it); break; }
        }
#if defined(_WIN32)
        g.linger.erase(std::remove_if(g.linger.begin(), g.linger.end(),
            [userID](const LingerEntry &e) { return e.userId == static_cast<int>(userID); }),
            g.linger.end());
#endif
    }
    scheduleRepaint();
}

void mumble_onUserTalkingStateChanged(mumble_connection_t connection, mumble_userid_t userID,
                                      mumble_talking_state_t talkingState) {
    if (!apiReady()) return;
    if (!isKnown(userID)) {
        recordUser(connection, userID, true);
    }
    recordTalk(userID, talkingState);
}
