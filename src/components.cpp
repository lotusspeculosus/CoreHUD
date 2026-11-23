#include "components.h"
#include "tablet.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <dxgi.h>
#include <tlhelp32.h>
#include <cwchar>

namespace comp {
namespace {

bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring Env(const wchar_t* s) {
    wchar_t b[MAX_PATH * 2];
    DWORD n = ExpandEnvironmentStringsW(s, b, MAX_PATH * 2);
    return n ? std::wstring(b) : std::wstring();
}

std::wstring RegString(HKEY root, const wchar_t* key, const wchar_t* value, REGSAM view = 0) {
    HKEY k;
    if (RegOpenKeyExW(root, key, 0, KEY_READ | view, &k) != ERROR_SUCCESS) return {};
    wchar_t buf[MAX_PATH * 2];
    DWORD type = 0, size = sizeof(buf) - sizeof(wchar_t);
    std::wstring out;
    if (RegQueryValueExW(k, value, nullptr, &type, reinterpret_cast<BYTE*>(buf), &size) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ)) {
        buf[size / sizeof(wchar_t)] = 0;
        out = buf;
    }
    RegCloseKey(k);
    return out;
}

// exe name (lower-case) -> full path of running processes
struct Proc { std::wstring exe, path; };
std::vector<Proc> Processes() {
    std::vector<Proc> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{sizeof pe};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        Proc p;
        p.exe = pe.szExeFile;
        for (auto& c : p.exe) c = wchar_t(towlower(c));
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (h) {
            wchar_t path[MAX_PATH];
            DWORD n = MAX_PATH;
            if (QueryFullProcessImageNameW(h, 0, path, &n)) p.path = path;
            CloseHandle(h);
        }
        out.push_back(p);
    }
    CloseHandle(snap);
    return out;
}

const Proc* FindProc(const std::vector<Proc>& ps, const wchar_t* exe) {
    for (auto& p : ps) if (p.exe == exe) return &p;
    return nullptr;
}

// winget "portable" packages unpack to %LOCALAPPDATA%\Microsoft\WinGet\Packages\<id>_<source-hash>\...
std::wstring FindWingetPortable(const wchar_t* idPrefix, const wchar_t* exe) {
    std::wstring base = Env(L"%LOCALAPPDATA%\\Microsoft\\WinGet\\Packages\\");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((base + idPrefix + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::wstring found;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::wstring p = base + fd.cFileName + L"\\" + exe;
        if (FileExists(p)) { found = p; break; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

} // namespace

bool WingetAvailable() {
    return FileExists(Env(L"%LOCALAPPDATA%\\Microsoft\\WindowsApps\\winget.exe"));
}

std::vector<Component> Check() {
    auto procs = Processes();
    std::vector<Component> out;

    // Graphics driver(s): one row per GPU vendor present (NVIDIA, AMD, Intel).
    // A GPU without a vendor driver shows up as "Microsoft Basic Display Adapter" (vendor 0x1414).
    {
        bool nv = false, amd = false, intel = false, basic = false;
        IDXGIFactory1* f = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f)))) {
            IDXGIAdapter1* a = nullptr;
            for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
                DXGI_ADAPTER_DESC1 d{};
                a->GetDesc1(&d);
                a->Release();
                if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
                if (d.VendorId == 0x10DE) nv = true;
                else if (d.VendorId == 0x1002 || d.VendorId == 0x1022) amd = true;
                else if (d.VendorId == 0x8086) intel = true;
                else if (d.VendorId == 0x1414) basic = true;
            }
            f->Release();
        }
        if (nv) {
            Component c;
            c.id = L"nvidia";
            c.name = L"NVIDIA graphics driver";
            c.purpose = L"GPU stats and 12V-2x6 per-pin connector monitoring (on cards with a pin sensor).";
            c.hint = L"“Install latest” installs the NVIDIA App, which downloads the newest driver.";
            c.url = L"https://www.nvidia.com/en-us/drivers/";
            c.wingetId = L"XP8CLZL93F5Z4P";   // NVIDIA App (Microsoft Store source)
            c.wingetSource = L"msstore";
            c.required = true;
            c.installed = FileExists(Env(L"%SystemRoot%\\System32\\nvapi64.dll")) && FileExists(Env(L"%SystemRoot%\\System32\\nvml.dll"));
            c.running = c.installed;
            out.push_back(c);
        }
        if (amd) {
            Component c;
            c.id = L"amd";
            c.name = L"AMD graphics driver";
            c.purpose = L"Installed — GPU load, VRAM and temperature come from Windows.";
            c.hint = L"Update with AMD Software: Adrenalin Edition (download page).";
            c.url = L"https://www.amd.com/en/support/download/drivers.html";
            c.required = true;
            c.installed = c.running = true;   // DXGI only reports the AMD vendor id with the AMD driver loaded
            out.push_back(c);
        }
        if (intel) {
            Component c;
            c.id = L"intel";
            c.name = L"Intel graphics driver";
            c.purpose = L"Installed — GPU load, VRAM and temperature come from Windows.";
            c.hint = L"“Install latest” installs Intel Driver & Support Assistant, which finds the newest driver.";
            c.url = L"https://www.intel.com/content/www/us/en/support/detect.html";
            c.wingetId = L"Intel.IntelDriverAndSupportAssistant";
            c.required = true;
            c.installed = c.running = true;
            out.push_back(c);
        }
        if (!nv && !amd && !intel) {
            Component c;
            c.id = L"gpu";
            c.name = L"Graphics driver";
            c.purpose = basic ? L"Your GPU runs on the basic Windows display driver — install the driver from your GPU maker."
                              : L"No GPU found. Install the driver from your GPU maker.";
            c.hint = L"NVIDIA: nvidia.com/drivers · AMD: amd.com/support · Intel: intel.com/support";
            c.url = L"https://www.amd.com/en/support/download/drivers.html";
            c.required = true;
            out.push_back(c);
        }
    }
    {   // RTSS
        Component c;
        c.id = L"rtss";
        c.name = L"RivaTuner Statistics Server (RTSS)";
        c.purpose = L"Optional — provides FPS and frame time for the sensor panel.";
        c.hint = L"Must be running while you game.";
        c.url = L"https://www.guru3d.com/download/rtss-rivatuner-statistics-server-download/";
        c.wingetId = L"Guru3D.RTSS";
        c.needsRunning = true;
        std::wstring dir = RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Unwinder\\RTSS", L"InstallDir", KEY_WOW64_32KEY);
        if (dir.empty()) dir = Env(L"%ProgramFiles(x86)%\\RivaTuner Statistics Server");
        if (FileExists(dir + L"\\RTSS.exe")) c.exePath = dir + L"\\RTSS.exe";
        const Proc* p = FindProc(procs, L"rtss.exe");
        c.running = p != nullptr;
        if (p && !p->path.empty()) c.exePath = p->path;
        c.installed = !c.exePath.empty() || c.running;
        out.push_back(c);
    }
    {   // LibreHardwareMonitor
        Component c;
        c.id = L"lhm";
        c.name = L"LibreHardwareMonitor";
        c.purpose = L"Optional, free — CPU, motherboard, RAM, drive and fan sensors.";
        c.hint = L"Run it (admin), then Options \u203A Remote Web Server \u203A Run (port 8085). Versions before 0.9.5 work without that step.";
        c.url = L"https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/releases/latest";
        c.wingetId = L"LibreHardwareMonitor.LibreHardwareMonitor";
        c.needsRunning = true;
        c.runAsAdmin = true;
        const Proc* p = FindProc(procs, L"librehardwaremonitor.exe");
        c.running = p != nullptr;
        if (p && !p->path.empty()) c.exePath = p->path;
        if (c.exePath.empty()) c.exePath = FindWingetPortable(L"LibreHardwareMonitor.LibreHardwareMonitor", L"LibreHardwareMonitor.exe");
        c.installed = !c.exePath.empty() || c.running;
        out.push_back(c);
    }
    {   // Android platform-tools (adb): only needed for a USB tablet/phone panel
        Component c;
        c.id = L"adb";
        c.name = L"Android platform-tools (adb)";
        c.purpose = L"Optional, free \u2014 lets an old Android tablet or phone show the panel over USB.";
        c.hint = L"Sensor panel \u203A Tablet / phone. Wi-Fi tablets and iPads don't need this.";
        c.url = L"https://developer.android.com/tools/releases/platform-tools";
        c.wingetId = L"Google.PlatformTools";
        c.exePath = tab::FindAdb();
        c.installed = !c.exePath.empty();
        c.running = c.installed;
        out.push_back(c);
    }
    {   // HWiNFO
        Component c;
        c.id = L"hwinfo";
        c.name = L"HWiNFO64";
        c.purpose = L"Optional, free — every sensor HWiNFO sees (alternative to LibreHardwareMonitor).";
        c.hint = L"Free mode: Sensor settings › HWiNFO Gadget › “Enable reporting to Gadget”, then tick “Report value in Gadget” per sensor.";
        c.url = L"https://www.hwinfo.com/download/";
        c.wingetId = L"REALiX.HWiNFO";
        c.needsRunning = true;
        const Proc* p = FindProc(procs, L"hwinfo64.exe");
        if (!p) p = FindProc(procs, L"hwinfo.exe");
        c.running = p != nullptr;
        if (p && !p->path.empty()) c.exePath = p->path;
        for (const wchar_t* cand : {L"%ProgramFiles%\\HWiNFO64\\HWiNFO64.exe", L"%ProgramFiles%\\HWiNFO64\\HWiNFO.exe"})
            if (c.exePath.empty() && FileExists(Env(cand))) c.exePath = Env(cand);
        if (c.exePath.empty()) c.exePath = FindWingetPortable(L"REALiX.HWiNFO", L"HWiNFO64.exe");
        c.installed = !c.exePath.empty() || c.running;
        out.push_back(c);
    }
    return out;
}

void OpenUrl(const std::wstring& url) {
    if (url.rfind(L"https://", 0) == 0) ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

bool InstallWithWinget(const Component& c) {
    if (c.wingetId.empty()) return false;
    std::wstring args = L"/c title Installing " + c.name + L" & winget install --id " + c.wingetId + L" -e";
    if (!c.wingetSource.empty()) args += L" --source " + c.wingetSource;
    args += L" --accept-source-agreements --accept-package-agreements & echo. & echo Finished. Go back to CoreHUD and press Re-check. & pause";
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"cmd.exe", args.c_str(), nullptr, SW_SHOWNORMAL)) > 32;
}

bool Launch(const Component& c) {
    if (c.exePath.empty()) return false;
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, c.runAsAdmin ? L"runas" : L"open", c.exePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}

} // namespace comp
