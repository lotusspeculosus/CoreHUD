// CoreHUD - local hardware monitor + customizable sensor panel, with per-pin 12V-2x6
// connector protection on cards that have a per-pin sensor (ASUS ROG Astral).
// Win32 + Direct2D/DirectWrite, no third-party dependencies.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "monitor.h"
#include "panel.h"
#include "sensors.h"
#include "components.h"
#include "updater.h"
#include "version.h"
#include "tablet.h"
#include "connector.h"
#include "third_party/qrcodegen/qrcodegen.hpp"
#include <objbase.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <codecapi.h>
#include <atomic>
#include <thread>

namespace {

constexpr UINT WM_APP_SAMPLE = WM_APP + 1;

// ---------- theme ----------
constexpr uint32_t kBg = 0x0B0E12, kSurface = 0x15181D, kSurface2 = 0x1D2128, kSurface3 = 0x252A33;
constexpr uint32_t kBorder = 0x2A303A, kInk = 0xEEF1F4, kInk2 = 0xAEB5BF, kMuted = 0x7D8590;
constexpr uint32_t kAccent = 0x2FD9C4, kGrid = 0x232830, kAxis = 0x39404B;
constexpr uint32_t kGood = 0x0CA30C, kWarn = 0xFAB219, kSerious = 0xEC835A, kCrit = 0xD03B3B;
// Categorical series (validated for this dark surface: adjacent CVD dE >= 8.4, normal >= 19.3)
constexpr uint32_t kSeries[6] = {0x3987E5, 0xD95926, 0x199E70, 0xC98500, 0xD55181, 0x0CA30C};

D2D1_COLOR_F Col(uint32_t rgb, float a = 1.f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.f, ((rgb >> 8) & 0xFF) / 255.f, (rgb & 0xFF) / 255.f, a);
}

// ---------- globals ----------
HWND g_hwnd = nullptr;
ID2D1Factory* g_d2d = nullptr;
IDWriteFactory* g_dw = nullptr;
ID2D1RenderTarget* g_rt = nullptr;        // what all drawing code uses (main window, panel window or off-screen)
ID2D1HwndRenderTarget* g_hrt = nullptr;   // the main window's own target
ID2D1SolidColorBrush* g_br = nullptr;
ID2D1StrokeStyle* g_dash = nullptr;
std::map<int, IDWriteTextFormat*> g_fmts;

enum { TAB_DASH, TAB_CHART, TAB_PANEL, TAB_EVENTS, TAB_COUNT };
int g_tab = TAB_DASH;
int g_mode = 0;              // 0 A, 1 W, 2 V
int g_range = 1;             // 0: 1 min, 1: 5 min, 2: 30 min
int g_eventScroll = 0;
float g_mx = -1, g_my = -1;  // mouse in DIPs
bool g_tracking = false;

enum HitId {
    HIT_TAB = 100, HIT_MODE = 200, HIT_RANGE = 300,
    HIT_RESTART = 400, HIT_PEAKS, HIT_CSV, HIT_INI, HIT_LOGS, HIT_COMPONENTS, HIT_TOUR
};
struct Hit { D2D1_RECT_F r; int id; std::function<void()> fn; };
std::vector<Hit> g_hits;
// Named screen areas, recorded while painting, so the guided tour can point at them.
std::map<std::string, D2D1_RECT_F> g_anchors;
void Anchor(const char* name, const D2D1_RECT_F& r) { g_anchors[name] = r; }

std::wstring g_ini;
mon::Snapshot g_snap;
sensors::Registry g_reg;   // every sensor (built-in, any-vendor GPU, HWiNFO, LHM, RTSS)
std::vector<mon::HistPoint> g_hist;
std::vector<mon::Event> g_events;
// The card's power connector(s): detected from power-rail telemetry, or the user's choice in Settings.
conn::Layout g_connChoice = conn::UNKNOWN;   // [Gpu] Connector (UNKNOWN = auto)
conn::Result g_connDetected, g_conn;           // g_conn = what the UI uses

void LoadConnectorChoice() {
    wchar_t b[32] = {};
    GetPrivateProfileStringW(L"Gpu", L"Connector", L"auto", b, 32, g_ini.c_str());
    g_connChoice = conn::FromId(b);
}

// After every sample: what do the sensors say about the connector? A detection sticks for the session
// (HWiNFO / LibreHardwareMonitor may be closed later).
void UpdateConnector() {
    std::vector<conn::Input> in;
    in.reserve(g_reg.All().size());
    for (auto& s : g_reg.All()) in.push_back({s.group, s.name, s.unit, s.value, s.ok});
    // Testing aid: COREHUD_SIMULATE_RAILS=<1..3> adds fake "GPU 8-pin #n" rails (like HWiNFO reports them),
    // to check the 8-pin view on a PC without such a card. Never set in normal use.
    static const int simRails = [] { const char* e = getenv("COREHUD_SIMULATE_RAILS"); return e ? std::clamp(atoi(e), 0, 3) : 0; }();
    for (int i = 1; i <= simRails; ++i) {
        std::wstring g = L"HWiNFO: GPU [#0]: simulated card", n = L"GPU 8-pin #" + std::to_wstring(i) + L" Input ";
        in.push_back({g, n + L"Voltage", L"V", 12.1 - 0.03 * i, true});
        in.push_back({g, n + L"Power", L"W", 60.0 + 55.0 * i + (GetTickCount64() / 500 % 7), true});
    }
    double amps = 0;
    for (int i = 0; i < g_snap.pinCount && i < pins::kMaxPins; ++i) amps += g_snap.amps[i];
    conn::Result d = conn::Detect(in, g_snap.sensorEnabled && g_snap.pinsOk && amps > 0.5);
    if (d.layout != conn::UNKNOWN || !g_connDetected.fromTelemetry) g_connDetected = d;
    else g_connDetected.rails.clear();   // keep the layout, drop stale rail values
    g_conn = conn::Effective(g_connDetected, g_connChoice);
}

// ---------- drawing helpers ----------
template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

IDWriteTextFormat* Fmt(float size, int weight = 0, int align = 0, bool icons = false) {
    // quantize so continuously-fitted panel text doesn't create unbounded formats
    size = size < 24 ? std::round(size * 2) / 2 : std::round(size);
    size = std::clamp(size, 4.f, 600.f);
    int key = int(size * 2) * 100 + weight * 10 + align + (icons ? 10000000 : 0);
    auto it = g_fmts.find(key);
    if (it != g_fmts.end()) return it->second;
    DWRITE_FONT_WEIGHT w = weight == 2 ? DWRITE_FONT_WEIGHT_BOLD : weight == 1 ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL;
    IDWriteTextFormat* f = nullptr;
    g_dw->CreateTextFormat(icons ? L"Segoe Fluent Icons" : L"Segoe UI", nullptr, w, DWRITE_FONT_STYLE_NORMAL,
                           DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f);
    f->SetTextAlignment(align == 1 ? DWRITE_TEXT_ALIGNMENT_CENTER : align == 2 ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    g_fmts[key] = f;
    return f;
}

D2D1_RECT_F R(float x, float y, float w, float h) { return D2D1::RectF(x, y, x + w, y + h); }
bool In(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
bool Hover(const D2D1_RECT_F& r) { return In(r, g_mx, g_my); }

void Fill(const D2D1_RECT_F& r, uint32_t c, float rad = 0, float a = 1.f) {
    g_br->SetColor(Col(c, a));
    if (rad > 0) g_rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), g_br);
    else g_rt->FillRectangle(r, g_br);
}
void Stroke(const D2D1_RECT_F& r, uint32_t c, float rad = 0, float w = 1.f, float a = 1.f) {
    g_br->SetColor(Col(c, a));
    D2D1_RECT_F q = D2D1::RectF(r.left + w / 2, r.top + w / 2, r.right - w / 2, r.bottom - w / 2);
    if (rad > 0) g_rt->DrawRoundedRectangle(D2D1::RoundedRect(q, rad, rad), g_br, w);
    else g_rt->DrawRectangle(q, g_br, w);
}
void Line(float x0, float y0, float x1, float y1, uint32_t c, float w = 1.f, bool dashed = false, float a = 1.f) {
    g_br->SetColor(Col(c, a));
    g_rt->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), g_br, w, dashed ? g_dash : nullptr);
}
void Text(const std::wstring& s, const D2D1_RECT_F& r, IDWriteTextFormat* f, uint32_t c, float a = 1.f) {
    g_br->SetColor(Col(c, a));
    g_rt->DrawTextW(s.c_str(), UINT32(s.size()), f, r, g_br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}
float Measure(const std::wstring& s, IDWriteTextFormat* f) {
    IDWriteTextLayout* l = nullptr;
    if (FAILED(g_dw->CreateTextLayout(s.c_str(), UINT32(s.size()), f, 4000, 200, &l))) return 0;
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    l->Release();
    return m.widthIncludingTrailingWhitespace;
}
void Circle(float x, float y, float r, uint32_t c, float a = 1.f) {
    g_br->SetColor(Col(c, a));
    g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), r, r), g_br);
}

std::wstring F(const wchar_t* fmt, ...) {
    wchar_t b[256];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(b, 256, fmt, ap);
    va_end(ap);
    return b;
}
std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// value + unit (unit in lighter ink), left-aligned at x
float ValueUnit(const std::wstring& v, const std::wstring& unit, float x, float y, float h, float size, uint32_t c = kInk) {
    auto fv = Fmt(size, 1), fu = Fmt(size * 0.8f, 0);
    float wv = Measure(v, fv);
    Text(v, R(x, y, wv + 4, h), fv, c);
    Text(unit, R(x + wv + size * 0.25f, y + size * 0.06f, 200, h), fu, kInk2);
    return wv + size * 0.25f + Measure(unit, fu);
}

void Button(const D2D1_RECT_F& r, const std::wstring& label, int id, bool active = false, float fontSize = 13.f) {
    bool hv = Hover(r);
    Fill(r, active ? kSurface3 : hv ? kSurface2 : kSurface, 6);
    Stroke(r, active ? kAccent : kBorder, 6, 1.f, active ? 0.9f : 1.f);
    Text(label, r, Fmt(fontSize, 1, 1), active ? kAccent : hv ? kInk : kInk2);
    g_hits.push_back({r, id});
}

void Panel(const D2D1_RECT_F& r, bool accentBorder = false) {
    Fill(r, kSurface, 10);
    Stroke(r, accentBorder ? kAccent : kBorder, 10, 1.f, accentBorder ? 0.55f : 1.f);
}

uint32_t StateColor(int st) { return st == 2 ? kCrit : st == 1 ? kWarn : kAccent; }

// ---------- top bar ----------
void DrawTopBar(float W) {
    Fill(R(0, 0, W, 48), kSurface);
    Line(0, 47.5f, W, 47.5f, kBorder);
    const wchar_t* tabs[] = {L"Dashboard", L"Performance chart", L"Sensor panel", L"Event log"};
    float x = 12;
    for (int i = 0; i < TAB_COUNT; ++i) {
        auto f = Fmt(14, 1, 1);
        float w = Measure(tabs[i], f) + 32;
        D2D1_RECT_F r = R(x, 6, w, 36);
        bool act = g_tab == i;
        if (act) Fill(r, kSurface2, 6);
        else if (Hover(r)) Fill(r, kSurface2, 6, 0.6f);
        Text(tabs[i], r, f, act ? kAccent : kInk2);
        if (act) Fill(R(x + 12, 44, w - 24, 2), kAccent, 1);
        g_hits.push_back({r, HIT_TAB + i});
        Anchor(i == 0 ? "tab0" : i == 1 ? "tab1" : i == 2 ? "tab2" : "tab3", r);
        x += w + 4;
    }

    // status pill
    const auto& s = g_snap;
    std::wstring st;
    uint32_t sc = kMuted;
    int worst = 0;
    for (int i = 0; i < 6; ++i) worst = std::max(worst, s.pinState[i]);
    if (!s.started || (s.samples == 0 && s.nvapiOk)) { st = L"Starting"; }
    else if (!s.sensorEnabled) { st = L"Ready · no pin sensor"; sc = kGood; }   // AMD/Intel or a card without one
    else if (!s.pinsOk) { st = L"Sensor error"; sc = kCrit; }
    else if (worst == 2) { st = L"Over limit"; sc = kCrit; }
    else if (worst == 1) { st = L"Warning"; sc = kWarn; }
    else { st = L"Ready"; sc = kGood; }
    auto pf = Fmt(12.5f, 1, 0);
    float pw = Measure(st, pf) + 34;
    float rx = W - 12;
    D2D1_RECT_F pill = R(rx - pw, 10, pw, 28);
    Fill(pill, sc, 14, 0.12f);
    Stroke(pill, sc, 14, 1.f, 0.7f);
    Circle(pill.left + 13, 24, 3.5f, sc);
    Text(st, R(pill.left + 22, 10, pw - 22, 28), pf, kInk);
    rx -= pw + 10;

    struct B { const wchar_t* label; int id; bool active; };
    std::wstring csvLabel = s.csvOn ? L"CSV log: on" : L"CSV log: off";
    B btns[] = {{L"Tour", HIT_TOUR, false}, {L"Components", HIT_COMPONENTS, false}, {L"Settings", HIT_INI, false}, {csvLabel.c_str(), HIT_CSV, s.csvOn},
                {L"Reset peaks", HIT_PEAKS, false}, {L"Restart sensor", HIT_RESTART, false}};
    for (auto& b : btns) {
        float w = Measure(b.label, Fmt(13, 1, 1)) + 28;
        rx -= w;
        if (rx < x + 20) break;
        Button(R(rx, 10, w, 28), b.label, b.id, b.active);
        if (b.id == HIT_INI) Anchor("settings", R(rx, 10, w, 28));
        if (b.id == HIT_TOUR) Anchor("tour", R(rx, 10, w, 28));
        rx -= 8;
    }
}

// ---------- dashboard ----------
void DrawKpi(const D2D1_RECT_F& r, const std::wstring& label, const std::wstring& value, const std::wstring& unit,
             const std::wstring& sub, const wchar_t* glyph) {
    Panel(r, true);
    Fill(R(r.left + 14, r.top + 14, 24, 24), kAccent, 6, 0.9f);
    Text(glyph, R(r.left + 14, r.top + 14, 24, 24), Fmt(13, 0, 1, true), kBg);
    Text(label, R(r.left + 46, r.top + 14, r.right - r.left - 60, 24), Fmt(12, 1), kInk2);
    ValueUnit(value, unit, r.left + 16, r.top + 42, 38, 28);
    if (!sub.empty()) Text(sub, R(r.left + 16, r.bottom - 24, r.right - r.left - 32, 16), Fmt(12), kMuted);
}

void DrawStatBox(const D2D1_RECT_F& r, const std::wstring& label, const std::wstring& value, const std::wstring& sub, bool highlight) {
    Fill(r, kSurface2, 8);
    if (highlight) Stroke(r, kAccent, 8, 1.f, 0.6f);
    Text(label, R(r.left + 12, r.top + 8, r.right - r.left - 20, 16), Fmt(11, 1), highlight ? kAccent : kMuted);
    Text(value, R(r.left + 12, r.top + 26, r.right - r.left - 20, 24), Fmt(17, 1), kInk);
    if (!sub.empty()) Text(sub, R(r.left + 12, r.top + 50, r.right - r.left - 20, 14), Fmt(11), kMuted);
}

// A small drawing of the card's power connector(s): 12V-2x6 = 2x6 power pins + 4 small sense pins on top,
// 8-pin PCIe = 2x4 pins (one drawing per connector). Returns the width used.
float DrawConnectorGlyph(conn::Layout l, float x, float y, float h, uint32_t c, float alpha = 1.f, bool draw = true) {
    int n = conn::PinConnectors(l);
    if (n == 0) return 0;
    bool hp = l == conn::HPWR16;
    int cols = hp ? 6 : 4;
    float cell = h / (hp ? 2.9f : 2.3f);
    float bodyW = cols * cell + cell * 0.5f, gap = cell * 0.9f;
    float bx = x;
    for (int k = 0; k < n; ++k) {
        if (draw) {
            float top = y + (hp ? cell * 0.6f : 0.f);
            Stroke(D2D1::RectF(bx, top, bx + bodyW, y + h), c, cell * 0.25f, std::max(1.f, cell * 0.12f), alpha);
            for (int r = 0; r < 2; ++r)
                for (int i = 0; i < cols; ++i)
                    Fill(R(bx + cell * 0.25f + i * cell + cell * 0.15f, top + cell * 0.2f + r * cell + cell * 0.15f, cell * 0.7f, cell * 0.6f), c,
                         cell * 0.12f, alpha * 0.85f);
            if (hp)
                for (int i = 0; i < 4; ++i) Fill(R(bx + bodyW / 2 - cell * 1.15f + i * cell * 0.6f, y, cell * 0.4f, cell * 0.4f), c, 1.f, alpha * 0.7f);
        }
        bx += bodyW + gap;
    }
    return bx - gap - x;
}

// Cards with 8-pin PCIe connectors (or slot power only): no per-pin monitoring needed; show the connectors
// and, when the card reports them, the power per 8-pin input.
void DrawPcieConnectors(const D2D1_RECT_F& m) {
    Fill(m, kSurface2, 8);
    float cx = (m.left + m.right) / 2, w = m.right - m.left;
    bool railPower = false;
    for (auto& r : g_conn.rails) railPower |= std::isfinite(r.Power());
    float contentH = g_conn.layout == conn::NONE ? 100.f : 60 + 34 + (railPower ? 90.f : 30.f) + 40;
    float y = m.top + std::max(24.f, (m.bottom - m.top - 40 - contentH) / 2);   // centred above the footer line
    if (g_conn.layout == conn::NONE) {
        Text(L"No power cable to watch", R(m.left, y + 40, w, 26), Fmt(16, 1, 1), kInk);
        Text(L"This card gets all its power from the PCIe slot on the motherboard (up to 75 W).", R(m.left + 10, y + 70, w - 20, 22), Fmt(12.5f, 0, 1), kInk2);
    } else {
        float gw = DrawConnectorGlyph(g_conn.layout, 0, 0, 44, kAccent, 1.f, false);
        DrawConnectorGlyph(g_conn.layout, cx - gw / 2, y, 44, kAccent);
        y += 60;
        Text(conn::Name(g_conn.layout), R(m.left, y, w, 26), Fmt(16, 1, 1), kInk);
        y += 34;
        bool anyPower = false;
        for (auto& r : g_conn.rails) anyPower |= std::isfinite(r.Power());
        if (anyPower) {
            int n = int(g_conn.rails.size());
            float colW = std::min(220.f, (w - 40) / n), x = cx - colW * n / 2;
            for (const auto& r : g_conn.rails) {
                double p = r.Power();
                float bx = x + 14, bwid = colW - 28;
                Text(F(L"8-pin #%d", r.index), R(x, y, colW, 18), Fmt(12, 1, 1), kInk2);
                Text(std::isfinite(p) ? F(L"%.0f W", p) : std::wstring(L"—"), R(x, y + 18, colW, 26), Fmt(18, 1, 1), p > 150 ? kWarn : kInk);
                Fill(R(bx, y + 50, bwid, 8), kSurface3, 4);
                if (std::isfinite(p) && p > 0) Fill(R(bx, y + 50, bwid * float(std::min(p, 300.0) / 300), 8), p > 150 ? kWarn : kAccent, 4);
                Line(bx + bwid / 2, y + 46, bx + bwid / 2, y + 62, kMuted, 1.f);   // the 150 W rating, at the middle
                Text(std::isfinite(r.volts) ? F(L"150 W rated · %.2f V", r.volts) : std::wstring(L"150 W rated"), R(x, y + 62, colW, 16),
                     Fmt(11, 0, 1), kMuted);
                x += colW;
            }
            y += 90;
        } else {
            Text(L"Your card reports only its total power, not the power per connector.", R(m.left + 10, y, w - 20, 20), Fmt(12, 0, 1), kInk2);
            y += 30;
        }
        Text(L"8-pin connectors have a big safety margin: each is rated 150 W and its three 12 V pins can carry far more.",
             R(m.left + 10, y, w - 20, 20), Fmt(12, 0, 1), kInk2);
        Text(L"Per-pin monitoring matters for the 12V-2x6 (600 W over six pins), not for these.", R(m.left + 10, y + 20, w - 20, 20), Fmt(12, 0, 1), kInk2);
    }
    std::wstring how = g_conn.fromTelemetry ? L"Detected from the " + g_conn.how + L"  ·  wrong? Settings › Power connector"
                                            : L"Set in Settings › Power connector";
    Text(how, R(m.left + 10, m.bottom - 30, w - 20, 20), Fmt(11.5f, 0, 1), kMuted);
}

void DrawPinPanel(const D2D1_RECT_F& P) {
    const auto& s = g_snap;
    const auto& c = s.cfg;
    Panel(P);
    Anchor("pins", P);
    float x0 = P.left + 18, x1 = P.right - 18;
    float y = P.top + 14;
    bool pcie = !s.sensorEnabled && (g_conn.layout == conn::PCIE8_1 || g_conn.layout == conn::PCIE8_2 || g_conn.layout == conn::PCIE8_3 ||
                                     g_conn.layout == conn::NONE);
    Fill(R(x0, y + 4, 26, 26), kAccent, 6, 0.9f);
    Text(L"", R(x0, y + 4, 26, 26), Fmt(13, 0, 1, true), kBg);
    Text(pcie ? L"Power connectors" : L"Pin distribution", R(x0 + 36, y, 170, 34), Fmt(16, 1), kInk);
    if (s.sensorEnabled && !s.pinSource.empty()) {
        Text(L"\u00b7  " + s.pinSource, R(x0 + 190, y, x1 - x0 - 190 - 130, 34), Fmt(12), kMuted);
    } else if (!pcie) {   // which connector the card has (or that it isn't known)
        float gx = x0 + 196 + DrawConnectorGlyph(g_conn.layout, x0 + 196, y + 9, 16, kInk2, 0.9f);
        Text(g_conn.layout == conn::UNKNOWN ? std::wstring(L"Connector not detected") : std::wstring(conn::Name(g_conn.layout)),
             R(gx + 8, y, x1 - gx - 140, 34), Fmt(12), kMuted);
    }
    if (pcie) {
        DrawPcieConnectors(D2D1::RectF(x0, y + 46, x1, P.bottom - 14));
        return;
    }
    const wchar_t* modes[] = {L"A", L"W", L"V"};
    for (int i = 0; i < 3; ++i) Button(R(x1 - (3 - i) * 40 + 4, y + 2, 36, 30), modes[i], HIT_MODE + i, g_mode == i, 14);
    y += 46;

    // values in the current mode
    double val[6];
    for (int i = 0; i < 6; ++i)
        val[i] = g_mode == 0 ? s.amps[i] : g_mode == 1 ? s.amps[i] * s.volts[i] : s.volts[i];
    const wchar_t* unit = g_mode == 0 ? L"A" : g_mode == 1 ? L"W" : L"V";
    const wchar_t* vf = g_mode == 1 ? L"%.1f" : L"%.2f";
    int hi = 0, lo = 0;
    for (int i = 1; i < 6; ++i) { if (val[i] > val[hi]) hi = i; if (val[i] < val[lo]) lo = i; }
    int peakPin = 0;
    for (int i = 1; i < 6; ++i) if (s.peakA[i] > s.peakA[peakPin]) peakPin = i;

    float bw = (x1 - x0 - 3 * 10) / 4;
    bool ok = s.pinsOk;
    std::wstring dash = L"—";
    DrawStatBox(R(x0, y, bw, 68), g_mode == 2 ? L"VOLTAGE SPREAD (Δ)" : g_mode == 1 ? L"POWER SPREAD (Δ)" : L"CURRENT SPREAD (Δ)",
                ok ? F(g_mode == 2 ? L"%.0f mV" : (std::wstring(vf) + L" %s").c_str(), g_mode == 2 ? (val[hi] - val[lo]) * 1000 : val[hi] - val[lo], unit) : dash,
                L"Highest − lowest pin", true);
    DrawStatBox(R(x0 + (bw + 10), y, bw, 68), L"HIGHEST PIN", ok ? F((L"P%d · " + std::wstring(vf) + L" %s").c_str(), hi + 1, val[hi], unit) : dash, L"", false);
    DrawStatBox(R(x0 + 2 * (bw + 10), y, bw, 68), L"LOWEST PIN", ok ? F((L"P%d · " + std::wstring(vf) + L" %s").c_str(), lo + 1, val[lo], unit) : dash, L"", false);
    DrawStatBox(R(x0 + 3 * (bw + 10), y, bw, 68), L"SESSION PEAK", s.sensorEnabled && s.samples ? F(L"P%d · %.2f A", peakPin + 1, s.peakA[peakPin]) : dash,
                s.sensorEnabled ? F(L"Connector peak %.0f W", s.peakConnW) : std::wstring(), false);
    y += 80;

    const wchar_t* cap = g_mode == 0 ? L"CURRENT HEADROOM TO LIMIT" : g_mode == 1 ? L"POWER HEADROOM TO LIMIT" : L"DEVIATION FROM AVERAGE VOLTAGE";
    Text(cap, R(x0, y, x1 - x0, 16), Fmt(11, 1, 1), kMuted);
    y += 22;

    // footer line
    float footerH = 22;
    float chartBottom = P.bottom - 14 - footerH;
    if (s.wearOn || s.blackBoxRecording) {   // connector wear check + black box status
        chartBottom -= 20;
        float ly = chartBottom + 2, recW = 0;
        if (s.blackBoxRecording) {
            std::wstring rec = L"Black box recording";
            recW = Measure(rec, Fmt(11.5f, 1)) + 18;
            Circle(x1 - recW + 5, ly + 9, 4, kCrit);
            Text(rec, R(x1 - recW + 14, ly, recW, 18), Fmt(11.5f, 1), kCrit);
        }
        if (s.wearOn) {
            uint32_t wc = s.wear.state == wear::WARN ? kWarn : s.wear.state == wear::WATCH ? kSerious : s.wear.state == wear::OK ? kGood : kMuted;
            Circle(x0 + 5, ly + 9, 3.5f, wc);
            Text(s.wear.text, R(x0 + 14, ly, x1 - x0 - 14 - recW - 10, 18), Fmt(11.5f), s.wear.state >= wear::WATCH ? kInk : kInk2);
        }
    }
    std::wstring footer = s.pinSourceId == "astral"
        ? F(L"IT8915FN @ 0x%02X \u00b7 I2C port %d \u00b7 read %.0f ms \u00b7 %llu samples \u00b7 %llu errors \u00b7 limit %.1f A, warn %.1f A",
            c.i2cAddr, c.i2cPort, s.readMs, (unsigned long long)s.samples, (unsigned long long)s.failures, c.limitA, c.warnA)
        : F(L"%s \u00b7 read %.0f ms \u00b7 %llu errors \u00b7 limit %.1f A, warn %.1f A%s", s.pinSource.c_str(), s.readMs,
            (unsigned long long)s.failures, c.limitA, c.warnA, s.perPinVolts ? L"" : L" \u00b7 voltage = PSU 12V rail");
    if (s.pinsOk && s.pinCount > 6) {
        double c2 = 0;
        for (int i = 6; i < 12; ++i) c2 = std::max(c2, s.amps[i]);
        footer += F(L" \u00b7 connector 2 highest pin %.2f A", c2);
    }
    if (s.sensorEnabled) Text(footer, R(x0, P.bottom - 14 - footerH, x1 - x0, footerH), Fmt(11, 0, 1), kMuted);

    if (!s.sensorEnabled || (!ok && s.samples > 0)) {
        D2D1_RECT_F m = R(x0, y + 10, x1 - x0, chartBottom - y - 20);
        Fill(m, kSurface2, 8);
        std::wstring t1, t2;
        bool info = !s.sensorEnabled;   // not an error: this GPU simply has no per-pin sensor
        if (!s.sensorEnabled) {
            if (g_conn.layout == conn::HPWR16)
                t1 = L"Your card has a 12V-2x6 connector, but no per-pin sensor or meter was found";
            else
                t1 = s.haveGpu ? F(L"No per-pin sensor on this card (subsystem %04X:%04X) and no external meter found", s.subSystemId & 0xFFFF, s.subSystemId >> 16)
                               : std::wstring(L"No per-pin sensor or external meter found");
            t2 = L"Per-pin monitoring works with any GPU through a Thermal Grizzly WireView Pro II, ElmorLabs PMD2X or MSI MPG Ai1300TS/Ai1600TS PSU (USB connected).";
            if (g_conn.layout == conn::UNKNOWN) {   // most cards use 8-pin: let the user say which one
                float my = m.top + (m.bottom - m.top) / 2 + 62;
                Text(L"Most cards use 8-pin connectors and don't need this. Which one does yours have?", R(m.left + 10, my, m.right - m.left - 20, 20),
                     Fmt(12, 1, 1), kInk2);
                Button(R((m.left + m.right) / 2 - 90, my + 26, 180, 30), L"Set the connector…", HIT_INI, true);
            }
        } else if (s.pinSourceId == "astral") {
            t1 = L"Per-pin sensor read failed";
            t2 = Wide(s.pinError) + L"  \u2014  close other tools polling the GPU I2C bus (Armoury Crate, GPU Tweak) or check I2CPort/I2CAddr.";
        } else {
            t1 = L"Per-pin meter not responding";
            t2 = Wide(s.pinError) + L"  \u2014  check the USB cable, and close the vendor app (WireView / MSI Center) if it holds the device.";
        }
        if (!s.sensorEnabled)
            Text(L"Cards with an on-card sensor: ASUS ROG Astral (if yours isn't recognised, try Force=1 in Settings).",
                 R(m.left + 10, m.top + (m.bottom - m.top) / 2 + 34, m.right - m.left - 20, 20), Fmt(11.5f, 0, 1), kMuted);
        Text(L"", R(m.left, m.top + (m.bottom - m.top) / 2 - 50, m.right - m.left, 30), Fmt(22, 0, 1, true), info ? kMuted : kWarn);
        Text(t1, R(m.left, m.top + (m.bottom - m.top) / 2 - 14, m.right - m.left, 24), Fmt(15, 1, 1), kInk);
        Text(t2, R(m.left + 10, m.top + (m.bottom - m.top) / 2 + 12, m.right - m.left - 20, 22), Fmt(12, 0, 1), kInk2);
        return;
    }

    // axis range
    double vmin = 0, vmax, step;
    if (g_mode == 0) { vmax = std::max(10.0, std::ceil(std::max(val[hi], c.limitA) + 0.8)); step = vmax > 14 ? 4 : 2; }
    else if (g_mode == 1) {
        double m = std::max(val[hi], c.limitA * 12.0);
        vmax = std::ceil((m + 10) / 20) * 20; step = vmax > 160 ? 40 : 20;
    } else { vmin = 11.0; vmax = 12.75; step = 0.25; if (val[lo] < vmin) vmin = std::floor(val[lo] * 2) / 2; }

    float chipH = 24, valH = 20, labH = 26;
    float plotTop = y + chipH + 6 + valH + 4;
    float plotBot = chartBottom - labH;
    float axisW = 44;
    float px0 = x0 + axisW, px1 = x1 - 8;
    auto Y = [&](double v) { return float(plotBot - (std::clamp(v, vmin, vmax) - vmin) / (vmax - vmin) * (plotBot - plotTop)); };

    // grid + ticks
    for (double t = vmin; t <= vmax + 1e-9; t += step) {
        float yy = Y(t);
        Line(px0 - 6, yy, px0, yy, kAxis);
        Text(g_mode == 2 ? F(L"%.2fV", t) : F(L"%g%s", t, unit), R(x0 - 6, yy - 8, axisW - 8, 16), Fmt(10.5f, 0, 2), kMuted);
    }
    Line(px0, plotTop, px0, plotBot, kAxis);

    float slot = (px1 - px0) / 6;
    float barW = std::min(56.f, slot * 0.42f);
    double avgV = 0;
    for (int i = 0; i < 6; ++i) avgV += s.volts[i] / 6;

    for (int i = 0; i < 6; ++i) {
        float cx = px0 + slot * (i + 0.5f);
        D2D1_RECT_F track = D2D1::RectF(cx - barW / 2, plotTop, cx + barW / 2, plotBot);
        Fill(track, kSurface3, 4, 0.55f);
        float top = Y(val[i]);
        uint32_t bc = g_mode == 2 ? kAccent : StateColor(s.pinState[i]);
        if (plotBot - top > 1) {
            Fill(D2D1::RectF(track.left, top, track.right, plotBot), bc, 4);
            Fill(D2D1::RectF(track.left, std::max(top, plotBot - 4), track.right, plotBot), bc);   // square base
        }
        // value + chip
        Text(F((std::wstring(vf) + L" %s").c_str(), val[i], unit), R(cx - slot / 2, plotTop - valH - 4, slot, valH), Fmt(13, 1, 1), kInk);
        std::wstring chip;
        uint32_t cc = kGood;
        if (g_mode == 0) { double h = c.limitA - s.amps[i]; chip = F(L"%+.2f A", h); }
        else if (g_mode == 1) { double h = (c.limitA - s.amps[i]) * s.volts[i]; chip = F(L"%+.0f W", h); }
        else { chip = F(L"%+.0f mV", (s.volts[i] - avgV) * 1000); }
        if (g_mode != 2) cc = s.pinState[i] == 2 ? kCrit : s.pinState[i] == 1 ? kWarn : kGood;
        else cc = kMuted;
        auto cf = Fmt(12, 1, 1);
        float cw = Measure(chip, cf) + 20;
        D2D1_RECT_F cr = R(cx - cw / 2, y, cw, chipH);
        Fill(cr, cc, 12, 0.12f);
        Stroke(cr, cc, 12, 1.f, 0.75f);
        Text(chip, cr, cf, kInk);
        // pin label
        Text(F(L"P%d", i + 1), R(cx - slot / 2, plotBot + 4, slot, labH - 4), Fmt(14, 1, 1), kInk);
        // tooltip
        if (In(R(cx - slot / 2, plotTop, slot, plotBot - plotTop), g_mx, g_my)) {
            Fill(track, kInk, 4, 0.06f);
        }
    }
    // reference lines
    if (g_mode != 2) {
        double lim = g_mode == 0 ? c.limitA : c.limitA * avgV, wrn = g_mode == 0 ? c.warnA : c.warnA * avgV;
        float yl = Y(lim), yw = Y(wrn);
        Line(px0, yl, px1, yl, kCrit, 1.f, true, 0.85f);
        Line(px0, yw, px1, yw, kWarn, 1.f, true, 0.7f);
        auto lf = Fmt(10.5f, 0, 2);
        Text(g_mode == 0 ? F(L"limit %.1f A", lim) : F(L"limit %.0f W", lim), R(px1 - 120, yl - 15, 120, 14), lf, kInk2);
        Text(g_mode == 0 ? F(L"warn %.1f A", wrn) : F(L"warn %.0f W", wrn), R(px1 - 120, yw - 15, 120, 14), lf, kInk2);
    }

    // hover tooltip for a pin
    for (int i = 0; i < 6; ++i) {
        float cx = px0 + slot * (i + 0.5f);
        if (!In(R(cx - slot / 2, plotTop, slot, plotBot - plotTop), g_mx, g_my)) continue;
        std::wstring lines[4] = {F(L"Pin %d", i + 1), F(L"%.3f A  ·  %.3f V", s.amps[i], s.volts[i]),
                                 F(L"%.1f W  ·  %.1f%% of connector", s.amps[i] * s.volts[i],
                                   [&] { double t = 0; for (int k = 0; k < 6; ++k) t += s.amps[k]; return t > 0 ? s.amps[i] / t * 100 : 0; }()),
                                 F(L"Session peak %.2f A", s.peakA[i])};
        float tw = 210, th = 86;
        float tx = std::min(g_mx + 14, P.right - tw - 8), ty = std::max(g_my - th - 8, P.top + 8);
        D2D1_RECT_F tr = R(tx, ty, tw, th);
        Fill(tr, kSurface3, 8);
        Stroke(tr, kBorder, 8);
        Text(lines[0], R(tx + 12, ty + 8, tw - 24, 18), Fmt(13, 1), kInk);
        for (int k = 1; k < 4; ++k) Text(lines[k], R(tx + 12, ty + 8 + 18 * k, tw - 24, 18), Fmt(12), kInk2);
    }
}

D2D1_RECT_F StatusRow(float x, float& y, float w, float h, const std::wstring& label, const wchar_t* glyph) {
    D2D1_RECT_F r = R(x, y, w, h);
    Fill(r, kSurface2, 8);
    Stroke(r, kBorder, 8, 1.f, 0.6f);
    Fill(R(x + 12, y + h / 2 - 13, 26, 26), kAccent, 6, 0.9f);
    Text(glyph, R(x + 12, y + h / 2 - 13, 26, 26), Fmt(13, 0, 1, true), kBg);
    Text(label, R(x + 48, y, 130, h), Fmt(14, 1), kInk);
    y += h + 8;
    return D2D1::RectF(x + 170, r.top, r.right - 14, r.bottom);
}

void Columns(const D2D1_RECT_F& c, const std::vector<std::pair<std::wstring, std::wstring>>& cols) {
    float w = (c.right - c.left) / cols.size();
    float h = c.bottom - c.top;
    for (size_t i = 0; i < cols.size(); ++i) {
        float x = c.left + w * i;
        if (i) Line(x, c.top + 10, x, c.bottom - 10, kBorder);
        Text(cols[i].first, R(x, c.top + h / 2 - 20, w, 18), Fmt(12, 0, 1), kInk2);
        Text(cols[i].second, R(x, c.top + h / 2 - 2, w, 22), Fmt(16, 1, 1), kAccent);
    }
}

void DrawGpuPanel(const D2D1_RECT_F& P) {
    Anchor("gpu", P);
    // Values come from the sensor registry, so this works for NVIDIA (NVML/NVAPI) and AMD/Intel (Windows counters).
    auto S = [](const char* key) -> const sensors::Sensor* {
        const sensors::Sensor* s = g_reg.Find(key);
        return s && s->ok ? s : nullptr;
    };
    auto V = [&](const char* key, const wchar_t* fmt) -> std::wstring {
        const sensors::Sensor* s = S(key);
        return s ? F(fmt, s->value) : std::wstring(L"—");
    };
    Panel(P);
    float x = P.left + 16, w = P.right - P.left - 32;
    float y = P.top + 14;
    Fill(R(x, y + 4, 26, 26), kAccent, 6, 0.9f);
    Text(L"", R(x, y + 4, 26, 26), Fmt(13, 0, 1, true), kBg);
    Text(L"GPU status", R(x + 36, y, 200, 34), Fmt(16, 1), kInk);
    y += 36;
    std::wstring sub = g_reg.gpuName.empty() ? L"No GPU found" : g_reg.gpuName;
    if (g_reg.gpuViaWindows && !g_reg.gpuName.empty())
        sub += g_reg.gpuFromLhm ? L"  \u00b7  Windows counters + LibreHardwareMonitor"
               : g_reg.lhmProcess ? L"  \u00b7  Windows counters (LibreHardwareMonitor: enable Options \u203A Remote Web Server \u203A Run)"
                                  : L"  \u00b7  Windows GPU counters (more detail: run LibreHardwareMonitor)";
    Text(sub, R(x, y, w, 18), Fmt(12), kInk2);
    y += 26;

    const int rows = 7;
    float h = std::clamp((P.bottom - 14 - y - 8 * (rows - 1)) / rows, 40.f, 64.f);
    std::wstring na = L"—";
    auto big = Fmt(17, 1, 2);

    D2D1_RECT_F c = StatusRow(x, y, w, h, L"GPU usage", L"");
    if (const sensors::Sensor* u = S("gpu.util")) {
        float bx = c.left, bw = c.right - c.left - 70;
        Fill(R(bx, (c.top + c.bottom) / 2 - 3, bw, 6), kSurface3, 3);
        Fill(R(bx, (c.top + c.bottom) / 2 - 3, bw * float(std::clamp(u->value, 0.0, 100.0)) / 100.f, 6), kAccent, 3);
    }
    Text(V("gpu.util", L"%.0f%%"), c, big, kAccent);

    c = StatusRow(x, y, w, h, L"Core voltage", L"");
    Text(V("gpu.core_mv", L"%.1f mV"), c, big, kAccent);

    c = StatusRow(x, y, w, h, L"Clocks", L"");
    Columns(c, {{L"Core clock", V("gpu.core_mhz", L"%.0f MHz")}, {L"Memory clock", V("gpu.mem_mhz", L"%.0f MHz")}});

    c = StatusRow(x, y, w, h, L"VRAM usage", L"");
    if (const sensors::Sensor* vu = S("gpu.vram_used")) {
        double used = vu->value, tot = vu->rangeMax;
        Text(F(L"%.2f GB / %.2f GB (%.0f%%)", used, tot, tot > 0 ? used / tot * 100 : 0), R(c.left, c.top + 4, c.right - c.left, h / 2), Fmt(14, 1, 2), kAccent);
        Fill(R(c.left, c.top + h * 0.62f, c.right - c.left, 5), kSurface3, 2.5f);
        if (tot > 0) Fill(R(c.left, c.top + h * 0.62f, float((c.right - c.left) * std::min(1.0, used / tot)), 5), kAccent, 2.5f);
    } else Text(na, c, big, kAccent);

    c = StatusRow(x, y, w, h, L"Temperatures", L"");
    {
        std::vector<std::pair<std::wstring, std::wstring>> cols;
        cols.push_back({L"Core temp", V("gpu.temp", L"%.0f °C")});
        if (S("gpu.hotspot")) cols.push_back({L"Hotspot", V("gpu.hotspot", L"%.1f °C")});
        if (g_reg.Find("gpu.vram_temp")) cols.push_back({L"VRAM temp", V("gpu.vram_temp", L"%.1f °C")});
        Columns(c, cols);
    }

    c = StatusRow(x, y, w, h, L"Fans", L"");
    {
        std::vector<std::pair<std::wstring, std::wstring>> cols;
        for (int f = 1; f <= 3; ++f) {
            char kr[32], kp[32];
            snprintf(kr, sizeof kr, "gpu.fan%d_rpm", f);
            snprintf(kp, sizeof kp, "gpu.fan%d_pct", f);
            const sensors::Sensor *r = S(kr), *pc = S(kp);
            if (!g_reg.Find(kr) && !g_reg.Find(kp)) break;
            std::wstring v = r && pc ? F(L"%.0f RPM (%.0f%%)", r->value, pc->value) : r ? F(L"%.0f RPM", r->value) : pc ? F(L"%.0f%%", pc->value) : na;
            cols.push_back({F(L"Fan %d", f), v});
        }
        if (cols.empty()) cols.push_back({L"Fans", na});
        Columns(c, cols);
    }

    c = StatusRow(x, y, w, h, L"Board power", L"");
    if (S("gpu.board_w") || !g_reg.gpuViaWindows) {
        const sensors::Sensor* bp = S("gpu.board_w");
        if (g_reg.gpuViaWindows)   // LibreHardwareMonitor value: no power limit known
            Columns(c, {{L"Board power", V("gpu.board_w", L"%.0f W")}, {L"Power (of limit)", V("gpu.power_pct", L"%.1f %%")}});
        else
            Columns(c, {{L"Driver reported", V("gpu.board_w", L"%.0f W")}, {L"Power limit", bp ? F(L"%.0f W", bp->rangeMax) : na}});
    } else {
        Columns(c, {{L"Power (of limit)", V("gpu.power_pct", L"%.1f %%")}});
    }
}

void DrawDashboard(float W, float H) {
    const auto& s = g_snap;
    float pad = 16, y = 48 + pad;
    // header
    Fill(R(pad, y + 2, 44, 44), kSurface2, 8);
    Stroke(R(pad, y + 2, 44, 44), kBorder, 8);
    Text(L"GPU", R(pad, y + 2, 44, 44), Fmt(13, 2, 1), kInk);
    std::wstring title = s.knownCard ? Wide(s.model) : s.haveGpu ? Wide(s.gpuName) : !g_reg.gpuName.empty() ? g_reg.gpuName : L"CoreHUD";
    Text(title, R(pad + 56, y, W - 400, 28), Fmt(22, 1), kInk);
    if (s.haveGpu)
        Text(F(L"nvidia-pci %02X · %08x / %08x · %hs", s.busId, s.deviceId, s.subSystemId, s.gpuName.c_str()),
             R(pad + 56, y + 28, W - 400, 20), Fmt(12), kMuted);
    else if (const gpug::Adapter* ga = g_reg.Gpus().Primary())
        Text(F(L"%s · PCI %04X:%04X · %.0f GB VRAM", gpug::VendorName(ga->vendor), ga->vendor, ga->device, ga->dedicatedBytes / 1073741824.0),
             R(pad + 56, y + 28, W - 400, 20), Fmt(12), kMuted);
    y += 60;

    // KPIs
    double connW = 0, totA = 0, avgV = 0, minV = 99;
    for (int i = 0; i < 6; ++i) { connW += s.volts[i] * s.amps[i]; totA += s.amps[i]; avgV += s.volts[i] / 6; minV = std::min(minV, s.volts[i]); }
    bool ok = s.pinsOk;
    std::wstring na = L"—";
    float kw = (W - 2 * pad - 2 * 12) / 3, kh = 110;
    // 8-pin cards that report their rails: the sum of the 8-pin inputs; a 16-pin total from telemetry otherwise
    double railW = 0;
    bool railOk = !g_conn.rails.empty();
    for (auto& r : g_conn.rails) { double p = r.Power(); railOk &= std::isfinite(p); railW += std::isfinite(p) ? p : 0; }
    std::wstring connVal = ok ? F(L"%.1f", connW) : railOk ? F(L"%.1f", railW) : std::isfinite(g_conn.hpwrWatts) ? F(L"%.1f", g_conn.hpwrWatts) : na;
    std::wstring connSub = ok ? (s.stats.powerW >= 0 ? F(L"Board total (driver): %.0f W  ·  rest via PCIe slot", s.stats.powerW) : std::wstring())
                         : railOk ? F(L"%d× 8-pin inputs, as reported by the card", int(g_conn.rails.size()))
                         : g_conn.layout == conn::NONE ? std::wstring(L"Slot-powered card: no power cable")
                         : g_conn.layout == conn::PCIE8_1 || g_conn.layout == conn::PCIE8_2 || g_conn.layout == conn::PCIE8_3
                             ? std::wstring(conn::Name(g_conn.layout)) + L" · the card doesn't report power per connector"
                             : std::wstring(L"No per-pin sensor or meter found (see Pin distribution)");
    DrawKpi(R(pad, y, kw, kh), L"CONNECTOR POWER", connVal, L"W", connSub, L"");
    DrawKpi(R(pad + kw + 12, y, kw, kh), L"TOTAL CURRENT", ok ? F(L"%.2f", totA) : na, L"A",
            ok ? F(L"Average %.2f A per pin", totA / 6) : L"", L"");
    DrawKpi(R(pad + 2 * (kw + 12), y, kw, kh), L"VOLTAGE", ok ? F(L"%.2f", avgV) : na, L"V",
            ok ? F(L"Lowest pin %.3f V  ·  session low %.3f V", minV, s.minVolts) : L"", L"");
    Anchor("kpis", R(pad, y, W - 2 * pad, kh));
    y += kh + pad;

    float leftW = std::floor((W - 3 * pad) * 0.6f);
    DrawPinPanel(D2D1::RectF(pad, y, pad + leftW, H - pad));
    DrawGpuPanel(D2D1::RectF(2 * pad + leftW, y, W - pad, H - pad));
}

// ---------- chart ----------
struct SeriesDef { std::wstring name; uint32_t color; };

void LineChart(const D2D1_RECT_F& P, const std::wstring& title, const std::wstring& unit, const std::vector<SeriesDef>& series,
               float (*get)(const mon::HistPoint&, int), double minMax, const std::vector<std::pair<double, uint32_t>>& refs) {
    Panel(P);
    float x0 = P.left + 18, x1 = P.right - 18;
    Text(title, R(x0, P.top + 12, 400, 26), Fmt(15, 1), kInk);
    // legend (always present for >= 2 series)
    float lx = x1;
    for (int i = int(series.size()) - 1; i >= 0; --i) {
        auto f = Fmt(12, 0, 0);
        float tw = Measure(series[size_t(i)].name, f);
        lx -= tw;
        Text(series[size_t(i)].name, R(lx, P.top + 14, tw + 2, 22), f, kInk2);
        lx -= 16;
        Fill(R(lx, P.top + 22, 10, 6), series[size_t(i)].color, 2);
        lx -= 18;
    }

    double window = g_range == 0 ? 60 : g_range == 1 ? 300 : 1800;
    double tNow = g_hist.empty() ? 0 : g_hist.back().t;
    double tMin = tNow - window;
    // y range
    double vmax = minMax;
    for (auto& h : g_hist)
        if (h.t >= tMin && h.ok)
            for (size_t s = 0; s < series.size(); ++s) vmax = std::max(vmax, double(get(h, int(s))));
    double mag = std::pow(10.0, std::floor(std::log10(std::max(vmax, 1e-3))));
    double step = mag;
    if (vmax / step < 3) step = mag / 2;
    if (vmax / step > 7) step = mag * 2;
    vmax = std::ceil(vmax * 1.08 / step) * step;

    float axisW = 52;
    float px0 = x0 + axisW, px1 = x1, py0 = P.top + 48, py1 = P.bottom - 34;
    auto X = [&](double t) { return float(px0 + (t - tMin) / window * (px1 - px0)); };
    auto Y = [&](double v) { return float(py1 - v / vmax * (py1 - py0)); };
    for (double v = 0; v <= vmax + 1e-9; v += step) {
        float yy = Y(v);
        Line(px0, yy, px1, yy, kGrid);
        Text(F(L"%g %s", v, unit.c_str()), R(x0, yy - 8, axisW - 8, 16), Fmt(10.5f, 0, 2), kMuted);
    }
    Line(px0, py1, px1, py1, kAxis);
    double tickStep = g_range == 0 ? 10 : g_range == 1 ? 60 : 300;
    for (double ago = window; ago >= -1e-9; ago -= tickStep) {
        float xx = X(tNow - ago);
        std::wstring lab = ago < 1e-9 ? std::wstring(L"now") : ago >= 60 ? F(L"-%.0f min", ago / 60) : F(L"-%.0f s", ago);
        Text(lab, R(xx - 30, py1 + 6, 60, 16), Fmt(10.5f, 0, 1), kMuted);
    }
    for (auto& rl : refs) {
        if (rl.first > vmax) continue;
        Line(px0, Y(rl.first), px1, Y(rl.first), rl.second, 1.f, true, 0.8f);
    }

    g_rt->PushAxisAlignedClip(D2D1::RectF(px0, py0 - 4, px1, py1 + 1), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    // lines (break on invalid samples)
    for (size_t s = 0; s < series.size(); ++s) {
        ID2D1PathGeometry* geo = nullptr;
        if (FAILED(g_d2d->CreatePathGeometry(&geo))) continue;
        ID2D1GeometrySink* sink = nullptr;
        geo->Open(&sink);
        bool open = false;
        size_t n = 0;
        for (auto& h : g_hist) {
            if (h.t < tMin - 1) continue;
            float v = get(h, int(s));
            if (!h.ok || v < 0) { if (open) { sink->EndFigure(D2D1_FIGURE_END_OPEN); open = false; } continue; }
            D2D1_POINT_2F p = D2D1::Point2F(X(h.t), Y(v));
            if (!open) { sink->BeginFigure(p, D2D1_FIGURE_BEGIN_HOLLOW); open = true; }
            else sink->AddLine(p);
            ++n;
        }
        if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        sink->Release();
        g_br->SetColor(Col(series[s].color));
        if (n > 1) g_rt->DrawGeometry(geo, g_br, 2.f);
        geo->Release();
    }
    g_rt->PopAxisAlignedClip();

    if (g_hist.empty()) {
        Text(L"Waiting for samples…", D2D1::RectF(px0, py0, px1, py1), Fmt(13, 0, 1), kMuted);
        return;
    }
    // crosshair + tooltip
    if (g_mx >= px0 && g_mx <= px1 && g_my >= py0 && g_my <= py1) {
        double t = tMin + (g_mx - px0) / (px1 - px0) * window;
        const mon::HistPoint* best = nullptr;
        for (auto& h : g_hist) if (!best || std::fabs(h.t - t) < std::fabs(best->t - t)) best = &h;
        if (best && best->t >= tMin) {
            float xx = X(best->t);
            Line(xx, py0, xx, py1, kInk2, 1.f, false, 0.5f);
            for (size_t s = 0; s < series.size(); ++s) {
                float v = get(*best, int(s));
                if (!best->ok || v < 0) continue;
                Circle(xx, Y(v), 6, kSurface);
                Circle(xx, Y(v), 4, series[s].color);
            }
            float tw = 190, th = 34 + 20.f * series.size();
            float tx = xx + 14 + tw > px1 ? xx - 14 - tw : xx + 14;
            float ty = std::clamp(g_my - th / 2, py0, py1 - th);
            D2D1_RECT_F tr = R(tx, ty, tw, th);
            Fill(tr, kSurface3, 8);
            Stroke(tr, kBorder, 8);
            double ago = tNow - best->t;
            Text(ago < 0.5 ? std::wstring(L"now") : F(L"%.1f s ago", ago), R(tx + 12, ty + 8, tw - 24, 18), Fmt(12, 1), kInk);
            for (size_t s = 0; s < series.size(); ++s) {
                float yy = ty + 30 + 20.f * s;
                Fill(R(tx + 12, yy + 6, 10, 6), series[s].color, 2);
                Text(series[s].name, R(tx + 30, yy, 90, 18), Fmt(12), kInk2);
                float v = get(*best, int(s));
                Text(best->ok && v >= 0 ? F(unit == L"W" ? L"%.1f %s" : L"%.2f %s", v, unit.c_str()) : L"—",
                     R(tx + 90, yy, tw - 102, 18), Fmt(12, 1, 2), kInk);
            }
        }
    }
}

float GetAmps(const mon::HistPoint& h, int s) { return h.amps[s]; }
float GetPower(const mon::HistPoint& h, int s) { return s == 0 ? h.connW : h.boardW; }

void DrawChart(float W, float H) {
    float pad = 16, y = 48 + pad;
    Text(L"Range", R(pad, y, 60, 30), Fmt(13, 1), kInk2);
    const wchar_t* ranges[] = {L"1 min", L"5 min", L"30 min"};
    for (int i = 0; i < 3; ++i) Button(R(pad + 60 + i * 78, y, 70, 30), ranges[i], HIT_RANGE + i, g_range == i);
    Anchor("chart", D2D1::RectF(pad, y, W - pad, H - pad));
    y += 42;
    float h1 = std::floor((H - y - pad - 12) * 0.6f);
    std::vector<SeriesDef> pins;
    for (int i = 0; i < 6; ++i) pins.push_back({F(L"P%d", i + 1), kSeries[i]});
    LineChart(R(pad, y, W - 2 * pad, h1), L"Per-pin current", L"A", pins, GetAmps, 2.0,
              {{g_snap.cfg.limitA, kCrit}, {g_snap.cfg.warnA, kWarn}});
    y += h1 + 12;
    LineChart(D2D1::RectF(pad, y, W - pad, H - pad), L"Power", L"W",
              {{L"12V-2x6 connector", kSeries[0]}, {L"Board (driver)", kSeries[1]}}, GetPower, 100.0, {});
}

// ---------- events ----------
void DrawEvents(float W, float H) {
    float pad = 16, y = 48 + pad;
    D2D1_RECT_F P = D2D1::RectF(pad, y, W - pad, H - pad);
    Panel(P);
    Anchor("events", P);
    Text(L"Event log", R(P.left + 18, P.top + 12, 300, 26), Fmt(15, 1), kInk);
    Button(R(P.right - 150, P.top + 10, 132, 28), L"Open logs folder", HIT_LOGS);
    float rowH = 30, ly = P.top + 50;
    int visible = int((P.bottom - ly - 10) / rowH);
    int n = int(g_events.size());
    g_eventScroll = std::clamp(g_eventScroll, 0, std::max(0, n - visible));
    if (n == 0) Text(L"No events yet", R(P.left, ly, P.right - P.left, 30), Fmt(13, 0, 1), kMuted);
    for (int k = 0; k < visible && k + g_eventScroll < n; ++k) {
        const auto& e = g_events[size_t(n - 1 - (k + g_eventScroll))];   // newest first
        float yy = ly + k * rowH;
        if (k % 2 == 0) Fill(R(P.left + 10, yy, P.right - P.left - 20, rowH), kSurface2, 6, 0.6f);
        Text(e.time, R(P.left + 22, yy, 80, rowH), Fmt(12.5f), kInk2);
        uint32_t c = e.level == 2 ? kCrit : e.level == 1 ? kWarn : kAccent;
        const wchar_t* lv = e.level == 2 ? L"CRITICAL" : e.level == 1 ? L"WARNING" : L"INFO";
        Circle(P.left + 112, yy + rowH / 2, 4, c);
        Text(lv, R(P.left + 122, yy, 80, rowH), Fmt(11.5f, 1), kInk2);
        Text(e.text, R(P.left + 205, yy, P.right - P.left - 225, rowH), Fmt(13), kInk);
    }
    if (n > visible) Text(F(L"%d events · scroll for more", n), R(P.right - 340, P.top + 10, 180, 28), Fmt(11.5f, 0, 2), kMuted);
}

#include "panel_ui.inc"
#include "setup_ui.inc"

// ---------- notifications (tray) ----------
NOTIFYICONDATAW g_nid{};
uint64_t g_notifiedSeq = 0;
constexpr UINT WM_APP_TRAY = WM_APP + 2;

void TrayInit(HICON icon) {
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = icon;
    wcscpy_s(g_nid.szTip, L"CoreHUD");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

void TrayNotify(int level, const std::wstring& text) {
    g_nid.uFlags = NIF_INFO;
    wcscpy_s(g_nid.szInfoTitle, level == 2 ? L"CoreHUD — CRITICAL" : L"CoreHUD — warning");
    wcsncpy_s(g_nid.szInfo, text.c_str(), _TRUNCATE);
    g_nid.dwInfoFlags = level == 2 ? NIIF_ERROR : NIIF_WARNING;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    FLASHWINFO fw{sizeof fw, g_hwnd, FLASHW_TRAY | FLASHW_TIMERNOFG, level == 2 ? 0u : 5u, 0};
    FlashWindowEx(&fw);
}

void CheckAlerts() {
    for (auto& e : g_events) {
        if (e.seq <= g_notifiedSeq) continue;
        g_notifiedSeq = e.seq;
        if (e.level >= 1) TrayNotify(e.level, e.text);
    }
}

#include "settings_ui.inc"
#include "tablet_ui.inc"
#include "tour_ui.inc"

// ---------- render plumbing ----------
void CreateTarget() {
    if (g_rt) return;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    float dpi = float(GetDpiForWindow(g_hwnd));
    auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), dpi, dpi);
    g_d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(g_hwnd, D2D1::SizeU(rc.right, rc.bottom)), &g_hrt);
    g_rt = g_hrt;
    if (!g_rt) return;
    g_rt->CreateSolidColorBrush(Col(kInk), &g_br);
    g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
}

void Paint() {
    CreateTarget();
    if (!g_rt) return;
    g_rt->BeginDraw();
    g_rt->SetTransform(D2D1::Matrix3x2F::Identity());
    g_rt->Clear(Col(kBg));
    auto sz = g_rt->GetSize();
    g_hits.clear();
    g_anchors.clear();
    if (g_tab == TAB_DASH) DrawDashboard(sz.width, sz.height);
    else if (g_tab == TAB_CHART) DrawChart(sz.width, sz.height);
    else if (g_tab == TAB_PANEL) DrawPanelEditor(sz.width, sz.height);
    else DrawEvents(sz.width, sz.height);
    DrawTopBar(sz.width);
    if (g_setupOpen) DrawSetupOverlay(sz.width, sz.height);
    else if (g_settingsOpen) DrawSettingsOverlay(sz.width, sz.height);
    else if (g_tabletOpen) DrawTabletOverlay(sz.width, sz.height);
    DrawAlarmBanner(sz.width);
    if (!g_setupOpen && !g_settingsOpen && !g_tabletOpen) DrawTour(sz.width, sz.height);
    if (g_rt->EndDraw() == D2DERR_RECREATE_TARGET) { ReleaseBmp(g_bmpMain); SafeRelease(g_br); SafeRelease(g_hrt); g_rt = nullptr; }
}

void OnClick(float x, float y) {
    for (auto it = g_hits.rbegin(); it != g_hits.rend(); ++it) {
        if (!In(it->r, x, y)) continue;
        int id = it->id;
        if (g_focus && id != HIT_FIELD + g_focus) CommitFocus();
        if (id >= HIT_FIELD) { FocusField(id - HIT_FIELD); InvalidateRect(g_hwnd, nullptr, FALSE); return; }
        if (it->fn) {
            auto fn = it->fn;   // hits are rebuilt on the next paint
            fn();
            InvalidateRect(g_hwnd, nullptr, FALSE);
            return;
        }
        if (id >= HIT_TAB && id < HIT_TAB + TAB_COUNT) g_tab = id - HIT_TAB;
        else if (id >= HIT_MODE && id < HIT_MODE + 3) g_mode = id - HIT_MODE;
        else if (id >= HIT_RANGE && id < HIT_RANGE + 3) g_range = id - HIT_RANGE;
        else if (id == HIT_RESTART) mon::Restart(mon::LoadConfig(g_ini));
        else if (id == HIT_PEAKS) mon::ResetPeaks();
        else if (id == HIT_CSV) mon::SetCsv(!g_snap.csvOn);
        else if (id == HIT_INI) { g_tabletOpen = false; OpenSettings(); }
        else if (id == HIT_COMPONENTS) { g_settingsOpen = false; OpenSetup(); }
        else if (id == HIT_TOUR) { g_settingsOpen = false; g_tabletOpen = false; StartTour(); }
        else if (id == HIT_LOGS) {
            std::wstring dir = mon::ExeDir() + L"\\logs";
            CreateDirectoryW(dir.c_str(), nullptr);
            ShellExecuteW(g_hwnd, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }
    if (g_focus) { CommitFocus(); InvalidateRect(g_hwnd, nullptr, FALSE); }
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_APP_SAMPLE:
        mon::Get(g_snap);
        mon::GetHistory(g_hist);
        mon::GetEvents(g_events);
        OnSampleForPanel();
        UpdateConnector();
        if (g_setupOpen && GetTickCount() - g_compsTick > 3000) RecheckComponents();
        if (g_upd.applyPending) TryApplyUpdate();
        RenderTabletFrame();
        CheckAlerts();
        {
            double connW = 0;
            for (int i = 0; i < 6; ++i) connW += g_snap.volts[i] * g_snap.amps[i];
            wchar_t t[128];
            if (g_snap.pinsOk) swprintf(t, 128, L"CoreHUD — %.0f W · max pin %.2f A", connW, *std::max_element(g_snap.amps, g_snap.amps + 6));
            else wcscpy_s(t, L"CoreHUD");
            SetWindowTextW(h, t);
            if (g_nid.cbSize) {
                g_nid.uFlags = NIF_TIP;
                wcsncpy_s(g_nid.szTip, t, _TRUNCATE);
                Shell_NotifyIconW(NIM_MODIFY, &g_nid);
            }
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        Paint();
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE:
        if (g_hrt) g_hrt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_DPICHANGED: {
        SafeRelease(g_br);
        SafeRelease(g_hrt);
    g_rt = nullptr;
        auto r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto mmi = reinterpret_cast<MINMAXINFO*>(lp);
        float s = GetDpiForWindow(h) / 96.f;
        mmi->ptMinTrackSize = {LONG(1080 * s), LONG(720 * s)};
        return 0;
    }
    case WM_MOUSEMOVE: {
        float s = 96.f / GetDpiForWindow(h);
        g_mx = GET_X_LPARAM(lp) * s;
        g_my = GET_Y_LPARAM(lp) * s;
        if (g_drag) PreviewMouseMove(g_mx, g_my);
        if (!g_tracking) {
            TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
            TrackMouseEvent(&t);
            g_tracking = true;
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_tracking = false;
        g_mx = g_my = -1;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        float s = 96.f / GetDpiForWindow(h);
        if (!g_setupOpen && !g_settingsOpen && !g_tabletOpen && !TourActive() && g_tab == TAB_PANEL && PreviewMouseDown(GET_X_LPARAM(lp) * s, GET_Y_LPARAM(lp) * s)) InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g_drag) {
            g_drag = 0;
            ReleaseCapture();
            SaveLayout();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        float s = 96.f / GetDpiForWindow(h);
        float x = GET_X_LPARAM(lp) * s, y = GET_Y_LPARAM(lp) * s;
        if (!g_setupOpen && !g_settingsOpen && !g_tabletOpen && !TourActive() && g_tab == TAB_PANEL && In(g_preview, x, y)) return 0;   // handled on button-down
        OnClick(x, y);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int steps = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        if (g_tab == TAB_EVENTS) {
            g_eventScroll -= steps * 3;
            if (g_eventScroll < 0) g_eventScroll = 0;
        } else if (g_tab == TAB_PANEL && In(g_listRect, g_mx, g_my)) {
            g_listScroll -= steps * 3;
            if (g_listScroll < 0) g_listScroll = 0;
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_CHAR:
        if (!g_focus) return 0;
        if (wp == 27) { g_focus = 0; }                                   // Esc: cancel
        else if (wp == 13 || wp == 9) { CommitFocus(); }                 // Enter / Tab: apply
        else if (wp == 8) { if (!g_editBuf.empty()) g_editBuf.pop_back(); }
        else if (wp >= 32 && g_editBuf.size() < 200) g_editBuf += wchar_t(wp);
        if (g_focus == F_SEARCH) { g_search = g_editBuf; g_listScroll = 0; }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_KEYDOWN:
        if (g_focus) return 0;   // typing goes to the focused field (WM_CHAR)
        if (!g_setupOpen && !g_settingsOpen && !g_tabletOpen && TourKey(wp)) { InvalidateRect(h, nullptr, FALSE); return 0; }
        if (g_setupOpen || g_settingsOpen || g_tabletOpen) {
            if (wp == VK_ESCAPE) { g_setupOpen = false; g_settingsOpen = false; g_tabletOpen = false; InvalidateRect(h, nullptr, FALSE); }
            return 0;
        }
        if (g_tab == TAB_PANEL && g_sel >= 0 && g_sel < int(g_layout.widgets.size())) {
            auto& w = g_layout.widgets[size_t(g_sel)];
            bool handled = true;
            if (wp == VK_DELETE) { g_layout.widgets.erase(g_layout.widgets.begin() + g_sel); g_sel = -1; }
            else if (wp == VK_LEFT) TryPlace(w.x - 1, w.y, w.w, w.h);
            else if (wp == VK_RIGHT) TryPlace(w.x + 1, w.y, w.w, w.h);
            else if (wp == VK_UP) TryPlace(w.x, w.y - 1, w.w, w.h);
            else if (wp == VK_DOWN) TryPlace(w.x, w.y + 1, w.w, w.h);
            else if (wp == VK_ESCAPE) g_sel = -1;
            else handled = false;
            if (handled) { SaveLayout(); InvalidateRect(h, nullptr, FALSE); return 0; }
        }
        if (wp == VK_TAB) { g_tab = (g_tab + 1) % TAB_COUNT; InvalidateRect(h, nullptr, FALSE); }
        else if (wp >= '1' && wp <= '4') { g_tab = int(wp - '1'); InvalidateRect(h, nullptr, FALSE); }
        else if (g_tab == TAB_DASH && (wp == 'A' || wp == 'W' || wp == 'V')) { g_mode = wp == 'A' ? 0 : wp == 'W' ? 1 : 2; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_DISPLAYCHANGE:
        OnDisplaysChanged();
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_APP_PANEL_SNAP:
        if (Slot* s = SlotById(int(lp))) SnapPanel(*s, wp == 0);
        return 0;
    case WM_APP_UPDATE:
        OnUpdateResult(reinterpret_cast<UpdResult*>(lp));
        return 0;
    case WM_TIMER:
        if (wp == TIMER_UPDATE) {
            SetTimer(h, TIMER_UPDATE, 6 * 60 * 60 * 1000, nullptr);   // then every 6 hours
            if (UpdateMode() != upd::MODE_OFF) StartUpdateCheck(false);
        }
        return 0;
    case WM_APP_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == NIN_BALLOONUSERCLICK) {
            ShowWindow(h, IsIconic(h) ? SW_RESTORE : SW_SHOW);
            SetForegroundWindow(h);
        }
        return 0;
    case WM_DESTROY:
        g_adb.Stop();       // undo adb reverse / stay-awake on connected tablets
        g_tabSrv.Stop();
        if (g_kaThread.joinable()) g_kaThread.join();   // keep-awake video encoder (finishes within a second)
        CloseAllPanels();
        if (g_nid.cbSize) Shell_NotifyIconW(NIM_DELETE, &g_nid);
        mon::Stop();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

HICON MakeIcon(int size) {
    // Small teal "bars" icon drawn at runtime (no resource file needed).
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof bi;
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_RGB;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    auto px = static_cast<uint32_t*>(bits);
    const float hgt[6] = {0.55f, 0.75f, 0.65f, 0.85f, 0.6f, 0.7f};
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            float fx = (x + 0.5f) / size, fy = (y + 0.5f) / size;
            bool bg = fx > 0.04f && fx < 0.96f && fy > 0.04f && fy < 0.96f;
            uint32_t c = bg ? 0xFF15181D : 0;
            int bar = int((fx - 0.14f) / 0.12f);
            float inBar = (fx - 0.14f) - bar * 0.12f;
            if (bar >= 0 && bar < 6 && inBar < 0.08f && fy > 0.88f - hgt[bar] * 0.76f && fy < 0.88f) c = 0xFF2FD9C4;
            px[y * size + x] = c;
        }
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return ic;
}

} // namespace

int CliMain(int argc, char** argv);   // cli.cpp - console mode, built into this exe

// "CoreHUD.exe --cli ..." runs the console tools in the terminal it was started from (or into a pipe/file).
static int RunCli(int argc, LPWSTR* wargv) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD type = out && out != INVALID_HANDLE_VALUE ? GetFileType(out) : FILE_TYPE_UNKNOWN;
    bool redirected = type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE;
    // Always join the terminal it was started from, also when the output is piped/redirected: that way Ctrl+C
    // in the terminal stops it (otherwise "-n 0 | more" could keep running invisibly after more quits).
    bool attached = AttachConsole(ATTACH_PARENT_PROCESS) != FALSE;
    if (!redirected) {
        if (!attached) AllocConsole();
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
        printf("\n");
    }
    std::vector<std::string> args;
    args.push_back("CoreHUD.exe");
    for (int i = 2; i < argc; ++i) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string a(size_t(n > 0 ? n - 1 : 0), '\0');
        if (n > 1) WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, a.data(), n, nullptr, nullptr);
        args.push_back(a);
    }
    std::vector<char*> ptrs;
    for (auto& a : args) ptrs.push_back(a.data());
    int rc = CliMain(int(ptrs.size()), ptrs.data());
    fflush(stdout);
    return rc;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv && argc >= 2 && !_wcsicmp(argv[1], L"--cli")) {
            int rc = RunCli(argc, argv);
            LocalFree(argv);
            return rc;
        }
        if (argv) LocalFree(argv);
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // After an auto-update the new version is started with --wait-pid <old pid>: wait for the old one to exit.
    bool justUpdated = false;
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; argv && i < argc; ++i) {
            if (!wcscmp(argv[i], L"--updated")) justUpdated = true;
            if (!wcscmp(argv[i], L"--wait-pid") && i + 1 < argc) {
                HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, DWORD(_wtoi(argv[++i])));
                if (old) { WaitForSingleObject(old, 15000); CloseHandle(old); }
            }
        }
        if (argv) LocalFree(argv);
    }
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\CoreHUDSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(L"CoreHUDWnd", nullptr);
        if (other) { ShowWindow(other, SW_RESTORE); SetForegroundWindow(other); }
        return 0;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d)) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&g_dw)))) {
        MessageBoxW(nullptr, L"Direct2D / DirectWrite unavailable", L"CoreHUD", MB_ICONERROR);
        return 1;
    }
    float dashes[] = {4.f, 3.f};
    g_d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                         D2D1_LINE_JOIN_MITER, 10.f, D2D1_DASH_STYLE_CUSTOM, 0.f),
                             dashes, 2, &g_dash);
    g_d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                         D2D1_LINE_JOIN_ROUND), nullptr, 0, &g_round);

    HICON bigIcon = MakeIcon(32), smallIcon = MakeIcon(16);
    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = bigIcon;
    wc.hIconSm = smallIcon;
    wc.lpszClassName = L"CoreHUDWnd";
    RegisterClassExW(&wc);
    WNDCLASSEXW pc = wc;
    pc.style = CS_DBLCLKS;
    pc.lpfnWndProc = PanelProc;
    pc.lpszClassName = L"CoreHUDPanelWnd";
    RegisterClassExW(&pc);
    WNDCLASSEXW ic = wc;
    ic.style = 0;
    ic.lpfnWndProc = IdentifyProc;
    ic.lpszClassName = L"CoreHUDIdentify";
    RegisterClassExW(&ic);

    // sensor panel layouts
    g_ini = mon::DefaultIniPath();
    const bool firstRun = GetFileAttributesW(g_ini.c_str()) == INVALID_FILE_ATTRIBUTES;
    LoadSlots();
    LoadAlarmCfg();
    LoadConnectorChoice();
    EnumMonitors();

    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    UINT dpi = GetDpiForSystem();
    int w = std::min(MulDiv(1600, dpi, 96), int((wa.right - wa.left) * 0.95));
    int hgt = std::min(MulDiv(1000, dpi, 96), int((wa.bottom - wa.top) * 0.95));
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"CoreHUD", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, w, hgt,
                             nullptr, nullptr, inst, nullptr);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof dark);
    TrayInit(smallIcon);

    mon::Start(mon::LoadConfig(g_ini), g_hwnd, WM_APP_SAMPLE);
    MaybeShowSetupAtStart();
    InitTour(firstRun);
    upd::CleanupOld();
    g_tabCfg = LoadTabletCfg();
    ApplyTablet();
    MakeTabletIcon();
    if (justUpdated) {
        mon::LogEvent(0, L"Updated to CoreHUD " COREHUD_VERSION L" (build " COREHUD_BUILD L")");
        TrayNotify(1, L"CoreHUD was updated to " COREHUD_VERSION L".");
    }
    SetTimer(g_hwnd, TIMER_UPDATE, 20 * 1000, nullptr);   // first update check 20 s after start

    ShowWindow(g_hwnd, show);
    UpdateWindow(g_hwnd);
    ShowPanelsAtStart();
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    for (auto& f : g_fmts) f.second->Release();
    g_reg.Shutdown();
    ReleaseTabletRt();
    ReleaseBmp(g_bmpMain);
    SafeRelease(g_wic);
    SafeRelease(g_round);
    SafeRelease(g_br);
    SafeRelease(g_hrt);
    g_rt = nullptr;
    SafeRelease(g_dash);
    SafeRelease(g_dw);
    SafeRelease(g_d2d);
    if (single) CloseHandle(single);
    return 0;
}
