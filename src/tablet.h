// Tablet / phone panel: RigSense serves the sensor panel to any browser (old Android tablets, iPads, phones).
//  - Wi-Fi: http://<pc-ip>:<port>/?k=<key>   (QR code in the app; the secret key keeps other devices out)
//  - USB:   `adb reverse` maps the tablet's localhost:<port> to the PC, so no network is involved at all.
// The PC renders the panel at the tablet's exact resolution and the page swaps in a JPEG twice per second.
// Read-only: the server only answers GET for the page, the frame image, a manifest and an icon.
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tab {

enum Mode { MODE_OFF = 0, MODE_USB = 1, MODE_WIFI = 2 };   // MODE_WIFI = USB + Wi-Fi

class Server {
public:
    ~Server() { Stop(); }
    bool Start(int port, bool lan, const std::string& key, std::string& err);
    void Stop();
    bool Running() const { return running_; }

    // UI thread: is anyone watching, and at which size? (w/h 0 = no request yet)
    bool Wanted(int& w, int& h);
    void SetFrame(std::vector<unsigned char>&& jpeg, int w, int h);
    void SetIcon(std::vector<unsigned char>&& png);
    void SetKeepAwakeVideo(std::vector<unsigned char>&& mp4);   // tiny looping clip that keeps the tablet screen on
    int Viewers();             // distinct clients seen in the last 5 s
    bool LastSize(int& w, int& h);   // resolution the most recent tablet/phone asked for (physical pixels)

private:
    void Loop();
    void Handle(uintptr_t sock, const std::string& peer);

    std::thread thread_;
    std::atomic<bool> running_{false}, stop_{false};
    uintptr_t listen_ = ~uintptr_t(0);
    int port_ = 0;
    std::string key_;
    std::mutex mx_;
    std::vector<unsigned char> frame_, icon_, video_;
    int frameW_ = 0, frameH_ = 0, wantW_ = 0, wantH_ = 0;
    unsigned long lastRequest_ = 0;
    struct Peer { std::string addr; unsigned long tick; };
    std::vector<Peer> peers_;
};

// LAN IPv4 addresses of this PC (for the Wi-Fi URL / QR code), best first.
std::vector<std::string> LocalIPv4();
std::string RandomKey();

// --- USB via adb ---
struct AdbDevice { std::string serial, state, model; bool reversed = false; };
class Adb {
public:
    ~Adb() { Stop(); }
    void Start(int port, const std::string& key, bool stayOn);
    void Stop();
    std::vector<AdbDevice> Devices();
    std::wstring AdbPath();          // "" if adb isn't installed
    std::string Status();
    void OpenOnTablets();            // re-open the panel page on every connected tablet

private:
    void Loop();
    std::thread thread_;
    std::atomic<bool> stop_{false}, reopen_{false};
    int port_ = 0;
    std::string key_;
    bool stayOn_ = true;
    std::mutex mx_;
    std::vector<AdbDevice> devices_;
    std::string status_;
    std::wstring adb_;
};

std::wstring FindAdb();

} // namespace tab
