// Connector wear check: learns how evenly the current is spread over the six 12V-2x6 pins under real load,
// day by day, and warns when one pin slowly starts carrying a bigger share than it used to. That pattern is
// what a worn, dirty or not-fully-seated connector looks like - long before any pin gets near its limit.
//
// Per day it stores, for samples with a solid load (>= kMinTotalA across the connector), each pin's average
// share of the total current (ideal: 1/6 = 16.7 %) and the average imbalance (highest share x 6; 1.00 = even).
// The first kBaseDays qualifying days are the baseline; the last kRecentDays are compared against it.
#pragma once
#include <string>
#include <vector>

namespace wear {

constexpr double kMinTotalA = 20.0;     // ~240 W through the connector: only real load counts
constexpr double kMinMinutes = 10.0;    // a day needs this much loaded time to count
constexpr int kBaseDays = 3;
constexpr int kRecentDays = 3;
constexpr double kWatchPts = 2.5;       // a pin's share up by this many percentage points: "watch"
constexpr double kWarnPts = 4.0;        // ... by this many: warning (16.7 % -> 20.7 % = ~24 % more current on that pin)

struct Day {
    std::string date;          // YYYY-MM-DD
    double minutes = 0;        // loaded minutes that day
    double share[6] = {};      // average share of the total current per pin (0..1)
    double imbalance = 0;      // average (highest share x 6)
};

enum State { LEARNING = 0, OK = 1, WATCH = 2, WARN = 3 };

struct Status {
    State state = LEARNING;
    int qualifyingDays = 0;    // days with >= kMinMinutes of load
    int pin = -1;              // pin (0-5) whose share rose the most
    double deltaPts = 0;       // its rise in percentage points
    double baseShare[6] = {}, recentShare[6] = {};
    double baseImb = 0, recentImb = 0;
    std::wstring text;         // one line for the UI
};

bool Load(const std::wstring& path, std::vector<Day>& days);
bool Save(const std::wstring& path, const std::vector<Day>& days);
Status Analyze(const std::vector<Day>& days);

// Accumulates samples into today's row (time-weighted). Not thread-safe: used by the monitor thread only.
class Tracker {
public:
    void Open(const std::wstring& path);                 // loads existing days, analyzes
    // amps: the 6 pins of one connector. Returns true if the sample counted (enough load).
    bool Add(const double* amps, const std::string& date, double dtSec);
    bool Flush();                                        // writes today's row, re-analyzes; true if the state changed
    void Reset();                                        // archive the file (new cable / reseated): start learning again
    const Status& status() const { return status_; }
    const std::wstring& path() const { return path_; }

private:
    void LoadToday(const std::string& date);
    std::wstring path_;
    std::vector<Day> days_;
    std::string today_;
    double sec_ = 0, shareSec_[6] = {}, imbSec_ = 0;     // today's time-weighted sums
    bool dirty_ = false;
    Status status_;
};

} // namespace wear
