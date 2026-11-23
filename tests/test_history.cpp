// Tests for the connector wear check (wear.cpp) and the black box recorder (blackbox.cpp).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "../src/blackbox.h"
#include "../src/wear.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static std::wstring TempDir(const wchar_t* name) {
    wchar_t t[MAX_PATH];
    GetTempPathW(MAX_PATH, t);
    std::wstring d = std::wstring(t) + L"corehud_test_" + name + L"_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}
static std::vector<std::wstring> List(const std::wstring& dir, const wchar_t* pat) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\" + pat).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do out.push_back(fd.cFileName);
    while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}
static std::string ReadAll(const std::wstring& path) {
    std::string s;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return s;
    char b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    fclose(f);
    return s;
}
static int CountLines(const std::string& s) {
    int n = 0;
    for (char c : s) n += c == '\n';
    return n;
}
static void Wipe(const std::wstring& dir) {
    for (auto& f : List(dir, L"*")) if (f != L"." && f != L"..") DeleteFileW((dir + L"\\" + f).c_str());
    RemoveDirectoryW(dir.c_str());
}

static wear::Day MakeDay(const char* date, double minutes, double hotShare, int hotPin = 2) {
    wear::Day d;
    d.date = date;
    d.minutes = minutes;
    double rest = (1.0 - hotShare) / 5;
    for (int i = 0; i < 6; ++i) d.share[i] = i == hotPin ? hotShare : rest;
    d.imbalance = hotShare * 6;
    return d;
}

static void TestWearAnalyze() {
    std::vector<wear::Day> days;
    CHECK(wear::Analyze(days).state == wear::LEARNING);
    days.push_back(MakeDay("2026-11-01", 30, 0.17));
    days.push_back(MakeDay("2026-11-02", 5, 0.30));    // too little load: ignored
    days.push_back(MakeDay("2026-11-03", 40, 0.17));
    days.push_back(MakeDay("2026-11-04", 25, 0.17));
    auto st = wear::Analyze(days);
    CHECK(st.state == wear::LEARNING);                 // 3 qualifying days < 4
    CHECK(st.qualifyingDays == 3);
    days.push_back(MakeDay("2026-11-05", 60, 0.172));
    st = wear::Analyze(days);
    CHECK(st.state == wear::OK);
    CHECK(std::fabs(st.baseShare[2] - 0.17) < 1e-9);
    CHECK(!st.text.empty());
    // slowly rising share on pin 3 (index 2)
    days.push_back(MakeDay("2026-11-06", 60, 0.20));
    days.push_back(MakeDay("2026-11-07", 60, 0.20));
    days.push_back(MakeDay("2026-11-08", 60, 0.20));
    st = wear::Analyze(days);
    CHECK(st.state == wear::WATCH);                    // +3 points
    CHECK(st.pin == 2);
    CHECK(std::fabs(st.deltaPts - 3.0) < 0.01);
    days.push_back(MakeDay("2026-11-09", 60, 0.22));
    days.push_back(MakeDay("2026-11-10", 60, 0.22));
    days.push_back(MakeDay("2026-11-11", 60, 0.22));
    st = wear::Analyze(days);
    CHECK(st.state == wear::WARN);                     // +5 points
    CHECK(st.text.find(L"Pin 3") != std::wstring::npos);
    // a drop in share is not a warning
    std::vector<wear::Day> down = {MakeDay("2026-11-01", 30, 0.20), MakeDay("2026-11-02", 30, 0.20), MakeDay("2026-11-03", 30, 0.20),
                                   MakeDay("2026-11-04", 30, 0.15)};
    CHECK(wear::Analyze(down).state == wear::OK);
}

static void TestWearFiles() {
    std::wstring dir = TempDir(L"wear");
    std::wstring path = dir + L"\\wear-astral.csv";
    std::vector<wear::Day> days = {MakeDay("2026-11-01", 30, 0.17), MakeDay("2026-11-02", 12.5, 0.18, 4)};
    CHECK(wear::Save(path, days));
    std::vector<wear::Day> back;
    CHECK(wear::Load(path, back));
    CHECK(back.size() == 2);
    CHECK(back.size() == 2 && back[1].date == "2026-11-02" && std::fabs(back[1].minutes - 12.5) < 1e-6 && std::fabs(back[1].share[4] - 0.18) < 1e-5);
    // garbage / malformed lines are skipped, not fatal
    FILE* f = _wfopen(path.c_str(), L"ab");
    fputs("garbage line\n2026-11-03,abc\n2026-11-04,10,2,0,0,0,0,0,1\n2026-11-05,20,0.2,0.16,0.16,0.16,0.16,0.16,1.2\n", f);
    fclose(f);
    CHECK(wear::Load(path, back));
    CHECK(back.size() == 3);                           // share 2.0 rejected, the good line kept
    CHECK(!wear::Load(dir + L"\\missing.csv", back));
    CHECK(back.empty());

    // tracker: samples accumulate into today, survive a restart, roll over to a new day
    DeleteFileW(path.c_str());
    wear::Tracker t;
    t.Open(path);
    double even[6] = {5, 5, 5, 5, 5, 5};               // 30 A, perfectly even
    double low[6] = {1, 1, 1, 1, 1, 1};                // 6 A: below the load threshold
    CHECK(!t.Add(low, "2026-11-20", 0.5));
    int counted = 0;
    for (int i = 0; i < 1200; ++i) counted += t.Add(even, "2026-11-20", 0.5);   // 10 minutes
    CHECK(counted == 1200);
    t.Flush();
    CHECK(wear::Load(path, back) && back.size() == 1 && std::fabs(back[0].minutes - 10) < 1e-6);
    CHECK(back.size() == 1 && std::fabs(back[0].share[0] - 1.0 / 6) < 1e-4 && std::fabs(back[0].imbalance - 1.0) < 1e-4);
    wear::Tracker t2;                                   // "restart": today's row continues
    t2.Open(path);
    double hot[6] = {8, 4.4, 4.4, 4.4, 4.4, 4.4};       // pin 1 hot
    for (int i = 0; i < 1200; ++i) t2.Add(hot, "2026-11-20", 0.5);
    t2.Add(even, "2026-11-21", 0.5);                    // new day: the old one is written first
    t2.Flush();
    CHECK(wear::Load(path, back) && back.size() == 2);
    CHECK(back.size() == 2 && std::fabs(back[0].minutes - 20) < 1e-6);
    CHECK(back.size() == 2 && back[0].share[0] > 1.0 / 6 && back[0].share[0] < 8.0 / 30);   // average of even + hot
    CHECK(!t2.Add(hot, "2026-11-21", 0) && !t2.Add(hot, "2026-11-21", 120));                 // bad time steps ignored
    double nan6[6] = {NAN, 5, 5, 5, 5, 5};
    CHECK(!t2.Add(nan6, "2026-11-21", 0.5));
    t2.Reset();
    CHECK(GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES);
    CHECK(List(dir, L"*.old").size() == 1);
    CHECK(t2.status().state == wear::LEARNING);
    Wipe(dir);
}

static bbox::Sample S(double t, float a = 5.f, const wchar_t* ev = L"") {
    bbox::Sample s;
    s.t = t;
    char b[32];
    snprintf(b, sizeof b, "t=%.1f", t);
    s.stamp = b;
    s.ok = true;
    for (int i = 0; i < 6; ++i) { s.amps[i] = a; s.volts[i] = 12.f; }
    s.connW = a * 72;
    s.events = ev;
    return s;
}

static void TestBlackBox() {
    CHECK(bbox::CsvField(L"plain") == "plain");
    CHECK(bbox::CsvField(L"a,b") == "\"a,b\"");
    CHECK(bbox::CsvField(L"say \"hi\"") == "\"say \"\"hi\"\"\"");
    CHECK(bbox::CsvField(L"line\nbreak") == "\"line break\"");
    CHECK(bbox::CsvField(L"Pin 3 \u00b7 9.6 A") == "Pin 3 \xC2\xB7 9.6 A");   // UTF-8

    std::wstring dir = TempDir(L"bbox");
    {
        bbox::Recorder r;
        r.Configure(dir, 60, 60, 900, 3);
        for (int i = 0; i < 300; ++i) r.Add(S(i * 0.5), false);   // 150 s, nothing alarming: no file
        CHECK(List(dir, L"blackbox_*.csv").empty());
        CHECK(!r.Recording());
        r.Add(S(150.0, 9.5f, L"Pin 3 at 9.50 A, above the limit"), true, "pin3-limit");
        CHECK(r.Recording());
        double t = 150.5;
        for (; t < 209.9; t += 0.5) r.Add(S(t), false);
        CHECK(r.Recording());                                      // still inside the minute after
        r.Add(S(210.0), false);
        CHECK(!r.Recording());
        std::wstring done = r.TakeFinished();
        CHECK(!done.empty());
        CHECK(r.TakeFinished().empty());                           // reported once
        CHECK(done.find(L"pin3-limit") != std::wstring::npos);
        std::string csv = ReadAll(done);
        CHECK(csv.rfind("time,pins_ok,p1_V", 0) == 0);
        CHECK(csv.find("above the limit") != std::string::npos);
        int rows = CountLines(csv) - 1;
        CHECK(rows >= 239 && rows <= 243);                         // ~60 s before + trigger + 60 s after at 0.5 s
    }
    {
        // a second alert during the recording extends it; the total is capped
        bbox::Recorder r;
        r.Configure(dir, 10, 10, 30, 3);
        r.Add(S(0), false);
        r.Add(S(1), true, "a");
        for (double t = 2; t <= 8; t += 1) r.Add(S(t), false);
        r.Add(S(9), true, "b");                                    // extends the end to 19
        for (double t = 10; t <= 18; t += 1) r.Add(S(t), false);
        CHECK(r.Recording());
        r.Add(S(19), false);
        CHECK(!r.Recording());
        CHECK(!r.TakeFinished().empty());
        // alerts every 5 s keep extending it, but never past start + 30 s
        int closedAt = -1;
        for (int t = 101; t <= 160 && closedAt < 0; ++t) {
            r.Add(S(t), t % 5 == 1, "c");
            if (!r.Recording()) closedAt = t;
        }
        CHECK(closedAt == 131);
    }
    {
        // old recordings are pruned (keep 3)
        bbox::Recorder r;
        r.Configure(dir, 1, 1, 10, 3);
        for (int k = 0; k < 4; ++k) {
            Sleep(1100);                                           // file names have 1 s resolution
            r.Add(S(k * 100.0), true, "x");
            r.Add(S(k * 100.0 + 2), false);
        }
        CHECK(List(dir, L"blackbox_*.csv").size() == 3);
    }
    {
        // unwritable folder: no crash, nothing recorded
        bbox::Recorder r;
        r.Configure(L"Z:\\does\\not\\exist\\corehud", 10, 10);
        r.Add(S(0), true, "x");
        CHECK(!r.Recording());
    }
    Wipe(dir);
}

int main() {
    TestWearAnalyze();
    TestWearFiles();
    TestBlackBox();
    printf("\nhistory: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
