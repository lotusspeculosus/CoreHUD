#include "nvml_gpu.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdlib>

namespace nvml {
namespace {

using Ret = int;   // nvmlReturn_t, 0 == NVML_SUCCESS

struct PciInfo {   // nvmlPciInfo_t
    char busIdLegacy[16];
    unsigned domain, bus, device, pciDeviceId, pciSubSystemId;
    char busId[32];
};
struct Utilization { unsigned gpu, memory; };
struct Memory { unsigned long long total, free, used; };
struct FanSpeedInfo { unsigned version, fan, speed; };   // nvmlFanSpeedInfo_v1_t

HMODULE g_dll = nullptr;
Ret (*pInit)() = nullptr;
Ret (*pShutdown)() = nullptr;
Ret (*pCount)(unsigned*) = nullptr;
Ret (*pByIndex)(unsigned, Device*) = nullptr;
Ret (*pPci)(Device, PciInfo*) = nullptr;
Ret (*pUtil)(Device, Utilization*) = nullptr;
Ret (*pClock)(Device, int, unsigned*) = nullptr;
Ret (*pTemp)(Device, int, unsigned*) = nullptr;
Ret (*pMem)(Device, Memory*) = nullptr;
Ret (*pNumFans)(Device, unsigned*) = nullptr;
Ret (*pFanPct)(Device, unsigned, unsigned*) = nullptr;
Ret (*pFanRpm)(Device, FanSpeedInfo*) = nullptr;
Ret (*pPower)(Device, unsigned*) = nullptr;
Ret (*pPowerLimit)(Device, unsigned*) = nullptr;

template <typename T> void Load(T& fn, const char* name) {
    fn = reinterpret_cast<T>(GetProcAddress(g_dll, name));
}

} // namespace

bool Init(std::string& err) {
    if (g_dll) return true;
    if (getenv("COREHUD_SIMULATE_NO_NVIDIA")) { err = "simulated: no NVIDIA driver"; return false; }
    g_dll = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!g_dll) {
        wchar_t path[MAX_PATH];
        ExpandEnvironmentStringsW(L"%ProgramW6432%\\NVIDIA Corporation\\NVSMI\\nvml.dll", path, MAX_PATH);
        g_dll = LoadLibraryW(path);
    }
    if (!g_dll) { err = "nvml.dll not found"; return false; }
    Load(pInit, "nvmlInit_v2");
    Load(pShutdown, "nvmlShutdown");
    Load(pCount, "nvmlDeviceGetCount_v2");
    Load(pByIndex, "nvmlDeviceGetHandleByIndex_v2");
    Load(pPci, "nvmlDeviceGetPciInfo_v3");
    Load(pUtil, "nvmlDeviceGetUtilizationRates");
    Load(pClock, "nvmlDeviceGetClockInfo");
    Load(pTemp, "nvmlDeviceGetTemperature");
    Load(pMem, "nvmlDeviceGetMemoryInfo");
    Load(pNumFans, "nvmlDeviceGetNumFans");
    Load(pFanPct, "nvmlDeviceGetFanSpeed_v2");
    Load(pFanRpm, "nvmlDeviceGetFanSpeedRPM");
    Load(pPower, "nvmlDeviceGetPowerUsage");
    Load(pPowerLimit, "nvmlDeviceGetEnforcedPowerLimit");
    if (!pInit || !pCount || !pByIndex || pInit() != 0) {
        err = "nvmlInit failed";
        FreeLibrary(g_dll);
        g_dll = nullptr;
        return false;
    }
    return true;
}

void Shutdown() {
    if (!g_dll) return;
    if (pShutdown) pShutdown();
    FreeLibrary(g_dll);
    g_dll = nullptr;
}

Device FindByBus(uint32_t bus) {
    if (!g_dll) return nullptr;
    unsigned n = 0;
    if (pCount(&n) != 0) return nullptr;
    Device first = nullptr;
    for (unsigned i = 0; i < n; ++i) {
        Device d = nullptr;
        if (pByIndex(i, &d) != 0) continue;
        if (!first) first = d;
        PciInfo pci{};
        if (pPci && pPci(d, &pci) == 0 && pci.bus == bus) return d;
    }
    return n == 1 ? first : nullptr;
}

bool Read(Device dev, Stats& s) {
    if (!g_dll || !dev) return false;
    s = Stats{};
    unsigned v = 0;
    Utilization u{};
    if (pUtil && pUtil(dev, &u) == 0) s.gpuUtil = int(u.gpu);
    if (pClock && pClock(dev, 0, &v) == 0) s.coreMHz = int(v);   // NVML_CLOCK_GRAPHICS
    if (pClock && pClock(dev, 2, &v) == 0) s.memMHz = int(v);    // NVML_CLOCK_MEM
    if (pTemp && pTemp(dev, 0, &v) == 0) s.tempC = int(v);       // NVML_TEMPERATURE_GPU
    Memory m{};
    if (pMem && pMem(dev, &m) == 0) { s.memUsed = m.used; s.memTotal = m.total; }
    unsigned fans = 0;
    if (pNumFans && pNumFans(dev, &fans) == 0) {
        s.fanCount = int(fans > 4 ? 4 : fans);
        for (int f = 0; f < s.fanCount; ++f) {
            if (pFanPct && pFanPct(dev, unsigned(f), &v) == 0) s.fanPct[f] = int(v);
            if (pFanRpm) {
                FanSpeedInfo fi{};
                fi.version = unsigned(sizeof(fi)) | (1u << 24);
                fi.fan = unsigned(f);
                if (pFanRpm(dev, &fi) == 0) s.fanRpm[f] = int(fi.speed);
            }
        }
    }
    if (pPower && pPower(dev, &v) == 0) s.powerW = v / 1000.0;
    if (pPowerLimit && pPowerLimit(dev, &v) == 0) s.powerLimitW = v / 1000.0;
    return true;
}

} // namespace nvml
