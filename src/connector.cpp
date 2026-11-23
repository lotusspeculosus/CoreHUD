#include "connector.h"

#include <algorithm>
#include <cwctype>
#include <map>

namespace conn {
namespace {

std::wstring Lower(const std::wstring& s) {
    std::wstring r = s;
    for (auto& c : r) c = wchar_t(towlower(c));
    return r;
}
bool Has(const std::wstring& hay, const wchar_t* needle) { return hay.find(needle) != std::wstring::npos; }

// A GPU sensor (not the motherboard's CPU 8-pin EPS, not a PSU rail summary).
bool IsGpu(const std::wstring& group, const std::wstring& name) {
    std::wstring g = Lower(group), n = Lower(name);
    if (Has(n, L"cpu") || Has(n, L"eps") || Has(g, L"cpu") || Has(g, L"motherboard") || Has(g, L"psu")) return false;
    for (const wchar_t* k : {L"gpu", L"geforce", L"radeon", L"nvidia", L"intel arc", L"rtx", L"12vhpwr"})
        if (Has(g, k) || Has(n, k)) return true;
    return false;
}

bool Is16Pin(const std::wstring& n) {
    return Has(n, L"16-pin") || Has(n, L"16 pin") || Has(n, L"16pin") || Has(n, L"hvpwr") || Has(n, L"12vhpwr") ||
           Has(n, L"12v-2x6") || Has(n, L"12v 2x6") || Has(n, L"12v2x6");
}

// "8-pin" / "8 pin" not preceded by another digit (so "18-pin" doesn't count); returns the "#n" index or 1.
int EightPinIndex(const std::wstring& n) {
    for (const wchar_t* k : {L"8-pin", L"8 pin", L"8pin"}) {
        size_t p = n.find(k);
        while (p != std::wstring::npos) {
            if (p == 0 || !iswdigit(n[p - 1])) {
                size_t h = n.find(L'#', p);
                if (h != std::wstring::npos && h + 1 < n.size() && iswdigit(n[h + 1])) return std::clamp(int(n[h + 1] - L'0'), 1, 9);
                return 1;
            }
            p = n.find(k, p + 1);
        }
    }
    return 0;
}

void Store(Rail& r, const Input& in) {
    if (!in.ok || !std::isfinite(in.value)) return;
    std::wstring u = Lower(in.unit);
    if (u == L"w") r.watts = in.value;
    else if (u == L"v") r.volts = in.value;
    else if (u == L"a") r.amps = in.value;
}

} // namespace

double Rail::Power() const {
    if (std::isfinite(watts)) return watts;
    if (std::isfinite(volts) && std::isfinite(amps)) return volts * amps;
    return NAN;
}

Result Detect(const std::vector<Input>& sensors, bool perPinCurrent) {
    Result r;
    bool hp16 = false, hpPerPin = false;
    std::wstring hpName;
    std::map<int, Rail> rails;
    for (const auto& s : sensors) {
        if (!IsGpu(s.group, s.name)) continue;
        std::wstring n = Lower(s.name);
        if (Is16Pin(n)) {
            bool perPin = Has(n, L"pin 1") || Has(n, L"pin 2") || Has(n, L"pin 3") || Has(n, L"pin 4") || Has(n, L"pin 5") || Has(n, L"pin 6");
            if (!hp16 || (!perPin && hpPerPin)) { hpName = s.name; hpPerPin = perPin; }   // prefer the connector's own sensor name
            hp16 = true;
            // the connector total, not a single pin
            if (Lower(s.unit) == L"w" && s.ok && !perPin) r.hpwrWatts = s.value;
            continue;
        }
        int idx = EightPinIndex(n);
        if (idx > 0) {
            Rail& rail = rails[idx];
            rail.index = idx;
            Store(rail, s);
        }
    }
    if (perPinCurrent || hp16) {
        r.layout = HPWR16;
        r.fromTelemetry = true;
        r.how = hp16 ? hpName + L" sensor" : L"per-pin sensor";
        return r;
    }
    if (rails.empty()) return r;
    // Count the rails that are really connected: a rail whose voltage reads ~0 V has a sensor but no cable.
    for (auto& [i, rail] : rails)
        if (!std::isfinite(rail.volts) || rail.volts > 6) r.rails.push_back(rail);
    int n = int(std::min<size_t>(r.rails.size(), 3));
    if (n == 0) return r;
    r.layout = n == 1 ? PCIE8_1 : n == 2 ? PCIE8_2 : PCIE8_3;
    r.fromTelemetry = true;
    r.how = std::to_wstring(n) + (n == 1 ? L" GPU 8-pin rail" : L" GPU 8-pin rails") + L" reported";
    return r;
}

Result Effective(const Result& detected, Layout userChoice) {
    if (userChoice == UNKNOWN) return detected;
    Result r = detected;
    if (userChoice != detected.layout) {   // the telemetry describes another layout: don't show its values
        r.rails.clear();
        r.hpwrWatts = NAN;
    }
    r.layout = userChoice;
    r.fromTelemetry = false;
    r.how = L"set in Settings";
    return r;
}

int PinConnectors(Layout l) { return l == HPWR16 || l == PCIE8_1 ? 1 : l == PCIE8_2 ? 2 : l == PCIE8_3 ? 3 : 0; }

const wchar_t* Name(Layout l) {
    switch (l) {
    case HPWR16: return L"12V-2x6 (16-pin)";
    case PCIE8_1: return L"1× 8-pin PCIe";
    case PCIE8_2: return L"2× 8-pin PCIe";
    case PCIE8_3: return L"3× 8-pin PCIe";
    case NONE: return L"no power cable (slot only)";
    default: return L"not detected";
    }
}

const wchar_t* Id(Layout l) {
    switch (l) {
    case HPWR16: return L"12v2x6";
    case PCIE8_1: return L"8pin1";
    case PCIE8_2: return L"8pin2";
    case PCIE8_3: return L"8pin3";
    case NONE: return L"none";
    default: return L"auto";
    }
}

Layout FromId(const std::wstring& id) {
    std::wstring s = Lower(id);
    if (s == L"12v2x6" || s == L"16pin" || s == L"12vhpwr") return HPWR16;
    if (s == L"8pin1" || s == L"8pin") return PCIE8_1;
    if (s == L"8pin2") return PCIE8_2;
    if (s == L"8pin3") return PCIE8_3;
    if (s == L"none") return NONE;
    return UNKNOWN;
}

} // namespace conn
