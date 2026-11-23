// Vendor-neutral GPU monitoring (AMD, Intel, NVIDIA) using only what Windows provides:
//  - DXGI: adapter list, names, vendor ids, dedicated VRAM size
//  - PDH "GPU Engine" / "GPU Adapter Memory" counters (what Task Manager shows): load, VRAM in use
//  - D3DKMT adapter perf data (WDDM 2.4+ drivers): temperature, fan RPM, power %, memory clock
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace gpug {

struct Stats {
    double util = -1;          // % (busiest engine, like Task Manager)
    double vramUsedGB = -1;
    double vramTotalGB = 0;
    double tempC = -1;
    int fanRpm = -1;
    double powerPct = -1;      // % of the board's power limit
    int memMHz = -1;
};

struct Adapter {
    uint32_t luidLow = 0;
    int32_t luidHigh = 0;
    std::wstring name;
    uint32_t vendor = 0, device = 0, subsys = 0;
    uint64_t dedicatedBytes = 0;
    std::string key;           // stable across reboots: "gpux.<vendor>_<device>_<n>"
    Stats stats;
};

const wchar_t* VendorName(uint32_t vendorId);   // "NVIDIA", "AMD", "Intel", ...

class Monitor {
public:
    Monitor();
    ~Monitor();
    void Update();                                   // enumerate (first call) + sample
    const std::vector<Adapter>& Adapters() const { return adapters_; }
    const Adapter* Primary() const;                  // discrete GPU with the most VRAM

private:
    void Enumerate();
    std::vector<Adapter> adapters_;
    std::vector<unsigned> kmtHandles_;
    void* query_ = nullptr;
    void* engineCounter_ = nullptr;
    void* memCounter_ = nullptr;
    bool enumerated_ = false;
};

} // namespace gpug
