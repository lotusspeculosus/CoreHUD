// Sensor-panel layout model + persistence.
// A layout is a grid of cols x rows cells; widgets occupy whole cells, so the same layout
// scales to any panel resolution / aspect (1920x480 bar, 800x480, portrait 480x800, ...).
#pragma once
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace panel {

enum class WType { Value, Bar, Gauge, Graph, Pins, Clock, Text, Count };
enum class Bg { Card, Outline, None, Count };

const wchar_t* TypeName(WType t);
const wchar_t* BgName(Bg b);

constexpr int kColorCount = 9;
uint32_t AccentColor(int i);            // widget accent palette
constexpr int kPanelBgCount = 6;
uint32_t PanelBgColor(int i);

struct Widget {
    WType type = WType::Value;
    std::string sensor = "astral.conn_w";   // sensor key (see sensors.h)
    std::wstring text;                      // custom label (Text widget: the text itself); empty = sensor name
    int color = 0;                          // AccentColor index
    Bg bg = Bg::Card;
    int textScale = 100;                    // percent
    double min = NAN, max = NAN;            // NaN = auto
    double warn = NAN, crit = NAN;          // NaN = sensor default (pins: INI limits) / none
    int decimals = -1;                      // -1 = sensor default
    bool showLabel = true, showUnit = true;
    int graphSeconds = 60;
    int pinMode = 0;                        // Pins widget: 0 A, 1 W, 2 V
    int x = 0, y = 0, w = 3, h = 2;         // grid cells
};

struct Layout {
    int width = 1920, height = 480;         // panel resolution in pixels
    int cols = 16, rows = 4;
    int gap = 10, radius = 14;              // pixels at the layout resolution
    uint32_t bgColor = 0x0B0E12;
    std::wstring bgImage;
    int bgDim = 40;                         // % darkening over the image
    std::wstring display;                   // \\.\DISPLAYn
    bool autoShow = false;                  // open the panel window when the app starts
    bool snapOnDrop = true;                 // virtual panel dropped on another display -> fullscreen there
    std::vector<Widget> widgets;
};

std::wstring DefaultPath();                       // panel.txt next to the exe
bool Load(const std::wstring& path, Layout& out);
bool Save(const std::wstring& path, const Layout& l);
// Shareable single file (.rigpanel): the layout + the background image embedded as base64.
// Machine-specific settings (display, open-at-start) are not exported.
bool Export(const std::wstring& path, const Layout& l);
bool Import(const std::wstring& path, Layout& out, const std::wstring& imageDir, std::wstring& err);
Layout DefaultLayout();                           // 1920x480 starter layout
void AutoGrid(int width, int height, int& cols, int& rows);
// Friendly aspect ratio for a resolution: "16:9", "32:9", "21:9", "16:10", "4:1", "9:16" ... or "2.37:1".
std::wstring AspectLabel(int width, int height);
void Regrid(Layout& l, int cols, int rows);       // rescale widgets to a new grid
void Clamp(Layout& l);
bool FindFreeSpot(const Layout& l, int w, int h, int& x, int& y);
bool PlaceWidget(const Layout& l, Widget& w);     // free spot at w's size or smaller; false if the grid is full
void ResolveOverlaps(Layout& l);                  // moves overlapping widgets to free space where possible

} // namespace panel
