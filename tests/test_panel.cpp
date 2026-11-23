// Tests for the panel layout model: save/load, export/import round-trip, malformed input, regrid invariants.
// Build + run: tests\run_tests.bat
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>
#include "../src/panel.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static bool SameNum(double a, double b) { return (std::isnan(a) && std::isnan(b)) || a == b; }
static bool SameWidget(const panel::Widget& a, const panel::Widget& b) {
    return a.type == b.type && a.sensor == b.sensor && a.text == b.text && a.color == b.color && a.bg == b.bg &&
           a.textScale == b.textScale && SameNum(a.min, b.min) && SameNum(a.max, b.max) && SameNum(a.warn, b.warn) &&
           SameNum(a.crit, b.crit) && a.decimals == b.decimals && a.showLabel == b.showLabel && a.showUnit == b.showUnit &&
           a.graphSeconds == b.graphSeconds && a.pinMode == b.pinMode && a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}
static bool Inside(const panel::Layout& l) {
    for (auto& w : l.widgets)
        if (w.w < 1 || w.h < 1 || w.x < 0 || w.y < 0 || w.x + w.w > l.cols || w.y + w.h > l.rows) return false;
    return true;
}
static std::vector<unsigned char> ReadAll(const std::wstring& p) {
    std::vector<unsigned char> d;
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) return d;
    unsigned char b[65536];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) d.insert(d.end(), b, b + n);
    fclose(f);
    return d;
}
static void WriteAll(const std::wstring& p, const std::string& s) {
    FILE* f = _wfopen(p.c_str(), L"wb");
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
}

int main() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"rigsense_test";
    CreateDirectoryW(dir.c_str(), nullptr);

    // 1. default layout round-trip
    {
        panel::Layout a = panel::DefaultLayout();
        a.display = L"\\\\.\\DISPLAY2";
        a.autoShow = true;
        a.bgColor = 0x123456;
        a.widgets[1].text = L"Temp °C € with  spaces = and % signs";
        a.widgets[1].min = 1.5; a.widgets[1].max = 99.25; a.widgets[1].warn = 70; a.widgets[1].crit = NAN;
        a.widgets[2].sensor = "lhm./amdcpu/0/temperature/2";
        a.widgets[3].sensor = "hwg.CPU%20[#0]:%20AMD|Core%20Temp";
        a.widgets[4].decimals = 3; a.widgets[4].showLabel = false; a.widgets[4].showUnit = false;
        std::wstring p = dir + L"\\a.txt";
        CHECK(panel::Save(p, a));
        panel::Layout b;
        CHECK(panel::Load(p, b));
        CHECK(b.width == a.width && b.height == a.height && b.cols == a.cols && b.rows == a.rows);
        CHECK(b.display == a.display && b.autoShow && b.bgColor == 0x123456);
        CHECK(b.widgets.size() == a.widgets.size());
        bool all = b.widgets.size() == a.widgets.size();
        for (size_t i = 0; all && i < a.widgets.size(); ++i) all = SameWidget(a.widgets[i], b.widgets[i]);
        CHECK(all);
        CHECK(b.name.empty());                                   // no name line: stays empty (pre-1.4 files)
        // layout names (1.4): spaces, symbols, unicode, stray line breaks
        a.name = L"Case bar · 21:9 = mine 100%\r\nsecond";
        CHECK(panel::Save(p, a));
        CHECK(panel::Load(p, b));
        CHECK(b.name == L"Case bar · 21:9 = mine 100%  second");
        CHECK(b.widgets.size() == a.widgets.size());
    }

    // 2. export / import with an embedded background image
    {
        panel::Layout a = panel::DefaultLayout();
        std::wstring img = dir + L"\\my background.png";
        std::string bytes;
        for (int i = 0; i < 300000; ++i) bytes += char((i * 7919) & 0xFF);   // binary payload incl. zero bytes
        WriteAll(img, bytes);
        a.bgImage = img;
        a.display = L"\\\\.\\DISPLAY3";
        a.autoShow = true;
        std::wstring ex = dir + L"\\shared.rigpanel";
        CHECK(panel::Export(ex, a));
        panel::Layout b;
        std::wstring err;
        std::wstring imgDir = dir + L"\\imported";
        CHECK(panel::Import(ex, b, imgDir, err));
        CHECK(b.display.empty() && !b.autoShow);                // machine-specific settings not exported
        CHECK(!b.bgImage.empty() && b.bgImage.find(imgDir) == 0);
        auto got = ReadAll(b.bgImage);
        CHECK(got.size() == bytes.size() && std::equal(got.begin(), got.end(), bytes.begin(), [](unsigned char x, char y) { return x == (unsigned char)y; }));
        bool all = b.widgets.size() == a.widgets.size();
        for (size_t i = 0; all && i < a.widgets.size(); ++i) all = SameWidget(a.widgets[i], b.widgets[i]);
        CHECK(all);
        // export without image
        a.bgImage.clear();
        CHECK(panel::Export(ex, a));
        CHECK(panel::Import(ex, b, imgDir, err));
        CHECK(b.bgImage.empty());
    }

    // 3. malformed / hostile input must not crash, values get clamped
    {
        std::wstring p = dir + L"\\bad.txt";
        WriteAll(p, "panel width=-5 height=999999 cols=0 rows=1000 gap=x radius=\n"
                    "widget type=nope x=-100 y=99999 w=0 h=-3 color=999 scale=0 dec=99 graph=0 pins=7 text=\n"
                    "widget\nwidget type=bar sensor= text=" + std::string(10000, 'A') + "\n" + std::string(20000, 'Z') + "\n"
                    "imagedata name=..\\..\\evil.exe data=!!!notbase64\n");
        panel::Layout l;
        CHECK(panel::Load(p, l));
        CHECK(l.width >= 64 && l.height <= 7680 && l.cols >= 1 && l.rows <= 48);
        CHECK(Inside(l));
        std::wstring err;
        panel::Layout m;
        CHECK(panel::Import(p, m, dir + L"\\imp2", err));   // bad image data is ignored, not written
        CHECK(m.bgImage.empty());
        CHECK(GetFileAttributesW((dir + L"\\evil.exe").c_str()) == INVALID_FILE_ATTRIBUTES);
        WriteAll(p, "just some random text file\n");
        CHECK(!panel::Import(p, m, dir + L"\\imp3", err));
        CHECK(!panel::Load(dir + L"\\does_not_exist.txt", m));
    }

    // 4. regrid fuzz: widgets always stay inside the grid
    {
        std::mt19937 rng(1234);
        bool ok = true;
        for (int iter = 0; iter < 5000 && ok; ++iter) {
            panel::Layout l;
            l.cols = 1 + int(rng() % 40);
            l.rows = 1 + int(rng() % 20);
            int n = int(rng() % 12);
            for (int i = 0; i < n; ++i) {
                panel::Widget w;
                w.x = int(rng() % 60) - 10; w.y = int(rng() % 30) - 5;
                w.w = int(rng() % 20) - 2; w.h = int(rng() % 10) - 2;
                l.widgets.push_back(w);
            }
            panel::Clamp(l);
            ok = Inside(l);
            panel::Regrid(l, 1 + int(rng() % 48), 1 + int(rng() % 48));
            ok = ok && Inside(l);
            if (int(l.widgets.size()) <= l.cols * l.rows) {
                for (size_t i = 0; i < l.widgets.size() && ok; ++i)
                    for (size_t j = i + 1; j < l.widgets.size() && ok; ++j) {
                        auto& a = l.widgets[i]; auto& b = l.widgets[j];
                        if (a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h) {
                            ok = false;
                            printf("  fuzz overlap iter %d grid %dx%d n=%zu\n", iter, l.cols, l.rows, l.widgets.size());
                        }
                    }
            }
            int fx, fy;
            if (panel::FindFreeSpot(l, 1, 1, fx, fy)) ok = ok && fx >= 0 && fy >= 0 && fx < l.cols && fy < l.rows;
        }
        CHECK(ok);
    }

    // 5. switching presets keeps the default layout overlap-free (when the grid has room)
    {
        auto overlapFree = [](const panel::Layout& l) {
            for (size_t i = 0; i < l.widgets.size(); ++i)
                for (size_t j = i + 1; j < l.widgets.size(); ++j) {
                    auto& a = l.widgets[i]; auto& b = l.widgets[j];
                    if (a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h) return false;
                }
            return true;
        };
        const int presets[][2] = {{1920, 480}, {1280, 480}, {800, 480}, {1024, 600}, {480, 320}, {2560, 720}, {1920, 720}, {1920, 1080}, {480, 1920}};
        bool ok = true;
        for (auto& p : presets) {
            panel::Layout l = panel::DefaultLayout();
            int c, r;
            panel::AutoGrid(p[0], p[1], c, r);
            panel::Regrid(l, c, r);
            int cells = 0;
            for (auto& w : l.widgets) cells += 1;
            if (c * r >= cells && !overlapFree(l)) { ok = false; printf("  overlap after preset %dx%d (grid %dx%d)\n", p[0], p[1], c, r); }
            ok = ok && Inside(l);
        }
        CHECK(ok);
        // placing into a full grid fails instead of overlapping
        panel::Layout full = panel::DefaultLayout();
        panel::Widget w;
        CHECK(!panel::PlaceWidget(full, w));
    }

    // 6. aspect ratio labels
    CHECK(panel::AspectLabel(1920, 1080) == L"16:9");
    CHECK(panel::AspectLabel(2560, 1440) == L"16:9");
    CHECK(panel::AspectLabel(3840, 2160) == L"16:9");
    CHECK(panel::AspectLabel(5120, 1440) == L"32:9");
    CHECK(panel::AspectLabel(3840, 1080) == L"32:9");
    CHECK(panel::AspectLabel(3440, 1440) == L"21:9");
    CHECK(panel::AspectLabel(2560, 1080) == L"21:9");
    CHECK(panel::AspectLabel(1920, 1200) == L"16:10");
    CHECK(panel::AspectLabel(1280, 800) == L"16:10");
    CHECK(panel::AspectLabel(1920, 480) == L"4:1");
    CHECK(panel::AspectLabel(800, 480) == L"5:3");
    CHECK(panel::AspectLabel(1024, 600) == L"1.71:1");
    CHECK(panel::AspectLabel(480, 320) == L"3:2");
    CHECK(panel::AspectLabel(1080, 1920) == L"9:16");
    CHECK(panel::AspectLabel(800, 1280) == L"10:16");
    CHECK(panel::AspectLabel(2048, 1536) == L"4:3");
    CHECK(panel::AspectLabel(0, 100).empty());

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
