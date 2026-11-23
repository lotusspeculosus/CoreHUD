// Sensor panel: renderer, editor tab and the panel window.
// Included inside app.cpp's anonymous namespace (uses its drawing helpers).

panel::Layout g_layout;
std::wstring g_layoutPath;
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
enum FieldId { F_WIDTH = 1, F_HEIGHT, F_LABEL, F_MIN, F_MAX, F_WARN, F_CRIT, F_SEARCH };
int g_focus = 0;
std::wstring g_editBuf;
int g_commitId = 0;
std::wstring g_commitText;
std::map<int, std::wstring> g_fieldInit;

// panel window
HWND g_panelWnd = nullptr;
bool g_panelVirtual = false;    // virtual panel = floating "device" window with a bezel
// When the fullscreen display has a clearly different shape than the design, the panel window renders an
// adapted copy (proportionally regridded). The saved design (g_layout / panel.txt) is never modified.
bool g_hasAdapted = false;
panel::Layout g_adapted;
float g_virtScale = 1.f;
bool g_virtTop = false;
constexpr float kBezel = 18.f, kBezelBottom = 30.f;
ID2D1HwndRenderTarget* g_prt = nullptr;
ID2D1SolidColorBrush* g_pbr = nullptr;
struct BgBmp { ID2D1Bitmap* bmp = nullptr; std::wstring path; bool failed = false; };
BgBmp g_bmpMain, g_bmpPanel;
BgBmp* g_curBmp = &g_bmpMain;
IWICImagingFactory* g_wic = nullptr;
ID2D1StrokeStyle* g_round = nullptr;

struct MonInfo { std::wstring dev; RECT rc; bool primary; std::wstring name; };
std::vector<MonInfo> g_mons;
constexpr UINT WM_APP_PANEL_SNAP = WM_APP + 3;   // posted to the main window: wParam 0 = fullscreen on g_snapDev, 1 = back to window
std::wstring g_snapDev;

double NowSec() { return GetTickCount64() / 1000.0; }

// ---- virtual panel window sizing (pixels) ----
float FitVirtualScale() {
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    float k = std::min((wa.right - wa.left - 2 * kBezel - 40) / g_layout.width, (wa.bottom - wa.top - kBezel - kBezelBottom - 40) / g_layout.height);
    return std::clamp(k, 0.1f, 4.f);
}
SIZE VirtualWindowSize() {
    return {LONG(std::lround(g_layout.width * g_virtScale + 2 * kBezel)), LONG(std::lround(g_layout.height * g_virtScale + kBezel + kBezelBottom))};
}
void SetVirtualScale(float k) {
    if (!g_panelWnd || !g_panelVirtual) return;
    g_virtScale = std::clamp(k, 0.1f, 4.f);
    RECT r;
    GetWindowRect(g_panelWnd, &r);
    SIZE sz = VirtualWindowSize();
    int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    SetWindowPos(g_panelWnd, nullptr, cx - sz.cx / 2, cy - sz.cy / 2, sz.cx, sz.cy, SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(g_panelWnd, nullptr, FALSE);
}
void ToggleVirtualTop() {
    g_virtTop = !g_virtTop;
    if (g_panelWnd) SetWindowPos(g_panelWnd, g_virtTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void RefreshAdapted();

void SaveLayout() {
    panel::Clamp(g_layout);
    panel::Save(g_layoutPath, g_layout);
    RefreshAdapted();
    if (g_panelWnd) {
        if (g_panelVirtual) SetVirtualScale(g_virtScale);   // panel size may have changed
        InvalidateRect(g_panelWnd, nullptr, FALSE);
    }
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
    s += F(L" \u00b7 %d\u00d7%d", m.rc.right - m.rc.left, m.rc.bottom - m.rc.top);
    if (m.primary) s += L" (main)";
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

ID2D1Bitmap* BackgroundBitmap() {
    BgBmp& b = *g_curBmp;
    if (g_layout.bgImage.empty()) { ReleaseBmp(b); return nullptr; }
    if (b.path == g_layout.bgImage && (b.bmp || b.failed)) return b.bmp;
    ReleaseBmp(b);
    b.path = g_layout.bgImage;
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

D2D1_RECT_F WidgetRect(const panel::Widget& w, float W, float H) {
    float s = std::min(W / g_layout.width, H / g_layout.height);
    float gap = g_layout.gap * s;
    float cw = (W - gap) / g_layout.cols, ch = (H - gap) / g_layout.rows;
    return D2D1::RectF(w.x * cw + gap, w.y * ch + gap, (w.x + w.w) * cw, (w.y + w.h) * ch);
}

void RenderPanel(float W, float H, bool edit) {
    float s = std::min(W / g_layout.width, H / g_layout.height);
    Fill(D2D1::RectF(0, 0, W, H), g_layout.bgColor);
    if (ID2D1Bitmap* bmp = BackgroundBitmap()) {
        auto bs = bmp->GetSize();
        float k = std::max(W / bs.width, H / bs.height);   // cover
        float dw = bs.width * k, dh = bs.height * k;
        g_rt->PushAxisAlignedClip(D2D1::RectF(0, 0, W, H), D2D1_ANTIALIAS_MODE_ALIASED);
        g_rt->DrawBitmap(bmp, D2D1::RectF((W - dw) / 2, (H - dh) / 2, (W + dw) / 2, (H + dh) / 2), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        g_rt->PopAxisAlignedClip();
        if (g_layout.bgDim > 0) Fill(D2D1::RectF(0, 0, W, H), 0x000000, 0, g_layout.bgDim / 100.f);
    }
    if (edit) {
        float gap = g_layout.gap * s;
        float cw = (W - gap) / g_layout.cols, ch = (H - gap) / g_layout.rows;
        for (int x = 0; x < g_layout.cols; ++x)
            for (int y = 0; y < g_layout.rows; ++y)
                Stroke(D2D1::RectF(x * cw + gap, y * ch + gap, (x + 1) * cw, (y + 1) * ch), kInk, g_layout.radius * s, 1.f / g_previewScale, 0.07f);
    }
    for (auto& w : g_layout.widgets) {
        D2D1_RECT_F r = WidgetRect(w, W, H);
        g_rt->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        DrawWidget(w, r, s, g_layout.radius * s);
        g_rt->PopAxisAlignedClip();
    }
    if (edit && g_sel >= 0 && g_sel < int(g_layout.widgets.size())) {
        D2D1_RECT_F r = WidgetRect(g_layout.widgets[size_t(g_sel)], W, H);
        float lw = 2.f / g_previewScale;
        Stroke(r, kAccent, g_layout.radius * s, lw);
        float hs = 14.f / g_previewScale;
        Fill(D2D1::RectF(r.right - hs, r.bottom - hs, r.right, r.bottom), kAccent, 3.f / g_previewScale);
    }
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

void ShowPanel(bool on);
void RefreshAdapted();

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
    if (!g_layout.widgets.empty() &&
        MessageBoxW(g_hwnd, L"Replace your current panel with the imported one?\n(Export yours first if you want to keep it.)", L"RigSense",
                    MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return;
    panel::Layout l;
    std::wstring err;
    if (!panel::Import(file, l, mon::ExeDir() + L"\\panel_images", err)) {
        MessageBoxW(g_hwnd, err.c_str(), L"RigSense", MB_ICONERROR);
        return;
    }
    l.display = g_layout.display;     // keep this PC's display choice
    l.autoShow = g_layout.autoShow;
    int missing = 0;
    for (auto& w : l.widgets)
        if (w.type != panel::WType::Clock && w.type != panel::WType::Text && w.type != panel::WType::Pins && !g_reg.Find(w.sensor)) ++missing;
    g_layout = l;
    g_sel = -1;
    SaveLayout();
    if (missing)
        MessageBoxW(g_hwnd, F(L"Imported. %d widget(s) use sensors this PC doesn't have (or whose source isn't running) - select them and pick a sensor from the list.", missing).c_str(),
                    L"RigSense", MB_ICONINFORMATION);
}

void DrawPanelEditor(float W, float H) {
    if (g_sel >= int(g_layout.widgets.size())) g_sel = -1;
    float pad = 16, top = 48 + pad;
    float lw = float(g_layout.width), lh = float(g_layout.height);
    float areaW = W - 2 * pad, maxH = (H - top - pad) * 0.40f;
    float pw = areaW, ph = pw * lh / lw;
    if (ph > maxH) { ph = maxH; pw = ph * lw / lh; }
    float px = pad + (areaW - pw) / 2, py = top;
    g_preview = R(px, py, pw, ph);
    g_previewScale = pw / lw;
    D2D1_MATRIX_3X2_F old;
    g_rt->GetTransform(&old);
    g_rt->SetTransform(D2D1::Matrix3x2F::Scale(g_previewScale, g_previewScale) * D2D1::Matrix3x2F::Translation(px, py));
    g_rt->PushAxisAlignedClip(D2D1::RectF(0, 0, lw, lh), D2D1_ANTIALIAS_MODE_ALIASED);
    RenderPanel(lw, lh, true);
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
        float x = P.left + 14, y = P.top + 10, w = c1 - 28;
        Text(L"Panel", R(x, y, w, 26), Fmt(15, 1), kInk);
        y += 32;
        static const int presets[][2] = {{1920, 480}, {1280, 480}, {800, 480}, {1024, 600}, {480, 320}, {2560, 720}, {1920, 720}, {1920, 1080}};
        float bw = (w - 16) / 3;
        for (int i = 0; i < 9; ++i) {
            D2D1_RECT_F br = R(x + (i % 3) * (bw + 8), y + (i / 3) * rowH, bw, ctl);
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
        Text(L"px", R(cx + 166, y, 30, ctl), Fmt(12), kMuted);
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
                if (opts[n] < 0) g_layout.display = L"virtual";
                else {
                    auto& m = g_mons[size_t(opts[n])];
                    g_layout.display = m.dev;
                }
                bool open = g_panelWnd != nullptr;
                ShowPanel(false);
                SaveLayout();
                if (open) ShowPanel(true);
            });
        }
        y += rowH;
        Btn(R(x, y, (w - 8) / 2, ctl), g_panelWnd ? L"Close panel" : L"Open panel", g_panelWnd != nullptr, [] { ShowPanel(!g_panelWnd); });
        Btn(R(x + (w - 8) / 2 + 8, y, (w - 8) / 2, ctl), g_layout.autoShow ? L"Open at start: on" : L"Open at start: off", g_layout.autoShow,
            [] { g_layout.autoShow = !g_layout.autoShow; SaveLayout(); });
        y += rowH;
        Btn(R(x, y, (w - 8) / 2, ctl), L"Reset layout", false, [] {
            auto d = panel::DefaultLayout();
            int c, r;
            panel::AutoGrid(g_layout.width, g_layout.height, c, r);
            d.width = g_layout.width; d.height = g_layout.height; d.display = g_layout.display; d.autoShow = g_layout.autoShow;
            panel::Regrid(d, c, r);
            g_layout = d;
            g_sel = -1;
            SaveLayout();
        });
        Btn(R(x + (w - 8) / 2 + 8, y, (w - 8) / 2, ctl), L"Open panel.txt", false,
            [] { ShellExecuteW(g_hwnd, L"open", L"notepad.exe", (L"\"" + g_layoutPath + L"\"").c_str(), nullptr, SW_SHOWNORMAL); });
        y += rowH;
        Btn(R(x, y, (w - 8) / 2, ctl), L"Export panel…", false, [] { ExportPanel(); });
        Btn(R(x + (w - 8) / 2 + 8, y, (w - 8) / 2, ctl), L"Import panel…", false, [] { ImportPanel(); });
    }

    // ----- Widget column -----
    {
        D2D1_RECT_F P = R(pad * 2 + c1, cy0, c2, colH);
        Panel(P);
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
        D2D1_RECT_F r = WidgetRect(g_layout.widgets[size_t(g_sel)], lw, lh);
        float hs = 18.f / g_previewScale;
        if (lx >= r.right - hs && lx <= r.right + hs / 2 && ly >= r.bottom - hs && ly <= r.bottom + hs / 2) {
            g_drag = 2;
            SetCapture(g_hwnd);
            return true;
        }
    }
    g_sel = -1;
    for (int i = int(g_layout.widgets.size()) - 1; i >= 0; --i) {
        if (In(WidgetRect(g_layout.widgets[size_t(i)], lw, lh), lx, ly)) {
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
    if (changed && g_panelWnd) InvalidateRect(g_panelWnd, nullptr, FALSE);
}

// ---------- panel window ----------
void ReleasePanelTarget() {
    ReleaseBmp(g_bmpPanel);
    SafeRelease(g_pbr);
    SafeRelease(g_prt);
}

void PaintPanelWindow() {
    if (!g_prt) {
        RECT rc;
        GetClientRect(g_panelWnd, &rc);
        auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), 96.f, 96.f);
        if (FAILED(g_d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(g_panelWnd, D2D1::SizeU(rc.right, rc.bottom)), &g_prt))) return;
        g_prt->CreateSolidColorBrush(Col(kInk), &g_pbr);
        g_prt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    auto saveRt = g_rt;
    auto saveBr = g_br;
    float saveScale = g_previewScale;
    g_rt = g_prt;
    g_br = g_pbr;
    g_curBmp = &g_bmpPanel;
    g_previewScale = 1.f;
    g_prt->BeginDraw();
    g_prt->SetTransform(D2D1::Matrix3x2F::Identity());
    auto sz = g_prt->GetSize();
    if (g_panelVirtual) {
        // Virtual panel: draw a device bezel, then the panel at its real pixel size scaled by g_virtScale.
        g_prt->Clear(Col(0x050608));
        float b = kBezel, bb = kBezelBottom;
        Stroke(D2D1::RectF(0, 0, sz.width, sz.height), 0x2A2F38, 14, 2.f);
        Stroke(D2D1::RectF(b - 3, b - 3, sz.width - b + 3, sz.height - bb + 3), 0x000000, 4, 3.f);
        float pw = sz.width - 2 * b, ph = sz.height - b - bb;
        Text(F(L"RigSense · virtual %d×%d panel · %d%% · drag to move · wheel to resize · right-click for options",
               g_layout.width, g_layout.height, int(std::lround(g_virtScale * 100))),
             D2D1::RectF(b, sz.height - bb, sz.width - b, sz.height - 2), Fmt(10.5f, 0, 1), 0x6B7280);
        g_prt->SetTransform(D2D1::Matrix3x2F::Scale(g_virtScale, g_virtScale) * D2D1::Matrix3x2F::Translation(b, b));
        g_prt->PushAxisAlignedClip(D2D1::RectF(0, 0, pw / g_virtScale, ph / g_virtScale), D2D1_ANTIALIAS_MODE_ALIASED);
        RenderPanel(float(g_layout.width), float(g_layout.height), false);
        g_prt->PopAxisAlignedClip();
    } else {
        g_prt->Clear(Col(g_layout.bgColor));
        if (g_hasAdapted) std::swap(g_layout, g_adapted);   // render-only, swapped straight back
        RenderPanel(sz.width, sz.height, false);
        if (g_hasAdapted) std::swap(g_layout, g_adapted);
    }
    HRESULT hr = g_prt->EndDraw();
    g_rt = saveRt;
    g_br = saveBr;
    g_curBmp = &g_bmpMain;
    g_previewScale = saveScale;
    if (hr == D2DERR_RECREATE_TARGET) ReleasePanelTarget();
}

LRESULT CALLBACK PanelProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        PaintPanelWindow();
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE:
        if (g_prt) g_prt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
        if (h == g_panelWnd) RefreshAdapted();   // display resolution changed
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_NCHITTEST:
        if (g_panelVirtual) return HTCAPTION;   // drag anywhere to move the virtual panel
        break;
    case WM_EXITSIZEMOVE:
        // Dropped the virtual panel on another (non-main) display: go fullscreen there.
        if (g_panelVirtual && g_layout.snapOnDrop) {
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof mi;
            if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi) && !(mi.dwFlags & MONITORINFOF_PRIMARY)) {
                g_snapDev = mi.szDevice;
                PostMessageW(g_hwnd, WM_APP_PANEL_SNAP, 0, 0);
            }
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (g_panelVirtual) {
            float k = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1.1f : 1 / 1.1f;
            SetVirtualScale(g_virtScale * k);
        }
        return 0;
    case WM_NCRBUTTONUP:
    case WM_RBUTTONUP: {
        HMENU m2 = CreatePopupMenu();
        EnumMonitors();
        if (g_panelVirtual) {
            AppendMenuW(m2, MF_STRING | (g_virtTop ? MF_CHECKED : 0), 1, L"Always on top\tT");
            AppendMenuW(m2, MF_STRING, 2, L"Actual size (100%)\t1");
            AppendMenuW(m2, MF_STRING, 3, L"Fit to screen\tF");
            AppendMenuW(m2, MF_STRING | (g_layout.snapOnDrop ? MF_CHECKED : 0), 5, L"Go fullscreen when dropped on another display");
            AppendMenuW(m2, MF_SEPARATOR, 0, nullptr);
        } else {
            AppendMenuW(m2, MF_STRING, 6, L"Back to window (virtual panel)");
        }
        int curMon = MonitorIndex(g_layout.display);
        for (size_t i = 0; i < g_mons.size(); ++i) {
            if (g_mons[i].primary || (!g_panelVirtual && int(i) == curMon)) continue;
            AppendMenuW(m2, MF_STRING, UINT(100 + i), (L"Fullscreen on " + MonitorLabel(int(i))).c_str());
        }
        AppendMenuW(m2, MF_STRING, 7, L"Identify displays");
        AppendMenuW(m2, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m2, MF_STRING, 4, L"Close panel\tEsc");
        POINT pt;
        GetCursorPos(&pt);
        int cmd = TrackPopupMenu(m2, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, nullptr);
        DestroyMenu(m2);
        if (cmd == 1) ToggleVirtualTop();
        else if (cmd == 2) SetVirtualScale(1.f);
        else if (cmd == 3) SetVirtualScale(FitVirtualScale());
        else if (cmd == 4) DestroyWindow(h);
        else if (cmd == 5) { g_layout.snapOnDrop = !g_layout.snapOnDrop; SaveLayout(); }
        else if (cmd == 6) PostMessageW(g_hwnd, WM_APP_PANEL_SNAP, 1, 0);
        else if (cmd == 7) IdentifyDisplays();
        else if (cmd >= 100 && cmd < 100 + int(g_mons.size())) {
            g_snapDev = g_mons[size_t(cmd - 100)].dev;
            PostMessageW(g_hwnd, WM_APP_PANEL_SNAP, 0, 0);
        }
        return 0;
    }
    case WM_NCLBUTTONDBLCLK:
    case WM_LBUTTONDBLCLK:
        if (!g_panelVirtual) DestroyWindow(h);   // fullscreen panel: double-click closes
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) DestroyWindow(h);
        else if (g_panelVirtual && wp == 'T') ToggleVirtualTop();
        else if (g_panelVirtual && wp == '1') SetVirtualScale(1.f);
        else if (g_panelVirtual && wp == 'F') SetVirtualScale(FitVirtualScale());
        return 0;
    case WM_DESTROY:
        ReleasePanelTarget();
        g_panelWnd = nullptr;
        if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

void ShowPanel(bool on) {
    if (!on) {
        if (g_panelWnd) DestroyWindow(g_panelWnd);
        return;
    }
    if (g_panelWnd) return;
    EnumMonitors();
    const MonInfo* mon = nullptr;
    for (auto& m : g_mons) if (m.dev == g_layout.display) mon = &m;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (mon && !mon->primary) {
        // Real panel: borderless fullscreen on that display, no taskbar button, never steals focus.
        g_panelWnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"RigSensePanelWnd", L"RigSense panel", WS_POPUP,
                                     mon->rc.left, mon->rc.top, mon->rc.right - mon->rc.left, mon->rc.bottom - mon->rc.top,
                                     nullptr, nullptr, inst, nullptr);
        g_panelVirtual = false;
        RefreshAdapted();
    } else {
        // Virtual panel: a floating device-like window at the panel's real pixel size (scaled down if it doesn't fit).
        g_panelVirtual = true;
        g_virtScale = std::min(1.f, FitVirtualScale());
        SIZE sz = VirtualWindowSize();
        RECT wa;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        g_panelWnd = CreateWindowExW(WS_EX_APPWINDOW | (g_virtTop ? WS_EX_TOPMOST : 0), L"RigSensePanelWnd", L"RigSense virtual panel",
                                     WS_POPUP, wa.left + (wa.right - wa.left - sz.cx) / 2, wa.top + (wa.bottom - wa.top - sz.cy) / 2,
                                     sz.cx, sz.cy, nullptr, nullptr, inst, nullptr);
        DWORD corner = 2;   // DWMWCP_ROUND
        DwmSetWindowAttribute(g_panelWnd, 33, &corner, sizeof corner);
    }
    if (g_panelWnd) ShowWindow(g_panelWnd, g_panelVirtual ? SW_SHOW : SW_SHOWNOACTIVATE);
}

// Same shape as the design (e.g. a 1920x480 bar for a 1920x480 design): rendered as designed, just scaled.
// Clearly different shape: render a proportionally regridded copy so widgets keep their arrangement with
// roughly square cells instead of being stretched. The design itself stays untouched.
void RefreshAdapted() {
    g_hasAdapted = false;
    if (!g_panelWnd || g_panelVirtual) return;
    RECT rc;
    GetClientRect(g_panelWnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    double designAspect = double(g_layout.width) / g_layout.height, aspect = double(w) / h;
    if (std::fabs(aspect - designAspect) / designAspect <= 0.10) return;
    g_adapted = g_layout;
    g_adapted.width = w;
    g_adapted.height = h;
    int c, r;
    panel::AutoGrid(w, h, c, r);
    panel::Regrid(g_adapted, c, r);
    g_hasAdapted = true;
}

// Fullscreen on a display or back to the floating window (the design itself is not changed).
void SnapPanel(bool toDisplay) {
    EnumMonitors();
    if (toDisplay) {
        int i = MonitorIndex(g_snapDev);
        if (i < 0 || g_mons[size_t(i)].primary) return;
        const auto& m = g_mons[size_t(i)];
        g_layout.display = m.dev;
    } else {
        g_layout.display = L"virtual";
    }
    ShowPanel(false);
    SaveLayout();
    ShowPanel(true);
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}

// Displays plugged in / out: follow the saved panel display automatically.
void OnDisplaysChanged() {
    EnumMonitors();
    int i = MonitorIndex(g_layout.display);
    bool savedPresent = i >= 0 && !g_mons[size_t(i)].primary;
    if (g_panelWnd) {
        if (g_panelVirtual && savedPresent) { ShowPanel(false); ShowPanel(true); }        // display came back: fullscreen again
        else if (!g_panelVirtual && !savedPresent) { ShowPanel(false); ShowPanel(true); } // display gone: fall back to the window
        else if (!g_panelVirtual && savedPresent) {                                        // resolution / position changed
            const RECT& r = g_mons[size_t(i)].rc;
            SetWindowPos(g_panelWnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    } else if (g_layout.autoShow && savedPresent) {
        ShowPanel(true);
    }
}

void OnSampleForPanel() {
    g_reg.Update(g_snap);
    double now = NowSec();
    for (auto& w : g_layout.widgets) {
        if (w.type != panel::WType::Graph) continue;
        auto& dq = g_ghist[w.sensor];
        const sensors::Sensor* s = g_reg.Find(w.sensor);
        if (s && s->ok && (dq.empty() || dq.back().first < now - 0.05)) dq.push_back({now, float(s->value)});
    }
    for (auto& [k, dq] : g_ghist) while (!dq.empty() && dq.front().first < now - 610) dq.pop_front();
    if (g_panelWnd) InvalidateRect(g_panelWnd, nullptr, FALSE);
}
