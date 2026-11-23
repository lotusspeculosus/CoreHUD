// Tests for the power connector detection (connector.cpp): 12V-2x6 vs 8-pin from power-rail telemetry.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "../src/connector.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static conn::Input S(const wchar_t* group, const wchar_t* name, const wchar_t* unit, double v, bool ok = true) { return {group, name, unit, v, ok}; }

int main() {
    using namespace conn;
    const wchar_t* hwiGpu = L"HWiNFO: GPU [#0]: AMD Radeon RX 9070 XT";
    // nothing GPU-related: unknown
    CHECK(Detect({}, false).layout == UNKNOWN);
    CHECK(Detect({S(L"Motherboard", L"+12V", L"V", 12.1), S(L"HWiNFO: CPU", L"CPU Package Power", L"W", 60)}, false).layout == UNKNOWN);
    // a per-pin sensor seeing current = 12V-2x6
    {
        auto r = Detect({}, true);
        CHECK(r.layout == HPWR16 && r.fromTelemetry && r.how == L"per-pin sensor");
    }
    // HWiNFO-style 16-pin rail (NVIDIA RTX 40/50)
    {
        auto r = Detect({S(L"HWiNFO: GPU [#0]: NVIDIA GeForce RTX 4080", L"GPU 16-pin HVPWR Voltage", L"V", 12.05),
                         S(L"HWiNFO: GPU [#0]: NVIDIA GeForce RTX 4080", L"GPU 16-pin HVPWR Power", L"W", 280.5),
                         S(L"HWiNFO: GPU [#0]: NVIDIA GeForce RTX 4080", L"GPU PCIe +12V Input Power", L"W", 20)},
                        false);
        CHECK(r.layout == HPWR16);
        CHECK(std::fabs(r.hpwrWatts - 280.5) < 1e-9);
        CHECK(r.how.find(L"16-pin HVPWR") != std::wstring::npos);
    }
    // LibreHardwareMonitor Astral names: total "12VHPWR Connector" W is the connector power, pins aren't
    {
        auto r = Detect({S(L"NVIDIA GeForce RTX 5090", L"12VHPWR Pin 1", L"W", 7.1), S(L"NVIDIA GeForce RTX 5090", L"12VHPWR Connector", L"W", 44.1)}, false);
        CHECK(r.layout == HPWR16 && std::fabs(r.hpwrWatts - 44.1) < 1e-9);
        CHECK(r.how == L"12VHPWR Connector sensor");   // the connector's own name, not "Pin 1"
        auto e = Effective(r, PCIE8_2);                 // user says 8-pin: the 16-pin reading must not show up
        CHECK(e.layout == PCIE8_2 && std::isnan(e.hpwrWatts));
        CHECK(std::fabs(Effective(r, HPWR16).hpwrWatts - 44.1) < 1e-9);
    }
    // two 8-pin rails (AMD board partner card), with voltages and power
    {
        auto r = Detect({S(hwiGpu, L"GPU 8-pin #1 Input Voltage", L"V", 12.1), S(hwiGpu, L"GPU 8-pin #1 Input Power", L"W", 140),
                         S(hwiGpu, L"GPU 8-pin #2 Input Voltage", L"V", 12.08), S(hwiGpu, L"GPU 8-pin #2 Input Power", L"W", 132),
                         S(hwiGpu, L"GPU 8-pin #3 Input Voltage", L"V", 0.0)},   // a rail the card has a sensor for, but no cable
                        false);
        CHECK(r.layout == PCIE8_2);
        CHECK(r.rails.size() == 2 && r.rails[0].index == 1 && r.rails[1].index == 2);
        CHECK(r.rails.size() == 2 && std::fabs(r.rails[1].Power() - 132) < 1e-9);
        CHECK(PinConnectors(r.layout) == 2);
    }
    // three rails, power from volts x amps, no index on the first
    {
        auto r = Detect({S(hwiGpu, L"GPU 8-pin Input Current", L"A", 10), S(hwiGpu, L"GPU 8-pin Input Voltage", L"V", 12),
                         S(hwiGpu, L"GPU 8-pin #2 Input Power", L"W", 100), S(hwiGpu, L"GPU 8-pin #3 Input Power", L"W", 90)},
                        false);
        CHECK(r.layout == PCIE8_3);
        CHECK(r.rails.size() == 3 && std::fabs(r.rails[0].Power() - 120) < 1e-9);
    }
    // only power readings (no voltages): every rail seen counts
    CHECK(Detect({S(hwiGpu, L"GPU 8-pin #1 Input Power", L"W", 0)}, false).layout == PCIE8_1);
    // the motherboard's CPU 8-pin (EPS) and an "18-pin" name don't count
    CHECK(Detect({S(L"HWiNFO: ASUS ROG CROSSHAIR", L"CPU 8-pin EPS Power", L"W", 90)}, false).layout == UNKNOWN);
    CHECK(Detect({S(hwiGpu, L"GPU 18-pin Test", L"W", 5)}, false).layout == UNKNOWN);
    // 16-pin wins over 8-pin names on the same card
    CHECK(Detect({S(hwiGpu, L"GPU 8-pin #1 Input Power", L"W", 50), S(hwiGpu, L"GPU 16-pin HVPWR Power", L"W", 300)}, false).layout == HPWR16);
    // user choice overrides; auto keeps the detection
    {
        auto d = Detect({S(hwiGpu, L"GPU 8-pin #1 Input Voltage", L"V", 12), S(hwiGpu, L"GPU 8-pin #2 Input Voltage", L"V", 12)}, false);
        CHECK(Effective(d, UNKNOWN).layout == PCIE8_2 && Effective(d, UNKNOWN).fromTelemetry);
        auto e = Effective(d, PCIE8_3);
        CHECK(e.layout == PCIE8_3 && !e.fromTelemetry && e.rails.empty() && e.how == L"set in Settings");
        CHECK(Effective(d, PCIE8_2).rails.size() == 2);   // same as detected: keep the live rail values
    }
    // ids round-trip
    for (Layout l : {UNKNOWN, HPWR16, PCIE8_1, PCIE8_2, PCIE8_3, NONE}) CHECK(FromId(Id(l)) == l);
    CHECK(FromId(L"12VHPWR") == HPWR16 && FromId(L"garbage") == UNKNOWN);
    printf("\nconnector: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
