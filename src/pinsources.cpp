#include "pinsources.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <setupapi.h>
#include <devguid.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace pins {
namespace {

int16_t I16(const uint8_t* p) { return int16_t(uint16_t(p[0]) | (uint16_t(p[1]) << 8)); }
uint16_t U16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
int32_t I32(const uint8_t* p) { return int32_t(uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24)); }
uint32_t U32(const uint8_t* p) { return uint32_t(I32(p)); }

constexpr uint16_t SERIAL_VID = 0x0483, SERIAL_PID = 0x5740;
constexpr uint16_t MSI_VID = 0x0DB0;
struct MsiModel { uint16_t pid; const wchar_t* name; };
const MsiModel kMsiPerPin[] = {{0x808C, L"MSI MPG Ai1600TS"}, {0xAA6F, L"MSI MPG Ai1300TS"}};

// ---- serial helpers ----
bool WriteAll(HANDLE h, const uint8_t* d, DWORD n) {
    DWORD w = 0;
    return WriteFile(h, d, n, &w, nullptr) && w == n;
}
size_t ReadUpTo(HANDLE h, uint8_t* d, size_t n) {   // returns bytes read before the timeouts expire
    size_t got = 0;
    while (got < n) {
        DWORD r = 0;
        if (!ReadFile(h, d + got, DWORD(n - got), &r, nullptr) || r == 0) break;
        got += r;
    }
    return got;
}

std::vector<std::wstring> FindSerialPorts() {
    std::vector<std::wstring> ports;
    HDEVINFO di = SetupDiGetClassDevsW(&GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT);
    if (di == INVALID_HANDLE_VALUE) return ports;
    SP_DEVINFO_DATA dd{sizeof dd};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(di, i, &dd); ++i) {
        wchar_t hw[1024] = {};
        if (!SetupDiGetDeviceRegistryPropertyW(di, &dd, SPDRP_HARDWAREID, nullptr, reinterpret_cast<PBYTE>(hw), sizeof(hw) - 4, nullptr)) continue;
        std::wstring ids;
        for (const wchar_t* p = hw; *p; p += wcslen(p) + 1) ids += std::wstring(p) + L";";
        for (auto& c : ids) c = wchar_t(towupper(c));
        if (ids.find(L"VID_0483&PID_5740") == std::wstring::npos) continue;
        HKEY k = SetupDiOpenDevRegKey(di, &dd, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (k == INVALID_HANDLE_VALUE) continue;
        wchar_t name[64] = {};
        DWORD sz = sizeof(name) - 2, type = 0;
        if (RegQueryValueExW(k, L"PortName", nullptr, &type, reinterpret_cast<BYTE*>(name), &sz) == ERROR_SUCCESS && type == REG_SZ)
            ports.push_back(name);
        RegCloseKey(k);
    }
    SetupDiDestroyDeviceInfoList(di);
    return ports;
}

bool OverlappedIo(HANDLE h, bool write, void* buf, DWORD len, DWORD timeoutMs) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;
    DWORD done = 0;
    BOOL ok = write ? WriteFile(h, buf, len, nullptr, &ov) : ReadFile(h, buf, len, nullptr, &ov);
    if (!ok && GetLastError() != ERROR_IO_PENDING) { CloseHandle(ov.hEvent); return false; }
    if (WaitForSingleObject(ov.hEvent, timeoutMs) != WAIT_OBJECT_0) {
        CancelIo(h);
        GetOverlappedResult(h, &ov, &done, TRUE);
        CloseHandle(ov.hEvent);
        return false;
    }
    ok = GetOverlappedResult(h, &ov, &done, FALSE);
    CloseHandle(ov.hEvent);
    return ok && done > 0;
}

} // namespace

// ---------------- decoders ----------------
double Linear11(uint16_t word) {
    int exp = int(word >> 11);
    if (exp & 0x10) exp -= 32;               // signed 5-bit exponent
    int mant = int(word & 0x7FF);            // 11-bit mantissa, unsigned per the MSI protocol notes
    return std::ldexp(double(mant), exp);
}

bool DecodeWireView(const uint8_t* b, size_t len, Reading& r) {
    if (len != 100) return false;
    r = Reading{};
    r.pinCount = 6;
    r.perPinVolts = true;
    for (int i = 0; i < 6; ++i) {
        const uint8_t* p = b + 12 + 12 * i;
        r.volts[i] = I16(p) / 1000.0;
        r.amps[i] = U32(p + 4) / 1000.0;
    }
    auto temp = [&](int off, const char* key, const wchar_t* name) {
        double t = I16(b + off) / 10.0;
        if (t > -99.0 && t < 200.0) r.extras.push_back({key, name, L"°C", t, 1});
    };
    temp(0, "wireview.temp_in", L"Connector temp (input)");
    temp(2, "wireview.temp_out", L"Connector temp (output)");
    temp(4, "wireview.temp_ext1", L"External probe 1");
    temp(6, "wireview.temp_ext2", L"External probe 2");
    r.extras.push_back({"wireview.vavg", L"Average 12V voltage", L"V", U16(b + 92) / 1000.0, 3});
    r.activeFaults = U16(b + 96);
    return true;
}

bool DecodePmd2x(const uint8_t* b, size_t len, Reading& r) {
    if (len != 192) return false;
    r = Reading{};
    r.pinCount = 6;
    r.perPinVolts = true;
    // u16 Vdd, i16 Tchip, i16 Ts[2], 16 x {i16 V, i32 I, i32 P}, u16 Eps, Pcie, Mb, Total, u8 Ocp[16]  (pack 1)
    const uint8_t* sensors = b + 8;
    for (int w = 0; w < 6; ++w) {             // indexes 10..15 = HPWR wires 1..6
        const uint8_t* p = sensors + 10 * (10 + w);
        r.volts[w] = I16(p) / 1000.0;
        r.amps[w] = I32(p + 2) / 1000.0;
    }
    const uint8_t* tail = sensors + 160;
    r.extras.push_back({"pmd2x.eps_w", L"EPS power", L"W", double(U16(tail + 0)), 0});
    r.extras.push_back({"pmd2x.pcie_w", L"PCIe slot power", L"W", double(U16(tail + 2)), 0});
    r.extras.push_back({"pmd2x.mb_w", L"Motherboard power", L"W", double(U16(tail + 4)), 0});
    r.extras.push_back({"pmd2x.total_w", L"Total measured power", L"W", double(U16(tail + 6)), 0});
    r.extras.push_back({"pmd2x.tchip", L"Meter chip temp", L"°C", I16(b + 2) / 10.0, 1});
    return true;
}

bool DecodeMsiPsu(const uint8_t* d, size_t len, Reading& r) {
    if (len < 44) return false;
    double v12 = Linear11(U16(d + 38)) / 1000.0;
    if (v12 < 10.0 || v12 > 14.0) return false;   // wrong offset / not a GPU Safeguard+ report
    r = Reading{};
    r.perPinVolts = false;
    double conn2 = 0;
    for (int i = 0; i < 12; ++i) {
        r.amps[i] = Linear11(U16(d + 6 + 2 * i)) / 1000.0;
        r.volts[i] = v12;
        if (i >= 6) conn2 += r.amps[i];
    }
    r.pinCount = conn2 > 0.3 ? 12 : 6;            // second connector only when it carries current
    r.extras.push_back({"msipsu.v12", L"PSU +12V voltage", L"V", v12, 3});
    r.extras.push_back({"msipsu.i12", L"PSU +12V current", L"A", Linear11(U16(d + 0)) / 1000.0, 2});
    r.extras.push_back({"msipsu.power", L"PSU output power", L"W", Linear11(U16(d + 30)) / 1e6, 0});
    r.extras.push_back({"msipsu.eff", L"PSU efficiency", L"%", Linear11(U16(d + 32)) / 100.0, 1});
    r.extras.push_back({"msipsu.temp", L"PSU temperature", L"°C", Linear11(U16(d + 34)) / 1000.0, 1});
    r.extras.push_back({"msipsu.fan", L"PSU fan", L"RPM", Linear11(U16(d + 36)), 0});
    return true;
}

// ---------------- device access ----------------
Sources::~Sources() { Close(); }

void Sources::Close() {
    if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
    kind_ = Kind::None;
    port_.clear();
    model_.clear();
    failures_ = 0;
}

std::wstring Sources::Name() const {
    switch (kind_) {
    case Kind::WireView: return L"Thermal Grizzly WireView Pro II (" + port_ + L")";
    case Kind::Pmd2x: return L"ElmorLabs PMD2X (" + port_ + L")";
    case Kind::MsiPsu: return model_ + L" (GPU Safeguard+)";
    default: return L"";
    }
}

std::string Sources::Id() const {
    switch (kind_) {
    case Kind::WireView: return "wireview";
    case Kind::Pmd2x: return "pmd2x";
    case Kind::MsiPsu: return "msipsu";
    default: return "";
    }
}

bool Sources::ProbeSerial(const std::wstring& port) {
    HANDLE h = CreateFileW((L"\\\\.\\" + port).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;   // busy (e.g. the vendor app holds the port) or gone
    DCB dcb{sizeof dcb};
    GetCommState(h, &dcb);
    dcb.BaudRate = 115200;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fOutxCtsFlow = dcb.fOutxDsrFlow = FALSE;
    dcb.fOutX = dcb.fInX = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    COMMTIMEOUTS to{};
    to.ReadIntervalTimeout = 60;
    to.ReadTotalTimeoutConstant = 600;
    to.WriteTotalTimeoutConstant = 500;
    if (!SetCommState(h, &dcb) || !SetCommTimeouts(h, &to)) { CloseHandle(h); return false; }
    // the devices send a greeting once DTR/RTS are asserted
    uint8_t greet[128] = {};
    size_t g = ReadUpTo(h, greet, sizeof greet - 1);
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    // 0483:5740 is ST's generic USB-serial id, used by other gadgets too: only talk to a port whose
    // greeting identifies a WireView or an ElmorLabs meter, so unknown devices never receive a byte.
    std::string gtext;
    for (size_t i = 0; i < g; ++i) gtext += char(tolower(greet[i]));
    if (gtext.find("wireview") == std::string::npos && gtext.find("elmor") == std::string::npos && gtext.find("pmd") == std::string::npos) {
        CloseHandle(h);
        return false;
    }
    uint8_t cmd = 0x01, id[3] = {};
    size_t n = WriteAll(h, &cmd, 1) ? ReadUpTo(h, id, 3) : 0;
    PurgeComm(h, PURGE_RXCLEAR);
    bool wireview = (g && strstr(reinterpret_cast<char*>(greet), "WireView")) || (n >= 2 && id[0] == 0xEF && id[1] == 0x05);
    bool pmd2x = n >= 2 && id[0] == 0xEE && id[1] == 0x17;
    if (!wireview && !pmd2x) { CloseHandle(h); return false; }   // PMD2 (0x15) has no per-wire data
    to.ReadTotalTimeoutConstant = 1000;
    SetCommTimeouts(h, &to);
    handle_ = h;
    port_ = port;
    kind_ = wireview ? Kind::WireView : Kind::Pmd2x;
    return true;
}

bool Sources::ProbeHid() {
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO di = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    SP_DEVICE_INTERFACE_DATA ifd{sizeof ifd};
    for (DWORD i = 0; !found && SetupDiEnumDeviceInterfaces(di, nullptr, &hidGuid, i, &ifd); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(di, &ifd, nullptr, 0, &need, nullptr);
        if (!need) continue;
        std::vector<BYTE> buf(need);
        auto detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(di, &ifd, detail, need, nullptr, nullptr)) continue;
        std::wstring path = detail->DevicePath;
        std::wstring lower = path;
        for (auto& c : lower) c = wchar_t(towlower(c));
        if (lower.find(L"vid_0db0") == std::wstring::npos) continue;
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES at{sizeof at};
        const MsiModel* model = nullptr;
        if (HidD_GetAttributes(h, &at) && at.VendorID == MSI_VID)
            for (auto& m : kMsiPerPin) if (m.pid == at.ProductID) model = &m;
        PHIDP_PREPARSED_DATA pp = nullptr;
        HIDP_CAPS caps{};
        bool vendorPage = false;
        if (model && HidD_GetPreparsedData(h, &pp)) {
            vendorPage = HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS && caps.UsagePage == 0xFF00 &&
                         caps.OutputReportByteLength >= 2 && caps.InputReportByteLength >= 46;
            HidD_FreePreparsedData(pp);
        }
        if (!vendorPage) { CloseHandle(h); continue; }
        handle_ = h;
        kind_ = Kind::MsiPsu;
        model_ = model->name;
        outLen_ = caps.OutputReportByteLength;
        inLen_ = caps.InputReportByteLength;
        found = true;
    }
    SetupDiDestroyDeviceInfoList(di);
    return found;
}

bool Sources::Discover() {
    DWORD now = GetTickCount();
    if (lastScan_ && now - lastScan_ < 5000) return false;
    lastScan_ = now;
    if (preferred_ == "off") return false;
    if (preferred_ == "auto" || preferred_ == "wireview" || preferred_ == "pmd2x")
        for (auto& p : FindSerialPorts()) {
            if (!ProbeSerial(p)) continue;
            if (preferred_ == "auto" || preferred_ == Id()) return true;
            Close();
        }
    if ((preferred_ == "auto" || preferred_ == "msipsu") && ProbeHid()) return true;
    return false;
}

bool Sources::ReadSerial(Reading& out, std::string& err) {
    HANDLE h = static_cast<HANDLE>(handle_);
    const size_t want = kind_ == Kind::WireView ? 100 : 192;
    uint8_t buf[192];
    PurgeComm(h, PURGE_RXCLEAR);
    uint8_t cmd = 0x04;
    if (!WriteAll(h, &cmd, 1)) { err = "write failed"; return false; }
    size_t got = ReadUpTo(h, buf, want);
    if (got != want) { err = "short read (" + std::to_string(got) + "/" + std::to_string(want) + " bytes)"; return false; }
    bool ok = kind_ == Kind::WireView ? DecodeWireView(buf, got, out) : DecodePmd2x(buf, got, out);
    if (!ok) err = "decode failed";
    return ok;
}

bool Sources::ReadHid(Reading& out, std::string& err) {
    HANDLE h = static_cast<HANDLE>(handle_);
    for (int attempt = 0; attempt < 2; ++attempt) {   // another app may poll the PSU too: retry once on a foreign reply
        std::vector<uint8_t> req(outLen_, 0), resp(inLen_, 0);
        req[0] = 0x00;   // report id
        req[1] = 0xE0;   // READ_EVERYTHING (read-only bulk status)
        if (!OverlappedIo(h, true, req.data(), DWORD(req.size()), 500)) { err = "HID write failed"; return false; }
        if (!OverlappedIo(h, false, resp.data(), DWORD(resp.size()), 700)) { err = "HID read timed out"; return false; }
        // reply: [report id][0xE0 echo][payload] - accept the echo at index 0 or 1, and verify via the 12V rail
        for (size_t e = 0; e < 2; ++e) {
            if (resp[e] != 0xE0) continue;
            for (size_t start : {e + 1, e}) {
                if (start + 44 > resp.size()) continue;
                if (DecodeMsiPsu(resp.data() + start, resp.size() - start, out)) return true;
            }
        }
    }
    err = "unexpected PSU reply";
    return false;
}

bool Sources::Read(Reading& out, std::string& err) {
    if (kind_ == Kind::None && !Discover()) { err = "no external per-pin meter found"; return false; }
    bool ok = kind_ == Kind::MsiPsu ? ReadHid(out, err) : ReadSerial(out, err);
    if (ok) { failures_ = 0; return true; }
    if (++failures_ >= 3) { Close(); lastScan_ = 0; }   // unplugged / taken by another app: rediscover
    return false;
}

} // namespace pins
