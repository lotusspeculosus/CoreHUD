#include "sensors.h"
#include "json.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <wbemidl.h>
#include <winhttp.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>

namespace sensors {
namespace {
using uchar = unsigned char;

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
    if (http_) { WinHttpCloseHandle(http_); http_ = nullptr; }
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

// ---- LibreHardwareMonitor ----
// v0.9.5+ removed WMI (.NET 10): data comes from its local web server (Options > Remote Web Server > Run,
// http://127.0.0.1:8085/data.json). Older versions publish ROOT\LibreHardwareMonitor via WMI.
namespace {

using json::JVal;
using json::JParser;

// "45,3 °C" / "1,234.5 MHz" / "12.0" -> number (handles ',' or '.' decimal separators)
bool LeadingNumber(const std::string& s, double& out) {
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') ++i;
    size_t st = i;
    while (i < s.size() && (isdigit(uchar(s[i])) || s[i] == '.' || s[i] == ',' || s[i] == '-')) ++i;
    std::string n = s.substr(st, i - st);
    if (n.empty()) return false;
    size_t lastDot = n.rfind('.'), lastComma = n.rfind(',');
    char dec = 0;
    if (lastDot != std::string::npos && lastComma != std::string::npos) dec = lastDot > lastComma ? '.' : ',';
    else if (lastDot != std::string::npos) dec = n.find('.') == lastDot ? '.' : 0;
    else if (lastComma != std::string::npos) dec = (n.find(',') == lastComma && n.size() - lastComma - 1 != 3) ? ',' : 0;
    std::string c;
    for (char ch : n) {
        if (ch == '.' || ch == ',') { if (ch == dec) c += '.'; }
        else c += ch;
    }
    char* end = nullptr;
    out = strtod(c.c_str(), &end);
    return end != c.c_str();
}

std::wstring FromU8(const std::string& s) { return FromCp(s.c_str(), s.size(), CP_UTF8); }

} // namespace


bool Registry::LhmViaWmi(std::vector<LhmItem>& out) {
    DWORD now = GetTickCount();
    if (!wmiServices_) {
        if (lhmRetryTick_ && now - lhmRetryTick_ < 10000) return false;
        lhmRetryTick_ = now;
        if (!wmiLocator_) {
            IWbemLocator* loc = nullptr;
            if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator, reinterpret_cast<void**>(&loc))))
                return false;
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
        if (!svc) return false;
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
    bool ok = query(L"SELECT Identifier, Name, SensorType, Value, Parent FROM Sensor", [&](IWbemClassObject* o) {
        LhmItem it;
        it.id = str(o, L"Identifier");
        it.name = str(o, L"Name");
        it.type = str(o, L"SensorType");
        it.hwId = str(o, L"Parent");
        auto hw = lhmHardware_.find(it.hwId);
        it.hwName = hw != lhmHardware_.end() ? hw->second : it.hwId;
        VARIANT v;
        VariantInit(&v);
        it.value = 0;
        it.have = false;
        if (SUCCEEDED(o->Get(L"Value", 0, &v, nullptr, nullptr))) {
            if (v.vt == VT_R4) { it.value = v.fltVal; it.have = true; }
            else if (v.vt == VT_R8) { it.value = v.dblVal; it.have = true; }
        }
        VariantClear(&v);
        if (it.type == L"Throughput") it.value /= 1048576.0;   // B/s -> MB/s
        out.push_back(std::move(it));
    });
    if (!ok || out.empty()) {   // LHM closed (or 0.9.5+ with an empty provider): drop the connection
        svc->Release();
        wmiServices_ = nullptr;
        lhmRetryTick_ = now;
        return false;
    }
    lhmVia = L"WMI";
    return true;
}

bool Registry::LhmViaHttp(std::vector<LhmItem>& out, int port) {
    DWORD now = GetTickCount();
    if (lhmHttpFailTick_ && now - lhmHttpFailTick_ < 5000) return false;   // not running: don't knock every sample
    if (!http_) http_ = WinHttpOpen(L"RigSense", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!http_) return false;
    WinHttpSetTimeouts(http_, 300, 300, 500, 1500);
    std::string body;
    bool ok = false;
    HINTERNET c = WinHttpConnect(http_, L"127.0.0.1", INTERNET_PORT(port), 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", L"/data.json", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : nullptr;
    if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
        DWORD status = 0, sz = sizeof status;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(r, &avail) && avail) {
                size_t off = body.size();
                body.resize(off + avail);
                DWORD got = 0;
                if (!WinHttpReadData(r, &body[off], avail, &got)) { body.resize(off); break; }
                body.resize(off + got);
                if (body.size() > 16 * 1024 * 1024) break;
            }
            ok = !body.empty();
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (!ok) { lhmHttpFailTick_ = now; return false; }

    JVal root;
    JParser jp{body.data(), body.data() + body.size()};
    if (!jp.val(root)) { lhmHttpFailTick_ = now; return false; }
    // Walk the tree: hardware > sensor-type group > sensor (sensor nodes carry "SensorId").
    std::vector<std::string> path;
    std::function<void(const JVal&)> walk = [&](const JVal& n) {
        if (n.t != JVal::Obj) return;
        const JVal* text = n.get("Text");
        const JVal* sid = n.get("SensorId");
        if (sid && sid->t == JVal::Str) {
            LhmItem it;
            it.id = FromU8(sid->str);
            it.name = text && text->t == JVal::Str ? FromU8(text->str) : it.id;
            const JVal* type = n.get("Type");
            it.type = type && type->t == JVal::Str ? FromU8(type->str) : L"";
            size_t a = sid->str.rfind('/');
            size_t b = a == std::string::npos || a == 0 ? std::string::npos : sid->str.rfind('/', a - 1);
            it.hwId = FromU8(b == std::string::npos ? sid->str : sid->str.substr(0, b));
            it.hwName = path.size() >= 2 ? FromU8(path[path.size() - 2]) : it.hwId;
            it.value = 0;
            it.have = false;
            const JVal* raw = n.get("RawValue");
            const JVal* val = n.get("Value");
            if (raw && raw->t == JVal::Num) { it.value = raw->num; it.have = true; }
            else if (raw && raw->t == JVal::Str) it.have = LeadingNumber(raw->str, it.value);
            if (!it.have && val && val->t == JVal::Str) {
                it.have = LeadingNumber(val->str, it.value);
                if (it.have && it.type == L"Throughput") {   // "12.3 KB/s" -> MB/s
                    if (val->str.find("KB/s") != std::string::npos) it.value /= 1024.0;
                    else if (val->str.find("GB/s") != std::string::npos) it.value *= 1024.0;
                    else if (val->str.find("MB/s") == std::string::npos) it.value /= 1048576.0;
                }
            } else if (it.have && it.type == L"Throughput") it.value /= 1048576.0;   // raw B/s
            out.push_back(std::move(it));
            return;
        }
        path.push_back(text && text->t == JVal::Str ? text->str : "");
        if (const JVal* ch = n.get("Children"))
            if (ch->t == JVal::Arr)
                for (auto& c2 : ch->arr) walk(c2);
        path.pop_back();
    };
    walk(root);
    if (out.empty()) { lhmHttpFailTick_ = now; return false; }
    lhmHttpFailTick_ = 0;
    lhmVia = L"web server";
    return true;
}

void Registry::AddLhm(int port) {
    lhmOk = false;
    lhmCount = 0;
    lhmGpu_.clear();
    DWORD now = GetTickCount();
    static DWORD procTick = 0;
    if (!procTick || now - procTick > 3000) {
        const wchar_t* names[] = {L"LibreHardwareMonitor.exe"};
        lhmProcess = ProcessRunning(names, 1);
        procTick = now;
    }
    if (!lhmProcess) { lhmCache_.clear(); return; }
    if (!lhmCacheTick_ || now - lhmCacheTick_ >= 1000) {   // data.json can be large: refresh once per second
        std::vector<LhmItem> items;
        if (!LhmViaWmi(items)) { items.clear(); LhmViaHttp(items, port); }
        lhmCache_ = std::move(items);
        lhmCacheTick_ = now;
    }
    struct TypeInfo { const wchar_t* type; const wchar_t* unit; int dec; int kind; double range; };
    static const TypeInfo kTypes[] = {
        {L"Voltage", L"V", 3, K_VOLT, 0}, {L"Current", L"A", 2, K_CURRENT, 0}, {L"Power", L"W", 1, K_POWER, 0},
        {L"Clock", L"MHz", 0, K_CLOCK, 0}, {L"Temperature", L"°C", 1, K_TEMP, 100}, {L"Load", L"%", 0, K_USAGE, 100},
        {L"Frequency", L"Hz", 0, K_OTHER, 0}, {L"Fan", L"RPM", 0, K_FAN, 0}, {L"Flow", L"L/h", 0, K_OTHER, 0},
        {L"Control", L"%", 0, K_USAGE, 100}, {L"Level", L"%", 0, K_USAGE, 100}, {L"Factor", L"", 2, K_OTHER, 0},
        {L"Data", L"GB", 1, K_OTHER, 0}, {L"SmallData", L"MB", 0, K_OTHER, 0}, {L"Throughput", L"MB/s", 1, K_OTHER, 0},
        {L"TimeSpan", L"s", 0, K_OTHER, 0}, {L"Energy", L"mWh", 0, K_OTHER, 0}, {L"Noise", L"dBA", 0, K_OTHER, 0},
        {L"Humidity", L"%", 0, K_OTHER, 100},
    };
    for (auto& it : lhmCache_) {
        const TypeInfo* ti = nullptr;
        for (auto& t : kTypes) if (it.type == t.type) ti = &t;
        bool have = it.have && !std::isnan(it.value);
        Add("lhm." + EscKey(ToU8(it.id)), L"LHM · " + it.hwName, it.name, ti ? ti->unit : L"", it.value, have,
            ti ? ti->range : 0, ti ? ti->dec : 1, ti ? ti->kind : K_OTHER);
        if (it.hwId.rfind(L"/gpu", 0) == 0 && have)   // /gpu-amd/0, /gpu-intel/0, /gpu-intel-integrated/...
            lhmGpu_.push_back({it.hwId, it.hwName, it.type, it.name, it.value});
        ++lhmCount;
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
    AddLhm(s.cfg.lhmPort);
    EnrichGpuFromLhm();
    AddHwinfo();
}

// AMD / Intel GPUs: Windows counters give load, VRAM, temperature, fan and power %. When LibreHardwareMonitor
// runs (it reads AMD ADL / Intel IGCL itself), fill in what Windows can't: core clock, hotspot, VRAM temp,
// core voltage, board power in watts, fan duty. Values RigSense already has stay as they are.
void Registry::EnrichGpuFromLhm() {
    gpuFromLhm = false;
    if (!gpuViaWindows || lhmGpu_.empty()) return;
    auto lower = [](std::wstring s) { for (auto& c : s) c = wchar_t(towlower(c)); return s; };
    // pick the LHM GPU that matches the main GPU's name, else the first non-NVIDIA GPU
    std::wstring want = lower(gpuName), parent;
    for (auto& g : lhmGpu_)
        if (!want.empty() && (lower(g.hwName).find(want) != std::wstring::npos || want.find(lower(g.hwName)) != std::wstring::npos)) { parent = g.parent; break; }
    if (parent.empty())
        for (auto& g : lhmGpu_) if (g.parent.rfind(L"/gpu-nvidia", 0) != 0) { parent = g.parent; break; }
    if (parent.empty()) return;

    std::wstring group = std::wstring(L"GPU \u00b7 ") + (gpuName.empty() ? L"main GPU" : gpuName);
    auto fill = [&](const char* key, const wchar_t* name, const wchar_t* unit, double v, double range, int dec, int kind) {
        auto it = index_.find(key);
        if (it != index_.end()) {
            Sensor& s = list_[it->second];
            if (s.ok) return;            // keep the existing (Windows) value
            s.value = v;
            s.ok = true;
            if (range > 0) s.rangeMax = range;
        } else {
            Add(key, group, name, unit, v, true, range, dec, kind);
        }
        gpuFromLhm = true;
    };
    bool havePower = false;
    for (int pass = 0; pass < 2; ++pass)   // pass 0: preferred names, pass 1: fallbacks
        for (auto& g : lhmGpu_) {
            if (g.parent != parent) continue;
            std::wstring n = lower(g.name);
            bool hot = n.find(L"hot spot") != std::wstring::npos || n.find(L"hotspot") != std::wstring::npos || n.find(L"junction") != std::wstring::npos;
            bool mem = n.find(L"memory") != std::wstring::npos || n.find(L"vram") != std::wstring::npos;
            bool core = n.find(L"core") != std::wstring::npos;
            if (pass == 0) {
                if (g.type == L"Clock" && core) fill("gpu.core_mhz", L"Core clock", L"MHz", g.value, 3300, 0, K_CLOCK);
                else if (g.type == L"Clock" && mem) fill("gpu.mem_mhz", L"Memory clock", L"MHz", g.value, 0, 0, K_CLOCK);
                else if (g.type == L"Temperature" && hot) fill("gpu.hotspot", L"Hotspot temperature", L"\u00b0C", g.value, 110, 1, K_TEMP);
                else if (g.type == L"Temperature" && mem) fill("gpu.vram_temp", L"VRAM temperature", L"\u00b0C", g.value, 110, 1, K_TEMP);
                else if (g.type == L"Temperature" && core) fill("gpu.temp", L"GPU temperature", L"\u00b0C", g.value, 100, 0, K_TEMP);
                else if (g.type == L"Voltage" && core) fill("gpu.core_mv", L"Core voltage", L"mV", g.value * 1000, 1300, 0, K_VOLT);
                else if (g.type == L"Power" && (n.find(L"package") != std::wstring::npos || n.find(L"total") != std::wstring::npos || n.find(L"board") != std::wstring::npos)) {
                    if (!havePower) fill("gpu.board_w", L"Board power", L"W", g.value, 0, 0, K_POWER);
                    havePower = true;
                } else if (g.type == L"Fan") fill("gpu.fan1_rpm", L"Fan speed", L"RPM", g.value, 3500, 0, K_FAN);
                else if (g.type == L"Control") fill("gpu.fan1_pct", L"Fan duty", L"%", g.value, 100, 0, K_USAGE);
                else if (g.type == L"Load" && core) fill("gpu.util", L"GPU load", L"%", g.value, 100, 0, K_USAGE);
            } else if (g.type == L"Power" && !havePower) {
                fill("gpu.board_w", L"Board power", L"W", g.value, 0, 0, K_POWER);
                havePower = true;
            }
        }
}

void Registry::AddBuiltin(const mon::Snapshot& s) {
    const std::wstring g = s.pinSource.empty() ? std::wstring(L"12V-2x6 connector (per-pin)") : L"12V-2x6 \u00b7 " + s.pinSource;
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
    if (s.pinCount > 6)   // MSI PSU with a second loaded 12V-2x6 connector
        for (int i = 6; i < 12; ++i) {
            snprintf(key, sizeof key, "astral.c2_pin%d_a", i - 5);
            swprintf(name, 32, L"Connector 2 pin %d current", i - 5);
            auto& p = Add(key, g, name, L"A", s.amps[i], ok, 10, 2, K_CURRENT);
            p.warn = c.warnA;
            p.crit = c.limitA;
        }
    for (auto& e : s.pinExtras)
        Add(e.key, g, e.name, e.unit, e.value, ok, e.unit == L"\u00b0C" ? 100 : 0, e.decimals, e.unit == L"\u00b0C" ? K_TEMP : K_OTHER);

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
        // Not available from Windows: published offline so panels show the right name; LibreHardwareMonitor fills them in.
        Add("gpu.core_mhz", gg, L"Core clock", L"MHz", 0, false, 3300, 0, K_CLOCK);
        Add("gpu.vram_temp", gg, L"VRAM temperature", L"°C", 0, false, 110, 1, K_TEMP);
        Add("gpu.core_mv", gg, L"Core voltage", L"mV", 0, false, 1300, 0, K_VOLT);
        Add("gpu.board_w", gg, L"Board power", L"W", 0, false, 0, 0, K_POWER);
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
