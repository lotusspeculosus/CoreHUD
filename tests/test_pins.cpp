// Decoder tests for the external per-pin meters. Test vectors are built from the published protocol
// layouts (WireView Pro II: Gustav0ar/wireview-pro-ii docs/protocol.md, PMD2X: ElmorLabs/PMD2-Python,
// MSI GPU Safeguard+: boboom66/msi-psu-monitor docs/protocol.md).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "../src/pinsources.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
static bool Near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

static void Put16(uint8_t* p, int v) { p[0] = uint8_t(v & 0xFF); p[1] = uint8_t((v >> 8) & 0xFF); }
static void Put32(uint8_t* p, int64_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t((v >> (8 * i)) & 0xFF); }

// encodes value = mantissa * 2^exp with an 11-bit unsigned mantissa (as the MSI protocol notes describe)
static uint16_t L11(double value) {
    for (int e = -16; e <= 15; ++e) {
        double m = value / std::ldexp(1.0, e);
        if (m >= 0 && m < 2048 && std::fabs(m - std::round(m)) < 1e-9) return uint16_t(((e & 0x1F) << 11) | (int(std::round(m)) & 0x7FF));
    }
    for (int e = -16; e <= 15; ++e) {   // nearest representable
        double m = value / std::ldexp(1.0, e);
        if (m >= 0 && m < 2048) return uint16_t(((e & 0x1F) << 11) | (int(std::round(m)) & 0x7FF));
    }
    return 0;
}

int main() {
    // ---- LINEAR11 ----
    CHECK(Near(pins::Linear11(0x0000), 0));
    CHECK(Near(pins::Linear11(uint16_t((0 << 11) | 1000)), 1000));            // exp 0
    CHECK(Near(pins::Linear11(uint16_t((2 << 11) | 3000 / 4)), 3000));        // exp +2
    CHECK(Near(pins::Linear11(uint16_t((0x1E << 11) | 48)), 12.0));           // exp -2: 48 / 4
    CHECK(Near(pins::Linear11(uint16_t((0x10 << 11) | 1)), std::ldexp(1.0, -16)));   // most negative exponent

    // ---- WireView Pro II (100 bytes, little endian) ----
    {
        uint8_t b[100] = {};
        Put16(b + 0, 412);    // 41.2 C in
        Put16(b + 2, 398);    // 39.8 C out
        Put16(b + 4, -1000);  // probe absent (-100.0 C)
        Put16(b + 6, 255);    // 25.5 C
        const double amps[6] = {7.512, 7.4, 8.05, 7.9, 7.333, 7.6};
        for (int i = 0; i < 6; ++i) {
            uint8_t* p = b + 12 + 12 * i;
            Put16(p, 12050 - i * 10);                   // mV
            Put32(p + 4, int64_t(std::llround(amps[i] * 1000)));   // mA
        }
        Put16(b + 92, 12030);
        Put16(b + 96, 0x0004);
        pins::Reading r;
        CHECK(pins::DecodeWireView(b, 100, r));
        CHECK(r.pinCount == 6 && r.perPinVolts);
        bool ok = true;
        for (int i = 0; i < 6; ++i) ok = ok && Near(r.amps[i], amps[i], 1e-9) && Near(r.volts[i], (12050 - i * 10) / 1000.0, 1e-9);
        CHECK(ok);
        CHECK(r.activeFaults == 0x0004);
        int temps = 0;
        bool absentSkipped = true;
        for (auto& e : r.extras) {
            if (e.key == "wireview.temp_in") { CHECK(Near(e.value, 41.2, 1e-9)); ++temps; }
            if (e.key == "wireview.temp_out") { CHECK(Near(e.value, 39.8, 1e-9)); ++temps; }
            if (e.key == "wireview.temp_ext1") absentSkipped = false;
            if (e.key == "wireview.temp_ext2") { CHECK(Near(e.value, 25.5, 1e-9)); ++temps; }
            if (e.key == "wireview.vavg") CHECK(Near(e.value, 12.03, 1e-9));
        }
        CHECK(temps == 3 && absentSkipped);
        CHECK(!pins::DecodeWireView(b, 99, r));
    }

    // ---- ElmorLabs PMD2X (192 bytes, pack 1) ----
    {
        uint8_t b[192] = {};
        Put16(b + 0, 3300);
        Put16(b + 2, 351);    // 35.1 C chip
        for (int s = 0; s < 16; ++s) {
            uint8_t* p = b + 8 + 10 * s;
            Put16(p, 11900 + s);
            Put32(p + 2, 1000 * s + 123);   // mA
            Put32(p + 6, 0);
        }
        uint8_t* tail = b + 8 + 160;
        Put16(tail + 0, 120);
        Put16(tail + 2, 45);
        Put16(tail + 4, 30);
        Put16(tail + 6, 640);
        pins::Reading r;
        CHECK(pins::DecodePmd2x(b, 192, r));
        bool ok = r.pinCount == 6;
        for (int w = 0; w < 6; ++w) ok = ok && Near(r.amps[w], (1000 * (10 + w) + 123) / 1000.0, 1e-9) && Near(r.volts[w], (11900 + 10 + w) / 1000.0, 1e-9);
        CHECK(ok);
        bool total = false;
        for (auto& e : r.extras) if (e.key == "pmd2x.total_w") total = Near(e.value, 640);
        CHECK(total);
        CHECK(!pins::DecodePmd2x(b, 122, r));   // PMD2 (no per-wire data) is not accepted
    }

    // ---- MSI MPG Ai1600TS GPU Safeguard+ payload (44 data bytes, LINEAR11) ----
    {
        uint8_t d[62] = {};
        Put16(d + 0, L11(48000));           // 48 A total 12V (mA)
        const double pinmA[12] = {8000, 7936, 8064, 7808, 8128, 7872, 0, 0, 0, 0, 0, 0};
        for (int i = 0; i < 12; ++i) Put16(d + 6 + 2 * i, L11(pinmA[i]));
        Put16(d + 30, L11(576.0 * 1e6));    // ~576 W in microwatts (nearest representable)
        Put16(d + 32, L11(9300));           // 93.00 %
        Put16(d + 34, L11(41000));          // 41 C in milli-degrees
        Put16(d + 36, L11(800));            // fan RPM
        Put16(d + 38, L11(12032));          // 12.032 V in mV
        pins::Reading r;
        CHECK(pins::DecodeMsiPsu(d, sizeof d, r));
        CHECK(r.pinCount == 6 && !r.perPinVolts);
        bool ok = true;
        for (int i = 0; i < 6; ++i) ok = ok && Near(r.amps[i], pinmA[i] / 1000.0, 0.02) && Near(r.volts[i], 12.032, 0.01);
        CHECK(ok);
        // second connector loaded -> 12 pins
        for (int i = 6; i < 12; ++i) Put16(d + 6 + 2 * i, L11(4000));
        CHECK(pins::DecodeMsiPsu(d, sizeof d, r) && r.pinCount == 12 && Near(r.amps[11], 4.0, 0.01));
        // wrong offset / garbage: the 12 V plausibility check rejects it
        uint8_t junk[62];
        for (int i = 0; i < 62; ++i) junk[i] = uint8_t(i * 37);
        CHECK(!pins::DecodeMsiPsu(junk, sizeof junk, r));
        CHECK(!pins::DecodeMsiPsu(d, 40, r));
    }

    printf("\npins: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
