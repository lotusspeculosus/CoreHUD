// NVAPI access (nvapi64.dll ships with the NVIDIA driver; loaded at runtime, no SDK needed).
// Used for: GPU enumeration, PCI ids, core voltage, hotspot/VRAM temps, and the
// READ-ONLY I2C access to the ITE IT8915FN per-pin monitor on ROG Astral cards.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace nvapi {

using GpuHandle = void*;

struct Gpu {
    GpuHandle handle = nullptr;
    std::string name;
    uint32_t deviceId = 0;      // (device << 16) | vendor, e.g. 0x2B8510DE
    uint32_t subSystemId = 0;   // (subdevice << 16) | subvendor, e.g. 0x8A611043
    uint32_t busId = 0;
};

// Per-pin connector reading. Index 0 = pin 1 ... index 5 = pin 6.
struct PinReading {
    double volts[6] = {};
    double amps[6] = {};
};

bool Init(std::string& err);
void Shutdown();
std::vector<Gpu> EnumGpus();

// True if the subsystem id is a card known to carry the per-pin sensor.
bool IsKnownAstral(uint32_t subSystemId, std::string* model = nullptr);

// Reads the 24 data bytes (6 rails x [mV BE16, mA BE16]) byte-by-byte from the
// sensor and validates them (every pin voltage must be 6..13 V, otherwise the
// read is treated as bogus: a failed read decodes to a "healthy" 0 A).
bool ReadPins(GpuHandle gpu, uint8_t port, uint8_t addr7, PinReading& out, std::string& err);

bool GetCoreVoltage(GpuHandle gpu, double& volts);                     // undocumented, optional
bool GetExtraTemps(GpuHandle gpu, double& hotspotC, double& vramC);    // undocumented, optional

} // namespace nvapi
