// Sensor registry for the panel: every value the panel can show, from
//  - built-in sources (per-pin connector sensor, GPU stats for NVIDIA/AMD/Intel, CPU load, RAM)
//  - HWiNFO64 shared memory (all sensors HWiNFO sees; needs "Shared Memory Support" on)
//  - RTSS shared memory (framerate / frame time of the foreground 3D app)
#pragma once
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>
#include "monitor.h"
#include "gpu_generic.h"

namespace sensors {

enum Kind { K_OTHER, K_TEMP, K_VOLT, K_FAN, K_CURRENT, K_POWER, K_CLOCK, K_USAGE, K_FPS };

struct Sensor {
    std::string key;           // stable id stored in the layout ("astral.conn_w", "hwi.f000.0.1000000", "rtss.fps")
    std::wstring group, name, unit;
    double value = 0;
    bool ok = false;
    double rangeMax = 100;     // default range for bars/gauges
    double warn = NAN, crit = NAN;   // default alert levels (NaN = none)
    int decimals = 1;
    int kind = K_OTHER;
};

class Registry {
public:
    ~Registry();
    void Shutdown();            // release COM / shared memory (call before CoUninitialize)
    void Update(const mon::Snapshot& s);
    const std::vector<Sensor>& All() const { return list_; }
    const Sensor* Find(const std::string& key) const;

    bool hwinfoOk = false;      // HWiNFO shared memory (Pro / 12 h in free version) - optional
    int hwinfoCount = 0;
    bool gadgetOk = false;      // HWiNFO "Gadget" registry values (free, no time limit)
    int gadgetCount = 0;
    bool lhmOk = false;         // LibreHardwareMonitor (free, open source): web server (0.9.5+) or WMI (older)
    int lhmCount = 0;
    bool lhmProcess = false;    // LibreHardwareMonitor.exe is running
    std::wstring lhmVia;        // "web server" / "WMI"
    bool rtssOk = false;
    std::wstring rtssApp;
    std::wstring gpuName;       // main GPU (any vendor) behind the gpu.* sensors
    bool gpuViaWindows = false; // gpu.* comes from Windows counters (no NVML)
    bool gpuFromLhm = false;    // gpu.* was completed with LibreHardwareMonitor values (AMD / Intel detail)
    const gpug::Monitor& Gpus() const { return gpus_; }

private:
    void AddBuiltin(const mon::Snapshot& s);
    void AddSystem();
    void AddGpusGeneric(bool nvml);
    void AddHwinfo();
    void AddHwinfoGadget();
    struct LhmItem { std::wstring id, hwId, hwName, name, type; double value; bool have; };
    void AddLhm(int port);
    bool LhmViaWmi(std::vector<LhmItem>& out);
    bool LhmViaHttp(std::vector<LhmItem>& out, int port);
    std::vector<LhmItem> lhmCache_;
    unsigned long lhmCacheTick_ = 0, lhmHttpFailTick_ = 0;
    void* http_ = nullptr;   // HINTERNET session
    void EnrichGpuFromLhm();
    struct LhmGpuSensor { std::wstring parent, hwName, type, name; double value; };
    std::vector<LhmGpuSensor> lhmGpu_;
    void AddRtss();
    Sensor& Add(const std::string& key, const std::wstring& group, const std::wstring& name, const std::wstring& unit,
                double value, bool ok, double rangeMax, int decimals, int kind);

    std::vector<Sensor> list_;
    std::unordered_map<std::string, size_t> index_;
    std::unordered_map<std::string, double> peak_;

    void* hwiMap_ = nullptr;
    const unsigned char* hwiView_ = nullptr;
    unsigned long hwiRetryTick_ = 0;
    void* rtssMap_ = nullptr;
    const unsigned char* rtssView_ = nullptr;
    unsigned long rtssRetryTick_ = 0;

    void* wmiLocator_ = nullptr;      // IWbemLocator*
    void* wmiServices_ = nullptr;     // IWbemServices*
    unsigned long lhmRetryTick_ = 0;
    std::unordered_map<std::wstring, std::wstring> lhmHardware_;
    unsigned long lhmHardwareTick_ = 0;

    gpug::Monitor gpus_;

    unsigned long long cpuIdle_ = 0, cpuTotal_ = 0;
    double cpuLoad_ = -1;
};

} // namespace sensors
