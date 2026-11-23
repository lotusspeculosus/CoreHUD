#include "nvapi_gpu.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace nvapi {
namespace {

using Status = int;               // NvAPI_Status, 0 == NVAPI_OK
constexpr Status NVAPI_OK = 0;

// NVAPI_I2C_SPEED_DEPRECATED / NVAPI_I2C_SPEED_DEFAULT
constexpr uint32_t I2C_SPEED_DEPRECATED = 0xFFFF;
constexpr uint32_t I2C_SPEED_DEFAULT = 0;

#pragma pack(push, 8)
struct NV_I2C_INFO_V3 {
    uint32_t version;
    uint32_t displayMask;
    uint8_t  bIsDDCPort;
    uint8_t  i2cDevAddress;     // 8-bit form: 7-bit address << 1
    uint8_t* pbI2cRegAddress;
    uint32_t regAddrSize;
    uint8_t* pbData;
    uint32_t cbSize;
    uint32_t i2cSpeed;          // deprecated, must be 0xFFFF
    uint32_t i2cSpeedKhz;       // NV_I2C_SPEED
    uint8_t  portId;
    uint32_t bIsPortIdSet;
};

struct NV_VOLT_STATUS_V1 {      // NvAPI_GPU_ClientVoltRailsGetStatus
    uint32_t version;
    uint32_t flags;
    uint32_t reserved1[8];
    uint32_t valueMicroVolts;
    uint32_t reserved2[8];
};

struct NV_THERMAL_SENSORS_V2 {  // NvAPI_GPU_ThermalGetSensors
    uint32_t version;
    uint32_t mask;
    int32_t  reserved[8];
    int32_t  temperatures[32];  // 1/256 degC
};
#pragma pack(pop)

constexpr uint32_t MakeVer(size_t size, uint32_t ver) { return uint32_t(size) | (ver << 16); }

using QueryInterface_t = void* (__cdecl*)(unsigned int);
using Initialize_t = Status(__cdecl*)();
using Unload_t = Status(__cdecl*)();
using EnumPhysicalGPUs_t = Status(__cdecl*)(GpuHandle*, uint32_t*);
using GetFullName_t = Status(__cdecl*)(GpuHandle, char*);
using GetBusId_t = Status(__cdecl*)(GpuHandle, uint32_t*);
using GetPCIIdentifiers_t = Status(__cdecl*)(GpuHandle, uint32_t*, uint32_t*, uint32_t*, uint32_t*);
using I2CReadEx_t = Status(__cdecl*)(GpuHandle, NV_I2C_INFO_V3*, uint32_t*);
using I2CRead_t = Status(__cdecl*)(GpuHandle, NV_I2C_INFO_V3*);
using VoltStatus_t = Status(__cdecl*)(GpuHandle, NV_VOLT_STATUS_V1*);
using ThermalSensors_t = Status(__cdecl*)(GpuHandle, NV_THERMAL_SENSORS_V2*);

HMODULE g_dll = nullptr;
Unload_t pUnload = nullptr;
EnumPhysicalGPUs_t pEnum = nullptr;
GetFullName_t pName = nullptr;
GetBusId_t pBusId = nullptr;
GetPCIIdentifiers_t pPci = nullptr;
I2CReadEx_t pI2CReadEx = nullptr;
I2CRead_t pI2CRead = nullptr;
VoltStatus_t pVolt = nullptr;
ThermalSensors_t pThermal = nullptr;
uint32_t g_thermalMask = 0;
bool g_thermalProbed = false;
bool g_thermalDump = getenv("ASTRAL_THERMAL_DUMP") != nullptr;

template <typename T> T Q(QueryInterface_t qi, unsigned id) { return reinterpret_cast<T>(qi(id)); }

struct KnownCard { uint16_t sub; const char* model; };
// Subsystem ids reported by the community (astral-hwmon, LACT #906, LHM PR #2168).
const KnownCard kKnown[] = {
    {0x89E3, "ROG Astral RTX 5090 OC"},
    {0x89EA, "ROG Astral RTX 5090"},
    {0x8A61, "ROG Astral RTX 5090 (LC / Matrix)"},
    {0x89EC, "ROG Astral RTX 5090 LC / 5080"},
    {0x89ED, "ROG Astral RTX 50-series"},
    {0x89DE, "ROG Astral RTX 5080 OC"},
    {0x8A2E, "ROG Astral RTX 5090 OC White"},
    {0x8A2B, "ROG Astral RTX 5080 OC White"},
    {0x8A45, "ROG Astral RTX 5080 OC Miku"},
};

bool ReadByte(GpuHandle gpu, uint8_t port, uint8_t addr7, uint8_t reg, uint8_t& out) {
    uint8_t regAddr = reg;
    uint8_t data = 0;
    NV_I2C_INFO_V3 info{};
    info.version = MakeVer(sizeof(info), 3);
    info.displayMask = 0;
    info.bIsDDCPort = 0;
    info.i2cDevAddress = uint8_t(addr7 << 1);
    info.pbI2cRegAddress = &regAddr;
    info.regAddrSize = 1;
    info.pbData = &data;
    info.cbSize = 1;
    info.i2cSpeed = I2C_SPEED_DEPRECATED;
    info.i2cSpeedKhz = I2C_SPEED_DEFAULT;
    info.portId = port;
    info.bIsPortIdSet = 1;

    Status s = -1;
    if (pI2CReadEx) {
        uint32_t unknown = 0;
        s = pI2CReadEx(gpu, &info, &unknown);
    } else if (pI2CRead) {
        s = pI2CRead(gpu, &info);
    }
    if (s != NVAPI_OK) return false;
    out = data;
    return true;
}

} // namespace

bool Init(std::string& err) {
    if (g_dll) return true;
    if (getenv("RIGSENSE_SIMULATE_NO_NVIDIA")) { err = "simulated: no NVIDIA driver"; return false; }   // testing the AMD/Intel path
    g_dll = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!g_dll) { err = "nvapi64.dll not found (NVIDIA driver not installed?)"; return false; }
    auto qi = reinterpret_cast<QueryInterface_t>(GetProcAddress(g_dll, "nvapi_QueryInterface"));
    if (!qi) { err = "nvapi_QueryInterface missing"; FreeLibrary(g_dll); g_dll = nullptr; return false; }

    auto pInit = Q<Initialize_t>(qi, 0x0150E828);
    pUnload    = Q<Unload_t>(qi, 0xD22BDD7E);
    pEnum      = Q<EnumPhysicalGPUs_t>(qi, 0xE5AC921F);
    pName      = Q<GetFullName_t>(qi, 0xCEEE8E9F);
    pBusId     = Q<GetBusId_t>(qi, 0x1BE0B8E5);
    pPci       = Q<GetPCIIdentifiers_t>(qi, 0x2DDFB66E);
    pI2CReadEx = Q<I2CReadEx_t>(qi, 0x4D7B0709);
    pI2CRead   = Q<I2CRead_t>(qi, 0x2FDE12C5);
    pVolt      = Q<VoltStatus_t>(qi, 0x465F9BCF);
    pThermal   = Q<ThermalSensors_t>(qi, 0x65FE3AAD);

    if (!pInit || !pEnum) { err = "NVAPI entry points missing"; FreeLibrary(g_dll); g_dll = nullptr; return false; }
    Status s = pInit();
    if (s != NVAPI_OK) { err = "NvAPI_Initialize failed (" + std::to_string(s) + ")"; FreeLibrary(g_dll); g_dll = nullptr; return false; }
    if (!pI2CReadEx && !pI2CRead) err = "NVAPI I2C read not available in this driver";
    return true;
}

void Shutdown() {
    if (!g_dll) return;
    if (pUnload) pUnload();
    FreeLibrary(g_dll);
    g_dll = nullptr;
    g_thermalProbed = false;
}

std::vector<Gpu> EnumGpus() {
    std::vector<Gpu> out;
    if (!pEnum) return out;
    GpuHandle handles[64] = {};
    uint32_t count = 0;
    if (pEnum(handles, &count) != NVAPI_OK) return out;
    for (uint32_t i = 0; i < count; ++i) {
        Gpu g;
        g.handle = handles[i];
        char name[64] = {};
        if (pName && pName(handles[i], name) == NVAPI_OK) g.name = name;
        if (pBusId) pBusId(handles[i], &g.busId);
        uint32_t rev = 0, ext = 0;
        if (pPci) pPci(handles[i], &g.deviceId, &g.subSystemId, &rev, &ext);
        out.push_back(g);
    }
    return out;
}

bool IsKnownAstral(uint32_t subSystemId, std::string* model) {
    if ((subSystemId & 0xFFFF) != 0x1043) return false;   // ASUS subvendor
    uint16_t sub = uint16_t(subSystemId >> 16);
    for (auto& k : kKnown)
        if (k.sub == sub) { if (model) *model = k.model; return true; }
    return false;
}

bool ReadPins(GpuHandle gpu, uint8_t port, uint8_t addr7, PinReading& out, std::string& err) {
    if (!pI2CReadEx && !pI2CRead) { err = "NVAPI I2C not available"; return false; }
    // The IT8915 does not do block reads: one SMBus byte-data read per register.
    uint8_t buf[24];
    for (int i = 0; i < 24; ++i) {
        bool ok = ReadByte(gpu, port, addr7, uint8_t(0x80 + i), buf[i]);
        if (!ok) ok = ReadByte(gpu, port, addr7, uint8_t(0x80 + i), buf[i]);   // one retry
        if (!ok) { err = "I2C read failed at register 0x" + std::to_string(0x80 + i); return false; }
    }
    PinReading r;
    for (int rail = 0; rail < 6; ++rail) {
        const uint8_t* p = buf + rail * 4;
        double mv = double((p[0] << 8) | p[1]);
        double ma = double((p[2] << 8) | p[3]);
        int pin = 5 - rail;                    // rail 0 = pin 6 ... rail 5 = pin 1
        r.volts[pin] = mv / 1000.0;
        r.amps[pin] = ma / 1000.0;
    }
    for (int i = 0; i < 6; ++i) {
        if (r.volts[i] < 6.0 || r.volts[i] > 13.0) {
            char m[96];
            snprintf(m, sizeof m, "implausible data (pin %d = %.2f V) - sensor not at this address/port?", i + 1, r.volts[i]);
            err = m;
            return false;
        }
    }
    out = r;
    return true;
}

bool GetCoreVoltage(GpuHandle gpu, double& volts) {
    if (!pVolt) return false;
    NV_VOLT_STATUS_V1 s{};
    s.version = MakeVer(sizeof(s), 1);
    if (pVolt(gpu, &s) != NVAPI_OK) return false;
    if (s.valueMicroVolts == 0 || s.valueMicroVolts > 2000000) return false;
    volts = s.valueMicroVolts / 1e6;
    return true;
}

bool GetExtraTemps(GpuHandle gpu, double& hotspotC, double& vramC) {
    if (!pThermal) return false;
    if (!g_thermalProbed) {
        g_thermalProbed = true;
        // Find the widest sensor mask the driver accepts.
        for (int bit = 0; bit < 32; ++bit) {
            NV_THERMAL_SENSORS_V2 s{};
            s.version = MakeVer(sizeof(s), 2);
            s.mask = 1u << bit;
            if (pThermal(gpu, &s) != NVAPI_OK) { g_thermalMask = bit ? (1u << bit) - 1 : 0; break; }
            if (bit == 31) g_thermalMask = 0xFFFFFFFFu;
        }
    }
    if (!g_thermalMask) return false;
    if (g_thermalDump) {
        NV_THERMAL_SENSORS_V2 d{};
        d.version = MakeVer(sizeof(d), 2);
        d.mask = g_thermalMask;
        Status st = pThermal(gpu, &d);
        printf("thermal mask 0x%08X status %d\n", g_thermalMask, st);
        for (int i = 0; i < 32; ++i) if (d.temperatures[i]) printf("  [%d] %.2f\n", i, d.temperatures[i] / 256.0);
    }
    NV_THERMAL_SENSORS_V2 s{};
    s.version = MakeVer(sizeof(s), 2);
    s.mask = g_thermalMask;
    if (pThermal(gpu, &s) != NVAPI_OK) return false;
    // Unused channels read 255.0. Ada: [1] hotspot, [9] memory junction.
    // Blackwell (verified on ROG Astral 5090): [1] ~= core (no hotspot), [2] = VRAM.
    auto sane = [](double t) { return t > 1.0 && t < 130.0; };
    hotspotC = s.temperatures[1] / 256.0;
    double t9 = s.temperatures[9] / 256.0, t2 = s.temperatures[2] / 256.0;
    vramC = sane(t9) ? t9 : t2;
    if (!sane(hotspotC)) hotspotC = -1;
    if (!sane(vramC)) vramC = -1;
    return hotspotC > 0 || vramC > 0;
}

} // namespace nvapi
