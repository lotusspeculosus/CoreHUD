// Connector "black box": keeps the last minute of every sample in memory. When something goes wrong (a pin
// over its warning level or limit, a big imbalance, the sensor failing, a meter fault) it saves that minute
// plus the minute after it to a CSV file - so you always have hard evidence of what each pin did.
#pragma once
#include <deque>
#include <string>
#include <vector>

namespace bbox {

constexpr int kMaxPins = 12;

struct Sample {
    double t = 0;              // seconds (monotonic)
    std::string stamp;         // "YYYY-MM-DD HH:MM:SS.mmm"
    bool ok = false;           // pin readings valid
    int pinCount = 6;
    float volts[kMaxPins] = {}, amps[kMaxPins] = {};
    float connW = 0, boardW = -1;
    int gpuUtil = -1;
    float coreC = -1, hotspotC = -1, vramC = -1;
    int maxState = 0;          // highest pin state: 0 ok, 1 warning, 2 over the limit
    std::wstring events;       // events that happened at this sample ("" = none)
};

class Recorder {
public:
    // dir: where recordings go. pre/post: seconds kept before / recorded after the last trigger.
    void Configure(const std::wstring& dir, double preSec = 60, double postSec = 60, double maxSec = 900, int keepFiles = 100);
    // trigger = something alarming happened at this sample; reason = short name for the file ("pin3-limit").
    void Add(const Sample& s, bool trigger, const std::string& reason = "");
    std::wstring TakeFinished();   // path of a recording that just completed (once), else ""
    bool Recording() const { return f_ != nullptr; }
    void Close();                  // finish the current recording now (shutdown)
    ~Recorder() { Close(); }

private:
    void WriteRow(const Sample& s);
    void Prune();
    std::wstring dir_;
    double pre_ = 60, post_ = 60, max_ = 900;
    int keep_ = 100;
    std::deque<Sample> ring_;
    void* f_ = nullptr;            // FILE*
    std::wstring path_, finished_;
    int pins_ = 6;
    double start_ = 0, end_ = 0;
};

std::string CsvField(const std::wstring& s);   // UTF-8, quoted when needed (exposed for tests)

} // namespace bbox
