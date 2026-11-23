// Tablet / phone panel: settings, frame rendering (off-screen Direct2D -> JPEG) and the "Tablet / phone" dialog.
// Included inside app.cpp's anonymous namespace (after panel_ui.inc, setup_ui.inc, settings_ui.inc).

tab::Server g_tabSrv;
tab::Adb g_adb;
struct TabletCfg { int mode = tab::MODE_OFF; int port = 8086; std::string key; bool stayOn = true; };
TabletCfg g_tabCfg;
std::string g_tabError;
bool g_tabletOpen = false;
bool TabletLastSize(int& w, int& h) { return g_tabSrv.Running() && g_tabSrv.LastSize(w, h); }
void OpenTabletDialog() { g_settingsOpen = false; g_setupOpen = false; g_tabletOpen = true; InvalidateRect(g_hwnd, nullptr, FALSE); }

// off-screen render target for frames
IWICBitmap* g_tabBmp = nullptr;
ID2D1RenderTarget* g_tabRt = nullptr;
ID2D1SolidColorBrush* g_tabBr = nullptr;
int g_tabW = 0, g_tabH = 0;
BgBmp g_bmpTablet;

TabletCfg LoadTabletCfg() {
    TabletCfg c;
    wchar_t b[64] = {};
    GetPrivateProfileStringW(L"Tablet", L"Mode", L"off", b, 64, g_ini.c_str());
    c.mode = !_wcsicmp(b, L"wifi") ? tab::MODE_WIFI : !_wcsicmp(b, L"usb") ? tab::MODE_USB : tab::MODE_OFF;
    c.port = std::clamp(int(GetPrivateProfileIntW(L"Tablet", L"Port", 8086, g_ini.c_str())), 1024, 65535);
    c.stayOn = GetPrivateProfileIntW(L"Tablet", L"StayOn", 1, g_ini.c_str()) != 0;
    GetPrivateProfileStringW(L"Tablet", L"Key", L"", b, 64, g_ini.c_str());
    for (wchar_t* p = b; *p; ++p) if (iswxdigit(*p)) c.key += char(towlower(*p));
    if (c.key.size() < 16) {
        c.key = tab::RandomKey();
        WritePrivateProfileStringW(L"Tablet", L"Key", Wide(c.key).c_str(), g_ini.c_str());
    }
    return c;
}

void SaveTabletCfg() {
    const wchar_t* m = g_tabCfg.mode == tab::MODE_WIFI ? L"wifi" : g_tabCfg.mode == tab::MODE_USB ? L"usb" : L"off";
    WritePrivateProfileStringW(L"Tablet", L"Mode", m, g_ini.c_str());
    WritePrivateProfileStringW(L"Tablet", L"Port", std::to_wstring(g_tabCfg.port).c_str(), g_ini.c_str());
    WritePrivateProfileStringW(L"Tablet", L"StayOn", g_tabCfg.stayOn ? L"1" : L"0", g_ini.c_str());
    WritePrivateProfileStringW(L"Tablet", L"Key", Wide(g_tabCfg.key).c_str(), g_ini.c_str());
}

void ApplyTablet() {
    g_adb.Stop();
    g_tabSrv.Stop();
    g_tabError.clear();
    if (g_tabCfg.mode == tab::MODE_OFF) return;
    if (!g_tabSrv.Start(g_tabCfg.port, g_tabCfg.mode == tab::MODE_WIFI, g_tabCfg.key, g_tabError)) return;
    g_adb.Start(g_tabCfg.port, g_tabCfg.key, g_tabCfg.stayOn);
}

std::vector<unsigned char> EncodeWic(IWICBitmap* bmp, const GUID& container, float quality) {
    std::vector<unsigned char> out;
    IStream* st = nullptr;
    if (!g_wic || FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &st))) return out;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fr = nullptr;
    IPropertyBag2* bag = nullptr;
    UINT w = 0, h = 0;
    bmp->GetSize(&w, &h);
    if (SUCCEEDED(g_wic->CreateEncoder(container, nullptr, &enc)) && SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&fr, &bag))) {
        if (container == GUID_ContainerFormatJpeg && bag) {
            PROPBAG2 opt{};
            opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_R4;
            v.fltVal = quality;
            bag->Write(1, &opt, &v);
        }
        WICPixelFormatGUID fmt = container == GUID_ContainerFormatJpeg ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA;
        if (SUCCEEDED(fr->Initialize(bag)) && SUCCEEDED(fr->SetSize(w, h)) && SUCCEEDED(fr->SetPixelFormat(&fmt)) &&
            SUCCEEDED(fr->WriteSource(bmp, nullptr)) && SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit())) {
            STATSTG stat{};
            HGLOBAL hg = nullptr;
            if (SUCCEEDED(st->Stat(&stat, STATFLAG_NONAME)) && SUCCEEDED(GetHGlobalFromStream(st, &hg))) {
                const void* p = GlobalLock(hg);
                if (p) {
                    out.assign(static_cast<const unsigned char*>(p), static_cast<const unsigned char*>(p) + stat.cbSize.QuadPart);
                    GlobalUnlock(hg);
                }
            }
        }
    }
    SafeRelease(bag);
    SafeRelease(fr);
    SafeRelease(enc);
    SafeRelease(st);
    return out;
}

void ReleaseTabletRt() {
    ReleaseBmp(g_bmpTablet);
    SafeRelease(g_tabBr);
    SafeRelease(g_tabRt);
    SafeRelease(g_tabBmp);
    g_tabW = g_tabH = 0;
}

bool EnsureTabletRt(int w, int h) {
    if (g_tabRt && g_tabW == w && g_tabH == h) return true;
    ReleaseTabletRt();
    if (!g_wic) CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic));
    if (!g_wic || FAILED(g_wic->CreateBitmap(UINT(w), UINT(h), GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &g_tabBmp))) return false;
    auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                              D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
    if (FAILED(g_d2d->CreateWicBitmapRenderTarget(g_tabBmp, props, &g_tabRt))) { ReleaseTabletRt(); return false; }
    g_tabRt->CreateSolidColorBrush(Col(kInk), &g_tabBr);
    g_tabRt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    g_tabW = w;
    g_tabH = h;
    return true;
}

// Same idea as the fullscreen panel: a tablet with a different shape gets a proportionally regridded copy.
bool AdaptedFor(int w, int h, panel::Layout& out) {
    D2D1_RECT_F lb;
    if (LetterboxRect(float(w), float(h), lb)) return false;   // rotated tablet: letterboxed instead
    double designAspect = double(g_layout.width) / g_layout.height, aspect = double(w) / h;
    if (std::fabs(aspect - designAspect) / designAspect <= 0.10) return false;
    out = g_layout;
    out.width = w;
    out.height = h;
    int c, r;
    panel::AutoGrid(w, h, c, r);
    panel::Regrid(out, c, r);
    return true;
}

// Called after every sample: render a frame only while a tablet is actually watching.
void RenderTabletFrame() {
    int w = 0, h = 0;
    if (!g_tabSrv.Running() || !g_tabSrv.Wanted(w, h)) return;
    if (!EnsureTabletRt(w, h)) return;
    panel::Layout adapted;
    bool useAdapted = AdaptedFor(w, h, adapted);
    auto saveRt = g_rt;
    auto saveBr = g_br;
    auto saveBmp = g_curBmp;
    float saveScale = g_previewScale;
    g_rt = g_tabRt;
    g_br = g_tabBr;
    g_curBmp = &g_bmpTablet;
    g_previewScale = 1.f;
    if (useAdapted) std::swap(g_layout, adapted);
    g_tabRt->BeginDraw();
    g_tabRt->SetTransform(D2D1::Matrix3x2F::Identity());
    g_tabRt->Clear(Col(g_layout.bgColor));
    D2D1_RECT_F lb;
    if (!useAdapted && LetterboxRect(float(w), float(h), lb)) RenderLetterboxed(float(w), float(h), lb);
    else RenderPanel(float(w), float(h), false);
    HRESULT hr = g_tabRt->EndDraw();
    if (useAdapted) std::swap(g_layout, adapted);
    g_rt = saveRt;
    g_br = saveBr;
    g_curBmp = saveBmp;
    g_previewScale = saveScale;
    if (FAILED(hr)) { ReleaseTabletRt(); return; }
    auto jpeg = EncodeWic(g_tabBmp, GUID_ContainerFormatJpeg, 0.85f);
    if (!jpeg.empty()) g_tabSrv.SetFrame(std::move(jpeg), w, h);
}

// 192x192 app icon for "Add to home screen" (teal bars, like the window icon).
void MakeTabletIcon() {
    if (!EnsureTabletRt(192, 192)) return;
    g_tabRt->BeginDraw();
    g_tabRt->Clear(Col(kSurface));
    g_tabBr->SetColor(Col(kAccent));
    const float hgt[6] = {0.55f, 0.75f, 0.65f, 0.85f, 0.6f, 0.7f};
    for (int i = 0; i < 6; ++i)
        g_tabRt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(28.f + i * 24, 160 - hgt[i] * 120, 44.f + i * 24, 160), 4, 4), g_tabBr);
    if (SUCCEEDED(g_tabRt->EndDraw())) g_tabSrv.SetIcon(EncodeWic(g_tabBmp, GUID_ContainerFormatPng, 1.f));
    ReleaseTabletRt();
}

std::string TabletUrl() {
    auto ips = tab::LocalIPv4();
    return "http://" + (ips.empty() ? std::string("<pc-ip>") : ips[0]) + ":" + std::to_string(g_tabCfg.port) + "/?k=" + g_tabCfg.key;
}

void DrawQr(const std::string& text, D2D1_RECT_F r) {
    try {
        auto qr = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        int n = qr.getSize();
        float side = std::min(r.right - r.left, r.bottom - r.top);
        float cell = std::floor(side / (n + 8));   // 4-module quiet zone each side
        float total = cell * (n + 8);
        float x0 = r.left + (side - total) / 2, y0 = r.top + (side - total) / 2;
        Fill(D2D1::RectF(x0, y0, x0 + total, y0 + total), 0xFFFFFF, 6);
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
                if (qr.getModule(x, y)) Fill(R(x0 + (x + 4) * cell, y0 + (y + 4) * cell, cell + 0.5f, cell + 0.5f), 0x000000);
    } catch (...) {
        Text(L"(QR unavailable)", r, Fmt(12, 0, 1), kMuted);
    }
}

void DrawTabletOverlay(float W, float H) {
    g_hits.clear();
    Fill(D2D1::RectF(0, 0, W, H), 0x000000, 0, 0.62f);
    float cw = std::min(980.f, W - 40), ch = std::min(H - 40, 640.f);
    float x0 = (W - cw) / 2, y0 = (H - ch) / 2;
    Fill(R(x0, y0, cw, ch), kSurface, 12);
    Stroke(R(x0, y0, cw, ch), kBorder, 12);
    Text(L"Tablet / phone panel", R(x0 + 24, y0 + 16, cw - 48, 30), Fmt(19, 1), kInk);
    Text(L"Use any old tablet or phone as a sensor panel — nothing to install on it, its browser is enough.",
         R(x0 + 24, y0 + 46, cw - 48, 20), Fmt(12.5f), kInk2);
    float y = y0 + 84, x = x0 + 24;
    Text(L"Mode", R(x, y, 120, 30), Fmt(13, 1), kInk2);
    const wchar_t* modes[] = {L"Off", L"USB only", L"USB + Wi-Fi"};
    for (int i = 0; i < 3; ++i)
        Btn(R(x + 120 + i * 132, y, 126, 30), modes[i], g_tabCfg.mode == i, [i] {
            g_tabCfg.mode = i;
            SaveTabletCfg();
            ApplyTablet();
        });
    Btn(R(x + 520, y, 210, 30), g_tabCfg.stayOn ? L"Keep tablet awake (USB): on" : L"Keep tablet awake (USB): off", g_tabCfg.stayOn, [] {
        g_tabCfg.stayOn = !g_tabCfg.stayOn;
        SaveTabletCfg();
        ApplyTablet();
    });
    y += 44;
    std::wstring st;
    uint32_t sc = kMuted;
    if (g_tabCfg.mode == tab::MODE_OFF) st = L"Off — RigSense is not serving the panel.";
    else if (!g_tabError.empty()) { st = L"Could not start: " + Wide(g_tabError); sc = kWarn; }
    else {
        int v = g_tabSrv.Viewers();
        st = F(L"Serving the panel on port %d  ·  %d device%s watching", g_tabCfg.port, v, v == 1 ? L"" : L"s");
        sc = v ? kGood : kInk2;
    }
    Circle(x + 6, y + 10, 4, sc);
    Text(st, R(x + 18, y, cw - 60, 20), Fmt(12.5f), kInk);
    y += 26;
    {   // detected tablet resolution vs. the design
        int tw = 0, th = 0;
        if (TabletLastSize(tw, th)) {
            std::wstring a = panel::AspectLabel(tw, th), d = panel::AspectLabel(g_layout.width, g_layout.height);
            double ta = double(tw) / th, da = double(g_layout.width) / g_layout.height;
            bool differs = std::fabs(ta - da) / da > 0.10;
            Text(F(L"Detected tablet / phone: %d\u00d7%d (%s)  \u00b7  your design: %d\u00d7%d (%s)%s", tw, th, a.c_str(), g_layout.width,
                   g_layout.height, d.c_str(), differs ? L" \u2014 shown fitted" : L" \u2014 same shape"),
                 R(x + 18, y, cw - 300, 20), Fmt(12), differs ? kWarn : kInk2);
            if (differs) Btn(R(x0 + cw - 24 - 230, y - 4, 230, 28), L"Design for this tablet", true, [tw, th] { DesignFor(tw, th); });
        }
    }
    y += 30;

    float colW = (cw - 72) / 2;
    // ----- USB column -----
    float ux = x, uy = y;
    Text(L"USB (recommended)", R(ux, uy, colW, 24), Fmt(14, 1), kAccent);
    uy += 30;
    std::wstring adb = g_adb.AdbPath();
    if (g_tabCfg.mode == tab::MODE_OFF) {
        Text(L"Choose “USB only” or “USB + Wi-Fi” above.", R(ux, uy, colW, 20), Fmt(12.5f), kInk2);
        uy += 26;
    } else if (adb.empty() && tab::FindAdb().empty()) {
        Text(L"Needs Google's free adb tool (Android platform-tools).", R(ux, uy, colW, 20), Fmt(12.5f), kWarn);
        uy += 28;
        Btn(R(ux, uy, 200, 30), L"Install adb (winget)", true, [] {
            comp::Component c;
            c.name = L"Android platform-tools (adb)";
            c.wingetId = L"Google.PlatformTools";
            comp::InstallWithWinget(c);
        });
        uy += 40;
    } else {
        auto devs = g_adb.Devices();
        if (devs.empty()) { Text(L"No tablet connected over USB.", R(ux, uy, colW, 20), Fmt(12.5f), kInk2); uy += 24; }
        for (auto& d : devs) {
            std::wstring name = Wide(d.model.empty() ? d.serial : d.model);
            std::wstring s = d.state == "device" ? (d.reversed ? L"connected — panel opened" : L"connecting…")
                           : d.state == "unauthorized" ? L"tap “Allow” on the tablet (USB debugging prompt)"
                                                       : Wide(d.state);
            Circle(ux + 6, uy + 10, 4, d.reversed ? kGood : kWarn);
            Text(name + L":  " + s, R(ux + 18, uy, colW - 18, 20), Fmt(12.5f), kInk);
            uy += 24;
        }
        uy += 6;
        Btn(R(ux, uy, 180, 30), L"Open on tablet again", false, [] { g_adb.OpenOnTablets(); });
        uy += 40;
    }
    const wchar_t* steps[] = {
        L"One-time setup on the tablet (Android 5 or newer):",
        L"1. Settings › About tablet › tap “Build number” 7 times",
        L"2. Settings › Developer options › turn on “USB debugging”",
        L"3. Plug it in and tap “Allow” — the panel opens by itself",
        L"Tap the panel once for fullscreen. Works without any network.",
    };
    for (auto* s : steps) {
        Text(s, R(ux, uy, colW, 20), Fmt(12), s == steps[0] ? kInk2 : kMuted);
        uy += 21;
    }

    // ----- Wi-Fi column -----
    float wx = x + colW + 24, wy = y;
    Text(L"Wi-Fi", R(wx, wy, colW, 24), Fmt(14, 1), kAccent);
    wy += 30;
    if (g_tabCfg.mode != tab::MODE_WIFI) {
        Text(L"Choose “USB + Wi-Fi” to also allow Wi-Fi (same network).", R(wx, wy, colW, 20), Fmt(12.5f), kInk2);
    } else {
        std::string url = TabletUrl();
        float q = std::min(200.f, colW * 0.45f);
        DrawQr(url, R(wx, wy, q, q));
        float tx = wx + q + 16, tw = colW - q - 16;
        Text(L"Scan with the tablet's camera", R(tx, wy, tw, 20), Fmt(12), kInk2);
        Text(L"or type the address below.", R(tx, wy + 20, tw, 20), Fmt(12), kInk2);
        Text(Wide(url), R(wx, wy + q + 8, colW, 22), Fmt(FitSize(Wide(url), 1, 13.f, colW), 1), kInk);
        Btn(R(tx, wy + 60, 130, 28), L"Copy address", false, [url] {
            if (OpenClipboard(g_hwnd)) {
                EmptyClipboard();
                std::wstring w = Wide(url);
                HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
                if (hg) {
                    memcpy(GlobalLock(hg), w.c_str(), (w.size() + 1) * sizeof(wchar_t));
                    GlobalUnlock(hg);
                    SetClipboardData(CF_UNICODETEXT, hg);
                }
                CloseClipboard();
            }
        });
        Btn(R(tx, wy + 94, 130, 28), L"New secret key", false, [] {
            g_tabCfg.key = tab::RandomKey();
            SaveTabletCfg();
            ApplyTablet();
        });
        Text(L"(disconnects the old address)", R(tx, wy + 126, tw, 18), Fmt(11), kMuted);
        float ny = wy + q + 40;
        const wchar_t* notes[] = {L"Windows may ask to allow RigSense on private networks: click Allow.",
                                  L"Only devices with this secret address can see the panel.",
                                  L"The tablet can only view it - it can't control your PC."};
        for (auto* n : notes) { Text(n, R(wx, ny, colW, 18), Fmt(11.5f), kMuted); ny += 19; }
    }

    float fy = y0 + ch - 54;
    Btn(R(x0 + cw - 24 - 110, fy, 110, 32), L"Close", true, [] { g_tabletOpen = false; });
}
