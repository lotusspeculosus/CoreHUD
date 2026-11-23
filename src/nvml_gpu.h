// NVML (nvml.dll ships with the NVIDIA driver; loaded at runtime). Documented API for
// usage, clocks, temperature, VRAM, fans and board power.
#pragma once
#include <cstdint>
#include <string>

namespace nvml {

using Device = void*;

struct Stats {
    int gpuUtil = -1;            // %
    int coreMHz = -1;
    int memMHz = -1;
    int tempC = -1;
    uint64_t memUsed = 0, memTotal = 0;
    int fanCount = 0;
    int fanPct[4] = {-1, -1, -1, -1};
    int fanRpm[4] = {-1, -1, -1, -1};
    double powerW = -1;          // board power as reported by the driver
    double powerLimitW = -1;
};

bool Init(std::string& err);
void Shutdown();
Device FindByBus(uint32_t bus);  // nullptr if not found
bool Read(Device dev, Stats& out);

} // namespace nvml
