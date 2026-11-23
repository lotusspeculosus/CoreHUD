// Which power connector(s) the graphics card has: one 12V-2x6 (16-pin), one to three 8-pin PCIe, or none
// (slot power only). There is no driver API for this, so it is read from the power-rail telemetry that
// monitoring tools expose ("GPU 16-pin HVPWR Voltage", "GPU 8-pin #2 Input Power", "12VHPWR Pin 3", ...),
// from a per-pin sensor/meter seeing current, or set by the user in Settings when the card reports nothing.
#pragma once
#include <cmath>
#include <string>
#include <vector>

namespace conn {

enum Layout { UNKNOWN = 0, HPWR16, PCIE8_1, PCIE8_2, PCIE8_3, NONE };

struct Input {                 // one sensor from the registry
    std::wstring group, name, unit;
    double value = 0;
    bool ok = false;
};

struct Rail {                  // one 8-pin input with whatever the card reports for it
    int index = 1;             // "#1".."#3"
    double watts = NAN, volts = NAN, amps = NAN;
    double Power() const;      // watts, or volts x amps, or NAN
};

struct Result {
    Layout layout = UNKNOWN;
    bool fromTelemetry = false;    // detected (not set by the user)
    std::wstring how;              // "GPU 16-pin HVPWR sensor", "2 GPU 8-pin rails", "per-pin sensor", "set in Settings"
    std::vector<Rail> rails;       // 8-pin rails found (sorted by index)
    double hpwrWatts = NAN;        // 16-pin power if reported
};

// perPinCurrent: a per-pin source (on-card sensor / USB meter) reads real current on a 12V-2x6 cable.
Result Detect(const std::vector<Input>& sensors, bool perPinCurrent);
// The user's choice wins unless it is "auto" (UNKNOWN); detection fills in otherwise.
Result Effective(const Result& detected, Layout userChoice);

int PinConnectors(Layout l);           // number of physical connectors (0 for NONE/UNKNOWN)
const wchar_t* Name(Layout l);         // "12V-2x6 (16-pin)", "2× 8-pin PCIe", ...
const wchar_t* Id(Layout l);           // ini value: auto, 12v2x6, 8pin1, 8pin2, 8pin3, none
Layout FromId(const std::wstring& id);

} // namespace conn
