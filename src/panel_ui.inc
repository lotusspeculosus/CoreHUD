// Sensor panel: renderer, editor tab and the panel window.
// Included inside app.cpp's anonymous namespace (uses its drawing helpers).

panel::Layout g_layout;          // working copy of the layout open in the editor (= g_slots[g_edit]); SaveLayout() stores it
std::unordered_map<std::string, std::deque<std::pair<double, float>>> g_ghist;   // graph history per sensor key

int g_sel = -1;                 // selected widget
int g_drag = 0;                 // 0 none, 1 move, 2 resize
int g_dragDX = 0, g_dragDY = 0;
D2D1_RECT_F g_preview{};
float g_previewScale = 1.f;
D2D1_RECT_F g_listRect{};
int g_listScroll = 0;
std::wstring g_search;

// text fields (immediate mode)
constexpr int HIT_FIELD = 10000;
enum FieldId { F_WIDTH = 1, F_HEIGHT, F_LABEL, F_MIN, F_MAX, F_WARN, F_CRIT, F_SEARCH, F_SLOTNAME };
int g_focus = 0;
std::wstring g_editBuf;
int g_commitId = 0;
std::wstring g_commitText;
std::map<int, std::wstring> g_fieldInit;

struct BgBmp { ID2D1Bitmap* bmp = nullptr; std::wstring path; bool failed = false; };
BgBmp g_bmpMain;
BgBmp* g_curBmp = &g_bmpMain;
IWICImagingFactory* g_wic = nullptr;
ID2D1StrokeStyle* g_round = nullptr;

// ---- layouts ("save slots"): layouts\panel-<id>.txt, each with its own display and its own panel window ----
// Any number can be open at the same time (one fullscreen per display, plus floating virtual windows);
// the tablet shows the layout picked in the Tablet dialog.
struct Slot {
    int id = 0;
    std::wstring path;
    panel::Layout layout;      // saved design; for the layout open in the editor, g_layout is the live copy
    HWND wnd = nullptr;
    bool virt = false;         // virtual panel = floating "device" window with a bezel
    float virtScale = 1.f;
    bool virtTop = false;
    std::wstring snapDev;      // display picked by dragging / the menu (applied via WM_APP_PANEL_SNAP)
    ID2D1HwndRenderTarget* prt = nullptr;
    ID2D1SolidColorBrush* pbr = nullptr;
    BgBmp bmp;
};
std::vector<std::unique_ptr<Slot>> g_slots;
int g_edit = 0;                // index of the layout open in the editor
constexpr size_t kMaxSlots = 16;
constexpr float kBezel = 18.f, kBezelBottom = 30.f;

Slot& EditSlot() { return *g_slots[size_t(g_edit)]; }
bool IsEdit(const Slot& s) { return &s == g_slots[size_t(g_edit)].get(); }
const panel::Layout& LayoutOf(const Slot& s) { return IsEdit(s) ? g_layout : s.layout; }
panel::Layout& MutLayout(Slot& s) { return IsEdit(s) ? g_layout : s.layout; }
Slot* SlotById(int id) {
    for (auto& s : g_slots) if (s->id == id) return s.get();
    return nullptr;
}
int SlotIndex(const Slot* p) {
    for (size_t i = 0; i < g_slots.size(); ++i) if (g_slots[i].get() == p) return int(i);
    return -1;
}
std::wstring SlotName(const Slot& s) {
    const auto& n = LayoutOf(s).name;
    return n.empty() ? F(L"Layout %d", s.id) : n;
}

struct MonInfo { std::wstring dev; RECT rc; bool primary; std::wstring name; };
std::vector<MonInfo> g_mons;
constexpr UINT WM_APP_PANEL_SNAP = WM_APP + 3;   // posted to the main window: wParam 0 = fullscreen on slot.snapDev, 1 = back to window; lParam = slot id

double NowSec() { return GetTickCount64() / 1000.0; }

// ---- connector alarm on every panel + tablet ----
bool g_alarmPanels = true;          // [Alarm] Panels
bool g_alarmTabletSound = true;     // [Alarm] TabletSound
ULONGLONG g_alarmTestUntil = 0;     // Settings > Test alarm
void LoadAlarmCfg() {
    g_alarmPanels = GetPrivateProfileIntW(L"Alarm", L"Panels", 1, g_ini.c_str()) != 0;
    g_alarmTabletSound = GetPrivateProfileIntW(L"Alarm", L"TabletSound", 1, g_ini.c_str()) != 0;
}
// 2 = a pin is over its limit, 1 = test alarm, 0 = none
int AlarmActive() {
    if (g_snap.pinsOk)
        for (int i = 0; i < g_snap.pinCount && i < pins::kMaxPins; ++i)
            if (g_snap.pinState[i] == 2) return 2;
    return GetTickCount64() < g_alarmTestUntil ? 1 : 0;
}

// ---- virtual panel window sizing (pixels) ----
float FitVirtualScale(const Slot& s) {
    const auto& L = LayoutOf(s);
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    float k = std::min((wa.right - wa.left - 2 * kBezel - 40) / L.width, (wa.bottom - wa.top - kBezel - kBezelBottom - 40) / L.height);
    return std::clamp(k, 0.1f, 4.f);
}
SIZE VirtualWindowSize(const Slot& s) {
    const auto& L = LayoutOf(s);
    return {LONG(std::lround(L.width * s.virtScale + 2 * kBezel)), LONG(std::lround(L.height * s.virtScale + kBezel + kBezelBottom))};
}
void SetVirtualScale(Slot& s, float k) {
    if (!s.wnd || !s.virt) return;
    s.virtScale = std::clamp(k, 0.1f, 4.f);
    RECT r;
    GetWindowRect(s.wnd, &r);
    SIZE sz = VirtualWindowSize(s);
    int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    SetWindowPos(s.wnd, nullptr, cx - sz.cx / 2, cy - sz.cy / 2, sz.cx, sz.cy, SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(s.wnd, nullptr, FALSE);
}
void ToggleVirtualTop(Slot& s) {
    s.virtTop = !s.virtTop;
    if (s.wnd) SetWindowPos(s.wnd, s.virtTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// How a design is shown on a W x H surface (case display, tablet):
//   same shape (within 10%)          -> as designed, scaled
//   rotated (portrait vs landscape)  -> letterboxed: the design is kept intact and fitted, centred
//   other shape, same orientation    -> a proportionally regridded copy (AdaptedLayout); the design itself is untouched
bool LetterboxRect(const panel::Layout& L, float W, float H, D2D1_RECT_F& r);
void RenderOnSurface(const panel::Layout& L, float W, float H);

void RepaintSlot(Slot& s) {
    if (!s.wnd) return;
    if (s.virt) SetVirtualScale(s, s.virtScale);   // the panel size may have changed
    InvalidateRect(s.wnd, nullptr, FALSE);
}

// Stores the editor's working copy into its layout file.
void SaveLayout() {
    panel::Clamp(g_layout);
    Slot& s = EditSlot();
    s.layout = g_layout;
    panel::Save(s.path, g_layout);
    RepaintSlot(s);
}
void SaveSlot(Slot& s) {
    if (IsEdit(s)) { SaveLayout(); return; }
    panel::Clamp(s.layout);
    panel::Save(s.path, s.layout);
    RepaintSlot(s);
}

void EnumMonitors() {
    g_mons.clear();
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM) -> BOOL {
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof mi;
        if (GetMonitorInfoW(m, &mi)) g_mons.push_back({mi.szDevice, mi.rcMonitor, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0, L""});
        return TRUE;
    }, 0);
    // Friendly monitor names ("ASUS VG27A", "8.8 inch bar") via the display configuration API.
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) return;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS) return;
    for (UINT32 i = 0; i < np; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof src;
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id = paths[i].sourceInfo.id;
        DISPLAYCONFIG_TARGET_DEVICE_NAME tgt{};
        tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tgt.header.size = sizeof tgt;
        tgt.header.adapterId = paths[i].targetInfo.adapterId;
        tgt.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS || DisplayConfigGetDeviceInfo(&tgt.header) != ERROR_SUCCESS) continue;
        for (auto& mo : g_mons)
            if (mo.dev == src.viewGdiDeviceName && tgt.monitorFriendlyDeviceName[0]) mo.name = tgt.monitorFriendlyDeviceName;
    }
}

int MonitorIndex(const std::wstring& dev) {
    for (size_t i = 0; i < g_mons.size(); ++i) if (g_mons[i].dev == dev) return int(i);
    return -1;
}

std::wstring MonitorLabel(int i) {
    if (i < 0 || i >= int(g_mons.size())) return L"";
    const auto& m = g_mons[size_t(i)];
    std::wstring s = F(L"Display %d", i + 1);
    if (!m.name.empty()) s += L" \u00b7 " + m.name;
    s += F(L" \u00b7 %d\u00d7%d (%s)", m.rc.right - m.rc.left, m.rc.bottom - m.rc.top,
           panel::AspectLabel(m.rc.right - m.rc.left, m.rc.bottom - m.rc.top).c_str());
    if (m.primary) s += L" \u00b7 main";
    return s;
}

// --- "Identify displays": a big number on every screen for a few seconds ---
LRESULT CALLBACK IdentifyProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(0x15, 0x18, 0x1D));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        HBRUSH edge = CreateSolidBrush(RGB(0x2F, 0xD9, 0xC4));
        FrameRect(dc, &rc, edge);
        DeleteObject(edge);
        int idx = int(GetWindowLongPtrW(h, GWLP_USERDATA));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(0x2F, 0xD9, 0xC4));
        HFONT fontBig = CreateFontW(-(rc.bottom * 55 / 100), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HGDIOBJ old = SelectObject(dc, fontBig);
        RECT top = rc;
        top.bottom = rc.bottom * 70 / 100;
        std::wstring num = std::to_wstring(idx + 1);
        DrawTextW(dc, num.c_str(), -1, &top, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        HFONT fontSmall = CreateFontW(-(rc.bottom / 11), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        SelectObject(dc, fontSmall);
        SetTextColor(dc, RGB(0xEE, 0xF1, 0xF4));
        RECT bottom = rc;
        bottom.top = top.bottom - 8;
        bottom.left += 12;
        bottom.right -= 12;
        std::wstring lab = MonitorLabel(idx);
        DrawTextW(dc, lab.c_str(), -1, &bottom, DT_CENTER | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
        SelectObject(dc, old);
        DeleteObject(fontBig);
        DeleteObject(fontSmall);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_TIMER:
    case WM_LBUTTONUP:
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

void IdentifyDisplays() {
    EnumMonitors();
    HINSTANCE inst = GetModuleHandleW(nullptr);
    for (size_t i = 0; i < g_mons.size(); ++i) {
        const RECT& r = g_mons[i].rc;
        int mw = r.right - r.left, mh = r.bottom - r.top;
        int h = std::clamp(mh * 45 / 100, 120, 420), w = std::min(mw - 20, h * 16 / 10);
        HWND wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"RigSenseIdentify", L"", WS_POPUP,
                                   r.left + (mw - w) / 2, r.top + (mh - h) / 2, w, h, nullptr, nullptr, inst, nullptr);
        if (!wnd) continue;
        SetWindowLongPtrW(wnd, GWLP_USERDATA, LONG_PTR(i));
        ShowWindow(wnd, SW_SHOWNOACTIVATE);
        SetTimer(wnd, 1, 3000, nullptr);
    }
}

void ReleaseBmp(BgBmp& b) { SafeRelease(b.bmp); b.path.clear(); b.failed = false; }

ID2D1Bitmap* BackgroundBitmap(const panel::Layout& L) {
    BgBmp& b = *g_curBmp;
    if (L.bgImage.empty()) { ReleaseBmp(b); return nullptr; }
    if (b.path == L.bgImage && (b.bmp || b.failed)) return b.bmp;
    ReleaseBmp(b);
    b.path = L.bgImage;
    if (!g_wic) CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic));
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICFormatConverter* cv = nullptr;
    if (g_wic && SUCCEEDED(g_wic->CreateDecoderFromFilename(b.path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(g_wic->CreateFormatConverter(&cv)) &&
        SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeMedianCut)))
        g_rt->CreateBitmapFromWicBitmap(cv, nullptr, &b.bmp);
    SafeRelease(cv);
    SafeRelease(fr);
    SafeRelease(dec);
    b.failed = b.bmp == nullptr;
    return b.bmp;
}

// ---------- widget rendering ----------
std::wstring FmtNum(double v, int dec) {
    wchar_t b[64];
    swprintf(b, 64, L"%.*f", std::clamp(dec, 0, 4), v);
    return b;
}
int StateOf(double v, double warn, double crit) {
    if (!std::isnan(crit) && v >= crit) return 2;
    if (!std::isnan(warn) && v >= warn) return 1;
    return 0;
}
float FitSize(const std::wstring& t, int weight, float size, float maxW) {
    if (t.empty() || maxW <= 0) return size;
    float w = Measure(t, Fmt(size, weight));
    return w > maxW ? std::max(6.f, size * maxW / w * 0.97f) : size;
}

ID2D1PathGeometry* ArcGeometry(float cx, float cy, float r, float a0, float a1) {
    ID2D1PathGeometry* g = nullptr;
    if (FAILED(g_d2d->CreatePathGeometry(&g))) return nullptr;
    ID2D1GeometrySink* s = nullptr;
    g->Open(&s);
    auto pt = [&](float a) { float rad = a * 3.14159265f / 180.f; return D2D1::Point2F(cx + r * std::cos(rad), cy + r * std::sin(rad)); };
    s->BeginFigure(pt(a0), D2D1_FIGURE_BEGIN_HOLLOW);
    s->AddArc(D2D1::ArcSegment(pt(a1), D2D1::SizeF(r, r), 0.f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                               a1 - a0 > 180.f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    s->EndFigure(D2D1_FIGURE_END_OPEN);
    s->Close();
    s->Release();
    return g;
}

void DrawArc(float cx, float cy, float r, float a0, float a1, uint32_t c, float width, float alpha = 1.f) {
    if (a1 - a0 < 0.3f) return;
    ID2D1PathGeometry* g = ArcGeometry(cx, cy, r, a0, std::min(a1, a0 + 359.9f));
    if (!g) return;
    g_br->SetColor(Col(c, alpha));
    g_rt->DrawGeometry(g, g_br, width, g_round);
    g->Release();
}

void DrawWidget(const panel::Widget& w, const D2D1_RECT_F& r, float s, float radius) {
    const sensors::Sensor* sen = (w.type == panel::WType::Clock || w.type == panel::WType::Text || w.type == panel::WType::Pins)
                                     ? nullptr : g_reg.Find(w.sensor);
    uint32_t accent = panel::AccentColor(w.color);
    float rw = r.right - r.left, rh = r.bottom - r.top;
    float ts = w.textScale / 100.f;
    float p = std::min(rw, rh) * 0.08f + 2 * s;

    if (w.bg == panel::Bg::Card) {
        Fill(r, kSurface, radius, 0.9f);
        Stroke(r, kBorder, radius, std::max(1.f, s));
    } else if (w.bg == panel::Bg::Outline) {
        Stroke(r, accent, radius, std::max(1.5f, 1.5f * s), 0.65f);
    }

    std::wstring label = !w.text.empty() ? w.text : sen ? sen->name : std::wstring();
    bool ok = sen && sen->ok;
    double v = ok ? sen->value : 0;
    int dec = w.decimals >= 0 ? w.decimals : sen ? sen->decimals : 1;
    double warn = !std::isnan(w.warn) ? w.warn : sen ? sen->warn : NAN;
    double crit = !std::isnan(w.crit) ? w.crit : sen ? sen->crit : NAN;
    int state = ok ? StateOf(v, warn, crit) : 0;
    uint32_t vc = state == 2 ? kCrit : state == 1 ? kWarn : accent;
    std::wstring unit = sen && w.showUnit ? sen->unit : L"";
    std::wstring val = ok ? FmtNum(v, dec) : L"—";
    double vmin = !std::isnan(w.min) ? w.min : 0;
    double vmax = !std::isnan(w.max) ? w.max : sen ? sen->rangeMax : 100;
    if (vmax <= vmin) vmax = vmin + 1;
    double frac = std::clamp((v - vmin) / (vmax - vmin), 0.0, 1.0);

    if (w.type != panel::WType::Clock && w.type != panel::WType::Text && w.type != panel::WType::Pins && !sen) {
        label = w.text.empty() ? L"Sensor not available" : w.text;
    }

    float labelSize = std::clamp(rh * 0.12f, 8.f, 56.f) * ts;
    auto drawLabel = [&](float y, float maxW) {
        if (!w.showLabel || label.empty()) return 0.f;
        float x = r.left + p;
        // shrink long labels to fit (down to 70%), beyond that they get clipped
        float fit = FitSize(label, 1, labelSize, maxW - (state > 0 ? labelSize * 1.3f : 0));
        labelSize = std::max(fit, labelSize * 0.55f);
        if (state > 0) {
            Text(L"", R(x, y, labelSize * 1.3f, labelSize * 1.35f), Fmt(labelSize * 0.95f, 0, 0, true), vc);
            x += labelSize * 1.3f;
            maxW -= labelSize * 1.3f;
        }
        Text(label, R(x, y, maxW, labelSize * 1.35f), Fmt(labelSize, 1), kInk2);
        return labelSize * 1.4f;
    };

    switch (w.type) {
    case panel::WType::Value: {
        float top = r.top + p + drawLabel(r.top + p, rw - 2 * p);
        float avail = r.bottom - p - top;
        float vs = std::max(8.f, avail * 0.8f * ts);
        float us = vs * 0.42f;
        float total = Measure(val, Fmt(vs, 1)) + (unit.empty() ? 0 : us * 0.3f + Measure(unit, Fmt(us, 0)));
        if (total > rw - 2 * p) { float k = (rw - 2 * p) / total * 0.97f; vs *= k; us *= k; }
        float vy = top + (avail - vs * 1.25f) / 2;
        float vw = Measure(val, Fmt(vs, 1));
        Text(val, R(r.left + p, vy, vw + 4, vs * 1.25f), Fmt(vs, 1), vc);
        if (!unit.empty()) Text(unit, R(r.left + p + vw + us * 0.3f, vy + (vs - us) * 0.62f, rw, us * 1.3f), Fmt(us, 0), kInk2);
        break;
    }
    case panel::WType::Bar: {
        float ls = std::clamp(rh * 0.22f, 8.f, 64.f) * ts;
        labelSize = ls;
        std::wstring vu = val + (unit.empty() ? L"" : L" " + unit);
        float vs = FitSize(vu, 1, ls * 1.15f, rw - 2 * p);
        float vw = Measure(vu, Fmt(vs, 1));
        if (vw < (rw - 2 * p) * 0.62f) drawLabel(r.top + p, rw - 2 * p - vw - p);   // no room: value only
        Text(vu, R(r.right - p - vw - 2, r.top + p - ls * 0.05f, vw + 2, ls * 1.4f), Fmt(vs, 1, 2), vc);
        float th = std::clamp(rh * 0.2f, 4.f * s, 48.f * s);
        float by = r.bottom - p - th;
        D2D1_RECT_F track = D2D1::RectF(r.left + p, by, r.right - p, by + th);
        Fill(track, kSurface3, th / 2, 0.9f);
        if (frac > 0) Fill(D2D1::RectF(track.left, by, track.left + float((track.right - track.left) * frac), by + th), vc, th / 2);
        for (double t : {warn, crit}) {
            if (std::isnan(t) || t <= vmin || t >= vmax) continue;
            float tx = track.left + float((t - vmin) / (vmax - vmin)) * (track.right - track.left);
            Line(tx, by - th * 0.4f, tx, by + th * 1.4f, t == crit ? kCrit : kWarn, std::max(1.f, 1.5f * s));
        }
        break;
    }
    case panel::WType::Gauge: {
        float labelH = w.showLabel && !label.empty() ? std::clamp(rh * 0.13f, 8.f, 60.f) * ts * 1.4f : 0;
        float availH = rh - 2 * p - labelH;
        float rad = std::min((rw - 2 * p) / 2, availH / 1.55f);
        float thick = rad * 0.15f;
        rad -= thick / 2;
        float cx = (r.left + r.right) / 2, cy = r.top + p + thick / 2 + rad + (availH - (rad + thick / 2) * 1.55f) / 2;
        DrawArc(cx, cy, rad, 150, 390, kSurface3, thick, 0.9f);
        DrawArc(cx, cy, rad, 150, 150 + 240 * float(frac), vc, thick);
        for (double t : {warn, crit}) {
            if (std::isnan(t) || t <= vmin || t >= vmax) continue;
            float a = (150 + 240 * float((t - vmin) / (vmax - vmin))) * 3.14159265f / 180.f;
            Line(cx + (rad - thick * 0.9f) * std::cos(a), cy + (rad - thick * 0.9f) * std::sin(a), cx + (rad + thick * 0.9f) * std::cos(a),
                 cy + (rad + thick * 0.9f) * std::sin(a), t == crit ? kCrit : kWarn, std::max(1.f, 2 * s));
        }
        float vs = FitSize(val, 1, rad * 0.55f * ts, rad * 1.35f);
        Text(val, R(cx - rad, cy - vs * 0.75f, rad * 2, vs * 1.25f), Fmt(vs, 1, 1), kInk);
        if (!unit.empty()) Text(unit, R(cx - rad, cy + vs * 0.45f, rad * 2, vs * 0.6f), Fmt(vs * 0.36f, 0, 1), kInk2);
        if (labelH > 0) {
            float ls = labelH / 1.4f;
            std::wstring lt = (state > 0 ? L"⚠ " : L"") + label;
            Text(lt, R(r.left + p, r.bottom - p - labelH, rw - 2 * p, labelH), Fmt(FitSize(lt, 1, ls, rw - 2 * p), 1, 1), state ? vc : kInk2);
        }
        break;
    }
    case panel::WType::Graph: {
        float ls = std::clamp(rh * 0.13f, 8.f, 60.f) * ts;
        labelSize = ls;
        std::wstring vu = val + (unit.empty() ? L"" : L" " + unit);
        float vs = FitSize(vu, 1, ls * 1.25f, rw - 2 * p);
        float vw = Measure(vu, Fmt(vs, 1));
        if (vw < (rw - 2 * p) * 0.62f) drawLabel(r.top + p, rw - 2 * p - vw - p);   // no room: value only
        Text(vu, R(r.right - p - vw - 2, r.top + p - ls * 0.1f, vw + 2, ls * 1.5f), Fmt(vs, 1, 2), vc);
        D2D1_RECT_F pr = D2D1::RectF(r.left + p, r.top + p + ls * 1.7f, r.right - p, r.bottom - p);
        if (pr.bottom - pr.top < 8) break;
        double now = NowSec(), win = w.graphSeconds;
        auto it = g_ghist.find(w.sensor);
        double gmax = !std::isnan(w.max) ? w.max : 0, gmin = !std::isnan(w.min) ? w.min : 0;
        if (it != g_ghist.end() && std::isnan(w.max)) {
            for (auto& pt : it->second) if (pt.first >= now - win) gmax = std::max(gmax, double(pt.second));
            gmax = gmax > 0 ? gmax * 1.15 : (sen ? sen->rangeMax : 1);
        }
        if (gmax <= gmin) gmax = gmin + 1;
        Line(pr.left, pr.bottom, pr.right, pr.bottom, kAxis, std::max(1.f, s));
        for (double t : {warn, crit}) {
            if (std::isnan(t) || t <= gmin || t >= gmax) continue;
            float ty = pr.bottom - float((t - gmin) / (gmax - gmin)) * (pr.bottom - pr.top);
            Line(pr.left, ty, pr.right, ty, t == crit ? kCrit : kWarn, std::max(1.f, s), true, 0.8f);
        }
        if (it == g_ghist.end() || it->second.size() < 2) break;
        auto X = [&](double t) { return pr.left + float((t - (now - win)) / win) * (pr.right - pr.left); };
        auto Y = [&](double vv) { return pr.bottom - float(std::clamp((vv - gmin) / (gmax - gmin), 0.0, 1.0)) * (pr.bottom - pr.top); };
        ID2D1PathGeometry *area = nullptr, *line = nullptr;
        g_d2d->CreatePathGeometry(&area);
        g_d2d->CreatePathGeometry(&line);
        ID2D1GeometrySink *sa = nullptr, *sl = nullptr;
        area->Open(&sa);
        line->Open(&sl);
        bool started = false;
        float lastX = pr.left;
        for (auto& pt : it->second) {
            if (pt.first < now - win - 1) continue;
            D2D1_POINT_2F q = D2D1::Point2F(std::max(pr.left, X(pt.first)), Y(pt.second));
            if (!started) {
                sa->BeginFigure(D2D1::Point2F(q.x, pr.bottom), D2D1_FIGURE_BEGIN_FILLED);
                sa->AddLine(q);
                sl->BeginFigure(q, D2D1_FIGURE_BEGIN_HOLLOW);
                started = true;
            } else {
                sa->AddLine(q);
                sl->AddLine(q);
            }
            lastX = q.x;
        }
        if (started) {
            sa->AddLine(D2D1::Point2F(lastX, pr.bottom));
            sa->EndFigure(D2D1_FIGURE_END_CLOSED);
            sl->EndFigure(D2D1_FIGURE_END_OPEN);
        }
        sa->Close();
        sl->Close();
        sa->Release();
        sl->Release();
        if (started) {
            g_br->SetColor(Col(accent, 0.18f));
            g_rt->FillGeometry(area, g_br);
            g_br->SetColor(Col(accent));
            g_rt->DrawGeometry(line, g_br, std::max(1.5f, 2.f * s), g_round);
        }
        area->Release();
        line->Release();
        break;
    }
    case panel::WType::Pins: {
        const auto& sn = g_snap;
        const auto& c = sn.cfg;
        float ls = std::clamp(rh * 0.075f, 8.f, 40.f) * ts;
        labelSize = ls;
        float top = r.top + p + (w.showLabel ? drawLabel(r.top + p, rw - 2 * p) : 0);
        double vals[6];
        for (int i = 0; i < 6; ++i) vals[i] = w.pinMode == 0 ? sn.amps[i] : w.pinMode == 1 ? sn.amps[i] * sn.volts[i] : sn.volts[i];
        double pmin = !std::isnan(w.min) ? w.min : w.pinMode == 2 ? 11.0 : 0.0;
        double pmax = !std::isnan(w.max) ? w.max : w.pinMode == 0 ? std::max(10.0, c.limitA + 0.8) : w.pinMode == 1 ? c.limitA * 13 : 12.75;
        const wchar_t* pu = w.pinMode == 0 ? L"A" : w.pinMode == 1 ? L"W" : L"V";
        float slot = (rw - 2 * p) / 6;
        float bw = slot * 0.55f;
        float vs = std::min(ls * 1.05f, slot * 0.28f) ;
        float pinLab = vs * 1.4f;
        float valH = vs * 1.4f;
        float bTop = top + valH, bBot = r.bottom - p - pinLab;
        if (bBot - bTop < 6) break;
        for (int i = 0; i < 6; ++i) {
            float cx = r.left + p + slot * (i + 0.5f);
            D2D1_RECT_F tr = D2D1::RectF(cx - bw / 2, bTop, cx + bw / 2, bBot);
            float rr = std::min(bw / 2, 4.f * s);
            Fill(tr, kSurface3, rr, 0.8f);
            double f = sn.pinsOk ? std::clamp((vals[i] - pmin) / (pmax - pmin), 0.0, 1.0) : 0;
            uint32_t bc = w.pinMode == 2 ? accent : sn.pinState[i] == 2 ? kCrit : sn.pinState[i] == 1 ? kWarn : accent;
            if (f > 0) Fill(D2D1::RectF(tr.left, bBot - float(f) * (bBot - bTop), tr.right, bBot), bc, rr);
            std::wstring tv = sn.pinsOk ? FmtNum(vals[i], w.decimals >= 0 ? w.decimals : w.pinMode == 1 ? 0 : 2) : L"—";
            if (w.showUnit) tv += pu;
            Text(tv, R(cx - slot / 2, top, slot, valH), Fmt(FitSize(tv, 1, vs, slot * 0.95f), 1, 1), kInk);
            Text(F(L"P%d", i + 1), R(cx - slot / 2, bBot, slot, pinLab), Fmt(vs * 0.9f, 1, 1), kInk2);
        }
        if (w.pinMode != 2) {
            double lim = w.pinMode == 0 ? c.limitA : c.limitA * 12.0;
            if (lim > pmin && lim < pmax) {
                float ly = bBot - float((lim - pmin) / (pmax - pmin)) * (bBot - bTop);
                Line(r.left + p, ly, r.right - p, ly, kCrit, std::max(1.f, s), true, 0.8f);
            }
        }
        break;
    }
    case panel::WType::Clock: {
        SYSTEMTIME st;
        GetLocalTime(&st);
        std::wstring t = F(L"%02u:%02u", st.wHour, st.wMinute);
        static const wchar_t* days[] = {L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat"};
        static const wchar_t* months[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
        std::wstring d = F(L"%s %u %s", days[st.wDayOfWeek], st.wDay, months[(st.wMonth + 11) % 12]);
        float dateH = w.showLabel ? rh * 0.2f * ts : 0;
        float ts2 = FitSize(t, 1, (rh - 2 * p - dateH) * 0.8f * ts, rw - 2 * p);
        float blockH = ts2 * 1.25f + dateH;
        float y0 = r.top + (rh - blockH) / 2;
        Text(t, R(r.left, y0, rw, ts2 * 1.25f), Fmt(ts2, 1, 1), accent);
        if (dateH > 0) Text(d, R(r.left, y0 + ts2 * 1.2f, rw, dateH), Fmt(FitSize(d, 0, dateH * 0.75f, rw - 2 * p), 0, 1), kInk2);
        break;
    }
    case panel::WType::Text: {
        std::wstring t = w.text.empty() ? L"Text" : w.text;
        float sz = FitSize(t, 2, (rh - 2 * p) * 0.6f * ts, rw - 2 * p);
        Text(t, D2D1::RectF(r.left + p, r.top, r.right - p, r.bottom), Fmt(sz, 2, 1), accent);
        break;
    }
    default: break;
    }
}

D2D1_RECT_F WidgetRect(const panel::Layout& L, const panel::Widget& w, float W, float H) {
    float s = std::min(W / L.width, H / L.height);
    float gap = L.gap * s;
    float cw = (W - gap) / L.cols, ch = (H - gap) / L.rows;
    return D2D1::RectF(w.x * cw + gap, w.y * ch + gap, (w.x + w.w) * cw, (w.y + w.h) * ch);
}

void RenderPanel(const panel::Layout& L, float W, float H, bool edit) {
    float s = std::min(W / L.width, H / L.height);
    Fill(D2D1::RectF(0, 0, W, H), L.bgColor);
    if (ID2D1Bitmap* bmp = BackgroundBitmap(L)) {
        auto bs = bmp->GetSize();
        float k = std::max(W / bs.width, H / bs.height);   // cover
        float dw = bs.width * k, dh = bs.height * k;
        g_rt->PushAxisAlignedClip(D2D1::RectF(0, 0, W, H), D2D1_ANTIALIAS_MODE_ALIASED);
        g_rt->DrawBitmap(bmp, D2D1::RectF((W - dw) / 2, (H - dh) / 2, (W + dw) / 2, (H + dh) / 2), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        g_rt->PopAxisAlignedClip();
        if (L.bgDim > 0) Fill(D2D1::RectF(0, 0, W, H), 0x000000, 0, L.bgDim / 100.f);
    }
    if (edit) {
        float gap = L.gap * s;
        float cw = (W - gap) / L.cols, ch = (H - gap) / L.rows;
        for (int x = 0; x < L.cols; ++x)
            for (int y = 0; y < L.rows; ++y)
                Stroke(D2D1::RectF(x * cw + gap, y * ch + gap, (x + 1) * cw, (y + 1) * ch), kInk, L.radius * s, 1.f / g_previewScale, 0.07f);
    }
    for (auto& w : L.widgets) {
        D2D1_RECT_F r = WidgetRect(L, w, W, H);
        g_rt->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        DrawWidget(w, r, s, L.radius * s);
        g_rt->PopAxisAlignedClip();
    }
    if (edit && g_sel >= 0 && g_sel < int(L.widgets.size())) {
        D2D1_RECT_F r = WidgetRect(L, L.widgets[size_t(g_sel)], W, H);
        float lw = 2.f / g_previewScale;
        Stroke(r, kAccent, L.radius * s, lw);
        float hs = 14.f / g_previewScale;
        Fill(D2D1::RectF(r.right - hs, r.bottom - hs, r.right, r.bottom), kAccent, 3.f / g_previewScale);
    }
}

// Connector alarm drawn over a panel surface (case display, virtual panel, tablet frame): the whole panel
// pulses red with a big banner naming the pin, so it can't be missed even while playing.
void DrawAlarmOverlay(float W, float H) {
    int a = AlarmActive();
    if (!a || (a == 2 && !g_alarmPanels)) return;
    bool on = (GetTickCount64() / 500) % 2 == 0;
    D2D1_RECT_F all = D2D1::RectF(0, 0, W, H);
    Fill(all, kCrit, 0, on ? 0.42f : 0.16f);
    float m = std::min(W, H);
    Stroke(all, kCrit, 0, std::max(4.f, m * 0.03f), on ? 1.f : 0.55f);
    std::wstring title, detail, hint;
    if (a == 2) {
        int worst = -1;
        for (int i = 0; i < g_snap.pinCount && i < pins::kMaxPins; ++i)
            if (g_snap.pinState[i] == 2 && (worst < 0 || g_snap.amps[i] > g_snap.amps[worst])) worst = i;
        if (worst < 0) worst = 0;   // (AlarmActive found one; keeps the analyzer and any race safe)
        int n = 0;
        for (int i = 0; i < g_snap.pinCount && i < pins::kMaxPins; ++i) n += g_snap.pinState[i] == 2;
        title = L"⚠ 12V-2x6 OVER LIMIT";
        detail = worst < 6 ? F(L"Pin %d: %.2f A  (limit %.1f A)", worst + 1, g_snap.amps[worst], g_snap.cfg.limitA)
                           : F(L"Connector 2 pin %d: %.2f A  (limit %.1f A)", worst - 5, g_snap.amps[worst], g_snap.cfg.limitA);
        if (n > 1) detail += F(L"  ·  %d pins over", n);
        hint = L"Lower the GPU load now — then check the connector";
    } else {
        title = L"TEST ALARM";
        detail = L"This is how a connector alarm looks on this screen";
        hint = L"It disappears by itself in a few seconds";
    }
    float bw = W * 0.86f, bh = std::min(H * 0.8f, std::max(H * 0.5f, bw * 0.28f));
    D2D1_RECT_F box = D2D1::RectF((W - bw) / 2, (H - bh) / 2, (W + bw) / 2, (H + bh) / 2);
    float rad = m * 0.03f;
    Fill(box, 0x1A0606, rad, 0.93f);
    Stroke(box, kCrit, rad, std::max(2.f, m * 0.008f));
    float tw = bw * 0.9f;
    float ts = FitSize(title, 2, bh * 0.30f, tw), ds = FitSize(detail, 1, bh * 0.16f, tw), hs = FitSize(hint, 0, bh * 0.11f, tw);
    float total = ts * 1.25f + ds * 1.3f + hs * 1.3f;
    float y = box.top + (bh - total) / 2;
    Text(title, D2D1::RectF(box.left, y, box.right, y + ts * 1.25f), Fmt(ts, 2, 1), on ? 0xFFFFFF : 0xFFB4B4);
    y += ts * 1.25f;
    Text(detail, D2D1::RectF(box.left, y, box.right, y + ds * 1.3f), Fmt(ds, 1, 1), 0xFFFFFF);
    y += ds * 1.3f;
    Text(hint, D2D1::RectF(box.left, y, box.right, y + hs * 1.3f), Fmt(hs, 0, 1), 0xFFD0D0);
}

// ---------- layouts (save slots) ----------
void CommitFocus();
void ShowPanel(Slot& s, bool on);
bool IsTabletSlot(const Slot& s);   // tablet_ui.inc

std::wstring DisplayShort(const std::wstring& dev) {
    int i = MonitorIndex(dev);
    if (i < 0) return L"display not connected";
    return F(L"Display %d", i + 1);
}
// Where a layout is visible right now ("Display 2", "window", "tablet", ...)
std::wstring SlotWhere(const Slot& s) {
    std::wstring w;
    if (s.wnd) w = s.virt ? L"window" : DisplayShort(LayoutOf(s).display);
    if (IsTabletSlot(s)) w += (w.empty() ? L"" : L" + ") + std::wstring(L"tablet");
    return w;
}

void RememberEditSlot() {
    WritePrivateProfileStringW(L"Panels", L"Edit", std::to_wstring(EditSlot().id).c_str(), g_ini.c_str());
}

void SelectSlot(int idx) {
    if (idx < 0 || idx >= int(g_slots.size()) || idx == g_edit) return;
    CommitFocus();
    g_focus = 0;
    SaveLayout();                       // the old one is stored as it is
    g_edit = idx;
    g_layout = EditSlot().layout;
    g_sel = -1;
    RememberEditSlot();
}

std::wstring SlotPath(int id) { return panel::LayoutsDir() + F(L"\\panel-%d.txt", id); }

// Loads layouts\panel-*.txt; the first start after updating moves the old single panel.txt in as "My panel".
void LoadSlots() {
    std::wstring dir = panel::LayoutsDir();
    CreateDirectoryW(dir.c_str(), nullptr);
    std::vector<int> ids;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\panel-*.txt").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            int id = _wtoi(fd.cFileName + 6);
            if (id > 0 && !_wcsicmp(fd.cFileName, F(L"panel-%d.txt", id).c_str())) ids.push_back(id);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(ids.begin(), ids.end());
    for (int id : ids) {
        auto s = std::make_unique<Slot>();
        s->id = id;
        s->path = SlotPath(id);
        if (!panel::Load(s->path, s->layout)) continue;
        if (s->layout.name.empty()) s->layout.name = F(L"Layout %d", id);
        g_slots.push_back(std::move(s));
        if (g_slots.size() >= kMaxSlots) break;
    }
    if (g_slots.empty()) {
        auto s = std::make_unique<Slot>();
        s->id = 1;
        s->path = SlotPath(1);
        if (!panel::Load(panel::DefaultPath(), s->layout)) {   // panel.txt from 1.3 and older stays as a backup
            s->layout = panel::DefaultLayout();
            s->layout.display = L"virtual";
        }
        if (s->layout.name.empty()) s->layout.name = L"My panel";
        panel::Save(s->path, s->layout);
        g_slots.push_back(std::move(s));
    }
    int editId = int(GetPrivateProfileIntW(L"Panels", L"Edit", g_slots[0]->id, g_ini.c_str()));
    g_edit = std::max(0, SlotIndex(SlotById(editId)));
    g_layout = EditSlot().layout;
}

// Ids are never reused (a deleted layout's id could still be remembered, e.g. as the tablet's layout).
int NextSlotId() {
    int id = int(GetPrivateProfileIntW(L"Panels", L"NextId", 1, g_ini.c_str()));
    for (auto& s : g_slots) id = std::max(id, s->id + 1);
    return id;
}

// New layout (empty starter or a copy of the current one) - it opens in the editor.
void AddSlot(const panel::Layout* copyOf, const std::wstring& name) {
    if (g_slots.size() >= kMaxSlots) {
        MessageBoxW(g_hwnd, F(L"You can have up to %zu layouts. Delete one first.", kMaxSlots).c_str(), L"RigSense", MB_ICONINFORMATION);
        return;
    }
    SaveLayout();
    auto s = std::make_unique<Slot>();
    s->id = NextSlotId();
    s->path = SlotPath(s->id);
    WritePrivateProfileStringW(L"Panels", L"NextId", std::to_wstring(s->id + 1).c_str(), g_ini.c_str());
    if (copyOf) s->layout = *copyOf;
    else {
        s->layout = panel::DefaultLayout();
        s->layout.width = g_layout.width;     // same screen size as the current one: usually what you want
        s->layout.height = g_layout.height;
        int c, r;
        panel::AutoGrid(s->layout.width, s->layout.height, c, r);
        panel::Regrid(s->layout, c, r);
    }
    s->layout.name = name;
    s->layout.display = L"virtual";           // never two layouts fullscreen on one display
    s->layout.autoShow = false;
    panel::Save(s->path, s->layout);
    g_slots.push_back(std::move(s));
    SelectSlot(int(g_slots.size()) - 1);
}

void DeleteEditSlot() {
    if (g_slots.size() <= 1) {
        MessageBoxW(g_hwnd, L"This is your only layout - it can't be deleted. Use “Reset layout” to start over.", L"RigSense", MB_ICONINFORMATION);
        return;
    }
    Slot& s = EditSlot();
    if (MessageBoxW(g_hwnd, (L"Delete the layout “" + SlotName(s) + L"”?\n\nIt is moved to layouts\\deleted, so you can still get it back.").c_str(),
                    L"RigSense", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return;
    CommitFocus();
    g_focus = 0;
    ShowPanel(s, false);
    std::wstring bin = panel::LayoutsDir() + L"\\deleted";
    CreateDirectoryW(bin.c_str(), nullptr);
    SYSTEMTIME t;
    GetLocalTime(&t);
    MoveFileExW(s.path.c_str(), (bin + F(L"\\panel-%d-%04u%02u%02u-%02u%02u%02u.txt", s.id, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond)).c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
    g_slots.erase(g_slots.begin() + g_edit);
    g_edit = std::min(g_edit, int(g_slots.size()) - 1);
    g_layout = EditSlot().layout;
    g_sel = -1;
    RememberEditSlot();
}

// Puts a layout fullscreen on a display (or back to "virtual"). Another layout that was on that display
// moves back to a window, so two layouts never fight over one screen.
void AssignDisplay(Slot& s, const std::wstring& dev) {
    if (dev != L"virtual" && !dev.empty()) {
        for (auto& o : g_slots) {
            if (o.get() == &s || LayoutOf(*o).display != dev) continue;
            bool wasOpen = o->wnd != nullptr;
            ShowPanel(*o, false);
            MutLayout(*o).display = L"virtual";
            SaveSlot(*o);
            if (wasOpen) ShowPanel(*o, true);
        }
    }
    bool open = s.wnd != nullptr;
    ShowPanel(s, false);
    MutLayout(s).display = dev;
    SaveSlot(s);
    if (open) ShowPanel(s, true);
}

// ---------- editor controls ----------
void CommitFocus() {
    if (!g_focus) return;
    if (g_focus == F_SEARCH) g_search = g_editBuf;
    else { g_commitId = g_focus; g_commitText = g_editBuf; }
    g_focus = 0;
}
void FocusField(int id) {
    if (g_focus == id) return;
    CommitFocus();
    g_focus = id;
    g_editBuf = id == F_SEARCH ? g_search : g_fieldInit[id];
}

bool TextField(const D2D1_RECT_F& r, int id, const std::wstring& value, const std::wstring& placeholder, std::wstring& out) {
    bool f = g_focus == id;
    Fill(r, kBg, 6);
    Stroke(r, f ? kAccent : Hover(r) ? kInk2 : kBorder, 6, 1.f, f ? 1.f : 0.8f);
    D2D1_RECT_F in = D2D1::RectF(r.left + 8, r.top, r.right - 6, r.bottom);
    std::wstring shown = f ? g_editBuf : value;
    if (shown.empty() && !f) Text(placeholder, in, Fmt(12.5f), kMuted);
    else Text(shown + (f ? L"│" : L""), in, Fmt(12.5f), kInk);
    g_fieldInit[id] = value;
    g_hits.push_back({r, HIT_FIELD + id});
    if (g_commitId == id) { out = g_commitText; g_commitId = 0; return true; }
    return false;
}

void Btn(const D2D1_RECT_F& r, const std::wstring& label, bool active, std::function<void()> fn, float fs = 12.5f) {
    bool hv = Hover(r);
    Fill(r, active ? kSurface3 : hv ? kSurface2 : kBg, 6);
    Stroke(r, active ? kAccent : kBorder, 6, 1.f, active ? 0.9f : 1.f);
    Text(label, r, Fmt(fs, 1, 1), active ? kAccent : hv ? kInk : kInk2);
    g_hits.push_back({r, 0, std::move(fn)});
}

void Swatch(const D2D1_RECT_F& r, uint32_t color, bool active, std::function<void()> fn) {
    Fill(r, color, 6);
    Stroke(r, active ? kInk : kBorder, 6, active ? 2.f : 1.f);
    g_hits.push_back({r, 0, std::move(fn)});
}

void Stepper(float x, float y, float h, const std::wstring& value, std::function<void()> minus, std::function<void()> plus) {
    Btn(R(x, y, 28, h), L"−", false, std::move(minus), 14);
    Text(value, R(x + 30, y, 56, h), Fmt(12.5f, 1, 1), kInk);
    Btn(R(x + 88, y, 28, h), L"+", false, std::move(plus), 14);
}

double ParseField(const std::wstring& s) {
    std::wstring t = s;
    std::replace(t.begin(), t.end(), L',', L'.');
    wchar_t* e = nullptr;
    double v = wcstod(t.c_str(), &e);
    return (t.empty() || e == t.c_str()) ? NAN : v;
}
std::wstring NumStr(double v) { return std::isnan(v) ? L"" : F(L"%g", v); }

void PanelFullMessage() {
    MessageBoxW(g_hwnd,
                L"There is no free space left on the panel.\n\nMake room first: select a widget and make it smaller (drag its corner), "
                L"delete one, or add columns/rows with the Grid buttons.",
                L"RigSense", MB_ICONINFORMATION);
}

void AddWidget(panel::WType t, const std::string& sensor = "") {
    panel::Widget w;
    w.type = t;
    static const int sz[][2] = {{3, 2}, {4, 1}, {3, 3}, {4, 2}, {5, 3}, {3, 2}, {4, 1}};
    w.w = std::min(sz[int(t)][0], g_layout.cols);
    w.h = std::min(sz[int(t)][1], g_layout.rows);
    if (!panel::PlaceWidget(g_layout, w)) { PanelFullMessage(); return; }
    if (!sensor.empty()) w.sensor = sensor;
    else if (g_sel >= 0 && g_sel < int(g_layout.widgets.size())) w.sensor = g_layout.widgets[size_t(g_sel)].sensor;
    if (t == panel::WType::Text) w.text = L"MY RIG";
    g_layout.widgets.push_back(w);
    g_sel = int(g_layout.widgets.size()) - 1;
    SaveLayout();
}

void OpenTabletDialog();
bool TabletLastSize(int& w, int& h);   // tablet_ui.inc

// Design the panel for a screen: adopt its resolution, rescale the grid proportionally (explicit user choice).
void DesignFor(int w, int h) {
    g_layout.width = w;
    g_layout.height = h;
    int c, r;
    panel::AutoGrid(w, h, c, r);
    panel::Regrid(g_layout, c, r);
    SaveLayout();
}

// "Detect screens" menu: every connected display and the last tablet/phone, with resolution + aspect ratio.
void ShowDetectMenu() {
    EnumMonitors();
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Design the panel for\u2026");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    for (size_t i = 0; i < g_mons.size(); ++i) AppendMenuW(m, MF_STRING, UINT(100 + i), MonitorLabel(int(i)).c_str());
    int tw = 0, th = 0;
    if (TabletLastSize(tw, th))
        AppendMenuW(m, MF_STRING, 50, F(L"Tablet / phone \u00b7 %d\u00d7%d (%s)", tw, th, panel::AspectLabel(tw, th).c_str()).c_str());
    else
        AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Tablet / phone \u00b7 open the panel on it once to detect its size");
    POINT pt;
    GetCursorPos(&pt);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(m);
    if (cmd == 50 && TabletLastSize(tw, th)) DesignFor(tw, th);
    else if (cmd >= 100 && cmd < 100 + int(g_mons.size())) {
        const auto& mo = g_mons[size_t(cmd - 100)];
        DesignFor(mo.rc.right - mo.rc.left, mo.rc.bottom - mo.rc.top);
        if (!mo.primary) {   // a separate display: also show this layout there
            AssignDisplay(EditSlot(), mo.dev);
            ShowPanel(EditSlot(), true);
        }
    }
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

void ExportPanel() {
    wchar_t file[MAX_PATH] = L"my-panel.rigpanel";
    OPENFILENAMEW of{sizeof of};
    of.hwndOwner = g_hwnd;
    of.lpstrFilter = L"RigSense panel (*.rigpanel)\0*.rigpanel\0";
    of.lpstrDefExt = L"rigpanel";
    of.lpstrFile = file;
    of.nMaxFile = MAX_PATH;
    of.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&of)) return;
    if (panel::Export(file, g_layout))
        MessageBoxW(g_hwnd, (std::wstring(L"Panel exported to\n") + file +
                             L"\n\nShare this file; others can load it with “Import panel”. The background image is included.\n"
                             L"Note: HWiNFO / LibreHardwareMonitor sensors only match on PCs with the same hardware.").c_str(),
                    L"RigSense", MB_ICONINFORMATION);
    else MessageBoxW(g_hwnd, L"Could not write the file.", L"RigSense", MB_ICONERROR);
}

void ImportPanel() {
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW of{sizeof of};
    of.hwndOwner = g_hwnd;
    of.lpstrFilter = L"RigSense panel (*.rigpanel)\0*.rigpanel\0Layout text (*.txt)\0*.txt\0All files\0*.*\0";
    of.lpstrFile = file;
    of.nMaxFile = MAX_PATH;
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&of)) return;
    panel::Layout l;
    std::wstring err;
    if (!panel::Import(file, l, mon::ExeDir() + L"\\panel_images", err)) {
        MessageBoxW(g_hwnd, err.c_str(), L"RigSense", MB_ICONERROR);
        return;
    }
    int missing = 0;
    for (auto& w : l.widgets)
        if (w.type != panel::WType::Clock && w.type != panel::WType::Text && w.type != panel::WType::Pins && !g_reg.Find(w.sensor)) ++missing;
    // An import never overwrites anything: it becomes a new layout, named after the file if it has no name.
    std::wstring name = l.name;
    if (name.empty()) {
        std::wstring f = file;
        size_t sl = f.find_last_of(L"\\/");
        size_t from = sl == std::wstring::npos ? 0 : sl + 1, dot = f.find_last_of(L'.');
        name = f.substr(from, dot != std::wstring::npos && dot > from ? dot - from : std::wstring::npos);
    }
    size_t before = g_slots.size();
    AddSlot(&l, name.substr(0, 40));
    if (g_slots.size() == before) return;
    if (missing)
        MessageBoxW(g_hwnd, F(L"Imported as a new layout. %d widget(s) use sensors this PC doesn't have (or whose source isn't running) - select them and pick a sensor from the list.", missing).c_str(),
                    L"RigSense", MB_ICONINFORMATION);
}

// The layout strip above the preview: one chip per layout (where it's showing), New / Duplicate / Delete, and the name.
float DrawLayoutStrip(float x, float y, float w) {
    const float h = 30;
    std::wstring tmp;
    Anchor("layouts", R(x - 6, y - 4, w + 12, h + 8));
    Text(L"Layouts", R(x, y, 70, h), Fmt(13, 1), kInk2);
    float right = x + w;
    // right side: name field + actions
    float bx = right - 76;
    Btn(R(bx, y, 76, h), L"Delete", false, [] { DeleteEditSlot(); });
    bx -= 92;
    Btn(R(bx, y, 86, h), L"Duplicate", false, [] {
        panel::Layout c = g_layout;
        AddSlot(&c, (SlotName(EditSlot()) + L" copy").substr(0, 40));
    });
    bx -= 76;
    Btn(R(bx, y, 70, h), L"+ New", true, [] { AddSlot(nullptr, F(L"Layout %d", NextSlotId())); });
    bx -= 206;
    if (TextField(R(bx, y, 200, h), F_SLOTNAME, g_layout.name, L"Layout name", tmp)) {
        while (!tmp.empty() && tmp.back() == L' ') tmp.pop_back();
        if (!tmp.empty()) { g_layout.name = tmp.substr(0, 40); SaveLayout(); }
    }
    Text(L"Name", R(bx - 46, y, 42, h), Fmt(12.5f, 1, 2), kInk2);
    // chips
    float cx = x + 74, avail = bx - 56 - cx;
    size_t n = g_slots.size();
    float chipW = std::clamp((avail - 6 * float(n - 1)) / float(n), 70.f, 230.f);
    for (size_t i = 0; i < n; ++i) {
        const Slot& s = *g_slots[i];
        D2D1_RECT_F r = R(cx + float(i) * (chipW + 6), y, chipW, h);
        if (r.right > bx - 56) break;   // too many to show: the rest are reachable by deleting / via the list order
        bool act = int(i) == g_edit;
        bool hv = Hover(r);
        Fill(r, act ? kSurface3 : hv ? kSurface2 : kBg, 7);
        Stroke(r, act ? kAccent : kBorder, 7, 1.f, act ? 0.9f : 1.f);
        std::wstring where = SlotWhere(s);
        bool live = !where.empty();
        Circle(r.left + 11, (r.top + r.bottom) / 2, 3.5f, live ? kGood : kMuted);
        std::wstring label = SlotName(s);
        float nameW = chipW - 24;
        if (live) {
            float ww = std::min(Measure(where, Fmt(11)) + 8, chipW * 0.45f);
            Text(where, D2D1::RectF(r.right - ww - 6, r.top, r.right - 6, r.bottom), Fmt(11, 0, 2), kMuted);
            nameW -= ww + 4;
        }
        Text(label, R(r.left + 20, r.top, nameW, h), Fmt(FitSize(label, 1, 12.5f, nameW), 1), act ? kAccent : kInk);
        int idx = int(i);
        g_hits.push_back({r, 0, [idx] { SelectSlot(idx); }});
    }
    return y + h;
}

void DrawPanelEditor(float W, float H) {
    if (g_sel >= int(g_layout.widgets.size())) g_sel = -1;
    float pad = 16;
    float top = DrawLayoutStrip(pad, 48 + 10, W - 2 * pad) + 10;
    float lw = float(g_layout.width), lh = float(g_layout.height);
    float areaW = W - 2 * pad, maxH = (H - top - pad) * 0.40f;
    float pw = areaW, ph = pw * lh / lw;
    if (ph > maxH) { ph = maxH; pw = ph * lw / lh; }
    float px = pad + (areaW - pw) / 2, py = top;
    g_preview = R(px, py, pw, ph);
    Anchor("preview", g_preview);
    g_previewScale = pw / lw;
    D2D1_MATRIX_3X2_F old;
    g_rt->GetTransform(&old);
    g_rt->SetTransform(D2D1::Matrix3x2F::Scale(g_previewScale, g_previewScale) * D2D1::Matrix3x2F::Translation(px, py));
    g_rt->PushAxisAlignedClip(D2D1::RectF(0, 0, lw, lh), D2D1_ANTIALIAS_MODE_ALIASED);
    RenderPanel(g_layout, lw, lh, true);
    g_rt->PopAxisAlignedClip();
    g_rt->SetTransform(old);
    Stroke(D2D1::RectF(px - 1, py - 1, px + pw + 1, py + ph + 1), kBorder, 0);
    Text(F(L"Preview %d × %d px · grid %d × %d  —  click to select, drag to move, drag the corner to resize, Del removes, arrows nudge",
           g_layout.width, g_layout.height, g_layout.cols, g_layout.rows),
         R(pad, py + ph + 4, areaW, 22), Fmt(11.5f, 0, 1), kMuted);

    float cy0 = py + ph + 32, colH = H - pad - cy0;
    float c1 = std::floor((W - 4 * pad) * 0.28f), c2 = std::floor((W - 4 * pad) * 0.38f), c3 = W - 4 * pad - c1 - c2;
    float rowH = std::clamp((colH - 52) / 12.f, 26.f, 36.f), ctl = rowH - 6;
    std::wstring tmp;

    // ----- Panel column -----
    {
        D2D1_RECT_F P = R(pad, cy0, c1, colH);
        Panel(P);
        Anchor("panelcol", P);
        float x = P.left + 14, y = P.top + 10, w = c1 - 28;
        Text(L"Panel", R(x, y, w, 26), Fmt(15, 1), kInk);
        y += 32;
        static const int presets[][2] = {{1920, 480}, {1280, 480}, {800, 480}, {1024, 600}, {480, 320}, {2560, 720}, {1920, 720}};
        float bw = (w - 16) / 3;
        for (int i = 0; i < 9; ++i) {
            D2D1_RECT_F br = R(x + (i % 3) * (bw + 8), y + (i / 3) * rowH, bw, ctl);
            if (i == 7) {
                Btn(br, L"Detect screens \u25B8", false, [] { ShowDetectMenu(); });
                continue;
            }
            if (i == 8) {
                Btn(br, L"↻ Rotate", false, [] {
                    std::swap(g_layout.width, g_layout.height);
                    int c, r;
                    panel::AutoGrid(g_layout.width, g_layout.height, c, r);
                    panel::Regrid(g_layout, c, r);
                    SaveLayout();
                });
                continue;
            }
            int pw2 = presets[i][0], ph2 = presets[i][1];
            bool act = g_layout.width == pw2 && g_layout.height == ph2;
            Btn(br, F(L"%d×%d", pw2, ph2), act, [pw2, ph2] {
                g_layout.width = pw2;
                g_layout.height = ph2;
                int c, r;
                panel::AutoGrid(pw2, ph2, c, r);
                panel::Regrid(g_layout, c, r);
                SaveLayout();
            });
        }
        y += 3 * rowH;
        auto label = [&](const wchar_t* t) { Text(t, R(x, y, 90, ctl), Fmt(12.5f, 1), kInk2); return x + 92; };

        float cx = label(L"Size");
        if (TextField(R(cx, y, 70, ctl), F_WIDTH, std::to_wstring(g_layout.width), L"width", tmp)) {
            double v = ParseField(tmp);
            if (!std::isnan(v)) { g_layout.width = int(v); SaveLayout(); }
        }
        Text(L"×", R(cx + 72, y, 18, ctl), Fmt(13, 0, 1), kInk2);
        if (TextField(R(cx + 92, y, 70, ctl), F_HEIGHT, std::to_wstring(g_layout.height), L"height", tmp)) {
            double v = ParseField(tmp);
            if (!std::isnan(v)) { g_layout.height = int(v); SaveLayout(); }
        }
        Text(L"px \u00b7 " + panel::AspectLabel(g_layout.width, g_layout.height), R(cx + 166, y, w - 166 - 92, ctl), Fmt(12), kMuted);
        y += rowH;
        cx = label(L"Grid cols/rows");
        Stepper(cx, y, ctl, std::to_wstring(g_layout.cols), [] { panel::Regrid(g_layout, g_layout.cols - 1, g_layout.rows); SaveLayout(); },
                [] { panel::Regrid(g_layout, g_layout.cols + 1, g_layout.rows); SaveLayout(); });
        Stepper(cx + 124, y, ctl, std::to_wstring(g_layout.rows), [] { panel::Regrid(g_layout, g_layout.cols, g_layout.rows - 1); SaveLayout(); },
                [] { panel::Regrid(g_layout, g_layout.cols, g_layout.rows + 1); SaveLayout(); });
        y += rowH;
        cx = label(L"Gap / corners");
        Stepper(cx, y, ctl, F(L"%d", g_layout.gap), [] { g_layout.gap -= 2; SaveLayout(); }, [] { g_layout.gap += 2; SaveLayout(); });
        Stepper(cx + 124, y, ctl, F(L"%d", g_layout.radius), [] { g_layout.radius -= 2; SaveLayout(); }, [] { g_layout.radius += 2; SaveLayout(); });
        y += rowH;
        cx = label(L"Background");
        for (int i = 0; i < panel::kPanelBgCount; ++i) {
            uint32_t c = panel::PanelBgColor(i);
            Swatch(R(cx + i * (ctl + 6), y, ctl, ctl), c, g_layout.bgColor == c, [c] { g_layout.bgColor = c; SaveLayout(); });
        }
        y += rowH;
        cx = label(L"Image");
        Btn(R(cx, y, 84, ctl), L"Choose…", !g_layout.bgImage.empty(), [] {
            wchar_t file[MAX_PATH] = {};
            OPENFILENAMEW of{sizeof of};
            of.hwndOwner = g_hwnd;
            of.lpstrFilter = L"Images\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.webp\0All files\0*.*\0";
            of.lpstrFile = file;
            of.nMaxFile = MAX_PATH;
            of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameW(&of)) { g_layout.bgImage = file; SaveLayout(); }
        });
        Btn(R(cx + 90, y, 56, ctl), L"Clear", false, [] { g_layout.bgImage.clear(); SaveLayout(); });
        Text(L"dim", R(cx + 152, y, 30, ctl), Fmt(12), kMuted);
        Stepper(cx + 180, y, ctl, F(L"%d%%", g_layout.bgDim), [] { g_layout.bgDim -= 10; SaveLayout(); }, [] { g_layout.bgDim += 10; SaveLayout(); });
        y += rowH;
        cx = label(L"Show on");
        {
            // option 0 = virtual panel, then every non-main display
            std::vector<int> opts = {-1};
            for (size_t i = 0; i < g_mons.size(); ++i) if (!g_mons[i].primary) opts.push_back(int(i));
            int cur = 0;
            for (size_t o = 1; o < opts.size(); ++o) if (g_mons[size_t(opts[o])].dev == g_layout.display) cur = int(o);
            std::wstring dl = L"Virtual panel (window)";
            if (cur > 0) dl = MonitorLabel(opts[size_t(cur)]) + L" \u00b7 fullscreen";
            else if (!g_layout.display.empty() && g_layout.display != L"virtual") dl = L"Window until the saved display is connected";
            Btn(R(cx + w - 92 - 78, y, 78, ctl), L"Identify", false, [] { IdentifyDisplays(); });
            Btn(R(cx, y, w - 92 - 84, ctl), dl + L"  \u25B8", false, [opts, cur] {
                size_t n = size_t(cur + 1) % opts.size();
                AssignDisplay(EditSlot(), opts[n] < 0 ? std::wstring(L"virtual") : g_mons[size_t(opts[n])].dev);
            });
        }
        y += rowH;
        bool open = EditSlot().wnd != nullptr;
        Btn(R(x, y, (w - 8) / 2, ctl), open ? L"Close panel" : L"Open panel", open, [open] { ShowPanel(EditSlot(), !open); });
        Btn(R(x + (w - 8) / 2 + 8, y, (w - 8) / 2, ctl), g_layout.autoShow ? L"Open at start: on" : L"Open at start: off", g_layout.autoShow,
            [] { g_layout.autoShow = !g_layout.autoShow; SaveLayout(); });
        y += rowH;
        Btn(R(x, y, (w - 8) / 2, ctl), L"Reset layout", false, [] {
            auto d = panel::DefaultLayout();
            int c, r;
            panel::AutoGrid(g_layout.width, g_layout.height, c, r);
            d.width = g_layout.width; d.height = g_layout.height; d.display = g_layout.display; d.autoShow = g_layout.autoShow; d.name = g_layout.name;
            panel::Regrid(d, c, r);
            g_layout = d;
            g_sel = -1;
            SaveLayout();
        });
        Btn(R(x + (w - 8) / 2 + 8, y, (w - 8) / 2, ctl), L"Tablet / phone\u2026", false, [] { OpenTabletDialog(); });
        y += rowH;
        Btn(R(x, y, (w - 8) / 2, ctl), L"Export panel…", false, [] { ExportPanel(); });
        Btn(R(x + (w - 8) / 2 + 8, y, (w - 8) / 2, ctl), L"Import panel…", false, [] { ImportPanel(); });
    }

    // ----- Widget column -----
    {
        D2D1_RECT_F P = R(pad * 2 + c1, cy0, c2, colH);
        Panel(P);
        Anchor("widgetcol", P);
        float x = P.left + 14, y = P.top + 10, w = c2 - 28;
        Text(L"Widget", R(x, y, 90, 26), Fmt(15, 1), kInk);
        Text(L"Add:", R(x + 76, y, 40, 26), Fmt(12.5f, 1), kInk2);
        float abw = (w - 116 - 6 * 4) / 7;
        for (int t = 0; t < int(panel::WType::Count); ++t)
            Btn(R(x + 116 + t * (abw + 4), y, abw, 26), panel::TypeName(panel::WType(t)), false, [t] { AddWidget(panel::WType(t)); }, 11.5f);
        y += 36;
        if (g_sel < 0) {
            Text(L"Select a widget in the preview, add one with the buttons above,", R(x, y + 10, w, 20), Fmt(12.5f), kInk2);
            Text(L"or click a sensor on the right to add it as a Value widget.", R(x, y + 30, w, 20), Fmt(12.5f), kInk2);
        } else {
            auto& wd = g_layout.widgets[size_t(g_sel)];
            auto label = [&](const wchar_t* t) { Text(t, R(x, y, 86, ctl), Fmt(12.5f, 1), kInk2); return x + 88; };
            float cw = w - 88;
            float cx = label(L"Type");
            float tbw = (cw - 6 * 4) / 7;
            for (int t = 0; t < int(panel::WType::Count); ++t)
                Btn(R(cx + t * (tbw + 4), y, tbw, ctl), panel::TypeName(panel::WType(t)), int(wd.type) == t, [t] {
                    g_layout.widgets[size_t(g_sel)].type = panel::WType(t);
                    SaveLayout();
                }, 11.5f);
            y += rowH;
            cx = label(wd.type == panel::WType::Text ? L"Text" : L"Label");
            const sensors::Sensor* sen = g_reg.Find(wd.sensor);
            std::wstring ph = wd.type == panel::WType::Text ? L"Your text" : wd.type == panel::WType::Pins ? L"12V-2x6 pins"
                              : wd.type == panel::WType::Clock ? L"(not used)" : sen ? sen->name + L" (sensor name)" : L"(sensor name)";
            if (TextField(R(cx, y, cw, ctl), F_LABEL, wd.text, ph, tmp)) { wd.text = tmp; SaveLayout(); }
            y += rowH;
            cx = label(L"Color");
            for (int i = 0; i < panel::kColorCount; ++i)
                Swatch(R(cx + i * (ctl + 5), y, ctl, ctl), panel::AccentColor(i), wd.color == i, [i] { g_layout.widgets[size_t(g_sel)].color = i; SaveLayout(); });
            y += rowH;
            cx = label(L"Background");
            for (int b = 0; b < int(panel::Bg::Count); ++b)
                Btn(R(cx + b * 84, y, 80, ctl), panel::BgName(panel::Bg(b)), int(wd.bg) == b, [b] { g_layout.widgets[size_t(g_sel)].bg = panel::Bg(b); SaveLayout(); });
            y += rowH;
            cx = label(L"Text size");
            Stepper(cx, y, ctl, F(L"%d%%", wd.textScale), [] { g_layout.widgets[size_t(g_sel)].textScale -= 10; SaveLayout(); },
                    [] { g_layout.widgets[size_t(g_sel)].textScale += 10; SaveLayout(); });
            Text(F(L"size %d × %d cells", wd.w, wd.h), R(cx + 130, y, 160, ctl), Fmt(12), kMuted);
            y += rowH;
            cx = label(L"Range");
            if (TextField(R(cx, y, 90, ctl), F_MIN, NumStr(wd.min), L"min: 0", tmp)) { wd.min = ParseField(tmp); SaveLayout(); }
            Text(L"to", R(cx + 92, y, 24, ctl), Fmt(12, 0, 1), kMuted);
            if (TextField(R(cx + 118, y, 90, ctl), F_MAX, NumStr(wd.max), sen ? F(L"max: %g", std::round(sen->rangeMax * 10) / 10) : L"max: auto", tmp)) {
                wd.max = ParseField(tmp);
                SaveLayout();
            }
            Text(L"empty = auto", R(cx + 214, y, 100, ctl), Fmt(11.5f), kMuted);
            y += rowH;
            cx = label(L"Alerts");
            if (TextField(R(cx, y, 90, ctl), F_WARN, NumStr(wd.warn), sen && !std::isnan(sen->warn) ? F(L"warn: %g", sen->warn) : L"warn: none", tmp)) {
                wd.warn = ParseField(tmp);
                SaveLayout();
            }
            if (TextField(R(cx + 118, y, 90, ctl), F_CRIT, NumStr(wd.crit), sen && !std::isnan(sen->crit) ? F(L"crit: %g", sen->crit) : L"crit: none", tmp)) {
                wd.crit = ParseField(tmp);
                SaveLayout();
            }
            Text(L"turns amber / red", R(cx + 214, y, 120, ctl), Fmt(11.5f), kMuted);
            y += rowH;
            cx = label(L"Decimals");
            for (int d = -1; d <= 3; ++d)
                Btn(R(cx + (d + 1) * 50, y, 46, ctl), d < 0 ? std::wstring(L"Auto") : std::to_wstring(d), wd.decimals == d,
                    [d] { g_layout.widgets[size_t(g_sel)].decimals = d; SaveLayout(); });
            y += rowH;
            cx = label(L"Show");
            Btn(R(cx, y, 110, ctl), wd.type == panel::WType::Clock ? L"Date" : L"Label", wd.showLabel,
                [] { auto& t = g_layout.widgets[size_t(g_sel)]; t.showLabel = !t.showLabel; SaveLayout(); });
            Btn(R(cx + 116, y, 110, ctl), L"Unit", wd.showUnit, [] { auto& t = g_layout.widgets[size_t(g_sel)]; t.showUnit = !t.showUnit; SaveLayout(); });
            y += rowH;
            if (wd.type == panel::WType::Graph) {
                cx = label(L"History");
                const int secs[] = {30, 60, 120, 300, 600};
                const wchar_t* names[] = {L"30 s", L"1 min", L"2 min", L"5 min", L"10 min"};
                for (int i = 0; i < 5; ++i) {
                    int sv = secs[i];
                    Btn(R(cx + i * 62, y, 58, ctl), names[i], wd.graphSeconds == sv, [sv] { g_layout.widgets[size_t(g_sel)].graphSeconds = sv; SaveLayout(); });
                }
                y += rowH;
            } else if (wd.type == panel::WType::Pins) {
                cx = label(L"Pins show");
                const wchar_t* modes[] = {L"Amps", L"Watts", L"Volts"};
                for (int i = 0; i < 3; ++i)
                    Btn(R(cx + i * 72, y, 68, ctl), modes[i], wd.pinMode == i, [i] { g_layout.widgets[size_t(g_sel)].pinMode = i; SaveLayout(); });
                y += rowH;
            } else {
                cx = label(L"Sensor");
                std::wstring sn = sen ? sen->group + L" › " + sen->name : Wide(wd.sensor) + L" (offline)";
                Text(sn, R(cx, y, cw, ctl), Fmt(12), sen ? kInk : kWarn);
                y += rowH;
            }
            float abw2 = (w - 16) / 3;
            Btn(R(x, y, abw2, ctl), L"Duplicate", false, [] {
                auto c = g_layout.widgets[size_t(g_sel)];
                if (!panel::PlaceWidget(g_layout, c)) { PanelFullMessage(); return; }
                g_layout.widgets.push_back(c);
                g_sel = int(g_layout.widgets.size()) - 1;
                SaveLayout();
            });
            Btn(R(x + abw2 + 8, y, abw2, ctl), L"Bring to front", false, [] {
                auto c = g_layout.widgets[size_t(g_sel)];
                g_layout.widgets.erase(g_layout.widgets.begin() + g_sel);
                g_layout.widgets.push_back(c);
                g_sel = int(g_layout.widgets.size()) - 1;
                SaveLayout();
            });
            Btn(R(x + 2 * (abw2 + 8), y, abw2, ctl), L"Delete", false, [] {
                g_layout.widgets.erase(g_layout.widgets.begin() + g_sel);
                g_sel = -1;
                SaveLayout();
            });
        }
    }

    // ----- Sensor column -----
    {
        D2D1_RECT_F P = R(pad * 3 + c1 + c2, cy0, c3, colH);
        Panel(P);
        Anchor("sensorcol", P);
        float x = P.left + 14, y = P.top + 10, w = c3 - 28;
        const auto& all = g_reg.All();
        Text(F(L"Sensors (%zu)", all.size()), R(x, y, w, 26), Fmt(15, 1), kInk);
        y += 28;
        auto status = [&](bool ok, const std::wstring& t) {
            Circle(x + 5, y + 9, 3.5f, ok ? kGood : kMuted);
            Text(t, R(x + 16, y, w - 16, 18), Fmt(11.5f), ok ? kInk2 : kMuted);
            y += 18;
        };
        status(true, g_snap.pinsOk ? L"Built in: 12V-2x6 per-pin sensor, GPU, CPU, RAM"
                             : g_snap.sensorEnabled ? L"Built in: GPU, CPU, RAM (per-pin sensor: read error)" : L"Built in: GPU, CPU, RAM (no per-pin sensor on this GPU)");
        status(g_reg.gadgetOk && g_reg.gadgetCount > 0,
               g_reg.gadgetOk ? F(L"HWiNFO Gadget: %d values (tick “Report value in Gadget” per sensor)", g_reg.gadgetCount)
                              : L"HWiNFO (free): run it, Sensor settings › HWiNFO Gadget › enable reporting");
        status(g_reg.lhmOk, g_reg.lhmOk ? F(L"LibreHardwareMonitor: %d sensors (%s)", g_reg.lhmCount, g_reg.lhmVia.c_str())
                            : g_reg.lhmProcess ? std::wstring(L"LibreHardwareMonitor running \u2014 enable Options \u203A Remote Web Server \u203A Run")
                                               : std::wstring(L"LibreHardwareMonitor: not running (free alternative)"));
        status(g_reg.rtssOk, g_reg.rtssOk ? (g_reg.rtssApp.empty() ? std::wstring(L"RTSS: running, no 3D app active") : L"RTSS: FPS from " + g_reg.rtssApp)
                                          : L"RTSS: not running (needed for FPS)");
        if (g_reg.hwinfoOk) status(true, F(L"HWiNFO shared memory: %d values", g_reg.hwinfoCount));
        y += 6;
        if (TextField(R(x, y, w, ctl), F_SEARCH, g_search, L"\U0001F50D  Search sensors (e.g. cpu temp, fps, pin)", tmp)) {}
        y += rowH + 2;

        // filtered rows
        std::wstring q = g_focus == F_SEARCH ? g_editBuf : g_search;
        for (auto& ch : q) ch = wchar_t(towlower(ch));
        std::vector<std::wstring> terms;
        for (size_t i = 0; i < q.size();) {
            size_t j = q.find(L' ', i);
            if (j == std::wstring::npos) j = q.size();
            if (j > i) terms.push_back(q.substr(i, j - i));
            i = j + 1;
        }
        struct Row { bool header; const sensors::Sensor* s; std::wstring text; };
        std::vector<Row> rows;
        std::wstring lastGroup;
        for (auto& s : all) {
            if (!terms.empty()) {
                std::wstring hay = s.group + L" " + s.name + L" " + s.unit;
                for (auto& ch : hay) ch = wchar_t(towlower(ch));
                bool match = true;
                for (auto& t : terms) if (hay.find(t) == std::wstring::npos) { match = false; break; }
                if (!match) continue;
            }
            if (s.group != lastGroup) { rows.push_back({true, nullptr, s.group}); lastGroup = s.group; }
            rows.push_back({false, &s, s.name});
        }
        float lrow = 24;
        g_listRect = D2D1::RectF(x, y, x + w, P.bottom - 10);
        int visible = int((g_listRect.bottom - g_listRect.top) / lrow);
        g_listScroll = std::clamp(g_listScroll, 0, std::max(0, int(rows.size()) - visible));
        std::string selSensor = g_sel >= 0 ? g_layout.widgets[size_t(g_sel)].sensor : "";
        for (int k = 0; k < visible && k + g_listScroll < int(rows.size()); ++k) {
            const Row& row = rows[size_t(k + g_listScroll)];
            float ry = y + k * lrow;
            D2D1_RECT_F rr = R(x, ry, w, lrow - 2);
            if (row.header) {
                Text(row.text, D2D1::RectF(rr.left + 2, rr.top, rr.right, rr.bottom), Fmt(11.5f, 1), kAccent);
                continue;
            }
            const sensors::Sensor* s = row.s;
            bool act = s->key == selSensor;
            if (act) Fill(rr, kSurface3, 5);
            else if (Hover(rr)) Fill(rr, kSurface2, 5);
            std::wstring vt = s->ok ? FmtNum(s->value, s->decimals) + (s->unit.empty() ? L"" : L" " + s->unit) : L"—";
            float vw = Measure(vt, Fmt(12, 1)) + 4;
            Text(s->name, D2D1::RectF(rr.left + 12, rr.top, rr.right - vw - 10, rr.bottom), Fmt(12), act ? kAccent : kInk);
            Text(vt, D2D1::RectF(rr.right - vw - 6, rr.top, rr.right - 6, rr.bottom), Fmt(12, 1, 2), kInk2);
            std::string key = s->key;
            g_hits.push_back({rr, 0, [key] {
                if (g_sel >= 0) {
                    auto& t = g_layout.widgets[size_t(g_sel)];
                    if (t.type == panel::WType::Clock || t.type == panel::WType::Text || t.type == panel::WType::Pins) t.type = panel::WType::Value;
                    t.sensor = key;
                    t.min = t.max = t.warn = t.crit = NAN;
                    SaveLayout();
                } else {
                    AddWidget(panel::WType::Value, key);
                }
            }});
        }
        if (rows.empty()) Text(L"No sensors match", g_listRect, Fmt(12.5f, 0, 1), kMuted);
        if (int(rows.size()) > visible)
            Text(F(L"%d–%d of %zu · scroll", g_listScroll + 1, std::min(int(rows.size()), g_listScroll + visible), rows.size()),
                 R(P.right - 170, P.top + 12, 156, 20), Fmt(11, 0, 2), kMuted);
    }
}

// ---------- editor mouse ----------
bool PreviewMouseDown(float mx, float my) {
    if (!In(g_preview, mx, my)) return false;
    CommitFocus();
    float lx = (mx - g_preview.left) / g_previewScale, ly = (my - g_preview.top) / g_previewScale;
    float lw = float(g_layout.width), lh = float(g_layout.height);
    float gap = float(g_layout.gap), cw = (lw - gap) / g_layout.cols, ch = (lh - gap) / g_layout.rows;
    int cx = std::clamp(int(std::floor((lx - gap / 2) / cw)), 0, g_layout.cols - 1);
    int cy = std::clamp(int(std::floor((ly - gap / 2) / ch)), 0, g_layout.rows - 1);
    if (g_sel >= 0) {
        D2D1_RECT_F r = WidgetRect(g_layout, g_layout.widgets[size_t(g_sel)], lw, lh);
        float hs = 18.f / g_previewScale;
        if (lx >= r.right - hs && lx <= r.right + hs / 2 && ly >= r.bottom - hs && ly <= r.bottom + hs / 2) {
            g_drag = 2;
            SetCapture(g_hwnd);
            return true;
        }
    }
    g_sel = -1;
    for (int i = int(g_layout.widgets.size()) - 1; i >= 0; --i) {
        if (In(WidgetRect(g_layout, g_layout.widgets[size_t(i)], lw, lh), lx, ly)) {
            g_sel = i;
            g_drag = 1;
            g_dragDX = cx - g_layout.widgets[size_t(i)].x;
            g_dragDY = cy - g_layout.widgets[size_t(i)].y;
            SetCapture(g_hwnd);
            break;
        }
    }
    return true;
}

// true if the rectangle overlaps any widget other than `idx`
bool Collides(int idx, int x, int y, int w, int h) {
    for (size_t i = 0; i < g_layout.widgets.size(); ++i) {
        if (int(i) == idx) continue;
        const auto& o = g_layout.widgets[i];
        if (x < o.x + o.w && o.x < x + w && y < o.y + o.h && o.y < y + h) return true;
    }
    return false;
}

// Moves/resizes the selected widget, never onto another widget (a move slides along one axis if needed).
bool TryPlace(int nx, int ny, int nw, int nh) {
    if (g_sel < 0 || g_sel >= int(g_layout.widgets.size())) return false;
    auto& w = g_layout.widgets[size_t(g_sel)];
    nw = std::clamp(nw, 1, g_layout.cols);
    nh = std::clamp(nh, 1, g_layout.rows);
    nx = std::clamp(nx, 0, g_layout.cols - nw);
    ny = std::clamp(ny, 0, g_layout.rows - nh);
    auto apply = [&](int x, int y, int ww, int hh) {
        bool changed = x != w.x || y != w.y || ww != w.w || hh != w.h;
        w.x = x; w.y = y; w.w = ww; w.h = hh;
        return changed;
    };
    if (!Collides(g_sel, nx, ny, nw, nh)) return apply(nx, ny, nw, nh);
    if (nw == w.w && nh == w.h) {   // moving: slide along one axis
        if (!Collides(g_sel, nx, w.y, nw, nh)) return apply(nx, w.y, nw, nh);
        if (!Collides(g_sel, w.x, ny, nw, nh)) return apply(w.x, ny, nw, nh);
        return false;
    }
    // resizing: grow/shrink whichever axis still fits
    if (!Collides(g_sel, w.x, w.y, nw, w.h)) return apply(w.x, w.y, nw, w.h);
    if (!Collides(g_sel, w.x, w.y, w.w, nh)) return apply(w.x, w.y, w.w, nh);
    return false;
}

void PreviewMouseMove(float mx, float my) {
    if (!g_drag || g_sel < 0) return;
    float lx = (mx - g_preview.left) / g_previewScale, ly = (my - g_preview.top) / g_previewScale;
    float gap = float(g_layout.gap), cw = (g_layout.width - gap) / g_layout.cols, ch = (g_layout.height - gap) / g_layout.rows;
    int cx = int(std::floor((lx - gap / 2) / cw)), cy = int(std::floor((ly - gap / 2) / ch));
    auto& w = g_layout.widgets[size_t(g_sel)];
    bool changed = g_drag == 1 ? TryPlace(cx - g_dragDX, cy - g_dragDY, w.w, w.h) : TryPlace(w.x, w.y, cx - w.x + 1, cy - w.y + 1);
    if (changed && EditSlot().wnd) InvalidateRect(EditSlot().wnd, nullptr, FALSE);
}

// ---------- panel windows (one per open layout) ----------
Slot* SlotOfWnd(HWND h) { return reinterpret_cast<Slot*>(GetWindowLongPtrW(h, GWLP_USERDATA)); }

void ReleasePanelTarget(Slot& s) {
    ReleaseBmp(s.bmp);
    SafeRelease(s.pbr);
    SafeRelease(s.prt);
}

void PaintPanelWindow(Slot& sl) {
    if (!sl.prt) {
        RECT rc;
        GetClientRect(sl.wnd, &rc);
        auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), 96.f, 96.f);
        if (FAILED(g_d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(sl.wnd, D2D1::SizeU(rc.right, rc.bottom)), &sl.prt))) return;
        sl.prt->CreateSolidColorBrush(Col(kInk), &sl.pbr);
        sl.prt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    const panel::Layout& L = LayoutOf(sl);
    auto saveRt = g_rt;
    auto saveBr = g_br;
    float saveScale = g_previewScale;
    g_rt = sl.prt;
    g_br = sl.pbr;
    g_curBmp = &sl.bmp;
    g_previewScale = 1.f;
    sl.prt->BeginDraw();
    sl.prt->SetTransform(D2D1::Matrix3x2F::Identity());
    auto sz = sl.prt->GetSize();
    if (sl.virt) {
        // Virtual panel: a device bezel, then the panel at its real pixel size scaled by virtScale.
        sl.prt->Clear(Col(0x050608));
        float b = kBezel, bb = kBezelBottom;
        Stroke(D2D1::RectF(0, 0, sz.width, sz.height), 0x2A2F38, 14, 2.f);
        Stroke(D2D1::RectF(b - 3, b - 3, sz.width - b + 3, sz.height - bb + 3), 0x000000, 4, 3.f);
        float pw = sz.width - 2 * b, ph = sz.height - b - bb;
        Text(F(L"%s · %d×%d · %d%% · drag to move · wheel to resize · right-click for options", SlotName(sl).c_str(),
               L.width, L.height, int(std::lround(sl.virtScale * 100))),
             D2D1::RectF(b, sz.height - bb, sz.width - b, sz.height - 2), Fmt(10.5f, 0, 1), 0x6B7280);
        sl.prt->SetTransform(D2D1::Matrix3x2F::Scale(sl.virtScale, sl.virtScale) * D2D1::Matrix3x2F::Translation(b, b));
        sl.prt->PushAxisAlignedClip(D2D1::RectF(0, 0, pw / sl.virtScale, ph / sl.virtScale), D2D1_ANTIALIAS_MODE_ALIASED);
        RenderPanel(L, float(L.width), float(L.height), false);
        DrawAlarmOverlay(float(L.width), float(L.height));
        sl.prt->PopAxisAlignedClip();
    } else {
        RenderOnSurface(L, sz.width, sz.height);
        DrawAlarmOverlay(sz.width, sz.height);
    }
    HRESULT hr = sl.prt->EndDraw();
    g_rt = saveRt;
    g_br = saveBr;
    g_curBmp = &g_bmpMain;
    g_previewScale = saveScale;
    if (hr == D2DERR_RECREATE_TARGET) ReleasePanelTarget(sl);
}

void SnapPanel(Slot& s, bool toDisplay);

LRESULT CALLBACK PanelProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(h, m, wp, lp);
    }
    Slot* sp = SlotOfWnd(h);
    if (!sp) return DefWindowProcW(h, m, wp, lp);
    Slot& s = *sp;
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        PaintPanelWindow(s);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE:
        if (s.prt) s.prt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_NCHITTEST:
        if (s.virt) return HTCAPTION;   // drag anywhere to move the virtual panel
        break;
    case WM_EXITSIZEMOVE:
        // Dropped the virtual panel on another (non-main) display: go fullscreen there.
        if (s.virt && LayoutOf(s).snapOnDrop) {
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof mi;
            if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi) && !(mi.dwFlags & MONITORINFOF_PRIMARY)) {
                s.snapDev = mi.szDevice;
                PostMessageW(g_hwnd, WM_APP_PANEL_SNAP, 0, s.id);
            }
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (s.virt) SetVirtualScale(s, s.virtScale * (GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1.1f : 1 / 1.1f));
        return 0;
    case WM_NCRBUTTONUP:
    case WM_RBUTTONUP: {
        HMENU m2 = CreatePopupMenu();
        EnumMonitors();
        AppendMenuW(m2, MF_STRING | MF_GRAYED, 0, SlotName(s).c_str());
        AppendMenuW(m2, MF_SEPARATOR, 0, nullptr);
        if (s.virt) {
            AppendMenuW(m2, MF_STRING | (s.virtTop ? MF_CHECKED : 0), 1, L"Always on top\tT");
            AppendMenuW(m2, MF_STRING, 2, L"Actual size (100%)\t1");
            AppendMenuW(m2, MF_STRING, 3, L"Fit to screen\tF");
            AppendMenuW(m2, MF_STRING | (LayoutOf(s).snapOnDrop ? MF_CHECKED : 0), 5, L"Go fullscreen when dropped on another display");
            AppendMenuW(m2, MF_SEPARATOR, 0, nullptr);
        } else {
            AppendMenuW(m2, MF_STRING, 6, L"Back to window (virtual panel)");
        }
        int curMon = MonitorIndex(LayoutOf(s).display);
        for (size_t i = 0; i < g_mons.size(); ++i) {
            if (g_mons[i].primary || (!s.virt && int(i) == curMon)) continue;
            AppendMenuW(m2, MF_STRING, UINT(100 + i), (L"Fullscreen on " + MonitorLabel(int(i))).c_str());
        }
        AppendMenuW(m2, MF_STRING, 7, L"Identify displays");
        AppendMenuW(m2, MF_STRING, 8, L"Edit this layout");
        AppendMenuW(m2, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m2, MF_STRING, 4, L"Close panel\tEsc");
        POINT pt;
        GetCursorPos(&pt);
        int cmd = TrackPopupMenu(m2, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, nullptr);
        DestroyMenu(m2);
        if (cmd == 1) ToggleVirtualTop(s);
        else if (cmd == 2) SetVirtualScale(s, 1.f);
        else if (cmd == 3) SetVirtualScale(s, FitVirtualScale(s));
        else if (cmd == 4) DestroyWindow(h);
        else if (cmd == 5) { MutLayout(s).snapOnDrop = !LayoutOf(s).snapOnDrop; SaveSlot(s); }
        else if (cmd == 6) PostMessageW(g_hwnd, WM_APP_PANEL_SNAP, 1, s.id);
        else if (cmd == 7) IdentifyDisplays();
        else if (cmd == 8) {
            SelectSlot(SlotIndex(&s));
            g_tab = TAB_PANEL;
            ShowWindow(g_hwnd, IsIconic(g_hwnd) ? SW_RESTORE : SW_SHOW);
            SetForegroundWindow(g_hwnd);
            InvalidateRect(g_hwnd, nullptr, FALSE);
        } else if (cmd >= 100 && cmd < 100 + int(g_mons.size())) {
            s.snapDev = g_mons[size_t(cmd - 100)].dev;
            PostMessageW(g_hwnd, WM_APP_PANEL_SNAP, 0, s.id);
        }
        return 0;
    }
    case WM_NCLBUTTONDBLCLK:
    case WM_LBUTTONDBLCLK:
        if (!s.virt) DestroyWindow(h);   // fullscreen panel: double-click closes
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) DestroyWindow(h);
        else if (s.virt && wp == 'T') ToggleVirtualTop(s);
        else if (s.virt && wp == '1') SetVirtualScale(s, 1.f);
        else if (s.virt && wp == 'F') SetVirtualScale(s, FitVirtualScale(s));
        return 0;
    case WM_DESTROY:
        ReleasePanelTarget(s);
        s.wnd = nullptr;
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

void ShowPanel(Slot& s, bool on) {
    if (!on) {
        if (s.wnd) DestroyWindow(s.wnd);
        return;
    }
    if (s.wnd) return;
    EnumMonitors();
    const panel::Layout& L = LayoutOf(s);
    const MonInfo* mon = nullptr;
    for (auto& m : g_mons) if (m.dev == L.display) mon = &m;
    // A display already used by another open layout: this one opens as a window instead.
    if (mon)
        for (auto& o : g_slots)
            if (o.get() != &s && o->wnd && !o->virt && LayoutOf(*o).display == mon->dev) mon = nullptr;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    std::wstring title = L"RigSense · " + SlotName(s);
    if (mon && !mon->primary) {
        // Real panel: borderless fullscreen on that display, no taskbar button, never steals focus.
        s.virt = false;
        s.wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"RigSensePanelWnd", title.c_str(), WS_POPUP, mon->rc.left, mon->rc.top,
                                mon->rc.right - mon->rc.left, mon->rc.bottom - mon->rc.top, nullptr, nullptr, inst, &s);
    } else {
        // Virtual panel: a floating device-like window at the panel's real pixel size (scaled down if it doesn't fit).
        s.virt = true;
        s.virtScale = std::min(1.f, FitVirtualScale(s));
        SIZE sz = VirtualWindowSize(s);
        RECT wa;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        int open = 0;   // cascade several virtual panels instead of stacking them exactly
        for (auto& o : g_slots) open += o.get() != &s && o->wnd && o->virt;
        int off = (open % 6) * 36;
        s.wnd = CreateWindowExW(WS_EX_APPWINDOW | (s.virtTop ? WS_EX_TOPMOST : 0), L"RigSensePanelWnd", title.c_str(), WS_POPUP,
                                wa.left + (wa.right - wa.left - sz.cx) / 2 + off, wa.top + (wa.bottom - wa.top - sz.cy) / 2 + off, sz.cx, sz.cy,
                                nullptr, nullptr, inst, &s);
        if (s.wnd) {
            DWORD corner = 2;   // DWMWCP_ROUND
            DwmSetWindowAttribute(s.wnd, 33, &corner, sizeof corner);
        }
    }
    if (s.wnd) ShowWindow(s.wnd, s.virt ? SW_SHOW : SW_SHOWNOACTIVATE);
}

void CloseAllPanels() {
    for (auto& s : g_slots) ShowPanel(*s, false);
}

// "Open at start" layouts, in list order (a display taken by an earlier layout makes the next one a window).
void ShowPanelsAtStart() {
    for (auto& s : g_slots)
        if (LayoutOf(*s).autoShow) ShowPanel(*s, true);
}

bool LetterboxRect(const panel::Layout& L, float W, float H, D2D1_RECT_F& r) {
    double design = double(L.width) / L.height, aspect = double(W) / H;
    if ((aspect > 1.0) == (design > 1.0) || std::fabs(aspect - design) / design <= 0.10) return false;
    float s = std::min(W / L.width, H / L.height);
    float rw = L.width * s, rh = L.height * s;
    r = D2D1::RectF((W - rw) / 2, (H - rh) / 2, (W + rw) / 2, (H + rh) / 2);
    return true;
}

// Same shape: as designed. Clearly different shape, same orientation: a proportionally regridded copy so
// widgets keep their arrangement with roughly square cells instead of being stretched.
bool AdaptedLayout(const panel::Layout& L, int w, int h, panel::Layout& out) {
    if (w <= 0 || h <= 0) return false;
    D2D1_RECT_F lb;
    if (LetterboxRect(L, float(w), float(h), lb)) return false;   // rotated: letterboxed instead
    double designAspect = double(L.width) / L.height, aspect = double(w) / h;
    if (std::fabs(aspect - designAspect) / designAspect <= 0.10) return false;
    out = L;
    out.width = w;
    out.height = h;
    int c, r;
    panel::AutoGrid(w, h, c, r);
    panel::Regrid(out, c, r);
    return true;
}

void RenderOnSurface(const panel::Layout& L, float W, float H) {
    g_rt->Clear(Col(L.bgColor));
    D2D1_RECT_F lb;
    panel::Layout adapted;
    if (AdaptedLayout(L, int(W), int(H), adapted)) {
        RenderPanel(adapted, W, H, false);
    } else if (LetterboxRect(L, W, H, lb)) {
        D2D1_MATRIX_3X2_F old;
        g_rt->GetTransform(&old);
        g_rt->SetTransform(D2D1::Matrix3x2F::Translation(lb.left, lb.top) * old);
        g_rt->PushAxisAlignedClip(D2D1::RectF(0, 0, lb.right - lb.left, lb.bottom - lb.top), D2D1_ANTIALIAS_MODE_ALIASED);
        RenderPanel(L, lb.right - lb.left, lb.bottom - lb.top, false);
        g_rt->PopAxisAlignedClip();
        g_rt->SetTransform(old);
    } else {
        RenderPanel(L, W, H, false);
    }
}

// Fullscreen on a display or back to the floating window (the design itself is not changed).
void SnapPanel(Slot& s, bool toDisplay) {
    EnumMonitors();
    if (toDisplay) {
        int i = MonitorIndex(s.snapDev);
        if (i < 0 || g_mons[size_t(i)].primary) return;
        AssignDisplay(s, g_mons[size_t(i)].dev);
    } else {
        AssignDisplay(s, L"virtual");
    }
    ShowPanel(s, true);
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}

// Displays plugged in / out: every layout follows its saved display automatically.
void OnDisplaysChanged() {
    EnumMonitors();
    for (auto& sp : g_slots) {
        Slot& s = *sp;
        const panel::Layout& L = LayoutOf(s);
        int i = MonitorIndex(L.display);
        bool savedPresent = i >= 0 && !g_mons[size_t(i)].primary;
        if (s.wnd) {
            if (s.virt && savedPresent) { ShowPanel(s, false); ShowPanel(s, true); }        // display came back: fullscreen again
            else if (!s.virt && !savedPresent) { ShowPanel(s, false); ShowPanel(s, true); } // display gone: fall back to the window
            else if (!s.virt && savedPresent) {                                             // resolution / position changed
                const RECT& r = g_mons[size_t(i)].rc;
                SetWindowPos(s.wnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
            }
        } else if (L.autoShow && savedPresent) {
            ShowPanel(s, true);
        }
    }
}

void OnSampleForPanel() {
    g_reg.Update(g_snap);
    double now = NowSec();
    for (auto& sp : g_slots) {
        for (auto& w : LayoutOf(*sp).widgets) {
            if (w.type != panel::WType::Graph) continue;
            auto& dq = g_ghist[w.sensor];
            const sensors::Sensor* s = g_reg.Find(w.sensor);
            if (s && s->ok && (dq.empty() || dq.back().first < now - 0.05)) dq.push_back({now, float(s->value)});
        }
    }
    for (auto& [k, dq] : g_ghist) while (!dq.empty() && dq.front().first < now - 610) dq.pop_front();
    for (auto& sp : g_slots) if (sp->wnd) InvalidateRect(sp->wnd, nullptr, FALSE);
}
