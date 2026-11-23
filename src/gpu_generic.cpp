#include "gpu_generic.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <map>
#include <tuple>

namespace gpug {
namespace {

// D3DKMT (gdi32) - declared locally to avoid the kernel-mode thunk header.
using D3DKMT_HANDLE = UINT;
struct KMT_OPENADAPTERFROMLUID { LUID AdapterLuid; D3DKMT_HANDLE hAdapter; };
struct KMT_CLOSEADAPTER { D3DKMT_HANDLE hAdapter; };
struct KMT_QUERYADAPTERINFO { D3DKMT_HANDLE hAdapter; UINT Type; void* pPrivateDriverData; UINT PrivateDriverDataSize; };
struct KMT_ADAPTER_PERFDATA {
    UINT32 PhysicalAdapterIndex;
    alignas(8) ULONGLONG MemoryFrequency;   // Hz
    alignas(8) ULONGLONG MaxMemoryFrequency;
    alignas(8) ULONGLONG MaxMemoryFrequencyOC;
    alignas(8) ULONGLONG MemoryBandwidth;
    alignas(8) ULONGLONG PCIEBandwidth;
    ULONG FanRPM;
    ULONG Power;          // tenths of a percent
    ULONG Temperature;    // deci-Celsius
    UCHAR PowerStateOverride;
};
constexpr UINT KMTQAITYPE_ADAPTERPERFDATA = 62;
using OpenFromLuid_t = LONG(APIENTRY*)(KMT_OPENADAPTERFROMLUID*);
using CloseAdapter_t = LONG(APIENTRY*)(const KMT_CLOSEADAPTER*);
using QueryInfo_t = LONG(APIENTRY*)(const KMT_QUERYADAPTERINFO*);

OpenFromLuid_t pOpen = nullptr;
CloseAdapter_t pClose = nullptr;
QueryInfo_t pQuery = nullptr;

void LoadKmt() {
    static bool done = false;
    if (done) return;
    done = true;
    HMODULE g = GetModuleHandleW(L"gdi32.dll");
    if (!g) g = LoadLibraryW(L"gdi32.dll");
    if (!g) return;
    pOpen = reinterpret_cast<OpenFromLuid_t>(GetProcAddress(g, "D3DKMTOpenAdapterFromLuid"));
    pClose = reinterpret_cast<CloseAdapter_t>(GetProcAddress(g, "D3DKMTCloseAdapter"));
    pQuery = reinterpret_cast<QueryInfo_t>(GetProcAddress(g, "D3DKMTQueryAdapterInfo"));
}

// Parses "...luid_0x00000000_0x0000D1A3_phys_0_eng_3_engtype_3D" (high part first).
bool ParseLuid(const wchar_t* inst, int32_t& high, uint32_t& low, int* phys = nullptr, int* eng = nullptr) {
    const wchar_t* p = wcsstr(inst, L"luid_0x");
    if (!p) return false;
    unsigned h = 0, l = 0;
    if (swscanf_s(p, L"luid_0x%x_0x%x", &h, &l) != 2) return false;
    high = int32_t(h);
    low = l;
    if (phys) { const wchar_t* q = wcsstr(p, L"phys_"); *phys = q ? _wtoi(q + 5) : 0; }
    if (eng) { const wchar_t* q = wcsstr(p, L"eng_"); *eng = q ? _wtoi(q + 4) : 0; }
    return true;
}

template <typename Fn> void ForEachInstance(PDH_HCOUNTER c, Fn&& fn) {
    DWORD size = 0, count = 0;
    PDH_STATUS st = PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, nullptr);
    if (st != PDH_MORE_DATA || !size) return;
    std::vector<BYTE> buf(size);
    auto items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
    if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, items) != ERROR_SUCCESS) return;
    for (DWORD i = 0; i < count; ++i)
        if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA || items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA)
            fn(items[i].szName, items[i].FmtValue.doubleValue);
}

} // namespace

const wchar_t* VendorName(uint32_t v) {
    switch (v) {
    case 0x10DE: return L"NVIDIA";
    case 0x1002: case 0x1022: return L"AMD";
    case 0x8086: return L"Intel";
    case 0x5143: return L"Qualcomm";
    case 0x1414: return L"Microsoft";
    default: return L"GPU";
    }
}

Monitor::Monitor() {}

Monitor::~Monitor() {
    if (query_) PdhCloseQuery(static_cast<PDH_HQUERY>(query_));
    LoadKmt();
    for (auto h : kmtHandles_)
        if (h && pClose) { KMT_CLOSEADAPTER c{h}; pClose(&c); }
}

void Monitor::Enumerate() {
    enumerated_ = true;
    IDXGIFactory1* f = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f)))) return;
    std::map<std::string, int> seen;
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        a->Release();
        if ((d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || d.VendorId == 0x1414) continue;   // WARP / Basic Display
        bool dup = false;
        for (auto& e : adapters_) if (e.luidLow == d.AdapterLuid.LowPart && e.luidHigh == d.AdapterLuid.HighPart) dup = true;
        if (dup) continue;
        Adapter ad;
        ad.luidLow = d.AdapterLuid.LowPart;
        ad.luidHigh = d.AdapterLuid.HighPart;
        ad.name = d.Description;
        ad.vendor = d.VendorId;
        ad.device = d.DeviceId;
        ad.subsys = d.SubSysId;
        ad.dedicatedBytes = d.DedicatedVideoMemory;
        char k[64];
        snprintf(k, sizeof k, "gpux.%04x_%04x", ad.vendor, ad.device);
        int n = seen[k]++;
        if (n) snprintf(k + strlen(k), sizeof k - strlen(k), "_%d", n);
        ad.key = k;
        adapters_.push_back(ad);
    }
    f->Release();

    LoadKmt();
    kmtHandles_.assign(adapters_.size(), 0);
    if (pOpen)
        for (size_t i = 0; i < adapters_.size(); ++i) {
            KMT_OPENADAPTERFROMLUID o{};
            o.AdapterLuid.LowPart = adapters_[i].luidLow;
            o.AdapterLuid.HighPart = adapters_[i].luidHigh;
            if (pOpen(&o) == 0) kmtHandles_[i] = o.hAdapter;
        }

    PDH_HQUERY q = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &q) == ERROR_SUCCESS) {
        PDH_HCOUNTER ce = nullptr, cm = nullptr;
        PdhAddEnglishCounterW(q, L"\\GPU Engine(*)\\Utilization Percentage", 0, &ce);
        PdhAddEnglishCounterW(q, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &cm);
        PdhCollectQueryData(q);   // rate counters need a first sample
        query_ = q;
        engineCounter_ = ce;
        memCounter_ = cm;
    }
}

const Adapter* Monitor::Primary() const {
    const Adapter* best = nullptr;
    for (auto& a : adapters_)
        if (!best || a.dedicatedBytes > best->dedicatedBytes) best = &a;
    return best;
}

void Monitor::Update() {
    if (!enumerated_) Enumerate();
    if (adapters_.empty()) return;
    for (auto& a : adapters_) {
        a.stats = Stats{};
        a.stats.vramTotalGB = a.dedicatedBytes / 1073741824.0;
    }
    auto find = [&](int32_t hi, uint32_t lo) -> Adapter* {
        for (auto& a : adapters_) if (a.luidHigh == hi && a.luidLow == lo) return &a;
        return nullptr;
    };
    if (query_ && PdhCollectQueryData(static_cast<PDH_HQUERY>(query_)) == ERROR_SUCCESS) {
        // load: sum per engine over all processes, adapter load = busiest engine
        std::map<std::tuple<int32_t, uint32_t, int, int>, double> perEngine;
        if (engineCounter_)
            ForEachInstance(static_cast<PDH_HCOUNTER>(engineCounter_), [&](const wchar_t* name, double v) {
                int32_t hi; uint32_t lo; int phys, eng;
                if (ParseLuid(name, hi, lo, &phys, &eng)) perEngine[{hi, lo, phys, eng}] += v;
            });
        for (auto& [k, v] : perEngine)
            if (Adapter* a = find(std::get<0>(k), std::get<1>(k))) a->stats.util = std::max(a->stats.util, std::min(v, 100.0));
        for (auto& a : adapters_) if (a.stats.util < 0 && engineCounter_ && !perEngine.empty()) a.stats.util = 0;
        if (memCounter_) {
            std::map<std::pair<int32_t, uint32_t>, double> mem;
            ForEachInstance(static_cast<PDH_HCOUNTER>(memCounter_), [&](const wchar_t* name, double v) {
                int32_t hi; uint32_t lo;
                if (ParseLuid(name, hi, lo)) mem[{hi, lo}] += v;
            });
            for (auto& [k, v] : mem)
                if (Adapter* a = find(k.first, k.second)) a->stats.vramUsedGB = v / 1073741824.0;
        }
    }
    if (pQuery)
        for (size_t i = 0; i < adapters_.size(); ++i) {
            if (!kmtHandles_[i]) continue;
            KMT_ADAPTER_PERFDATA pd{};
            KMT_QUERYADAPTERINFO qi{kmtHandles_[i], KMTQAITYPE_ADAPTERPERFDATA, &pd, sizeof pd};
            if (pQuery(&qi) != 0) continue;
            auto& s = adapters_[i].stats;
            if (pd.Temperature > 0 && pd.Temperature < 1500) {
                s.tempC = pd.Temperature / 10.0;
                // fan: only trusted when the driver reports temperature; integrated GPUs (little dedicated
                // memory) have no fan of their own, so a 0 there means "none", not "stopped"
                if (pd.FanRPM > 0 || adapters_[i].dedicatedBytes >= (2ull << 30)) s.fanRpm = int(pd.FanRPM);
            }
            if (pd.Power > 0 && pd.Power <= 2000) s.powerPct = pd.Power / 10.0;
            if (pd.MemoryFrequency > 0) s.memMHz = int(pd.MemoryFrequency / 1000000ULL);
        }
}

} // namespace gpug
