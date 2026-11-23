#include "sensors.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <wbemidl.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace sensors {
namespace {

// ---- HWiNFO shared memory (documented in the HWiNFO SDK) ----
constexpr uint32_t HWI_SIG_ACTIVE = 0x53695748;   // 'HWiS'
#pragma pack(push, 1)
struct HwiHeader {
    uint32_t signature, version, revision;
    int64_t pollTime;
    uint32_t offSensors, sizeSensor, numSensors;
    uint32_t offReadings, sizeReading, numReadings;
};
struct HwiSensor {             // base part; v2 appends utfSensorNameUser[128]
    uint32_t id, instance;
    char nameOrig[128];
    char nameUser[128];
};
struct HwiReading {            // base part; v2 appends utfLabelUser[128], utfUnit[16]
    uint32_t type, sensorIndex, id;
    char labelOrig[128];
    char labelUser[128];
    char unit[16];
    double value, valueMin, valueMax, valueAvg;
};
#pragma pack(pop)

// ---- RTSS shared memory (RTSSSharedMemory.h, v2.x) ----
constexpr uint32_t RTSS_SIG = 0x52545353;   // 'RTSS'
struct RtssHeader {
    uint32_t signature, version, appEntrySize, appArrOffset, appArrSize, osdEntrySize, osdArrOffset, osdArrSize, osdFrame;
};
struct RtssAppHead {           // start of RTSS_SHARED_MEMORY_APP_ENTRY
    uint32_t processId;
    char name[MAX_PATH];
    uint32_t flags, time0, time1, frames, frameTime;
};

std::wstring FromCp(const char* s, size_t max, UINT cp) {
    size_t n = strnlen(s, max);
    if (!n) return {};
    int len = MultiByteToWideChar(cp, 0, s, int(n), nullptr, 0);
    std::wstring w(size_t(len), L'\0');
    MultiByteToWideChar(cp, 0, s, int(n), w.data(), len);
    return w;
}

const unsigned char* MapRead(const wchar_t* name, void*& handle) {
    HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
    if (!h) return nullptr;
    void* v = MapViewOfFile(h, FILE_MAP_READ, 0, 0, 0);
    if (!v) { CloseHandle(h); return nullptr; }
    handle = h;
    return static_cast<const unsigned char*>(v);
}

void Unmap(void*& handle, const unsigned char*& view) {
    if (view) UnmapViewOfFile(view);
    if (handle) CloseHandle(handle);
    view = nullptr;
    handle = nullptr;
}

size_t MappedSize(const void* p) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof mbi)) return 0;
    return mbi.RegionSize;
}

std::string ToU8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// Keys are stored space-separated in panel.txt: escape spaces, '=' and '%'.
std::string EscKey(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == ' ') o += "%20";
        else if (c == '=') o += "%3D";
        else if (c == '%') o += "%25";
        else o += c;
    }
    return o;
}

std::wstring RegStr(HKEY k, const std::wstring& name) {
    wchar_t buf[512];
    DWORD type = 0, size = sizeof(buf) - sizeof(wchar_t);
    if (RegQueryValueExW(k, name.c_str(), nullptr, &type, reinterpret_cast<BYTE*>(buf), &size) != ERROR_SUCCESS) return {};
    if (type != REG_SZ && type != REG_EXPAND_SZ) return {};
    buf[size / sizeof(wchar_t)] = 0;
    return buf;
}

bool ProcessRunning(const wchar_t* const* names, int count) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{sizeof pe};
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
        for (int i = 0; i < count; ++i)
            if (!_wcsicmp(pe.szExeFile, names[i])) found = true;
    CloseHandle(snap);
    return found;
}

int KindFromUnit(const std::wstring& u) {
    if (u == L"°C" || u == L"°F") return K_TEMP;
    if (u == L"V" || u == L"mV") return K_VOLT;
    if (u == L"RPM") return K_FAN;
    if (u == L"A") return K_CURRENT;
    if (u == L"W") return K_POWER;
    if (u == L"MHz" || u == L"GHz") return K_CLOCK;
    if (u == L"%") return K_USAGE;
    if (u == L"FPS") return K_FPS;
    return K_OTHER;
}

std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t"), b = s.find_last_not_of(L" \t");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}

} // namespace

Registry::~Registry() { Shutdown(); }

void Registry::Shutdown() {
    Unmap(hwiMap_, hwiView_);
    Unmap(rtssMap_, rtssView_);
    if (wmiServices_) static_cast<IWbemServices*>(wmiServices_)->Release();
    if (wmiLocator_) static_cast<IWbemLocator*>(wmiLocator_)->Release();
    wmiServices_ = wmiLocator_ = nullptr;
}

// ---- HWiNFO "Gadget" values: HKCU\Software\HWiNFO64\VSB (Sensor<i>, Label<i>, Value<i>, ValueRaw<i>) ----
void Registry::AddHwinfoGadget() {
    gadgetOk = false;
    gadgetCount = 0;
    static DWORD checkTick = 0;
    static bool running = false;
    DWORD now = GetTickCount();
    if (!checkTick || now - checkTick > 3000) {
        const wchar_t* names[] = {L"HWiNFO64.exe", L"HWiNFO32.exe", L"HWiNFO.exe"};
        running = ProcessRunning(names, 3);
        checkTick = now;
    }
    if (!running) return;   // values left in the registry after HWiNFO exits are stale
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\HWiNFO64\\VSB", 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    gadgetOk = true;
    for (int i = 0; i < 2000; ++i) {
        std::wstring n = std::to_wstring(i);
        std::wstring label = RegStr(k, L"Label" + n), value = RegStr(k, L"Value" + n);
        if (label.empty() && value.empty()) break;
        std::wstring sensor = RegStr(k, L"Sensor" + n), raw = RegStr(k, L"ValueRaw" + n);
        // numeric prefix of the formatted value -> decimals + unit
        size_t p = 0;
        while (p < value.size() && (iswdigit(value[p]) || value[p] == L'.' || value[p] == L',' || value[p] == L'-')) ++p;
        std::wstring numPart = value.substr(0, p), unit = Trim(value.substr(p));
        int kind = KindFromUnit(unit);
        // Normalise to "1234.5": the formatted value may use ',' or '.' as decimal or thousands separator.
        auto normalise = [&](std::wstring s, bool formatted) {
            size_t lastDot = s.rfind(L'.'), lastComma = s.rfind(L',');
            wchar_t dec = 0;
            if (lastDot != std::wstring::npos && lastComma != std::wstring::npos) dec = lastDot > lastComma ? L'.' : L',';
            else if (lastDot != std::wstring::npos || lastComma != std::wstring::npos) {
                wchar_t c = lastDot != std::wstring::npos ? L'.' : L',';
                size_t pos = c == L'.' ? lastDot : lastComma;
                bool once = s.find(c) == pos;
                bool thousandsLike = formatted && s.size() - pos - 1 == 3 && (kind == K_FAN || kind == K_CLOCK);
                dec = once && !thousandsLike ? c : 0;
            }
            std::wstring o;
            for (wchar_t ch : s) {
                if (ch == L'.' || ch == L',') { if (ch == dec) o += L'.'; }
                else o += ch;
            }
            return o;
        };
        std::wstring num = raw.empty() ? normalise(numPart, true) : normalise(raw, false);
        size_t dp = num.find(L'.');
        int dec = dp == std::wstring::npos ? 0 : int(num.size() - dp - 1);
        if (kind == K_FAN) dec = 0;
        wchar_t* end = nullptr;
        double v = wcstod(num.c_str(), &end);
        bool ok = end != num.c_str();
        double range = unit == L"%" ? 100 : kind == K_TEMP ? 100 : 0;
        std::string key = "hwg." + EscKey(ToU8(sensor) + "|" + ToU8(label));
        Add(key, L"HWiNFO · " + (sensor.empty() ? std::wstring(L"Gadget") : sensor), label, unit, v, ok, range, std::min(dec, 3), kind);
        ++gadgetCount;
    }
    RegCloseKey(k);
}

// ---- LibreHardwareMonitor WMI: ROOT\LibreHardwareMonitor (Sensor / Hardware classes) ----
void Registry::AddLhm() {
    lhmOk = false;
    lhmCount = 0;
    DWORD now = GetTickCount();
    if (!wmiServices_) {
        if (lhmRetryTick_ && now - lhmRetryTick_ < 5000) return;
        lhmRetryTick_ = now;
        if (!wmiLocator_) {
            IWbemLocator* loc = nullptr;
            if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator, reinterpret_cast<void**>(&loc))))
                return;
            wmiLocator_ = loc;
        }
        auto loc = static_cast<IWbemLocator*>(wmiLocator_);
        IWbemServices* svc = nullptr;
        for (const wchar_t* ns : {L"ROOT\\LibreHardwareMonitor", L"ROOT\\OpenHardwareMonitor"}) {
            BSTR b = SysAllocString(ns);
            HRESULT hr = loc->ConnectServer(b, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &svc);
            SysFreeString(b);
            if (SUCCEEDED(hr)) break;
            svc = nullptr;
        }
        if (!svc) return;
        CoSetProxyBlanket(svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                          RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
        wmiServices_ = svc;
        lhmHardwareTick_ = 0;
    }
    auto svc = static_cast<IWbemServices*>(wmiServices_);
    auto query = [&](const wchar_t* q, auto&& fn) -> bool {
        BSTR lang = SysAllocString(L"WQL"), qs = SysAllocString(q);
        IEnumWbemClassObject* en = nullptr;
        HRESULT hr = svc->ExecQuery(lang, qs, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en);
        SysFreeString(lang);
        SysFreeString(qs);
        if (FAILED(hr) || !en) return false;
        IWbemClassObject* obj = nullptr;
        ULONG got = 0;
        while (en->Next(2000, 1, &obj, &got) == S_OK && got) {
            fn(obj);
            obj->Release();
        }
        en->Release();
        return true;
    };
    auto str = [](IWbemClassObject* o, const wchar_t* name) {
        VARIANT v;
        VariantInit(&v);
        std::wstring s;
        if (SUCCEEDED(o->Get(name, 0, &v, nullptr, nullptr)) && v.vt == VT_BSTR && v.bstrVal) s = v.bstrVal;
        VariantClear(&v);
        return s;
    };
    if (!lhmHardwareTick_ || now - lhmHardwareTick_ > 30000) {
        lhmHardware_.clear();
        query(L"SELECT Identifier, Name FROM Hardware", [&](IWbemClassObject* o) { lhmHardware_[str(o, L"Identifier")] = str(o, L"Name"); });
        lhmHardwareTick_ = now;
    }
    struct TypeInfo { const wchar_t* type; const wchar_t* unit; int dec; int kind; double range; double scale; };
    static const TypeInfo kTypes[] = {
        {L"Voltage", L"V", 3, K_VOLT, 0, 1}, {L"Current", L"A", 2, K_CURRENT, 0, 1}, {L"Power", L"W", 1, K_POWER, 0, 1},
        {L"Clock", L"MHz", 0, K_CLOCK, 0, 1}, {L"Temperature", L"°C", 1, K_TEMP, 100, 1}, {L"Load", L"%", 0, K_USAGE, 100, 1},
        {L"Frequency", L"Hz", 0, K_OTHER, 0, 1}, {L"Fan", L"RPM", 0, K_FAN, 0, 1}, {L"Flow", L"L/h", 0, K_OTHER, 0, 1},
        {L"Control", L"%", 0, K_USAGE, 100, 1}, {L"Level", L"%", 0, K_USAGE, 100, 1}, {L"Factor", L"", 2, K_OTHER, 0, 1},
        {L"Data", L"GB", 1, K_OTHER, 0, 1}, {L"SmallData", L"MB", 0, K_OTHER, 0, 1},
        {L"Throughput", L"MB/s", 1, K_OTHER, 0, 1.0 / 1048576.0}, {L"TimeSpan", L"s", 0, K_OTHER, 0, 1},
        {L"Energy", L"mWh", 0, K_OTHER, 0, 1}, {L"Noise", L"dBA", 0, K_OTHER, 0, 1}, {L"Humidity", L"%", 0, K_OTHER, 100, 1},
    };
    bool ok = query(L"SELECT Identifier, Name, SensorType, Value, Parent FROM Sensor", [&](IWbemClassObject* o) {
        std::wstring id = str(o, L"Identifier"), name = str(o, L"Name"), type = str(o, L"SensorType"), parent = str(o, L"Parent");
        VARIANT v;
        VariantInit(&v);
        double val = 0;
        bool have = false;
        if (SUCCEEDED(o->Get(L"Value", 0, &v, nullptr, nullptr))) {
            if (v.vt == VT_R4) { val = v.fltVal; have = true; }
            else if (v.vt == VT_R8) { val = v.dblVal; have = true; }
        }
        VariantClear(&v);
        const TypeInfo* ti = nullptr;
        for (auto& t : kTypes) if (type == t.type) ti = &t;
        auto hw = lhmHardware_.find(parent);
        std::wstring group = L"LHM · " + (hw != lhmHardware_.end() ? hw->second : parent);
        Add("lhm." + EscKey(ToU8(id)), group, name, ti ? ti->unit : L"", ti ? val * ti->scale : val, have && !std::isnan(val),
            ti ? ti->range : 0, ti ? ti->dec : 1, ti ? ti->kind : K_OTHER);
        ++lhmCount;
    });
    if (!ok) {   // LHM closed: drop the connection and retry later
        svc->Release();
        wmiServices_ = nullptr;
        lhmRetryTick_ = now;
        return;
    }
    lhmOk = lhmCount > 0;
}

const Sensor* Registry::Find(const std::string& key) const {
    auto it = index_.find(key);
    return it == index_.end() ? nullptr : &list_[it->second];
}

Sensor& Registry::Add(const std::string& key, const std::wstring& group, const std::wstring& name, const std::wstring& unit,
                      double value, bool ok, double rangeMax, int decimals, int kind) {
    Sensor s;
    s.key = key;
    s.group = group;
    s.name = name;
    s.unit = unit;
    s.value = value;
    s.ok = ok;
    s.decimals = decimals;
    s.kind = kind;
    if (ok) {
        double& pk = peak_[key];
        pk = std::max(pk, value);
    }
    s.rangeMax = rangeMax > 0 ? rangeMax : std::max({peak_[key] * 1.15, std::fabs(value) * 1.3, 1.0});
    index_[key] = list_.size();
    list_.push_back(std::move(s));
    return list_.back();
}

void Registry::Update(const mon::Snapshot& s) {
    list_.clear();
    index_.clear();
    gpus_.Update();
    AddBuiltin(s);
    AddGpusGeneric(s.nvmlOk);
    AddSystem();
    AddRtss();
    AddHwinfoGadget();
    AddLhm();
    AddHwinfo();
}

void Registry::AddBuiltin(const mon::Snapshot& s) {
    const std::wstring g = L"12V-2x6 connector (per-pin sensor)";
    const auto& c = s.cfg;
    bool ok = s.pinsOk;
    double connW = 0, totA = 0, maxA = 0, minA = 1e9, minV = 1e9;
    for (int i = 0; i < 6; ++i) {
        connW += s.volts[i] * s.amps[i];
        totA += s.amps[i];
        maxA = std::max(maxA, s.amps[i]);
        minA = std::min(minA, s.amps[i]);
        minV = std::min(minV, s.volts[i]);
    }
    Add("astral.conn_w", g, L"Connector power", L"W", connW, ok, 600, 1, K_POWER);
    Add("astral.total_a", g, L"Total current", L"A", totA, ok, 50, 2, K_CURRENT);
    auto& mx = Add("astral.max_pin_a", g, L"Highest pin current", L"A", maxA, ok, 10, 2, K_CURRENT);
    mx.warn = c.warnA;
    mx.crit = c.limitA;
    Add("astral.spread_a", g, L"Pin spread", L"A", ok ? maxA - minA : 0, ok, 3, 2, K_CURRENT).warn = c.spreadWarnA;
    Add("astral.min_pin_v", g, L"Lowest pin voltage", L"V", minV, ok, 13, 3, K_VOLT);
    char key[32];
    wchar_t name[32];
    for (int i = 0; i < 6; ++i) {
        snprintf(key, sizeof key, "astral.pin%d_a", i + 1);
        swprintf(name, 32, L"Pin %d current", i + 1);
        auto& p = Add(key, g, name, L"A", s.amps[i], ok, 10, 2, K_CURRENT);
        p.warn = c.warnA;
        p.crit = c.limitA;
    }
    for (int i = 0; i < 6; ++i) {
        snprintf(key, sizeof key, "astral.pin%d_v", i + 1);
        swprintf(name, 32, L"Pin %d voltage", i + 1);
        Add(key, g, name, L"V", s.volts[i], ok, 13, 3, K_VOLT);
    }

    const auto& st = s.stats;
    std::wstring gg = L"GPU";
    gpuViaWindows = !s.nvmlOk;
    if (!s.nvmlOk) {
        // No NVIDIA/NVML: the gpu.* sensors come from Windows counters for the main GPU (AMD, Intel, ...)
        const gpug::Adapter* a = gpus_.Primary();
        gpuName = a ? a->name : L"";
        if (a) gg += L" · " + a->name;
        gpug::Stats z;
        const gpug::Stats& g2 = a ? a->stats : z;
        Add("gpu.util", gg, L"GPU load", L"%", g2.util, g2.util >= 0, 100, 0, K_USAGE);
        Add("gpu.temp", gg, L"GPU temperature", L"°C", g2.tempC, g2.tempC > 0, 100, 0, K_TEMP);
        Add("gpu.mem_mhz", gg, L"Memory clock", L"MHz", g2.memMHz, g2.memMHz > 0, 0, 0, K_CLOCK);
        Add("gpu.vram_used", gg, L"VRAM used", L"GB", g2.vramUsedGB, g2.vramUsedGB >= 0, g2.vramTotalGB > 0 ? g2.vramTotalGB : 16, 2, K_OTHER);
        Add("gpu.vram_pct", gg, L"VRAM usage", L"%", g2.vramTotalGB > 0 ? 100.0 * g2.vramUsedGB / g2.vramTotalGB : 0,
            g2.vramUsedGB >= 0 && g2.vramTotalGB > 0, 100, 0, K_USAGE);
        Add("gpu.power_pct", gg, L"Power (of limit)", L"%", g2.powerPct, g2.powerPct >= 0, 100, 0, K_USAGE);
        Add("gpu.fan1_rpm", gg, L"Fan speed", L"RPM", g2.fanRpm, g2.fanRpm >= 0, 3500, 0, K_FAN);
        return;
    }
    if (!s.gpuName.empty()) {
        gpuName = FromCp(s.gpuName.c_str(), s.gpuName.size(), CP_UTF8);
        gg += L" · " + gpuName;
    }
    Add("gpu.util", gg, L"GPU load", L"%", st.gpuUtil, st.gpuUtil >= 0, 100, 0, K_USAGE);
    Add("gpu.core_mhz", gg, L"Core clock", L"MHz", st.coreMHz, st.coreMHz >= 0, 3300, 0, K_CLOCK);
    Add("gpu.mem_mhz", gg, L"Memory clock", L"MHz", st.memMHz, st.memMHz >= 0, 0, 0, K_CLOCK);
    Add("gpu.temp", gg, L"GPU temperature", L"°C", st.tempC, st.tempC >= 0, 100, 0, K_TEMP);
    Add("gpu.vram_temp", gg, L"VRAM temperature", L"°C", s.vramC, s.vramC > 0, 110, 1, K_TEMP);
    if (s.hotspotC > 0 && st.tempC >= 0 && s.hotspotC - st.tempC >= 1.0)
        Add("gpu.hotspot", gg, L"Hotspot temperature", L"°C", s.hotspotC, true, 110, 1, K_TEMP);
    Add("gpu.core_mv", gg, L"Core voltage", L"mV", s.coreV * 1000, s.coreV > 0, 1100, 0, K_VOLT);
    double totGB = st.memTotal / 1073741824.0;
    Add("gpu.vram_used", gg, L"VRAM used", L"GB", st.memUsed / 1073741824.0, st.memTotal > 0, totGB > 0 ? totGB : 32, 2, K_OTHER);
    Add("gpu.vram_pct", gg, L"VRAM usage", L"%", st.memTotal ? 100.0 * st.memUsed / st.memTotal : 0, st.memTotal > 0, 100, 0, K_USAGE);
    Add("gpu.board_w", gg, L"Board power", L"W", st.powerW, st.powerW >= 0, st.powerLimitW > 0 ? st.powerLimitW : 600, 0, K_POWER);
    for (int f = 0; f < st.fanCount; ++f) {
        snprintf(key, sizeof key, "gpu.fan%d_rpm", f + 1);
        swprintf(name, 32, L"Fan %d speed", f + 1);
        Add(key, gg, name, L"RPM", st.fanRpm[f], st.fanRpm[f] >= 0, 3500, 0, K_FAN);
        snprintf(key, sizeof key, "gpu.fan%d_pct", f + 1);
        swprintf(name, 32, L"Fan %d duty", f + 1);
        Add(key, gg, name, L"%", st.fanPct[f], st.fanPct[f] >= 0, 100, 0, K_USAGE);
    }
}

// Every other GPU (integrated graphics, second card...) gets its own group with Windows counters.
void Registry::AddGpusGeneric(bool nvml) {
    const gpug::Adapter* primary = gpus_.Primary();
    for (auto& a : gpus_.Adapters()) {
        if (!nvml && &a == primary) continue;      // already published as gpu.*
        if (nvml && a.vendor == 0x10DE) continue;  // NVIDIA card is covered by NVML
        std::wstring g = std::wstring(L"GPU · ") + a.name;
        const auto& st = a.stats;
        Add(a.key + ".util", g, L"GPU load", L"%", st.util, st.util >= 0, 100, 0, K_USAGE);
        Add(a.key + ".temp", g, L"GPU temperature", L"°C", st.tempC, st.tempC > 0, 100, 0, K_TEMP);
        Add(a.key + ".vram_used", g, L"VRAM used", L"GB", st.vramUsedGB, st.vramUsedGB >= 0, st.vramTotalGB > 0 ? st.vramTotalGB : 4, 2, K_OTHER);
        Add(a.key + ".power_pct", g, L"Power (of limit)", L"%", st.powerPct, st.powerPct >= 0, 100, 0, K_USAGE);
        Add(a.key + ".fan_rpm", g, L"Fan speed", L"RPM", st.fanRpm, st.fanRpm >= 0, 3500, 0, K_FAN);
        Add(a.key + ".mem_mhz", g, L"Memory clock", L"MHz", st.memMHz, st.memMHz > 0, 0, 0, K_CLOCK);
    }
}

void Registry::AddSystem() {
    const std::wstring g = L"System";
    FILETIME idle, kernel, user;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        auto u64 = [](FILETIME f) { return (unsigned long long(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
        unsigned long long i = u64(idle), t = u64(kernel) + u64(user);   // kernel time includes idle
        if (cpuTotal_ && t > cpuTotal_) cpuLoad_ = 100.0 * (1.0 - double(i - cpuIdle_) / double(t - cpuTotal_));
        cpuIdle_ = i;
        cpuTotal_ = t;
    }
    Add("sys.cpu", g, L"CPU load", L"%", std::clamp(cpuLoad_, 0.0, 100.0), cpuLoad_ >= 0, 100, 0, K_USAGE);
    MEMORYSTATUSEX ms{sizeof ms};
    if (GlobalMemoryStatusEx(&ms)) {
        double tot = ms.ullTotalPhys / 1073741824.0, used = (ms.ullTotalPhys - ms.ullAvailPhys) / 1073741824.0;
        Add("sys.ram_used", g, L"RAM used", L"GB", used, true, tot, 1, K_OTHER);
        Add("sys.ram_pct", g, L"RAM usage", L"%", used / tot * 100, true, 100, 0, K_USAGE);
    }
}

void Registry::AddRtss() {
    rtssOk = false;
    rtssApp.clear();
    if (!rtssView_) {
        DWORD now = GetTickCount();
        if (now - rtssRetryTick_ < 2000 && rtssRetryTick_) return;
        rtssRetryTick_ = now;
        rtssView_ = MapRead(L"RTSSSharedMemoryV2", rtssMap_);
        if (!rtssView_) return;
    }
    RtssHeader h;
    memcpy(&h, rtssView_, sizeof h);
    if (h.signature != RTSS_SIG || h.version < 0x00020000 || h.appEntrySize < sizeof(RtssAppHead)) {
        Unmap(rtssMap_, rtssView_);
        return;
    }
    size_t avail = MappedSize(rtssView_);
    DWORD fgPid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &fgPid);
    DWORD self = GetCurrentProcessId(), now = GetTickCount();
    RtssAppHead best{};
    bool found = false, bestFg = false;
    for (uint32_t i = 0; i < h.appArrSize; ++i) {
        size_t off = size_t(h.appArrOffset) + size_t(i) * h.appEntrySize;
        if (off + sizeof(RtssAppHead) > avail) break;
        RtssAppHead e;
        memcpy(&e, rtssView_ + off, sizeof e);
        if (!e.processId || e.processId == self || e.time1 <= e.time0) continue;
        if (now - e.time1 > 3000) continue;   // stale entry (app closed / paused)
        bool fg = e.processId == fgPid;
        if (!found || (fg && !bestFg) || (fg == bestFg && e.time1 > best.time1)) { best = e; found = true; bestFg = fg; }
    }
    rtssOk = true;
    double fps = 0, ft = 0;
    if (found) {
        fps = 1000.0 * best.frames / double(best.time1 - best.time0);
        ft = best.frameTime / 1000.0;
        std::wstring n = FromCp(best.name, MAX_PATH, CP_ACP);
        size_t slash = n.find_last_of(L"\\/");
        rtssApp = slash == std::wstring::npos ? n : n.substr(slash + 1);
    }
    std::wstring g = L"RTSS";
    if (!rtssApp.empty()) g += L" · " + rtssApp;
    Add("rtss.fps", g, L"Framerate", L"FPS", fps, found, 0, 0, K_FPS).rangeMax = std::max(144.0, peak_["rtss.fps"] * 1.1);
    Add("rtss.frametime", g, L"Frame time", L"ms", ft, found, 0, 1, K_OTHER);
}

void Registry::AddHwinfo() {
    hwinfoOk = false;
    hwinfoCount = 0;
    if (!hwiView_) {
        DWORD now = GetTickCount();
        if (now - hwiRetryTick_ < 2000 && hwiRetryTick_) return;
        hwiRetryTick_ = now;
        hwiView_ = MapRead(L"Global\\HWiNFO_SENS_SM2", hwiMap_);
        if (!hwiView_) hwiView_ = MapRead(L"HWiNFO_SENS_SM2", hwiMap_);
        if (!hwiView_) return;
    }
    HwiHeader h;
    memcpy(&h, hwiView_, sizeof h);
    size_t avail = MappedSize(hwiView_);
    if (h.signature != HWI_SIG_ACTIVE || h.sizeSensor < sizeof(HwiSensor) || h.sizeReading < sizeof(HwiReading) ||
        size_t(h.offReadings) + size_t(h.sizeReading) * h.numReadings > avail ||
        size_t(h.offSensors) + size_t(h.sizeSensor) * h.numSensors > avail) {
        Unmap(hwiMap_, hwiView_);   // HWiNFO closed ('DEAD') or layout unknown: retry later
        return;
    }
    // Copy once so a concurrent HWiNFO update can't tear strings mid-read.
    std::vector<unsigned char> buf(hwiView_, hwiView_ + size_t(h.offReadings) + size_t(h.sizeReading) * h.numReadings);
    const unsigned char* base = buf.data();
    const bool utfSensor = h.sizeSensor >= sizeof(HwiSensor) + 128;
    const bool utfReading = h.sizeReading >= sizeof(HwiReading) + 144;

    std::vector<std::wstring> sensorNames(h.numSensors);
    std::vector<std::pair<uint32_t, uint32_t>> sensorIds(h.numSensors);
    for (uint32_t i = 0; i < h.numSensors; ++i) {
        const unsigned char* p = base + h.offSensors + size_t(i) * h.sizeSensor;
        HwiSensor se;
        memcpy(&se, p, sizeof se);
        std::wstring n = utfSensor ? FromCp(reinterpret_cast<const char*>(p + sizeof(HwiSensor)), 128, CP_UTF8) : L"";
        if (n.empty()) n = FromCp(se.nameUser, 128, CP_ACP);
        if (n.empty()) n = FromCp(se.nameOrig, 128, CP_ACP);
        sensorNames[i] = n;
        sensorIds[i] = {se.id, se.instance};
    }
    for (uint32_t i = 0; i < h.numReadings; ++i) {
        const unsigned char* p = base + h.offReadings + size_t(i) * h.sizeReading;
        HwiReading r;
        memcpy(&r, p, sizeof r);
        if (r.type == 0 || r.sensorIndex >= h.numSensors) continue;
        std::wstring label = utfReading ? FromCp(reinterpret_cast<const char*>(p + sizeof(HwiReading)), 128, CP_UTF8) : L"";
        if (label.empty()) label = FromCp(r.labelUser, 128, CP_ACP);
        if (label.empty()) label = FromCp(r.labelOrig, 128, CP_ACP);
        std::wstring unit = utfReading ? FromCp(reinterpret_cast<const char*>(p + sizeof(HwiReading) + 128), 16, CP_UTF8) : L"";
        if (unit.empty()) unit = FromCp(r.unit, 16, CP_ACP);
        char key[64];
        snprintf(key, sizeof key, "hwi.%x.%x.%x", sensorIds[r.sensorIndex].first, sensorIds[r.sensorIndex].second, r.id);
        static const int kindMap[] = {K_OTHER, K_TEMP, K_VOLT, K_FAN, K_CURRENT, K_POWER, K_CLOCK, K_USAGE, K_OTHER};
        int kind = r.type < 9 ? kindMap[r.type] : K_OTHER;
        double range = 0;
        if (unit == L"%") range = 100;
        else if (kind == K_TEMP) range = std::max(100.0, r.valueMax * 1.1);
        else if (kind == K_FAN) range = std::max(2000.0, r.valueMax * 1.15);
        else range = std::max({r.valueMax * 1.15, std::fabs(r.value) * 1.3, 1.0});
        int dec = kind == K_VOLT ? 3 : (kind == K_FAN || kind == K_CLOCK || unit == L"%") ? 0 : 1;
        Add(key, sensorNames[r.sensorIndex], label, unit, r.value, true, range, dec, kind);
        ++hwinfoCount;
    }
    hwinfoOk = true;
}

} // namespace sensors
