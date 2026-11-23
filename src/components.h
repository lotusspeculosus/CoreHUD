// Detects the external components CoreHUD can use and helps installing them
// (download page or "winget install" of the latest version).
#pragma once
#include <string>
#include <vector>

namespace comp {

struct Component {
    std::wstring id;          // "nvidia", "rtss", "lhm", "hwinfo"
    std::wstring name;
    std::wstring purpose;     // what CoreHUD uses it for
    std::wstring hint;        // setup tip shown under the row
    std::wstring url;         // official download page
    std::wstring wingetId;    // empty = not installable via winget
    std::wstring wingetSource;
    bool required = false;    // NVIDIA driver: needed for GPU + pin monitoring
    bool installed = false;
    bool running = false;     // for apps that must be running to provide data
    bool needsRunning = false;
    bool runAsAdmin = false;
    std::wstring exePath;     // for "Start"
};

std::vector<Component> Check();
bool WingetAvailable();
void OpenUrl(const std::wstring& url);
bool InstallWithWinget(const Component& c);   // opens a console window running winget
bool Launch(const Component& c);

} // namespace comp
