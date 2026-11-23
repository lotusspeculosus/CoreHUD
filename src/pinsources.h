// External per-pin 12V-2x6 meters (work with ANY graphics card):
//  - Thermal Grizzly WireView Pro II   USB CDC serial 0483:5740, command 0x04 -> 100-byte report
//  - ElmorLabs PMD2X                   USB CDC serial 0483:5740, command 0x04 -> 192-byte report
//  - MSI MPG Ai1300TS / Ai1600TS PSU   USB HID 0DB0:AA6F / 0DB0:808C, command 0xE0 (GPU Safeguard+)
// Only documented READ commands are sent (identity 0x01, sensors 0x04, PSU bulk read 0xE0);
// nothing that changes device settings, thresholds or firmware.
// Protocol references (MIT): github.com/Gustav0ar/wireview-pro-ii, github.com/ElmorLabs/PMD2-Python,
// github.com/boboom66/msi-psu-monitor.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace pins {

constexpr int kMaxPins = 12;   // MSI PSUs report two 12V-2x6 connectors

struct Extra {                 // additional values a device reports (temperatures, rail voltage, ...)
    std::string key;
    std::wstring name, unit;
    double value = 0;
    int decimals = 1;
};

struct Reading {
    int pinCount = 6;          // 6, or 12 for two connectors
    double volts[kMaxPins] = {};
    double amps[kMaxPins] = {};
    bool perPinVolts = true;   // false: device has one rail voltage (copied to every pin)
    std::vector<Extra> extras;
    uint16_t activeFaults = 0; // WireView: device-side fault bitmask (0 = none)
};

// --- pure decoders (unit-tested) ---
double Linear11(uint16_t word);
bool DecodeWireView(const uint8_t* buf, size_t len, Reading& out);   // len must be 100
bool DecodePmd2x(const uint8_t* buf, size_t len, Reading& out);      // len must be 192
bool DecodeMsiPsu(const uint8_t* data, size_t len, Reading& out);    // payload after the 0xE0 echo (>= 44 bytes)

// --- device access ---
class Sources {
public:
    ~Sources();
    // Finds a device (rescans at most every few seconds) and reads it. Returns false if no device or read failed.
    bool Read(Reading& out, std::string& err);
    bool Present() const { return kind_ != Kind::None; }
    std::wstring Name() const;           // "Thermal Grizzly WireView Pro II (COM5)" ...
    std::string Id() const;              // "wireview" / "pmd2x" / "msipsu" / ""
    void SetPreferred(const std::string& id) { preferred_ = id; }   // "auto", "wireview", "pmd2x", "msipsu", "off"
    void Close();

private:
    enum class Kind { None, WireView, Pmd2x, MsiPsu };
    bool Discover();
    bool ProbeSerial(const std::wstring& port);
    bool ProbeHid();
    bool ReadSerial(Reading& out, std::string& err);
    bool ReadHid(Reading& out, std::string& err);

    Kind kind_ = Kind::None;
    void* handle_ = nullptr;   // HANDLE
    std::wstring port_;
    std::wstring model_;
    std::string preferred_ = "auto";
    unsigned long lastScan_ = 0;
    int failures_ = 0;
    unsigned outLen_ = 0, inLen_ = 0;   // HID report sizes
};

} // namespace pins
