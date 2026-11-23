// Tablet / phone panel: settings, frame rendering (off-screen Direct2D -> JPEG) and the "Tablet / phone" dialog.
// Included inside app.cpp's anonymous namespace (after panel_ui.inc, setup_ui.inc, settings_ui.inc).

tab::Server g_tabSrv;
tab::Adb g_adb;
struct TabletCfg { int mode = tab::MODE_OFF; int port = 8086; std::string key; bool stayOn = true; int slot = 0; };
TabletCfg g_tabCfg;
std::string g_tabError;
bool g_tabletOpen = false;
bool TabletLastSize(int& w, int& h) { return g_tabSrv.Running() && g_tabSrv.LastSize(w, h); }
void OpenTabletDialog() { g_settingsOpen = false; g_setupOpen = false; g_tabletOpen = true; InvalidateRect(g_hwnd, nullptr, FALSE); }
// The layout the tablet shows (Tablet dialog); falls back to the first one if it was deleted.
Slot& TabletSlot() {
    Slot* s = SlotById(g_tabCfg.slot);
    return s ? *s : *g_slots[0];
}
bool IsTabletSlot(const Slot& s) { return g_tabCfg.mode != tab::MODE_OFF && &s == &TabletSlot(); }

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
    c.slot = int(GetPrivateProfileIntW(L"Tablet", L"Layout", 0, g_ini.c_str()));
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
    WritePrivateProfileStringW(L"Tablet", L"Layout", std::to_wstring(TabletSlot().id).c_str(), g_ini.c_str());
    WritePrivateProfileStringW(L"Tablet", L"Key", Wide(g_tabCfg.key).c_str(), g_ini.c_str());
}

// A 2-second, 64x64 black H.264 MP4 made with Windows' own Media Foundation encoder (a few KB). The tablet page plays
// it muted and invisible on a loop, which keeps the tablet's screen from turning off - no download, no extra file.
// (The MP4 writer refuses an in-memory stream, so it is written to a temp file, read back and deleted.)
std::vector<unsigned char> MakeKeepAwakeVideo() {
    std::vector<unsigned char> out;
    // mfplat/mfreadwrite are delay-loaded: Windows "N" editions without the Media Feature Pack lack them, and
    // RigSense must still start there (the page then relies on the Wake Lock API / the tablet's own setting).
    if (!LoadLibraryW(L"mfplat.dll") || !LoadLibraryW(L"mfreadwrite.dll")) return out;
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return out;
    const UINT32 W = 64, H = 64, FPS = 10, FRAMES = 20;
    wchar_t tmp[MAX_PATH + 1] = {};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring file = std::wstring(tmp) + L"rigsense-keepawake-" + std::to_wstring(GetCurrentProcessId()) + L".mp4";
    IMFAttributes* attr = nullptr;
    IMFSinkWriter* wr = nullptr;
    IMFMediaType *mo = nullptr, *mi = nullptr;
    DWORD idx = 0;
    HRESULT hr = MFCreateAttributes(&attr, 1);
    if (SUCCEEDED(hr)) hr = attr->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
    if (SUCCEEDED(hr)) hr = MFCreateSinkWriterFromURL(file.c_str(), nullptr, attr, &wr);
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&mo);
    if (SUCCEEDED(hr)) {
        mo->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        mo->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        mo->SetUINT32(MF_MT_AVG_BITRATE, 40000);
        mo->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);   // Baseline: plays on the oldest tablets
        mo->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(mo, MF_MT_FRAME_SIZE, W, H);
        MFSetAttributeRatio(mo, MF_MT_FRAME_RATE, FPS, 1);
        MFSetAttributeRatio(mo, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        hr = wr->AddStream(mo, &idx);
    }
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&mi);
    if (SUCCEEDED(hr)) {
        mi->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        mi->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        mi->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        mi->SetUINT32(MF_MT_DEFAULT_STRIDE, W * 4);
        MFSetAttributeSize(mi, MF_MT_FRAME_SIZE, W, H);
        MFSetAttributeRatio(mi, MF_MT_FRAME_RATE, FPS, 1);
        MFSetAttributeRatio(mi, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        hr = wr->SetInputMediaType(idx, mi, nullptr);
    }
    if (SUCCEEDED(hr)) hr = wr->BeginWriting();
    for (UINT32 i = 0; SUCCEEDED(hr) && i < FRAMES; ++i) {
        IMFMediaBuffer* buf = nullptr;
        IMFSample* sm = nullptr;
        hr = MFCreateMemoryBuffer(W * H * 4, &buf);
        BYTE* p = nullptr;
        if (SUCCEEDED(hr) && SUCCEEDED(buf->Lock(&p, nullptr, nullptr))) {
            memset(p, 0, W * H * 4);   // black
            buf->Unlock();
            buf->SetCurrentLength(W * H * 4);
        }
        if (SUCCEEDED(hr)) hr = MFCreateSample(&sm);
        if (SUCCEEDED(hr)) hr = sm->AddBuffer(buf);
        if (SUCCEEDED(hr)) hr = sm->SetSampleTime(LONGLONG(i) * 10000000 / FPS);
        if (SUCCEEDED(hr)) hr = sm->SetSampleDuration(10000000 / FPS);
        if (SUCCEEDED(hr)) hr = wr->WriteSample(idx, sm);
        SafeRelease(sm);
        SafeRelease(buf);
    }
    if (SUCCEEDED(hr)) hr = wr->Finalize();
    SafeRelease(mi);
    SafeRelease(mo);
    SafeRelease(wr);   // closes the file
    SafeRelease(attr);
    MFShutdown();
    if (SUCCEEDED(hr)) {
        HANDLE f = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz{};
            if (GetFileSizeEx(f, &sz) && sz.QuadPart > 0 && sz.QuadPart < 1024 * 1024) {
                out.resize(size_t(sz.QuadPart));
                DWORD got = 0;
                if (!ReadFile(f, out.data(), DWORD(out.size()), &got, nullptr) || got != out.size()) out.clear();
            }
            CloseHandle(f);
        }
    }
    DeleteFileW(file.c_str());
    return out;
}

// Made once, on a worker thread (the encoder can take a moment to start); the page retries until it's ready.
std::thread g_kaThread;
std::atomic<bool> g_kaReady{false};
std::vector<unsigned char> g_keepAwakeVideo;   // written by g_kaThread before g_kaReady, read-only afterwards

void ApplyTablet() {
    g_adb.Stop();
    g_tabSrv.Stop();
    g_tabError.clear();
    if (g_tabCfg.mode == tab::MODE_OFF) return;
    if (!g_tabSrv.Start(g_tabCfg.port, g_tabCfg.mode == tab::MODE_WIFI, g_tabCfg.key, g_tabError)) return;
    if (g_kaReady) g_tabSrv.SetKeepAwakeVideo(std::vector<unsigned char>(g_keepAwakeVideo));
    else if (!g_kaThread.joinable())
        g_kaThread = std::thread([] {
            HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            g_keepAwakeVideo = MakeKeepAwakeVideo();   // stays empty if this PC has no H.264 encoder: not retried
            if (SUCCEEDED(co)) CoUninitialize();
            g_kaReady = true;
            g_tabSrv.SetKeepAwakeVideo(std::vector<unsigned char>(g_keepAwakeVideo));   // thread-safe; harmless if stopped
        });
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

// Called after every sample: render a frame only while a tablet is actually watching.
void RenderTabletFrame() {
    if (!g_tabSrv.Running()) return;
    int a = AlarmActive();
    g_tabSrv.SetAlarm(g_alarmTabletSound && (a == 1 || (a == 2 && g_alarmPanels)) ? a : 0);   // the page beeps while this is set
    int w = 0, h = 0;
    if (!g_tabSrv.Wanted(w, h)) return;
    if (!EnsureTabletRt(w, h)) return;
    auto saveRt = g_rt;
    auto saveBr = g_br;
    auto saveBmp = g_curBmp;
    float saveScale = g_previewScale;
    g_rt = g_tabRt;
    g_br = g_tabBr;
    g_curBmp = &g_bmpTablet;
    g_previewScale = 1.f;
    g_tabRt->BeginDraw();
    g_tabRt->SetTransform(D2D1::Matrix3x2F::Identity());
    RenderOnSurface(LayoutOf(TabletSlot()), float(w), float(h));
    DrawAlarmOverlay(float(w), float(h));
    HRESULT hr = g_tabRt->EndDraw();
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
    float cw = std::min(980.f, W - 40), ch = std::min(H - 40, 680.f);
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
    y += 40;
    {   // which layout the tablet shows
        Text(L"Shows layout", R(x, y, 120, 30), Fmt(13, 1), kInk2);
        Btn(R(x + 120, y, 264, 30), SlotName(TabletSlot()) + L"  ▸", false, [] {
            int i = SlotIndex(&TabletSlot());
            g_tabCfg.slot = g_slots[size_t(i + 1) % g_slots.size()]->id;
            SaveTabletCfg();
        });
        Btn(R(x + 392, y, 130, 30), L"Edit this layout", false, [] {
            SelectSlot(SlotIndex(&TabletSlot()));
            g_tab = TAB_PANEL;
            g_tabletOpen = false;
        });
        Text(L"Tip: make a layout just for the tablet with “+ New” in the Sensor panel tab.", R(x + 532, y, cw - 580, 30), Fmt(11.5f), kMuted);
        y += 40;
    }
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
            const panel::Layout& TL = LayoutOf(TabletSlot());
            std::wstring a = panel::AspectLabel(tw, th), d = panel::AspectLabel(TL.width, TL.height);
            double ta = double(tw) / th, da = double(TL.width) / TL.height;
            bool differs = std::fabs(ta - da) / da > 0.10;
            Text(F(L"Detected tablet / phone: %d\u00d7%d (%s)  \u00b7  layout: %d\u00d7%d (%s)%s", tw, th, a.c_str(), TL.width,
                   TL.height, d.c_str(), differs ? L" \u2014 shown fitted" : L" \u2014 same shape"),
                 R(x + 18, y, cw - 300, 20), Fmt(12), differs ? kWarn : kInk2);
            if (differs) Btn(R(x0 + cw - 24 - 230, y - 4, 230, 28), L"Design for this tablet", true, [tw, th] {
                SelectSlot(SlotIndex(&TabletSlot()));
                DesignFor(tw, th);
            });
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
        L"Screen still sleeps? Developer options \u203A \u201cStay awake\u201d (while charging).",
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
            CopyText(Wide(url));
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
