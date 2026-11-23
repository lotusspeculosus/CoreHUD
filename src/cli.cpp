// rigsense-cli: console reader for the ROG Astral per-pin sensor (handy for testing / scripting).
//   rigsense-cli.exe            list GPUs, then print 5 readings
//   rigsense-cli.exe -n 0       print forever (Ctrl+C to stop)
//   rigsense-cli.exe --force    read even if the card is not in the known list
//   rigsense-cli.exe --port 1 --addr 0x2B --gpu 0
//   rigsense-cli.exe --sensors  list every sensor the panel can use
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "nvapi_gpu.h"
#include "nvml_gpu.h"
#include "sensors.h"
#include <io.h>
#include <fcntl.h>

// Lists every sensor the panel can use (built-in, HWiNFO Gadget, LibreHardwareMonitor, RTSS).
static int ListSensors() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);
    auto u8 = [](const std::wstring& w) {
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string s(size_t(n > 0 ? n - 1 : 0), '\0');
        if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
        return s;
    };
    {   // the registry must release its COM objects before CoUninitialize
        sensors::Registry reg;
        mon::Snapshot snap;   // no GPU sampler here: built-in GPU values show as offline
        reg.Update(snap);
        Sleep(600);
        reg.Update(snap);
        printf("HWiNFO gadget: %s (%d)  LHM: %s (%d)  RTSS: %s  HWiNFO shared memory: %s (%d)\n\n", reg.gadgetOk ? "yes" : "no",
               reg.gadgetCount, reg.lhmOk ? "yes" : "no", reg.lhmCount, reg.rtssOk ? "yes" : "no", reg.hwinfoOk ? "yes" : "no",
               reg.hwinfoCount);
        fflush(stdout);
        for (auto& s : reg.All()) {
            printf("%-28s %-34s %-28s %s%.*f %s\n", s.key.c_str(), u8(s.group).c_str(), u8(s.name).c_str(), s.ok ? "" : "(offline) ",
                   s.decimals, s.value, u8(s.unit).c_str());
        }
        fflush(stdout);
    }
    CoUninitialize();
    return 0;
}

int main(int argc, char** argv) {
    int count = 5, port = 1, addr = 0x2B, gpuIndex = -1, interval = 500;
    bool force = false;
    SetConsoleOutputCP(CP_UTF8);
    for (int i = 1; i < argc; ++i) {
        auto next = [&](int& v) { if (i + 1 < argc) v = int(strtol(argv[++i], nullptr, 0)); };
        if (!strcmp(argv[i], "--sensors")) return ListSensors();
        if (!strcmp(argv[i], "-n")) next(count);
        else if (!strcmp(argv[i], "--port")) next(port);
        else if (!strcmp(argv[i], "--addr")) next(addr);
        else if (!strcmp(argv[i], "--gpu")) next(gpuIndex);
        else if (!strcmp(argv[i], "--interval")) next(interval);
        else if (!strcmp(argv[i], "--force")) force = true;
        else { printf("usage: rigsense-cli [-n count] [--interval ms] [--gpu i] [--port p] [--addr 0x2B] [--force]\n"); return 2; }
    }

    std::string err;
    if (!nvapi::Init(err)) { printf("NVAPI: %s\n", err.c_str()); return 1; }
    if (!err.empty()) printf("NVAPI warning: %s\n", err.c_str());
    auto gpus = nvapi::EnumGpus();
    if (gpus.empty()) { printf("No NVIDIA GPUs found\n"); return 1; }
    int pick = gpuIndex;
    for (size_t i = 0; i < gpus.size(); ++i) {
        std::string model;
        bool known = nvapi::IsKnownAstral(gpus[i].subSystemId, &model);
        printf("GPU %zu: %s  PCI %04X:%04X  subsys %04X:%04X  bus %u  %s\n", i, gpus[i].name.c_str(),
               gpus[i].deviceId & 0xFFFF, gpus[i].deviceId >> 16, gpus[i].subSystemId & 0xFFFF,
               gpus[i].subSystemId >> 16, gpus[i].busId, known ? ("[" + model + "]").c_str() : "[no per-pin sensor known]");
        if (pick < 0 && known) pick = int(i);
    }
    if (pick < 0) {
        if (!force) { printf("No known ROG Astral card. Use --force to try anyway.\n"); return 1; }
        pick = 0;
    }
    if (pick >= int(gpus.size())) { printf("GPU index out of range\n"); return 1; }
    auto& g = gpus[size_t(pick)];

    double v;
    if (nvapi::GetCoreVoltage(g.handle, v)) printf("Core voltage: %.3f V\n", v);
    double hot, vram;
    if (nvapi::GetExtraTemps(g.handle, hot, vram)) printf("Hotspot: %.1f C  VRAM: %.1f C\n", hot, vram);
    std::string nerr;
    if (nvml::Init(nerr)) {
        nvml::Stats s;
        if (nvml::Read(nvml::FindByBus(g.busId), s))
            printf("NVML: util %d%%  core %d MHz  mem %d MHz  temp %d C  board %.1f W  fans %d\n", s.gpuUtil, s.coreMHz,
                   s.memMHz, s.tempC, s.powerW, s.fanCount);
        nvml::Shutdown();
    } else {
        printf("NVML: %s\n", nerr.c_str());
    }

    printf("\nReading IT8915FN on port %d, address 0x%02X (read-only)\n", port, addr);
    for (int n = 0; count == 0 || n < count; ++n) {
        nvapi::PinReading r;
        DWORD t0 = GetTickCount();
        if (!nvapi::ReadPins(g.handle, uint8_t(port), uint8_t(addr), r, err)) {
            printf("read failed: %s\n", err.c_str());
        } else {
            double w = 0, a = 0;
            for (int i = 0; i < 6; ++i) { w += r.volts[i] * r.amps[i]; a += r.amps[i]; }
            for (int i = 0; i < 6; ++i) printf("P%d %5.2fA %5.2fV  ", i + 1, r.amps[i], r.volts[i]);
            printf("| %6.1f W %5.2f A  (%lu ms)\n", w, a, GetTickCount() - t0);
        }
        fflush(stdout);
        if (count == 0 || n + 1 < count) Sleep(DWORD(interval));
    }
    nvapi::Shutdown();
    return 0;
}
