#include "panel.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincrypt.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "monitor.h"

namespace panel {
namespace {

const char* kTypeIds[] = {"value", "bar", "gauge", "graph", "pins", "clock", "text"};
const char* kBgIds[] = {"card", "outline", "none"};
const uint32_t kAccents[kColorCount] = {0x2FD9C4, 0x4C9AFF, 0xFF8A3D, 0x3DDC84, 0xFF5FA2, 0xFFC93C, 0xA78BFA, 0xF2F4F7, 0xFF5252};
const uint32_t kPanelBgs[kPanelBgCount] = {0x000000, 0x0B0E12, 0x111827, 0x0D1F2D, 0x1A1025, 0x1F1F1F};

std::string U8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring W16(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}
std::string Num(double v) {
    if (std::isnan(v)) return "";
    char b[32];
    snprintf(b, sizeof b, "%g", v);
    return b;
}
double ParseNum(const std::string& s) {
    if (s.empty()) return NAN;
    char* e = nullptr;
    double v = strtod(s.c_str(), &e);
    return e == s.c_str() ? NAN : v;
}
int ParseInt(const std::string& s, int def) {
    double v = ParseNum(s);
    return std::isnan(v) ? def : int(v);
}

// Splits "k=v k=v ... text=rest of line" (text/image keep spaces).
std::vector<std::pair<std::string, std::string>> Tokens(const std::string& line) {
    std::vector<std::pair<std::string, std::string>> out;
    std::string head = line, tail;
    size_t tp = line.find(" text=");
    if (tp != std::string::npos) { head = line.substr(0, tp); tail = line.substr(tp + 6); }
    size_t i = 0;
    while (i < head.size()) {
        while (i < head.size() && head[i] == ' ') ++i;
        size_t j = head.find(' ', i);
        if (j == std::string::npos) j = head.size();
        std::string tok = head.substr(i, j - i);
        size_t eq = tok.find('=');
        if (eq != std::string::npos) out.push_back({tok.substr(0, eq), tok.substr(eq + 1)});
        i = j;
    }
    if (tp != std::string::npos) out.push_back({"text", tail});
    return out;
}

} // namespace

const wchar_t* TypeName(WType t) {
    static const wchar_t* n[] = {L"Value", L"Bar", L"Gauge", L"Graph", L"Pins", L"Clock", L"Text"};
    return n[int(t)];
}
const wchar_t* BgName(Bg b) {
    static const wchar_t* n[] = {L"Card", L"Outline", L"None"};
    return n[int(b)];
}
uint32_t AccentColor(int i) { return kAccents[std::clamp(i, 0, kColorCount - 1)]; }
uint32_t PanelBgColor(int i) { return kPanelBgs[std::clamp(i, 0, kPanelBgCount - 1)]; }

std::wstring DefaultPath() { return mon::ExeDir() + L"\\panel.txt"; }

bool Save(const std::wstring& path, const Layout& l) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    fprintf(f, "# RigSense sensor panel layout - edit it in the app (Sensor panel tab)\n");
    fprintf(f, "panel width=%d height=%d cols=%d rows=%d gap=%d radius=%d bg=%06X dim=%d autoshow=%d snap=%d display=%s\n", l.width,
            l.height, l.cols, l.rows, l.gap, l.radius, l.bgColor, l.bgDim, l.autoShow ? 1 : 0, l.snapOnDrop ? 1 : 0, U8(l.display).c_str());
    fprintf(f, "image text=%s\n", U8(l.bgImage).c_str());
    for (auto& w : l.widgets) {
        fprintf(f,
                "widget type=%s sensor=%s x=%d y=%d w=%d h=%d color=%d bg=%s scale=%d min=%s max=%s warn=%s crit=%s dec=%d "
                "showlabel=%d showunit=%d graph=%d pins=%d text=%s\n",
                kTypeIds[int(w.type)], w.sensor.c_str(), w.x, w.y, w.w, w.h, w.color, kBgIds[int(w.bg)], w.textScale,
                Num(w.min).c_str(), Num(w.max).c_str(), Num(w.warn).c_str(), Num(w.crit).c_str(), w.decimals,
                w.showLabel ? 1 : 0, w.showUnit ? 1 : 0, w.graphSeconds, w.pinMode, U8(w.text).c_str());
    }
    fclose(f);
    return true;
}

bool Load(const std::wstring& path, Layout& out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    Layout l;
    char buf[4096];
    while (fgets(buf, sizeof buf, f)) {
        std::string line = buf;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (line.rfind("panel ", 0) == 0) {
            for (auto& [k, v] : Tokens(line)) {
                if (k == "width") l.width = ParseInt(v, l.width);
                else if (k == "height") l.height = ParseInt(v, l.height);
                else if (k == "cols") l.cols = ParseInt(v, l.cols);
                else if (k == "rows") l.rows = ParseInt(v, l.rows);
                else if (k == "gap") l.gap = ParseInt(v, l.gap);
                else if (k == "radius") l.radius = ParseInt(v, l.radius);
                else if (k == "bg") l.bgColor = uint32_t(strtoul(v.c_str(), nullptr, 16));
                else if (k == "dim") l.bgDim = ParseInt(v, l.bgDim);
                else if (k == "autoshow") l.autoShow = v == "1";
                else if (k == "snap") l.snapOnDrop = v != "0";
                else if (k == "display") l.display = W16(v);
            }
        } else if (line.rfind("image ", 0) == 0) {
            for (auto& [k, v] : Tokens(line)) if (k == "text") l.bgImage = W16(v);
        } else if (line.rfind("widget ", 0) == 0) {
            Widget w;
            for (auto& [k, v] : Tokens(line)) {
                if (k == "type") { for (int i = 0; i < int(WType::Count); ++i) if (v == kTypeIds[i]) w.type = WType(i); }
                else if (k == "bg") { for (int i = 0; i < int(Bg::Count); ++i) if (v == kBgIds[i]) w.bg = Bg(i); }
                else if (k == "sensor") w.sensor = v;
                else if (k == "x") w.x = ParseInt(v, 0);
                else if (k == "y") w.y = ParseInt(v, 0);
                else if (k == "w") w.w = ParseInt(v, 1);
                else if (k == "h") w.h = ParseInt(v, 1);
                else if (k == "color") w.color = ParseInt(v, 0);
                else if (k == "scale") w.textScale = ParseInt(v, 100);
                else if (k == "min") w.min = ParseNum(v);
                else if (k == "max") w.max = ParseNum(v);
                else if (k == "warn") w.warn = ParseNum(v);
                else if (k == "crit") w.crit = ParseNum(v);
                else if (k == "dec") w.decimals = ParseInt(v, -1);
                else if (k == "showlabel") w.showLabel = v != "0";
                else if (k == "showunit") w.showUnit = v != "0";
                else if (k == "graph") w.graphSeconds = ParseInt(v, 60);
                else if (k == "pins") w.pinMode = ParseInt(v, 0);
                else if (k == "text") w.text = W16(v);
            }
            l.widgets.push_back(w);
        }
    }
    fclose(f);
    Clamp(l);
    out = l;
    return true;
}

bool Export(const std::wstring& path, const Layout& l) {
    Layout c = l;
    c.display.clear();
    c.autoShow = false;
    std::wstring imgName;
    std::vector<unsigned char> img;
    if (!c.bgImage.empty()) {
        FILE* f = _wfopen(c.bgImage.c_str(), L"rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (n > 0 && n < 40 * 1024 * 1024) {
                img.resize(size_t(n));
                if (fread(img.data(), 1, img.size(), f) != img.size()) img.clear();
            }
            fclose(f);
        }
        size_t slash = c.bgImage.find_last_of(L"\\/");
        imgName = slash == std::wstring::npos ? c.bgImage : c.bgImage.substr(slash + 1);
        c.bgImage = imgName;   // path is meaningless on another PC
    }
    if (!Save(path, c)) return false;
    if (!img.empty()) {
        DWORD len = 0;
        CryptBinaryToStringA(img.data(), DWORD(img.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &len);
        std::string b64(len, '\0');
        CryptBinaryToStringA(img.data(), DWORD(img.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64.data(), &len);
        b64.resize(len);
        FILE* f = _wfopen(path.c_str(), L"ab");
        if (!f) return false;
        std::string name = U8(imgName);
        std::replace(name.begin(), name.end(), ' ', '_');
        fprintf(f, "imagedata name=%s data=%s\n", name.c_str(), b64.c_str());
        fclose(f);
    }
    return true;
}

bool Import(const std::wstring& path, Layout& out, const std::wstring& imageDir, std::wstring& err) {
    Layout l;
    if (!Load(path, l)) { err = L"Could not read the file"; return false; }
    bool any = false;
    {
        FILE* f = _wfopen(path.c_str(), L"rb");
        if (!f) { err = L"Could not read the file"; return false; }
        std::string all;
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) all.append(buf, n);
        fclose(f);
        any = all.find("panel ") != std::string::npos;
        size_t p = all.find("\nimagedata ");
        l.bgImage.clear();
        if (p != std::string::npos) {
            size_t e = all.find('\n', p + 1);
            std::string line = all.substr(p + 1, e == std::string::npos ? std::string::npos : e - p - 1);
            std::string name, data;
            for (auto& [k, v] : Tokens(line)) {
                if (k == "name") name = v;
                else if (k == "data") data = v;
            }
            for (char& ch : name) if (ch == '\\' || ch == '/' || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|') ch = '_';
            if (name.empty()) name = "background.png";
            DWORD len = 0;
            if (!data.empty() && CryptStringToBinaryA(data.c_str(), DWORD(data.size()), CRYPT_STRING_BASE64, nullptr, &len, nullptr, nullptr)) {
                std::vector<unsigned char> img(len);
                CryptStringToBinaryA(data.c_str(), DWORD(data.size()), CRYPT_STRING_BASE64, img.data(), &len, nullptr, nullptr);
                CreateDirectoryW(imageDir.c_str(), nullptr);
                std::wstring dst = imageDir + L"\\" + W16(name);
                FILE* o = _wfopen(dst.c_str(), L"wb");
                if (o) {
                    fwrite(img.data(), 1, len, o);
                    fclose(o);
                    l.bgImage = dst;
                }
            }
        }
    }
    if (!any) { err = L"This is not an RigSense panel file"; return false; }
    out = l;
    return true;
}

Layout DefaultLayout() {
    Layout l;
    auto add = [&](WType t, const char* sensor, int x, int y, int w, int h, int color = 0) {
        Widget wd;
        wd.type = t;
        wd.sensor = sensor;
        wd.x = x; wd.y = y; wd.w = w; wd.h = h;
        wd.color = color;
        l.widgets.push_back(wd);
        return &l.widgets.back();
    };
    add(WType::Pins, "astral.pin1_a", 0, 0, 5, 4)->text = L"12V-2x6 pins";
    add(WType::Gauge, "astral.conn_w", 5, 0, 3, 4);
    add(WType::Value, "astral.max_pin_a", 8, 0, 2, 2);
    add(WType::Value, "gpu.temp", 10, 0, 2, 2, 2);
    add(WType::Value, "gpu.vram_temp", 12, 0, 2, 2, 2);
    add(WType::Clock, "", 14, 0, 2, 2, 7);
    add(WType::Graph, "rtss.fps", 8, 2, 4, 2, 1)->text = L"FPS";
    add(WType::Bar, "gpu.util", 12, 2, 4, 1, 1);
    add(WType::Bar, "sys.cpu", 12, 3, 4, 1, 3);
    return l;
}

std::wstring AspectLabel(int w, int h) {
    if (w <= 0 || h <= 0) return L"";
    bool portrait = h > w;
    double r = portrait ? double(h) / w : double(w) / h;
    // common screen / panel shapes; "21:9" covers the 2560x1080 (2.37), 3440x1440 (2.39) and 3840x1600 (2.4) family
    struct Named { double ratio; const wchar_t* land; const wchar_t* port; double tol; };
    static const Named kNamed[] = {
        {1.0, L"1:1", L"1:1", 0.02},       {5.0 / 4, L"5:4", L"4:5", 0.015},   {4.0 / 3, L"4:3", L"3:4", 0.015},
        {3.0 / 2, L"3:2", L"2:3", 0.015},   {16.0 / 10, L"16:10", L"10:16", 0.015}, {5.0 / 3, L"5:3", L"3:5", 0.012},
        {16.0 / 9, L"16:9", L"9:16", 0.02}, {2.0, L"2:1", L"1:2", 0.02},        {2.37, L"21:9", L"9:21", 0.035},
        {3.0, L"3:1", L"1:3", 0.03},        {32.0 / 9, L"32:9", L"9:32", 0.02},  {4.0, L"4:1", L"1:4", 0.03},
        {5.0, L"5:1", L"1:5", 0.03},        {6.0, L"6:1", L"1:6", 0.03},
    };
    for (auto& n : kNamed)
        if (std::fabs(r - n.ratio) / n.ratio <= n.tol) return portrait ? n.port : n.land;
    wchar_t b[32];
    if (portrait) swprintf(b, 32, L"1:%.2f", r);
    else swprintf(b, 32, L"%.2f:1", r);
    return b;
}

void AutoGrid(int width, int height, int& cols, int& rows) {
    cols = std::clamp(int(std::lround(width / 120.0)), 2, 32);
    rows = std::clamp(int(std::lround(height / 120.0)), 1, 32);
}

void Regrid(Layout& l, int cols, int rows) {
    cols = std::clamp(cols, 1, 48);
    rows = std::clamp(rows, 1, 48);
    for (auto& w : l.widgets) {
        int x0 = int(std::lround(double(w.x) * cols / l.cols)), x1 = int(std::lround(double(w.x + w.w) * cols / l.cols));
        int y0 = int(std::lround(double(w.y) * rows / l.rows)), y1 = int(std::lround(double(w.y + w.h) * rows / l.rows));
        w.x = x0; w.w = std::max(1, x1 - x0);
        w.y = y0; w.h = std::max(1, y1 - y0);
    }
    l.cols = cols;
    l.rows = rows;
    Clamp(l);
    ResolveOverlaps(l);
}

bool PlaceWidget(const Layout& l, Widget& w) {
    // preferred size first, then progressively smaller footprints
    for (int h = std::min(w.h, l.rows); h >= 1; --h)
        for (int ww = std::min(w.w, l.cols); ww >= 1; --ww)
            if (FindFreeSpot(l, ww, h, w.x, w.y)) { w.w = ww; w.h = h; return true; }
    return false;
}

void ResolveOverlaps(Layout& l) {
    auto overlaps = [](const Widget& a, const Widget& b) {
        return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
    };
    const int maxIter = 64 * int(l.widgets.size() + 1) * (l.cols + l.rows);
    for (int iter = 0; iter < maxIter; ++iter) {
        size_t a = 0, b = 0;
        bool found = false;
        for (size_t i = 0; i < l.widgets.size() && !found; ++i)
            for (size_t j = i + 1; j < l.widgets.size() && !found; ++j)
                if (overlaps(l.widgets[i], l.widgets[j])) { a = i; b = j; found = true; }
        if (!found) return;
        size_t small = l.widgets[a].w * l.widgets[a].h <= l.widgets[b].w * l.widgets[b].h ? a : b;
        size_t bigI = small == a ? b : a;
        // 1) move the smaller widget into free space (shrinking it if needed)
        Layout others = l;
        others.widgets.erase(others.widgets.begin() + long(small));
        Widget w = l.widgets[small];
        if (PlaceWidget(others, w)) { l.widgets[small] = w; continue; }
        // 2) no free space: shrink the bigger widget away from the smaller one
        Widget& big = l.widgets[bigI];
        const Widget& sm = l.widgets[small];
        bool canX = big.w > 1, canY = big.h > 1;
        if (!canX && !canY) {
            // both are 1x1: free a cell by shrinking the largest widget on the panel, then retry
            size_t k = 0;
            int best = 1;
            for (size_t i = 0; i < l.widgets.size(); ++i)
                if (l.widgets[i].w * l.widgets[i].h > best) { best = l.widgets[i].w * l.widgets[i].h; k = i; }
            if (best <= 1) return;   // more widgets than cells: nothing sensible left
            Widget& g = l.widgets[k];
            if (g.w >= g.h) g.w--; else g.h--;
            continue;
        }
        bool useX = canX && (!canY || big.w >= big.h);
        if (useX) {
            if (sm.x * 2 + sm.w >= big.x * 2 + big.w) big.w--;
            else { big.x++; big.w--; }
        } else {
            if (sm.y * 2 + sm.h >= big.y * 2 + big.h) big.h--;
            else { big.y++; big.h--; }
        }
    }
}

void Clamp(Layout& l) {
    l.width = std::clamp(l.width, 64, 7680);
    l.height = std::clamp(l.height, 64, 7680);
    l.cols = std::clamp(l.cols, 1, 48);
    l.rows = std::clamp(l.rows, 1, 48);
    l.gap = std::clamp(l.gap, 0, 64);
    l.radius = std::clamp(l.radius, 0, 64);
    l.bgDim = std::clamp(l.bgDim, 0, 95);
    for (auto& w : l.widgets) {
        w.w = std::clamp(w.w, 1, l.cols);
        w.h = std::clamp(w.h, 1, l.rows);
        w.x = std::clamp(w.x, 0, l.cols - w.w);
        w.y = std::clamp(w.y, 0, l.rows - w.h);
        w.color = std::clamp(w.color, 0, kColorCount - 1);
        w.textScale = std::clamp(w.textScale, 30, 300);
        w.decimals = std::clamp(w.decimals, -1, 4);
        w.graphSeconds = std::clamp(w.graphSeconds, 10, 600);
        w.pinMode = std::clamp(w.pinMode, 0, 2);
    }
}

bool FindFreeSpot(const Layout& l, int w, int h, int& outX, int& outY) {
    std::vector<char> used(size_t(l.cols * l.rows), 0);
    for (auto& wd : l.widgets)
        for (int y = wd.y; y < wd.y + wd.h; ++y)
            for (int x = wd.x; x < wd.x + wd.w; ++x)
                if (x < l.cols && y < l.rows) used[size_t(y * l.cols + x)] = 1;
    for (int y = 0; y + h <= l.rows; ++y)
        for (int x = 0; x + w <= l.cols; ++x) {
            bool free = true;
            for (int yy = y; yy < y + h && free; ++yy)
                for (int xx = x; xx < x + w && free; ++xx)
                    if (used[size_t(yy * l.cols + xx)]) free = false;
            if (free) { outX = x; outY = y; return true; }
        }
    return false;
}

} // namespace panel
