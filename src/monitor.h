// Background sampler: reads the per-pin sensor + GPU stats on a thread, keeps history,
// raises threshold events and (optionally) writes a CSV log.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "nvapi_gpu.h"
#include "nvml_gpu.h"
#include "pinsources.h"

namespace mon {

struct Config {
    int pollMs = 500;          // sample interval
    double limitA = 9.2;       // per-pin rated current (12V-2x6 terminal ~9.2 A)
    double warnA = 8.0;        // per-pin warning level
    double spreadWarnA = 2.5;  // max-min pin current warning level
    int i2cPort = 1;           // NVAPI I2C port of the sensor
    int i2cAddr = 0x2B;        // 7-bit address of the IT8915FN
    int gpuIndex = -1;         // -1 = auto (first known Astral, else first GPU)
    bool force = false;        // also read on cards not in the known-subsystem list
    std::string pinSource = "auto";   // auto | astral | wireview | pmd2x | msipsu | off
    int lhmPort = 8085;               // LibreHardwareMonitor web server port (Options > Remote Web Server)
    bool beep = true;          // beep on critical
    bool csv = false;          // start with CSV logging on
    int criticalAction = 0;    // 0 = notify only, 1 = start a 60 s Windows shutdown countdown
    int criticalSeconds = 5;   // how long a pin must stay over the limit before the action
};

Config LoadConfig(const std::wstring& iniPath);   // writes a default INI if missing
std::wstring DefaultIniPath();                     // next to the exe
std::wstring ExeDir();

struct Event {
    uint64_t seq = 0;          // increasing id (for notifications)
    std::wstring time;
    int level = 0;             // 0 info, 1 warning, 2 critical
    std::wstring text;
};

struct HistPoint {
    double t = 0;              // seconds since start
    bool ok = false;           // pins valid
    float amps[6] = {};
    float volts[6] = {};
    float connW = 0;
    float boardW = -1;
};

struct Snapshot {
    bool started = false;
    bool nvapiOk = false;
    std::string initError;
    bool haveGpu = false;
    std::string gpuName;
    uint32_t deviceId = 0, subSystemId = 0, busId = 0;
    std::string model;
    bool knownCard = false;
    bool sensorEnabled = false;  // a per-pin source exists (on-card sensor or external meter)
    bool pinsOk = false;
    std::string pinError;
    std::wstring pinSource;      // "ROG Astral on-card sensor", "Thermal Grizzly WireView Pro II (COM5)", ...
    std::string pinSourceId;     // astral | wireview | pmd2x | msipsu
    int pinCount = 6;            // 12 when an MSI PSU reports two loaded connectors (pins 7-12 = connector 2)
    bool perPinVolts = true;
    double volts[pins::kMaxPins] = {}, amps[pins::kMaxPins] = {};
    double peakA[pins::kMaxPins] = {};
    double peakConnW = 0;
    double minVolts = 0;
    int pinState[pins::kMaxPins] = {};   // 0 ok, 1 warn, 2 critical
    std::vector<pins::Extra> pinExtras;  // extra values from external meters (temps, PSU rail, ...)
    bool nvmlOk = false;
    nvml::Stats stats;
    double coreV = -1, hotspotC = -1, vramC = -1;
    uint64_t samples = 0, failures = 0;
    double readMs = 0;
    bool csvOn = false;
    std::wstring csvPath;
    Config cfg;
};

void Start(const Config& cfg, void* notifyHwnd, unsigned notifyMsg);
void Stop();
void Restart(const Config& cfg);
void ResetPeaks();
void SetCsv(bool on);

void Get(Snapshot& out);
void GetHistory(std::vector<HistPoint>& out);
void GetEvents(std::vector<Event>& out);

} // namespace mon
